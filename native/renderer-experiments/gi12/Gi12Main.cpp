/* SPDX-License-Identifier: Apache-2.0 */
#include <windows.h>
#include <capsaicin.h>
#include <renderer_lab/GltfExport.h>
#include <nlohmann/json.hpp>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
using Json = nlohmann::json;
namespace fs = std::filesystem;
constexpr double Pi = 3.14159265358979323846;

struct Options {
    fs::path packet, output, sdk;
    std::string adapter = "RTX 4080 SUPER";
    std::uint32_t width = 2560, height = 1440, warmup = 600, frames = 180, runs = 3;
    bool allowApproximations = false;
    bool taa = false;
};

void check(HRESULT result, const char* operation) {
    if (FAILED(result)) throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(result));
}

void checkGfx(GfxResult result, const char* operation) {
    if (result != kGfxResult_NoError)
        throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(result));
}

void writeJson(const fs::path& path, const Json& value) {
    std::ofstream file(path, std::ios::binary);
    file << value.dump(2) << '\n';
    if (!file) throw std::runtime_error("Cannot write " + path.string());
}

std::uint32_t number(const std::string& value, std::uint32_t maximum) {
    std::size_t end = 0;
    const auto result = std::stoul(value, &end);
    if (end != value.size() || result == 0 || result > maximum)
        throw std::runtime_error("Invalid positive integer: " + value);
    return static_cast<std::uint32_t>(result);
}

Options parse(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        if (key == "--allow-approximations") { options.allowApproximations = true; continue; }
        if (++i == argc) throw std::runtime_error("Missing value for " + key);
        const std::string value = argv[i];
        if (key == "--scene") options.packet = fs::absolute(value);
        else if (key == "--output") options.output = fs::absolute(value);
        else if (key == "--sdk-source") options.sdk = fs::absolute(value);
        else if (key == "--adapter") options.adapter = value;
        else if (key == "--aa") {
            if (value == "fsr2")
                throw std::runtime_error("GI-1.2 --aa fsr2 is unsupported: the pinned SDK produces spatially incorrect "
                    "fixture output even with corrected dispatch dimensions and separate resources. Use --aa none; "
                    "the anti-aliasing quality gap remains unresolved, with no automatic fallback.");
            if (value != "none") throw std::runtime_error("GI-1.2 currently supports only --aa none");
            options.taa = false;
        }
        else if (key == "--width") options.width = number(value, 16384);
        else if (key == "--height") options.height = number(value, 16384);
        else if (key == "--warmup") options.warmup = number(value, 100000);
        else if (key == "--frames") options.frames = number(value, 100000);
        else if (key == "--runs") options.runs = number(value, 100);
        else throw std::runtime_error("Unknown option: " + key);
    }
    if (options.packet.empty() || options.output.empty() || options.sdk.empty())
        throw std::runtime_error("Usage: RendererLabGI12App --scene frame.rslb --output NEW_DIRECTORY "
            "--sdk-source Capsaicin [--adapter NAME] [--width 2560 --height 1440] "
            "[--warmup 600 --frames 180 --runs 3] [--aa none] [--allow-approximations]");
    if (!fs::is_regular_file(options.packet)) throw std::runtime_error("The ScenePacket file does not exist");
    if (!fs::is_regular_file(options.sdk / "src/core/src/gpu_shared.h"))
        throw std::runtime_error("--sdk-source must identify the prepared Capsaicin checkout containing its shaders");
    if (fs::exists(options.output)) throw std::runtime_error("Output already exists; use a new directory for each experiment");
    if (options.adapter.empty()) throw std::runtime_error("An explicit hardware adapter name is required");
    return options;
}

std::string utf8(const wchar_t* value) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0) throw std::runtime_error("Cannot decode adapter name");
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), size, nullptr, nullptr);
    result.pop_back();
    return result;
}

struct Adapter {
    ComPtr<IDXGIAdapter1> handle;
    DXGI_ADAPTER_DESC1 description{};
    std::string name;
};

