/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */
#include <Havk/Havk.h>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <renderer_lab/ScenePacket.h>
#include "Render/Renderer.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
struct Options {
    std::filesystem::path packet, gltf, sdk, output;
    RendererId backend = RendererId::XBrickMap;
    unsigned width = 2560, height = 1440, warmup = 600, frames = 1800, gridSize = 512, bounces = 3;
    bool approximations = false, visible = false;
};

unsigned positive(const std::string& value) {
    std::size_t end = 0;
    auto result = std::stoul(value, &end);
    if (end != value.size() || !result || result > 16384) throw std::runtime_error("Invalid experiment size/count");
    return static_cast<unsigned>(result);
}

Options parse(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        std::string key = argv[i];
        if (key == "--allow-approximations") { options.approximations = true; continue; }
        if (key == "--visible") { options.visible = true; continue; }
        if (++i == argc) throw std::runtime_error("Missing experiment argument value");
        std::string value = argv[i];
        if (key == "--packet") options.packet = value;
        else if (key == "--gltf") options.gltf = value;
        else if (key == "--sdk-source") options.sdk = value;
        else if (key == "--output") options.output = value;
        else if (key == "--width") options.width = positive(value);
        else if (key == "--height") options.height = positive(value);
        else if (key == "--warmup") options.warmup = positive(value);
        else if (key == "--frames") options.frames = positive(value);
        else if (key == "--grid-size") options.gridSize = positive(value);
        else if (key == "--bounces") options.bounces = positive(value);
        else if (key == "--backend") {
            if (value == "brick") options.backend = RendererId::XBrickMap;
            else if (value == "tree64") options.backend = RendererId::Tree64;
            else throw std::runtime_error("Voxel backend must be brick or tree64");
        } else throw std::runtime_error("Unknown experiment argument: " + key);
    }
    if (options.packet.empty() || options.gltf.empty() || options.sdk.empty() || options.output.empty())
        throw std::runtime_error("Required: --packet capture.rslb --gltf scene.gltf --sdk-source VoxelRT --output directory");
    if (!options.approximations)
        throw std::runtime_error("Voxel traversal currently uses quantized materials and SDK lighting. Explicit --allow-approximations is required; results cannot prove Minecraft speedup.");
    if (options.gridSize > 4096 || options.bounces > 8) throw std::runtime_error("Voxel grid/bounce limit exceeded");
    return options;
}

// Avoid selecting the integrated GPU while comparing a discrete-GPU renderer.
void selectNvidia() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    VkInstance instance{};
    if (vkCreateInstance(&info, nullptr, &instance) != VK_SUCCESS) throw std::runtime_error("Vulkan 1.3 instance unavailable");
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());
    uint32_t deviceId = 0;
    for (auto device : devices) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(device, &properties);
        if (properties.vendorID == 0x10de) { deviceId = properties.deviceID; break; }
    }
    vkDestroyInstance(instance, nullptr);
    if (!deviceId) throw std::runtime_error("No NVIDIA Vulkan adapter found");
    char value[16];
    std::snprintf(value, sizeof(value), "%x", deviceId);
    if (_putenv_s("HAVK_SELECT_DEVICE", value)) throw std::runtime_error("Could not select Vulkan adapter");
}

void saveAlbedo(havk::DeviceContext& ctx, havk::Image& image, const std::filesystem::path& path) {
    auto size = std::uint64_t(image.Desc.Width) * image.Desc.Height * 4;
    auto readback = ctx.CreateBuffer({.Size = size, .Usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .AllocFlags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT});
    ctx.Submit([&](havk::CommandList commands) {
        commands.TransitionLayout(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {image.Desc.Width, image.Desc.Height, 1};
        vkCmdCopyImageToBuffer(commands.Buffer, image.Handle, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            readback->Handle, 1, &region);
        commands.Barrier(*readback, {VK_ACCESS_HOST_READ_BIT, VK_PIPELINE_STAGE_HOST_BIT});
    }).Wait();
    readback->Invalidate();
    std::ofstream output(path, std::ios::binary);
    output << "P6\n" << image.Desc.Width << ' ' << image.Desc.Height << "\n255\n";
    auto bytes = static_cast<const char*>(readback->MappedData);
    for (std::uint64_t i = 0; i < size; i += 4) output.write(bytes + i, 3);
    if (!output) throw std::runtime_error("Could not write voxel albedo diagnostic");
}
} // namespace

