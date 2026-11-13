/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 */
#include <renderer_lab/GltfExport.h>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace renderer_lab {
struct LaunchOptions {
    std::filesystem::path packet, directory;
    bool allowApproximations{};
    int aa=3;
    std::uint64_t frameLimit{};
    std::vector<std::string> sdkArguments;
};
inline LaunchOptions parseArguments(int argc, const char* const* argv) {
    LaunchOptions o; o.sdkArguments.push_back(argv[0]);
    auto argument=[&](int& i) { if (++i>=argc) throw std::runtime_error("Missing value for renderer lab option"); return std::string(argv[i]); };
    for (int i=1;i<argc;++i) {
        std::string key=argv[i];
        if (key=="--packet") o.packet=argument(i);
        else if (key=="--export-dir") o.directory=argument(i);
        else if (key=="--allow-approximations") o.allowApproximations=true;
        else if (key=="--frames") o.frameLimit=std::stoull(argument(i));
        else if (key=="--aa") {
            auto value=argument(i);
            if (value=="none") o.aa=0; else if (value=="taa") o.aa=1;
            else if (value=="sr") o.aa=2; else if (value=="rr") o.aa=3;
            else throw std::runtime_error("--aa must be none, taa, sr, or rr");
        } else if (key=="--scene" || key=="-s" || key=="--realtimeAA" || key=="--overrideToReferenceMode" || key=="--vk") {
            throw std::runtime_error("This adapter owns scene, AA and DX12 realtime mode; use its --packet and --aa options");
        } else o.sdkArguments.push_back(key);
    }
    if (o.packet.empty()) throw std::runtime_error("Provide --packet Minecraft-scene.rslb; a sample scene is never substituted");
    o.packet=std::filesystem::absolute(o.packet);
    if (o.directory.empty()) o.directory=o.packet.parent_path()/(o.packet.stem().string()+"-"+RENDERER_LAB_MODE_NAME);
    o.directory=std::filesystem::absolute(o.directory); return o;
}
inline const LaunchOptions* g_options{};
inline const SceneState* g_scene{};