Adapter selectAdapter(const std::string& wanted) {
    ComPtr<IDXGIFactory1> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "CreateDXGIFactory1");
    std::vector<Adapter> matches;
    for (UINT index = 0;; ++index) {
        Adapter candidate;
        const auto result = factory->EnumAdapters1(index, &candidate.handle);
        if (result == DXGI_ERROR_NOT_FOUND) break;
        check(result, "EnumAdapters1");
        check(candidate.handle->GetDesc1(&candidate.description), "GetDesc1");
        candidate.name = utf8(candidate.description.Description);
        if (!(candidate.description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            && candidate.name.find(wanted) != std::string::npos) matches.push_back(std::move(candidate));
    }
    if (matches.size() != 1) throw std::runtime_error("Adapter selection must match exactly one hardware GPU: " + wanted);
    return std::move(matches.front());
}

struct Graphics {
    GfxContext context;
    bool initialized = false;
    ~Graphics() {
        if (initialized) Capsaicin::Terminate();
        if (context) gfxDestroyContext(context);
    }
};

class GpuTimers {
public:
    GpuTimers(GfxContext context, std::uint32_t frames) : context_(context), frames_(frames) {
        auto* device = gfxGetDevice(context);
        check(gfxGetCommandQueue(context)->GetTimestampFrequency(&frequency_), "GetTimestampFrequency");
        if (!frequency_) throw std::runtime_error("GPU timestamp frequency is zero");
        D3D12_QUERY_HEAP_DESC query{};
        query.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        query.Count = 2 * frames;
        check(device->CreateQueryHeap(&query, IID_PPV_ARGS(&heap_)), "CreateQueryHeap");
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = static_cast<UINT64>(frames) * 2 * sizeof(UINT64);
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback_)), "Create timestamp readback");
    }

    void begin(std::uint32_t frame) const {
        gfxGetCommandList(context_)->EndQuery(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frame * 2);
    }

    void end(std::uint32_t frame) const {
        auto* commands = gfxGetCommandList(context_);
        commands->EndQuery(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frame * 2 + 1);
        commands->ResolveQueryData(heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, frame * 2, 2,
            readback_.Get(), static_cast<UINT64>(frame) * 2 * sizeof(UINT64));
    }

    std::vector<double> milliseconds() const {
        // gfx's public duration helper applies an EMA. Read raw DX12 query pairs
        // instead so percentile samples represent actual individual GPU frames.
        checkGfx(gfxFinish(context_), "Complete GPU timestamp readback");
        const auto size = static_cast<SIZE_T>(frames_) * 2 * sizeof(UINT64);
        D3D12_RANGE range{0, size};
        void* mapped = nullptr;
        check(readback_->Map(0, &range, &mapped), "Map GPU timestamps");
        const auto* ticks = static_cast<const UINT64*>(mapped);
        std::vector<double> samples;
        samples.reserve(frames_);
        for (std::uint32_t i = 0; i < frames_; ++i) {
            if (ticks[2 * i + 1] <= ticks[2 * i]) {
                const D3D12_RANGE unchanged{0, 0};
                readback_->Unmap(0, &unchanged);
                throw std::runtime_error("GPU timestamps are missing or degenerate; no timing result is valid");
            }
            samples.push_back(static_cast<double>(ticks[2 * i + 1] - ticks[2 * i]) * 1000.0 / frequency_);
        }
        const D3D12_RANGE unchanged{0, 0};
        readback_->Unmap(0, &unchanged);
        return samples;
    }

private:
    GfxContext context_;
    std::uint32_t frames_;
    UINT64 frequency_{};
    ComPtr<ID3D12QueryHeap> heap_;
    ComPtr<ID3D12Resource> readback_;
};

void limitation(const Options& options, std::vector<std::string>& warnings, const std::string& text) {
    if (std::find(warnings.begin(), warnings.end(), text) == warnings.end()) warnings.push_back(text);
    if (!options.allowApproximations) throw std::runtime_error(text + "; use --allow-approximations to record this limitation");
}