int main(int argc, char** argv) {
    try {
        auto options = parse(argc, argv);
        renderer_lab::SceneState scene;
        scene.load(options.packet);
        scene.validate();
        if (!scene.hasCamera || scene.incomplete) throw std::runtime_error("Incomplete capture or missing camera");
        std::cout << "Loaded captured scene: " << scene.instances.size() << " instances" << std::endl;
        std::filesystem::create_directories(options.output);
        auto packet = std::filesystem::absolute(options.packet);
        auto gltf = std::filesystem::absolute(options.gltf);
        auto sdk = std::filesystem::absolute(options.sdk);
        auto output = std::filesystem::absolute(options.output);
        std::filesystem::current_path(sdk);
        selectNvidia();
        std::cout << "Selected NVIDIA Vulkan adapter" << std::endl;
        if (!glfwInit()) throw std::runtime_error("GLFW initialization failed");
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        glfwWindowHint(GLFW_VISIBLE, options.visible ? GLFW_TRUE : GLFW_FALSE);
        auto window = glfwCreateWindow(options.width, options.height, "Minecraft Voxel Traversal Experiment", nullptr, nullptr);
        if (!window) throw std::runtime_error("Voxel rendering surface unavailable");
        {
            auto ctx = havk::Create({.Window = window, .ShaderBasePath = sdk / "src/VoxelRT/Shaders",
                .EnableShaderHotReload = false, .EnableDebugLayers = false});
            std::cout << "Created Vulkan device: " << ctx->PhysicalDeviceInfo.Props.deviceName << std::endl;
            if (ctx->PhysicalDeviceInfo.Props.vendorID != 0x10de) throw std::runtime_error("Unexpected GPU adapter");
            ctx->Swapchain->PresentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
            ctx->Swapchain->Initialize();
            ImGui::CreateContext();
            auto map = std::make_shared<VoxelMap>();
            map->VoxelizeModel(gltf.string(), glm::ivec3(0), glm::ivec3(options.gridSize));
            std::cout << "Voxelized captured geometry" << std::endl;
            havx::Camera camera{};
            camera.Position = (glm::make_vec3(scene.camera.position.data()) - map->importedOrigin) * map->importedScale + map->importedCenter;
            camera.ViewPosition = camera.Position;
            auto forward = glm::make_vec3(scene.camera.forward.data());
            auto up = glm::make_vec3(scene.camera.up.data());
            camera.ViewRotation = glm::quat_cast(glm::lookAt(glm::vec3(0), forward, up));
            camera.FieldOfView = scene.camera.fovYDegrees;
            camera.AspectRatio = float(options.width) / options.height;
            auto renderer = Renderer::Create(ctx.get(), map, options.backend);
            auto gpu = dynamic_cast<GpuRenderer*>(renderer.get());
            if (!gpu) throw std::runtime_error("Selected voxel renderer lacks GPU timestamp queries");
            GBuffer gbuffer(ctx.get());
            gbuffer.NumLightBounces = options.bounces;
            std::ofstream frames(output / "frames.csv");
            frames << "frame,standaloneWallNs,voxelTraceGpuNs\n";
            unsigned gpuSamples = 0;
            for (unsigned frame = 0; frame < options.warmup + options.frames; ++frame) {
                auto start = std::chrono::steady_clock::now();
                auto [image, commands] = ctx->Swapchain->AcquireImage();
                glfwPollEvents();
                if (glfwWindowShouldClose(window)) throw std::runtime_error("Voxel experiment interrupted");
                auto changed = renderer->SyncMap(camera, commands);
                gbuffer.SetCamera(commands, camera, {options.width, options.height}, changed);
                renderer->RenderFrame(camera, &gbuffer, commands);
                gbuffer.Resolve(image, commands);
                ctx->Swapchain->Present();
                ctx->Tick();
                auto wall = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
                if (frame >= options.warmup) {
                    auto begin = gpu->LastFrameStats.Counters[FramePerfStats::Frame_StartTS];
                    auto end = gpu->LastFrameStats.Counters[FramePerfStats::Frame_EndTS];
                    auto duration = end > begin ? end - begin : 0;
                    gpuSamples += duration > 0;
                    frames << frame - options.warmup << ',' << wall << ',' << duration << '\n';
                }
            }
            ctx->WaitDeviceIdle();
            saveAlbedo(*ctx, *gbuffer.AlbedoTex, output / "albedo.ppm");
            std::ofstream manifest(output / "experiment.json");
            manifest << "{\n  \"schemaVersion\":1,\n  \"scope\":\"standalone-voxel-diagnostic\",\n"
                << "  \"backend\":\"" << (options.backend == RendererId::Tree64 ? "tree64" : "brick") << "\",\n"
                << "  \"endToEndMinecraft\":false,\n  \"fidelityEligible\":false,\n  \"frameGeneration\":false,\n"
                << "  \"width\":" << options.width << ",\n  \"height\":" << options.height
                << ",\n  \"recordedFrames\":" << options.frames << ",\n  \"gpuTimestampSamples\":" << gpuSamples
                << ",\n  \"sourceInstances\":" << scene.instances.size() << ",\n  \"sourceMaterials\":" << scene.materials.size()
                << ",\n  \"gridSize\":" << options.gridSize << ",\n"
                << "  \"limitations\":[\"Voxelized surfaces and quantized 240-color palette\","
                   "\"Original PBR, transmission, animated updates and triangle fallback are not represented\","
                   "\"Uses the upstream voxel test lighting, not captured Minecraft lighting\","
                   "\"GPU timestamps measure voxel tracing only; wall times belong to this standalone application\"]\n}\n";
            if (!frames || !manifest || !gpuSamples) throw std::runtime_error("Voxel timestamp/report capture failed");
            std::cout << "VOXEL_EXPERIMENT_COMPLETE: recorded=" << options.frames << " gpuSamples=" << gpuSamples << " fidelityEligible=false\n";
            renderer.reset();
            ImGui::DestroyContext();
        }
        glfwDestroyWindow(window);
        glfwTerminate();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Voxel experiment failed: " << error.what() << '\n';
        return 1;
    }
}