class MinecraftRenderer final : public RENDERER_LAB_SDK_RENDERER {
    using Base=RENDERER_LAB_SDK_RENDERER;
    std::uint64_t m_frames{};
    std::chrono::steady_clock::time_point m_start{};
    bool m_reported{};
    void enforceRequestedFeatures(SampleUIData& ui) {
        ui.RealtimeMode=true;
        ui.RealtimeAA=g_options->aa;
        ui.RadianceCacheMode=1;
#ifdef RENDERER_LAB_HYBRID
        ui.UseSHaRC=true;
#endif
#if DONUT_WITH_STREAMLINE
        ui.DLSSFGMode=SI::DLSSGMode::eOff;
        if (g_options->aa==3&&!ui.IsDLSSRRSupported) throw std::runtime_error("DLSS Ray Reconstruction was requested but the SDK reports it unavailable; no fallback was selected");
        if (g_options->aa==2&&!ui.IsDLSSSuported) throw std::runtime_error("DLSS Super Resolution was requested but the SDK reports it unavailable; no fallback was selected");
#else
        if (g_options->aa>=2) throw std::runtime_error("This build has no Streamline support for the requested DLSS mode");
#endif
    }
public:
    MinecraftRenderer(donut::app::DeviceManager& manager,const CommandLineOptions& options):Base(manager,options) {}
    const char* GetWindowTitle() override { return "Minecraft renderer lab: " RENDERER_LAB_MODE_NAME " (snapshot, real frames)"; }
    std::unique_ptr<SampleUI> CreateUI(donut::app::DeviceManager* deviceManager, SampleBaseApp& app, SampleUIData& ui, bool ser, const CommandLineOptions& options) override {
        auto result=Base::CreateUI(deviceManager,app,ui,ser,options);
#ifdef RENDERER_LAB_HYBRID
        ApplyIntroStepPreset(ui,8); GetIntroUIData().IntroStepIndex=8;
#endif
        enforceRequestedFeatures(ui); return result;
    }
    void SceneLoaded() override {
        Base::SceneLoaded();
        SetCameraVerticalFOV(g_scene->camera.fovYDegrees*3.14159265358979323846f/180.f);
        enforceRequestedFeatures(m_ui);
    }
    void Render(nvrhi::IFramebuffer* framebuffer) override {
        // The application contains an optional FG UI, but generated frames are
        // excluded from this experiment and cannot be accidentally enabled.
#if DONUT_WITH_STREAMLINE
        m_ui.DLSSFGMode=SI::DLSSGMode::eOff;
#endif
        if (m_ui.RealtimeAA!=g_options->aa) throw std::runtime_error("The SDK changed the requested AA mode; refusing a silently downgraded measurement");
        Base::Render(framebuffer);
        if (m_ui.RealtimeAA!=g_options->aa) throw std::runtime_error("The SDK downgraded the requested AA mode while rendering; measurement was rejected");
        if (HasAsyncLoadingInProgress()) return;
        if (!m_reported) {
            std::ofstream report(g_options->directory/"renderer-started.json",std::ios::binary);
            report << "{\"mode\":\"" RENDERER_LAB_MODE_NAME "\",\"sdkCommit\":\"303b408ab81790a5051abd7573830b7a4112eb8e\",\"frameGeneration\":false,\"aa\":" << g_options->aa << ",\"sharc\":true,\"snapshotFrameId\":" << g_scene->frameId << "}";
            m_start=std::chrono::steady_clock::now(); m_reported=true;
        }
        ++m_frames;
        if (g_options->frameLimit&&m_frames>=g_options->frameLimit) {
            double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-m_start).count();
            std::ofstream report(g_options->directory/"render-run.json",std::ios::binary);
            // Wall-clock throughput is explicitly distinct from GPU timestamps.
            report << std::setprecision(12) << "{\"completedRenderCalls\":" << m_frames << ",\"wallSeconds\":" << elapsed << ",\"frameGeneration\":false,\"gpuTimings\":\"Use SDK perfLog/profiler; no synthetic GPU timings are emitted\"}";
            glfwSetWindowShouldClose(GetDeviceManager()->GetWindow(),1);
        }
    }
};
class MinecraftApp final : public SampleBaseApp {
    std::unique_ptr<Sample> CreateMainRenderPass(donut::app::DeviceManager& manager,const CommandLineOptions& options) override {
        return std::make_unique<MinecraftRenderer>(manager,options);
    }
};

// A callable native entrypoint as well as WinMain. The JNI/controller owns
// process lifetime; this prototype presents in its own DX12 window.
inline int run(int argc,const char* const* argv) {
    try {
        auto options=parseArguments(argc,argv); SceneState scene; scene.load(options.packet);
        auto exported=exportGltf(scene,options.directory,{options.allowApproximations});
        g_options=&options; g_scene=&scene;
        std::ostringstream camera; camera << std::setprecision(9);
        bool comma=false; for (const auto& v:{scene.camera.position,scene.camera.forward,scene.camera.up})
            for (float x:v) { if (comma) camera << ','; camera << x; comma=true; }
        auto& args=options.sdkArguments;
        args.insert(args.end(),{"--scene",exported.sceneFile.string(),"--width",std::to_string(scene.camera.width),"--height",std::to_string(scene.camera.height),"--cameraPosDirUp",camera.str(),"--realtimeAA",std::to_string(options.aa),"--overrideToRealtimeMode","--overrideRadianceCacheMode","1","--stopAnimations"});
        std::vector<const char*> pointers; for (const auto& a:args) pointers.push_back(a.c_str());
        MinecraftApp app; auto status=app.Init(int(pointers.size()),pointers.data());
        if (status!=SampleBaseApp::InitReturnCodes::Success) return int(status)+10;
        app.RunMainLoop(); app.End(); g_options=nullptr; g_scene=nullptr; return 0;
    } catch (const std::exception& e) {
        OutputDebugStringA(e.what());
        std::fprintf(stderr,"Renderer lab failed: %s\n",e.what());
        MessageBoxA(nullptr,e.what(),"Minecraft renderer lab failed",MB_OK|MB_ICONERROR);
        return 1;
    }
}
} // namespace renderer_lab
extern "C" __declspec(dllexport) int RendererLabMain(int argc,const char* const* argv) { return renderer_lab::run(argc,argv); }
int WINAPI WinMain(HINSTANCE,HINSTANCE,LPSTR,int) { return RendererLabMain(__argc,__argv); }