std::array<float, 4> directionRotation(const std::array<float, 3>& direction) {
    const auto length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
    if (!(length > 0) || !std::isfinite(length)) throw std::runtime_error("Invalid directional light vector");
    const float x = direction[0] / length, y = direction[1] / length, z = direction[2] / length;
    if (z > .99999f) return {0, 1, 0, 0};
    const auto scale = std::sqrt(2.f * (1.f - z));
    return {y / scale, -x / scale, 0, scale * .5f};
}

struct SceneImport {
    renderer_lab::ExportResult exported;
    fs::path gltf, environment;
    std::vector<std::string> warnings;
    std::uint32_t punctualLights{};
};

SceneImport prepareScene(const Options& options, const renderer_lab::SceneState& source) {
    SceneImport result;
    result.exported = renderer_lab::exportGltf(source, options.output / "scene", {options.allowApproximations});
    result.warnings = result.exported.limitations;
    std::ifstream input(result.exported.gltfFile);
    Json gltf;
    input >> gltf;
    auto lights = Json::array();
    for (const auto& [id, light] : source.lights) {
        if (light.type == 5) {
            if (!result.environment.empty()) {
                limitation(options, result.warnings, "Multiple source dome lights cannot be blended by this GI-1.2 adapter");
                continue;
            }
            if (!light.colorTexture || !source.textures.contains(light.colorTexture))
                throw std::runtime_error("The source dome references a missing captured texture");
            const auto& texture = source.textures.at(light.colorTexture);
            const auto extension = !texture.file.empty() ? texture.file.extension().string()
                : texture.format >= 6 ? ".hdr" : ".png";
            if (extension != ".hdr" && extension != ".exr") {
                limitation(options, result.warnings, "Capsaicin environment maps require HDR/EXR; source dome was not imported");
                continue;
            }
            result.environment = result.exported.gltfFile.parent_path()
                / "textures" / (std::to_string(light.colorTexture) + extension);
            if (light.radiance != std::array<float, 3>{1, 1, 1})
                limitation(options, result.warnings, "Dome radiance tint is not applied by this GI-1.2 adapter");
            continue;
        }
        if (light.type != 0 && light.type != 1) {
            limitation(options, result.warnings, "Rect/disk/cylinder source analytical lights are not imported into GI-1.2");
            continue;
        }
        const float peak = std::max({light.radiance[0], light.radiance[1], light.radiance[2]});
        if (!(peak > 0)) continue;
        const std::array<float, 3> color{light.radiance[0] / peak, light.radiance[1] / peak, light.radiance[2] / peak};
        // gfx converts glTF photometric intensity to radiometric units using
        // 683 lm/W; sphere-to-point normalization matches the shared exporter.
        const double intensity = 683.0 * Pi * peak * (light.type == 0 ? light.size[0] * light.size[0] : 1.0);
        lights.push_back({{"name", "Minecraft light " + std::to_string(id)},
            {"type", light.type == 0 ? "point" : "directional"}, {"color", color}, {"intensity", intensity}});
        Json node = {{"name", "Minecraft light " + std::to_string(id)},
            {"extensions", {{"KHR_lights_punctual", {{"light", lights.size() - 1}}}}}};
        if (light.type == 0) {
            node["translation"] = light.position;
            if (light.size[0] > 0)
                limitation(options, result.warnings, "Spherical analytical lights use point approximations without finite-radius shadows");
        } else {
            node["rotation"] = directionRotation(light.direction);
            if (light.size[0] > 0)
                limitation(options, result.warnings, "Directional analytical light angular extent is not reproduced");
        }
        gltf["scenes"][0]["nodes"].push_back(gltf["nodes"].size());
        gltf["nodes"].push_back(std::move(node));
    }
    if (!lights.empty()) {
        result.punctualLights = static_cast<std::uint32_t>(lights.size());
        gltf["extensionsUsed"].push_back("KHR_lights_punctual");
        gltf["extensions"]["KHR_lights_punctual"]["lights"] = std::move(lights);
    }
    for (const auto& [id, material] : source.materials) {
        if (material.kind != 0 || material.alphaMode == 2)
            limitation(options, result.warnings,
                "GI-1.2 transparent/transmissive material parity is unvalidated (material " + std::to_string(id) + ")");
    }
    result.gltf = result.exported.gltfFile.parent_path() / "gi12.gltf";
    writeJson(result.gltf, gltf);
    return result;
}

