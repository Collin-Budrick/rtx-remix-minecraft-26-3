/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */
// Reuse the SDK's device/driver setup and original entrypoint implementation.
// The renderer itself is compiled from a checked adaptation in the build tree.
#define WinMain RendererLabOriginalFullSampleWinMain
#include <main.cpp>
#undef WinMain
#include "RestirReplay.h"
#include <renderer_lab/GltfExport.h>
#include <App/Screenshot.h>
#include <Profiler.h>
#include <Scene/SampleScene.h>
#include <donut/app/Camera.h>
#include <donut/engine/SceneGraph.h>
#include <nlohmann/json.hpp>
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <stdexcept>

namespace renderer_lab {
RestirReplayConfig& replayConfig() { static RestirReplayConfig config; return config; }
namespace {
struct ReplayArguments {
    std::filesystem::path packet,output;
    bool approximations{};
    std::string aa="rr";
    std::uint32_t warmup=120,frames=120;
    std::vector<std::string> sdk;
};
ReplayArguments parseReplayArguments(int argc,char** argv) {
    ReplayArguments args; args.sdk.emplace_back(argv[0]);
    auto value=[&](int& i) { if (++i>=argc) throw std::runtime_error("Missing replay argument value"); return std::string(argv[i]); };
    auto count=[&](const std::string& text) {
        std::size_t end; auto n=std::stoull(text,&end);
        if (end!=text.size()||n>1000000) throw std::runtime_error("Replay frame count must be at most one million");
        return std::uint32_t(n);
    };
    for (int i=1;i<argc;++i) {
        std::string key=argv[i];
        if (key=="--packet") args.packet=value(i);
        else if (key=="--export-dir"||key=="--output") args.output=value(i);
        else if (key=="--allow-approximations") args.approximations=true;
        else if (key=="--warmup") args.warmup=count(value(i));
        else if (key=="--frames") args.frames=count(value(i));
        else if (key=="--aa") args.aa=value(i);
        else if (key.starts_with("--scene.")||key.starts_with("--camera.")||key.starts_with("--device.backBuffer")||key.starts_with("--profiling.")||key.starts_with("--rendering.")||key=="--lighting.indirectMode"||key=="--device.vk")
            throw std::runtime_error("Replay owns scene, camera, resolution, API, frame schedule and indirect mode; use its documented options");
        else args.sdk.push_back(key);
    }
    if (args.packet.empty()) throw std::runtime_error("Provide --packet capture.rslb; a sample world is never substituted");
    if (!args.frames) throw std::runtime_error("--frames must be positive");
    if (args.aa!="rr"&&args.aa!="nrd"&&args.aa!="taa"&&args.aa!="none") throw std::runtime_error("--aa must be rr, nrd, taa, or none");
    args.packet=std::filesystem::absolute(args.packet);
    if (args.output.empty()) args.output=args.packet.parent_path()/(args.packet.stem().string()+"-restir-pt");
    args.output=std::filesystem::absolute(args.output); return args;
}
void writeJson(const std::filesystem::path& path,const nlohmann::json& data) {
    std::ofstream file(path,std::ios::binary); file << data.dump(2) << '\n';
    if (!file) throw std::runtime_error("Could not write replay report: "+path.string());
}
void prepareScene(const ReplayArguments& args) {
    auto& config=replayConfig(); config.scene.load(args.packet);
    config.directory=args.output; config.warmupFrames=args.warmup; config.recordFrames=args.frames;
    config.requireRayReconstruction=args.aa=="rr";
    auto exported=exportGltf(config.scene,args.output,{args.approximations});
    nlohmann::json sceneFile; std::ifstream sceneInput(exported.sceneFile); sceneInput>>sceneFile;
    auto& graph=sceneFile["graph"];
    graph.erase(std::remove_if(graph.begin(),graph.end(),[](const auto& node) { return node.value("type",std::string{})=="EnvironmentLight"; }),graph.end());
    std::size_t domes=0;
    for (const auto& [id,light]:config.scene.lights) {
        if (light.type!=5) continue;
        if (++domes>1) throw std::runtime_error("ReSTIR replay currently supports one captured dome; refusing to silently discard extra domes");
        if (!light.colorTexture||!config.scene.textures.contains(light.colorTexture)) continue;
        const auto& texture=config.scene.textures.at(light.colorTexture);
        auto extension=texture.file.empty()?(texture.format>=6?".hdr":".png"):texture.file.extension().string();
        config.environmentPath="/Assets/Media/textures/"+std::to_string(light.colorTexture)+extension;
    }
    writeJson(args.output/"world-restir.scene.json",sceneFile);
}
class ReplayRenderer final:public SceneRenderer {
    UIData& m_ui;
public:
    ReplayRenderer(app::DeviceManager* manager,UIData& ui,const SceneRendererStartupSettings& settings):SceneRenderer(manager,ui,settings),m_ui(ui) {}
    void Animate(float) override {
        // Freeze world/camera and advance time-dependent exposure at a fixed step.
        if (m_ui.resources&&m_ui.resources->camera) {
            const auto& c=replayConfig().scene.camera;
            m_ui.resources->camera->LookTo(float3(c.position[0],c.position[1],c.position[2]),float3(c.forward[0],c.forward[1],c.forward[2]),float3(c.up[0],c.up[1],c.up[2]));
        }
        SceneRenderer::Animate(1.f/60.f);
    }
    void SceneLoaded() override {
        SceneRenderer::SceneLoaded();
        m_ui.verticalFov=replayConfig().scene.camera.fovYDegrees;
        m_ui.enableAnimations=false; m_ui.showUI=false;
        if (m_ui.resources->scene->GetSceneGraph()->GetMeshInstances().empty()) throw std::runtime_error("Captured scene loaded without mesh instances");
    }
};
} // namespace

void recordGpuFrame(UIData& ui,const std::shared_ptr<Profiler>& profiler,nvrhi::IDevice* device,
                    nvrhi::ITexture* color,std::uint32_t submittedFrame,app::DeviceManager* manager) {
    const auto& config=replayConfig();
    if (ui.indirectLightingMode!=IndirectLightingMode::ReStirPT || ui.restirPT.resamplingMode!=rtxdi::ReSTIRPT_ResamplingMode::TemporalAndSpatial || ui.restirPT.reconnection.reconnectionMode!=RTXDI_PTReconnectionMode::Footprint)
        throw std::runtime_error("ReSTIR PT Enhanced/footprint temporal-spatial mode was changed; replay measurement rejected");
    if (config.requireRayReconstruction&&ui.denoiserMode!=DenoiserMode::DLSS_RR)
        throw std::runtime_error("Requested Ray Reconstruction was not active; replay measurement rejected");
    if (!config.environmentPath.empty()&&ui.environmentMapIndex!=1) throw std::runtime_error("Captured dome is no longer selected; replay measurement rejected");
    if (submittedFrame<2) return; // The SDK resolves a two-bank GPU query history.
    auto queryFrame=submittedFrame-2;
    static std::ofstream csv;
    static std::uint32_t recorded{};
    static bool observedPtGpuWork{};
    if (queryFrame>=config.warmupFrames&&recorded<config.recordFrames) {
        if (!csv.is_open()) {
            csv.open(config.directory/"gpu-frames.csv",std::ios::binary);
            if (!csv) throw std::runtime_error("Cannot create GPU query CSV");
            csv << "queryFrame,frameMs,gbufferMs,ptInitialMs,ptTemporalMs,ptSpatialMs,ptFinalMs,dlssRrMs\n" << std::setprecision(12);
        }
        double initial=profiler->GetTimer(ProfilerSection::PTGenerateInitialSamples);
        double temporal=profiler->GetTimer(ProfilerSection::PTTemporalResampling);
        double spatial=profiler->GetTimer(ProfilerSection::PTSpatialResampling);
        observedPtGpuWork|=initial>0&&temporal>0&&spatial>0;
        csv << queryFrame << ',' << profiler->GetTimer(ProfilerSection::Frame) << ',' << profiler->GetTimer(ProfilerSection::GBufferFill) << ',' << initial << ',' << temporal << ',' << spatial << ',' << profiler->GetTimer(ProfilerSection::PTFinalShading) << ',' << profiler->GetTimer(ProfilerSection::DlssRr) << '\n';
        ++recorded;
    }
    if (recorded==config.recordFrames) {
        csv.close();
        if (!observedPtGpuWork) throw std::runtime_error("ReSTIR PT initial/temporal/spatial GPU queries did not prove execution; result rejected");
        device->waitForIdle();
        auto image=config.directory/"frame.png";
        if (!SaveScreenshot(device,color,image.string().c_str())) throw std::runtime_error("SDK GPU screenshot readback failed");
        writeJson(config.directory/"replay-result.json",{
            {"schema",1},{"sdkCommit","fdbd9b36dc55827b67a03f926e78cfad9b1c1c57"},
            {"backend","ReSTIR PT Enhanced"},{"indirectMode","RESTIRPT"},{"reconnectionMode","FOOTPRINT"},{"resamplingMode","TEMPORAL_SPATIAL"},
            {"gpuExecutionProved",observedPtGpuWork},{"frameGeneration",false},{"snapshotFrameId",config.scene.frameId},
            {"width",config.scene.camera.width},{"height",config.scene.camera.height},{"internalResolutionScale",ui.resolutionScale},
            {"warmupFrames",config.warmupFrames},{"recordedGpuFrames",recorded},{"fixedAnimationStepSeconds",1.0/60.0},
            {"denoiser",ui.denoiserMode==DenoiserMode::DLSS_RR?"DLSS_RR":ui.denoiserMode==DenoiserMode::NRD_REBLUR?"NRD_REBLUR":"NONE"},
            {"timingSource","Original SDK Profiler nvrhi timer queries; no generated frames"},
            {"scope","Static Minecraft scene replay in native window; excludes live Minecraft scene transfer, input, HUD and presentation integration"}});
        replayConfig().completed=true;
        glfwSetWindowShouldClose(manager->GetWindow(),1);
    }
}
int runRestirReplay(int argc,char** argv) {
    try {
        auto replay=parseReplayArguments(argc,argv); prepareScene(replay);
        log::SetCallback(&ApplicationLogCallback);
        app::DeviceCreationParameters deviceParams;
        deviceParams.swapChainBufferCount=3; deviceParams.enableRayTracingExtensions=true;
        deviceParams.backBufferWidth=replayConfig().scene.camera.width; deviceParams.backBufferHeight=replayConfig().scene.camera.height;
        deviceParams.vsyncEnabled=false;
        UIData ui; CommandLineArguments args;
        std::vector<char*> sdkArguments; for (auto& s:replay.sdk) sdkArguments.push_back(s.data());
        ProcessCommandLine(int(sdkArguments.size()),sdkArguments.data(),"Minecraft ReSTIR PT replay",deviceParams,ui,args);
        args.graphicsApi=nvrhi::GraphicsAPI::D3D12;
        const auto& camera=replayConfig().scene.camera;
        deviceParams.backBufferWidth=camera.width; deviceParams.backBufferHeight=camera.height;
        args.appStartupSettings.renderWidth=int(camera.width); args.appStartupSettings.renderHeight=int(camera.height);
        args.appStartupSettings.cameraPosition=float3(camera.position[0],camera.position[1],camera.position[2]);
        args.appStartupSettings.cameraDirection=float3(camera.forward[0],camera.forward[1],camera.forward[2]);
        args.appStartupSettings.cameraMovementSpeed=0.f; args.appStartupSettings.cameraRotateSpeed=0.f;
        ui.indirectLightingMode=IndirectLightingMode::ReStirPT;
        ui.restirPT.resamplingMode=rtxdi::ReSTIRPT_ResamplingMode::TemporalAndSpatial;
        ui.restirPT.reconnection.reconnectionMode=RTXDI_PTReconnectionMode::Footprint;
        ui.lightingSettings.directLightingMode=DirectLightingMode::ReStir;
        ui.enableAnimations=false; ui.enableFpsLimit=false; ui.showUI=false; ui.verticalFov=camera.fovYDegrees;
        ui.resolutionScale=1.f; ui.aaMode=AntiAliasingMode::TAA;
        ui.denoiserMode=replay.aa=="rr"?DenoiserMode::DLSS_RR:replay.aa=="nrd"?DenoiserMode::NRD_REBLUR:DenoiserMode::NONE;
        if (replay.aa=="none") ui.aaMode=AntiAliasingMode::None;
        std::unique_ptr<app::DeviceManager> manager(app::DeviceManager::Create(args.graphicsApi));
        ApplyLoggingSettings(args,"Minecraft ReSTIR PT Enhanced replay");
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        ProcessApiSpecificDeviceParams(deviceParams,args);
        if (!manager->CreateWindowDeviceAndSwapChain(deviceParams,"Minecraft ReSTIR PT Enhanced: captured scene, real frames")) throw std::runtime_error("Cannot create DX12 device/swapchain");
        if (!IsHardwareSupported(manager.get())) throw std::runtime_error("SDK hardware requirements were not met");
        ToggleBackgroundOptimization(args,manager.get()); EnableNVAPI(args.graphicsApi,manager.get());
        ui.graphicsAPI=args.graphicsApi;
        {
            ReplayRenderer renderer(manager.get(),ui,args.appStartupSettings);
            if (!renderer.Init()) throw std::runtime_error("SDK renderer initialization failed");
            manager->AddRenderPassToBack(&renderer);
            manager->RunMessageLoop(); manager->GetDevice()->waitForIdle(); manager->RemoveRenderPass(&renderer);
        }
        ui.resources.reset(); manager->Shutdown();
        if (!replayConfig().completed) throw std::runtime_error("Replay ended before all GPU samples and screenshot completed");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr,"ReSTIR replay failed: %s\n",e.what()); OutputDebugStringA(e.what());
        MessageBoxA(nullptr,e.what(),"Minecraft ReSTIR PT replay failed",MB_OK|MB_ICONERROR); return 1;
    }
}
} // namespace renderer_lab
int WINAPI WinMain(HINSTANCE,HINSTANCE,LPSTR,int) { return renderer_lab::runRestirReplay(__argc,__argv); }