Json statistics(const std::vector<double>& values) {
    auto sorted = values;
    std::sort(sorted.begin(), sorted.end());
    const auto percentile = [&sorted](double quantile) {
        const auto index = static_cast<std::size_t>(std::ceil(quantile * sorted.size())) - 1;
        return sorted[std::min(index, sorted.size() - 1)];
    };
    return {{"meanMs", std::accumulate(values.begin(), values.end(), 0.0) / values.size()},
        {"medianMs", percentile(.5)}, {"p95Ms", percentile(.95)}, {"p99Ms", percentile(.99)}};
}

glm::vec3 vector(const std::array<float, 3>& value) { return {value[0], value[1], value[2]}; }

Json renderRun(Graphics& graphics, const Options& options, const renderer_lab::SceneState& source,
    const SceneImport& imported, std::uint32_t run) {
    const auto directory = options.output / ("run_" + std::to_string(run));
    fs::create_directories(directory);
    Capsaicin::Initialize(graphics.context, nullptr);
    graphics.initialized = true;
    if (!Capsaicin::SetRenderer("GI-1.2") || Capsaicin::GetCurrentRenderer() != "GI-1.2")
        throw std::runtime_error("The imported SDK did not activate GI-1.2; no renderer fallback is allowed");
    if (!Capsaicin::SetDebugView("None")) throw std::runtime_error("Cannot select the lit GI-1.2 output");
    if (!Capsaicin::SetScene(imported.gltf))
        throw std::runtime_error("Capsaicin SetScene rejected the supplied Minecraft glTF");
    if (Capsaicin::GetSceneCameras().empty()) throw std::runtime_error("Capsaicin has no active scene camera");
    if (!Capsaicin::SetSceneCamera("User")) throw std::runtime_error("Capsaicin did not expose its explicit User camera");
    const auto& camera = source.camera;
    Capsaicin::SetSceneCameraView(vector(camera.position), vector(camera.forward), vector(camera.up));
    Capsaicin::SetSceneCameraFOV(static_cast<float>(camera.fovYDegrees * Pi / 180.0));
    Capsaicin::SetSceneCameraRange({camera.zNear, camera.zFar});
    Capsaicin::SetRenderDimensionsScale(1.f);
    Capsaicin::SetPaused(true);
    Capsaicin::SetFixedFrameRate(true);
    Capsaicin::SetFixedFrameTime(1.0 / 60.0);
    if (!Capsaicin::hasOption<bool>("taa_enable") || !Capsaicin::hasOption<std::uint8_t>("fsr_version"))
        throw std::runtime_error("GI-1.2's expected native-resolution FSR2/TAA settings are unavailable");
    Capsaicin::setOption("taa_enable", options.taa);
    Capsaicin::setOption("fsr_version", static_cast<std::uint8_t>(1));
    Capsaicin::setOption("atmosphere_enable", false);
    if (!imported.environment.empty() && !Capsaicin::SetEnvironmentMap(imported.environment))
        throw std::runtime_error("Capsaicin rejected the captured environment; no demo environment is substituted");
    for (std::uint32_t frame = 0; frame < options.warmup; ++frame) {
        Capsaicin::Render();
        checkGfx(gfxFrame(graphics.context, false), "Warmup frame");
    }
    // SetScene imports CPU objects; the SDK publishes triangle statistics while
    // updating GPU scene buffers on Render, not at SetScene return.
    const auto importedTriangles = Capsaicin::GetTriangleCount();
    if (importedTriangles == 0)
        throw std::runtime_error("Capsaicin imported no rendered triangles after warmup");
    if (importedTriangles != imported.exported.triangles)
        throw std::runtime_error("Capsaicin triangle count differs from the exported Minecraft scene after warmup");
    if (Capsaicin::GetRenderDimensions() != std::pair{options.width, options.height})
        throw std::runtime_error("GI-1.2 did not render at the requested native framebuffer dimensions");
    if (Capsaicin::GetCurrentRenderer() != "GI-1.2")
        throw std::runtime_error("The active renderer changed during warmup; timing is invalid");
    if (Capsaicin::getOption<bool>("taa_enable"))
        throw std::runtime_error("The SDK enabled unsupported anti-aliasing during warmup");
    const auto observedCamera = Capsaicin::GetSceneCameraView();
    if (glm::length(observedCamera.position - vector(camera.position)) > .0001f
        || glm::length(observedCamera.forward - glm::normalize(vector(camera.forward))) > .0001f
        || std::abs(Capsaicin::GetSceneCameraFOV() - static_cast<float>(camera.fovYDegrees * Pi / 180.0)) > .0001f)
        throw std::runtime_error("The active camera differs from the captured ScenePacket camera");
    GpuTimers timers(graphics.context, options.frames);
    std::vector<double> cpu;
    cpu.reserve(options.frames);
    for (std::uint32_t frame = 0; frame < options.frames; ++frame) {
        const auto start = std::chrono::steady_clock::now();
        timers.begin(frame);
        Capsaicin::Render();
        timers.end(frame);
        checkGfx(gfxFrame(graphics.context, false), "Measured frame");
        cpu.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
    }
    const auto gpu = timers.milliseconds();
    // At scale 1 the SDK's FSR pass writes Color in place. Its generic "None"
    // dump still selects the optional, unused ColorScaled allocation.
    Capsaicin::DumpDebugView(directory / "final.exr", "Color");
    Capsaicin::DumpDebugView(directory / "final.jpeg", "Color");
    checkGfx(gfxFrame(graphics.context, false), "Submit final image copies");
    // Primary-visibility diagnostics are outside the measured interval. They
    // distinguish scene/camera import failures from lighting/reconstruction.
    const auto debugViews = Capsaicin::GetDebugViews();
    for (const auto view : {std::string_view("MaterialAlbedo"), std::string_view("Depth")}) {
        if (std::find(debugViews.begin(), debugViews.end(), view) != debugViews.end()) {
            if (!Capsaicin::SetDebugView(view)) throw std::runtime_error("Cannot activate requested visibility diagnostic");
            Capsaicin::Render();
            Capsaicin::DumpDebugView(directory / (std::string(view) + ".jpeg"), view);
            checkGfx(gfxFrame(graphics.context, false), "Submit visibility diagnostic");
        }
    }
    Capsaicin::SetDebugView("None");
    Capsaicin::DumpCamera(directory / "camera.json", false);
    const auto triangles = Capsaicin::GetTriangleCount();
    const auto bvhBytes = Capsaicin::GetBvhDataSize();
    const auto deltaLights = Capsaicin::GetDeltaLightCount();
    const auto areaLights = Capsaicin::GetAreaLightCount();
    const auto environmentLights = Capsaicin::GetEnvironmentLightCount();
    Capsaicin::Terminate();
    graphics.initialized = false;
    for (const char* file : {"final.exr", "final.jpeg", "camera.json"})
        if (!fs::is_regular_file(directory / file) || fs::file_size(directory / file) == 0)
            throw std::runtime_error(std::string("The SDK did not produce required capture: ") + file);
    Json result = {{"run", run}, {"warmupFrames", options.warmup}, {"recordedFrames", options.frames},
        {"gpuRender", statistics(gpu)}, {"cpuRenderAndSubmit", statistics(cpu)},
        {"gpuFrameMs", gpu}, {"cpuFrameMs", cpu}, {"triangles", triangles}, {"bvhBytes", bvhBytes},
        {"deltaLights", deltaLights}, {"areaLights", areaLights}, {"environmentLights", environmentLights}};
    writeJson(directory / "timings.json", result);
    return result;
}
} // namespace

int main(int argc, char** argv) {
    fs::path output;
    try {
        const auto options = parse(argc, argv);
        output = options.output;
        fs::create_directories(output);
        renderer_lab::SceneState source;
        source.load(options.packet);
        if (!source.hasCamera || !(source.camera.fovYDegrees > 0 && source.camera.fovYDegrees < 180))
            throw std::runtime_error("A valid captured perspective camera is required");
        auto imported = prepareScene(options, source);
        const auto aspect = static_cast<float>(options.width) / options.height;
        if (std::abs(source.camera.aspect - aspect) > .0001f)
            limitation(options, imported.warnings, "Output aspect ratio differs from the captured camera");
        const auto adapter = selectAdapter(options.adapter);
        fs::current_path(options.sdk);
        Graphics graphics;
        graphics.context = gfxCreateContext(options.width, options.height,
            kGfxCreateContextFlag_EnableShaderCache, adapter.handle.Get());
        if (!graphics.context || !gfxIsRaytracingSupported(graphics.context))
            throw std::runtime_error("The selected adapter cannot initialize Capsaicin DX12 ray tracing");
        const auto actual = gfxGetDevice(graphics.context)->GetAdapterLuid();
        if (actual.HighPart != adapter.description.AdapterLuid.HighPart || actual.LowPart != adapter.description.AdapterLuid.LowPart)
            throw std::runtime_error("gfx initialized a different GPU than the explicitly selected adapter");
        auto runs = Json::array();
        for (std::uint32_t run = 1; run <= options.runs; ++run) {
            std::cout << "GI-1.2 standalone run " << run << '/' << options.runs << " on " << adapter.name << std::endl;
            runs.push_back(renderRun(graphics, options, source, imported, run));
        }
        writeJson(output / "manifest.json", {
            {"schemaVersion", 1}, {"status", "rendered"}, {"backend", "gi12"}, {"activeRenderer", "GI-1.2"},
            {"sdk", "Capsaicin v1.3"}, {"scope", "standalone-static-scene"}, {"endToEndMinecraft", false},
            {"qualityGatePassed", false}, {"speedupClaim", nullptr}, {"fallback", false}, {"headless", true},
            {"adapter", adapter.name}, {"vendorId", adapter.description.VendorId},
            {"dedicatedVideoMemoryBytes", adapter.description.DedicatedVideoMemory},
            {"width", options.width}, {"height", options.height}, {"renderScale", 1.0},
            {"antiAliasingRequested", "none"}, {"antiAliasingRuntimeVerified", true},
            {"knownAntiAliasingGap", "FSR2 is disabled/rejected because the pinned SDK produces spatially incorrect fixture output; no AA parity claim"},
            {"frameGeneration", false}, {"vsync", false},
            {"timingSource", "raw DX12 command-queue timestamp pairs around Capsaicin::Render"},
            {"scenePacket", options.packet.string()}, {"scenePacketVersion", renderer_lab::SceneState::Version},
            {"sourceFrameId", source.frameId}, {"captureIncomplete", source.incomplete},
            {"camera", {{"position", source.camera.position}, {"forward", source.camera.forward},
                {"up", source.camera.up}, {"fovYDegrees", source.camera.fovYDegrees},
                {"near", source.camera.zNear}, {"far", source.camera.zFar}}},
            {"sourceInstances", source.instances.size()}, {"sourceMaterials", source.materials.size()},
            {"sourceLights", source.lights.size()}, {"importedPunctualLights", imported.punctualLights},
            {"approximationsAllowed", options.allowApproximations}, {"sceneLimitations", imported.warnings},
            {"unvalidatedFeatures", {"Transparency/refraction parity", "Rough-reflection and PBR parity",
                "Dome orientation/blending and extended analytical-light shaping",
                "Dynamic entities and world edits", "Temporal camera-path stability", "Minecraft UI/GL interop",
                "Distant Horizons/LOD", "Remix conf layers, volumetrics and material extensions"}},
            {"runs", runs}});
        std::cout << "Wrote standalone GPU timings and images; no end-to-end Minecraft FPS claim: " << output << '\n';
        return 0;
    } catch (const std::exception& error) {
        if (!output.empty()) {
            try { writeJson(output / "failure.json", {{"status", "failed"}, {"scope", "standalone-static-scene"},
                {"qualityGatePassed", false}, {"speedupClaim", nullptr}, {"message", error.what()}}); }
            catch (...) {}
        }
        std::cerr << "GI-1.2 experiment failed: " << error.what() << '\n';
        return 1;
    }
}
