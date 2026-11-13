/*
 * SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */
#define WinMain RendererLabUnusedIntroSampleWinMain
#include <IntroSample/IntroSample.cpp>
#undef WinMain
#include "RendererLabHost.h"
#include <renderer_lab/GltfExport.h>
#include <SampleCommon/RenderTargets.h>
#include <donut/core/vfs/VFS.h>
#include <donut/engine/ShaderFactory.h>
#include <donut/engine/TextureCache.h>
#include <donut/app/StreamlineInterface.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <atomic>
#include <bit>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
static_assert(sizeof(RlabHostCreateInfo)==48,"Renderer host creation ABI drifted");
static_assert(sizeof(RlabHostSharedTargets)==104,"Renderer host shared-target ABI drifted");
#if DONUT_WITH_STREAMLINE
namespace donut::app {
void RendererLabHostStreamlineFrameEnd();
void RendererLabHostStreamlineReleaseViewport();
void RendererLabHostStreamlineShutdown();
}
#endif

namespace renderer_lab::host {
using Microsoft::WRL::ComPtr;
struct WinHandle {
    HANDLE value{};
    ~WinHandle() { if (value) CloseHandle(value); }
    WinHandle()=default;
    WinHandle(const WinHandle&)=delete;
    WinHandle& operator=(const WinHandle&)=delete;
    void reset(HANDLE handle=nullptr) { if (value) CloseHandle(value); value=handle; }
};
std::filesystem::path runtimeRoot;
std::atomic<bool> occupied{};
std::mutex sdkErrorMutex;
std::string sdkFatalError;
std::atomic<bool> sdkFatalCanThrow{true};
void checkSdkError() {
    std::string error;
    { std::lock_guard lock(sdkErrorMutex); error=sdkFatalError; }
    if (!error.empty()) throw std::runtime_error(error);
}
void check(HRESULT result,const char* action) {
    if (FAILED(result)) throw std::runtime_error(std::string(action)+" failed (HRESULT "+std::to_string(std::uint32_t(result))+")");
}
std::uint64_t luidBits(LUID value) { return std::uint64_t(value.LowPart)|(std::uint64_t(std::uint32_t(value.HighPart))<<32); }
std::uint32_t adapterIndex(std::uint64_t required) {
    ComPtr<IDXGIFactory4> factory; check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI factory creation");
    for (UINT index=0;;++index) {
        ComPtr<IDXGIAdapter1> adapter; auto result=factory->EnumAdapters1(index,&adapter);
        if (result==DXGI_ERROR_NOT_FOUND) break;
        check(result,"DXGI adapter enumeration");
        DXGI_ADAPTER_DESC1 description{}; check(adapter->GetDesc1(&description),"DXGI adapter description");
        if (luidBits(description.AdapterLuid)==required) {
            if (description.VendorId!=0x10de)
                throw std::runtime_error("RTXPT host requires the OpenGL context on an NVIDIA GPU; the requested DXGI adapter is not NVIDIA (vendor "+std::to_string(description.VendorId)+")");
            return index;
        }
    }
    throw std::runtime_error("The OpenGL adapter LUID is not a DXGI adapter; cross-GPU rendering is disabled");
}

// The public DX12 DeviceManager owns the actual device. This adapter supplies an
// offscreen framebuffer and the public IRenderPass frame clock without invoking
// DeviceManager's GLFW message loop, swapchain or protected DX12 implementation.
class HeadlessManager final:public donut::app::DeviceManager {
    std::unique_ptr<donut::app::DeviceManager> implementation;
    nvrhi::TextureHandle output;
protected:
    bool CreateInstanceInternal() override { throw std::logic_error("Host instance is already owned by the DX12 manager"); }
    bool CreateDevice() override { throw std::logic_error("Use the real DX12 headless manager"); }
    bool CreateSwapChain() override { throw std::logic_error("Minecraft owns presentation"); }
    void DestroyDeviceAndSwapChain() override {}
    void ResizeSwapChain() override { throw std::logic_error("Minecraft owns presentation"); }
    bool BeginFrame() override { return true; }
    bool Present() override { throw std::logic_error("Minecraft owns presentation"); }
public:
    explicit HeadlessManager(const donut::app::DeviceCreationParameters& parameters) {
        m_DeviceParams=parameters; m_DeviceParams.headlessDevice=true;
        implementation.reset(donut::app::DeviceManager::Create(nvrhi::GraphicsAPI::D3D12));
        if (!implementation) throw std::runtime_error("DX12 headless manager creation failed");
        try {
            if (!implementation->CreateHeadlessDevice(parameters)) throw std::runtime_error("DX12 headless device creation failed");
        } catch (...) {
            // The containing constructor has not completed, so its destructor
            // cannot perform the explicit DeviceManager shutdown.
            try { implementation->Shutdown(); } catch (...) {}
            implementation.reset();
            throw;
        }
    }
    ~HeadlessManager() override { output=nullptr; if (implementation) implementation->Shutdown(); }
    nvrhi::IDevice* GetDevice() const override { return implementation->GetDevice(); }
    const char* GetRendererString() const override { return implementation->GetRendererString(); }
    nvrhi::GraphicsAPI GetGraphicsAPI() const override { return nvrhi::GraphicsAPI::D3D12; }
    bool EnumerateAdapters(std::vector<donut::app::AdapterInfo>& adapters) override { return implementation->EnumerateAdapters(adapters); }
    nvrhi::ITexture* GetCurrentBackBuffer() override { return output; }
    nvrhi::ITexture* GetBackBuffer(std::uint32_t index) override { return index==0?output.Get():nullptr; }
    std::uint32_t GetCurrentBackBufferIndex() override { return 0; }
    std::uint32_t GetBackBufferCount() override { return output?1:0; }
    void Shutdown() override { output=nullptr; if (implementation) { implementation->Shutdown(); implementation.reset(); } }
    void setOutput(nvrhi::ITexture* texture,std::uint32_t width,std::uint32_t height) {
        output=texture; m_DeviceParams.backBufferWidth=width; m_DeviceParams.backBufferHeight=height;
    }
    void advance() { ++m_FrameIndex; m_PreviousFrameTimestamp=double(m_FrameIndex)/60.0; }
};

class HostIntro final:public IntroPathTracer {
    std::uint32_t aa;
public:
    HostIntro(HeadlessManager& manager,const CommandLineOptions& options,std::uint32_t requestedAa):IntroPathTracer(manager,options),aa(requestedAa) { configure(); }
    void configure() {
        ApplyIntroStepPreset(m_ui,8); GetIntroUIData().IntroStepIndex=8;
        m_ui.UseNEE=true; m_ui.NEEType=0; m_ui.UseReSTIRDI=false; m_ui.UseReSTIRGI=false;
        m_ui.RealtimeSamplesPerPixel=1; m_ui.AccumulationTarget=4096; m_ui.StandaloneDenoiser=true;
        m_ui.RealtimeMode=true; m_ui.RealtimeAA=int(aa); m_ui.RealtimeSubSamplingMode=0;
        m_ui.RadianceCacheMode=1; m_ui.UseSHaRC=true; m_ui.EnableAnimations=false;
        m_ui.EnableAsyncPrefix=false;
#if DONUT_WITH_STREAMLINE
        m_ui.DLSSFGMode=SI::DLSSGMode::eOff; m_ui.ReflexMode=SI::ReflexMode::eOff;
        m_ui.DLSSMode=SI::DLSSMode::eDLAA;
        if (aa==RLAB_HOST_AA_RR&&!m_ui.IsDLSSRRSupported) throw std::runtime_error("Headless device does not support requested DLSS-RR; no fallback selected");
        if (aa==RLAB_HOST_AA_SR&&!m_ui.IsDLSSSuported) throw std::runtime_error("Headless device does not support requested DLSS-SR; no fallback selected");
#else
        if (aa>=RLAB_HOST_AA_SR) throw std::runtime_error("Streamline was not compiled into this host");
#endif
    }
    void SceneLoaded() override { IntroPathTracer::SceneLoaded(); configure(); }
    void SceneUnloading() override {
        IntroPathTracer::SceneUnloading();
        // A retained packet can replace texture bytes at the same exported URI.
        // The public SDK cache otherwise reuses that URI's previous GPU image.
        if (m_TextureCache) m_TextureCache->Reset();
    }
    void camera(const Camera& value) {
        std::ostringstream pose; pose << std::setprecision(9); bool comma=false;
        for (const auto& v:{value.position,value.forward,value.up}) for (float x:v) { if (comma) pose << ','; pose << x; comma=true; }
        SetCurrentCameraPosDirUp(pose.str()); SetCameraVerticalFOV(value.fovYDegrees*3.14159265358979323846f/180.f);
    }
    bool complete() const { return m_renderTargets&&m_renderTargets->LdrColor&&m_renderTargets->Depth&&!HasAsyncLoadingInProgress(); }
    nvrhi::ITexture* color() const { return m_renderTargets->LdrColor; }
    nvrhi::ITexture* depth() const { return m_renderTargets->Depth; }
    // Sample swaps the just-rendered view into m_viewPrevious before returning.
    donut::math::float4x4 projection() const { return m_viewPrevious->GetProjectionMatrix(); }
    void Render(nvrhi::IFramebuffer* framebuffer) override {
#if DONUT_WITH_STREAMLINE
        m_ui.DLSSFGMode=SI::DLSSGMode::eOff;
#endif
        if (m_ui.RealtimeAA!=int(aa)) throw std::runtime_error("The SDK changed requested host AA mode");
        IntroPathTracer::Render(framebuffer);
        if (m_ui.RealtimeAA!=int(aa)) throw std::runtime_error("The SDK downgraded requested host AA mode");
    }
};

// Reprojects SDK reverse D3D depth to the ordinary finite Minecraft depth range.
// The source projection coefficients reconstruct signed SDK view Z; its absolute
// value is the distance along the explicit camera forward axis.
constexpr char depthShader[]=R"(
Texture2D<float> sourceDepth : register(t0);
RWTexture2D<float> destinationDepth : register(u0);
cbuffer Params : register(b0) { uint2 dimensions; float zNear; float zFar; float4 sourceZW; };
[numthreads(8,8,1)] void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy>=dimensions)) return;
    float d=sourceDepth[id.xy];
    if (d<=0 || !isfinite(d)) { destinationDepth[id.xy]=1; return; }
    float denominator=d*sourceZW.z-sourceZW.x;
    float distance=abs((sourceZW.y-d*sourceZW.w)/denominator);
    float finiteDepth=zFar>zNear ? zFar/(zFar-zNear)-(zFar*zNear)/((zFar-zNear)*distance) : 1-zNear/distance;
    destinationDepth[id.xy]=distance>0 ? saturate(finiteDepth) : 1;
})";
struct DepthParameters { std::uint32_t width,height; float zNear,zFar; float sourceZW[4]; };

struct Host {
    std::thread::id owner=std::this_thread::get_id();
    SceneState scene;
    std::filesystem::path directory;
    bool approximations{},sceneDirty=true,resetRequested{},emptyCleared{};
    std::uint32_t aa{},width{},height{};
    std::uint64_t generation{},readyValue{},adapter{};
    std::uint64_t renderAttempts{};
    std::uint64_t sdkFrameValue{};
    bool sdkFramePending{};
    bool traceEveryFrame{};
    std::unique_ptr<HeadlessManager> manager;
    CommandLineOptions commands;
    std::shared_ptr<donut::engine::ShaderFactory> shaders;
    std::unique_ptr<HostIntro> renderer;
    nvrhi::TextureHandle color,depth,sdkOutput;
    nvrhi::FramebufferHandle framebuffer;
    nvrhi::CommandListHandle transfer;
    nvrhi::ShaderHandle depthCompute;
    nvrhi::BindingLayoutHandle depthLayout;
    nvrhi::ComputePipelineHandle depthPipeline;
    ComPtr<ID3D12Fence> dxReady,glDone,sdkCompleted;
    WinHandle dxReadyHandle,glDoneHandle;
    // The pinned NVRHI Texture destructor releases descriptors/resource refs but
    // does not close its plain sharedHandle. Adopt those exact exported handles.
    WinHandle colorExportHandle,depthExportHandle;
    ID3D12CommandQueue* queue{};
    ID3D12Device* native{};
    RlabHostSharedTargets targets{};
    void trace(const char* stage) const {
        if (!traceEveryFrame) return;
        std::fprintf(stderr,"[RendererLabHost stage] attempt=%llu ready=%llu %s\n",
            static_cast<unsigned long long>(renderAttempts),static_cast<unsigned long long>(readyValue),stage);
        std::fflush(stderr);
    }
    void thread() const {
        if (owner!=std::this_thread::get_id()) throw std::runtime_error("Renderer host called outside its owning render thread");
        checkSdkError();
    }
    bool finishSdkFrame() {
        if (!sdkFramePending) return true;
        if (sdkCompleted->GetCompletedValue()<sdkFrameValue) return false;
#if DONUT_WITH_STREAMLINE
        donut::app::RendererLabHostStreamlineFrameEnd();
#endif
        sdkFramePending=false;
        return true;
    }
    void retireViewport() {
        manager->GetDevice()->waitForIdle();
        if (!finishSdkFrame()) throw std::runtime_error("SDK completion fence did not complete after device idle");
#if DONUT_WITH_STREAMLINE
        donut::app::RendererLabHostStreamlineReleaseViewport();
#endif
    }
    explicit Host(const RlabHostCreateInfo& info):directory(std::filesystem::u8path(info.sceneDirectoryUtf8)),
        approximations((info.flags&RLAB_HOST_ALLOW_APPROXIMATIONS)!=0),aa(info.aaMode),adapter(info.adapterLuid) {
        traceEveryFrame=(info.flags&RLAB_HOST_DEBUG_VALIDATION)!=0;
        { std::lock_guard lock(sdkErrorMutex); sdkFatalError.clear(); }
        sdkFatalCanThrow=true;
        runtimeRoot=std::filesystem::absolute(std::filesystem::u8path(info.runtimeDirectoryUtf8));
        directory=std::filesystem::absolute(directory); std::filesystem::create_directories(directory);
        HelpersSetNonInteractive();
        donut::log::ConsoleApplicationMode();
        donut::log::SetMinSeverity(traceEveryFrame?donut::log::Severity::Debug:donut::log::Severity::Info);
        donut::log::SetCallback([ownerThread=owner](donut::log::Severity severity,const char* message) {
            std::fprintf(stderr,"[RendererLabHost SDK %d] %s\n",int(severity),message?message:"");
            std::fflush(stderr);
            if (severity==donut::log::Severity::Fatal) {
                const std::string error=std::string("SDK fatal: ")+(message?message:"unknown");
                { std::lock_guard lock(sdkErrorMutex); sdkFatalError=error; }
                // Never abort Minecraft from a library callback. Owner calls
                // become C ABI errors; worker failures are latched for the next
                // owner boundary. Teardown must finish without throwing from a
                // destructor even if the SDK reports another fatal there.
                if (sdkFatalCanThrow&&std::this_thread::get_id()==ownerThread)
                    throw std::runtime_error(error);
            }
        });
        if (!std::filesystem::is_directory(runtimeRoot/"Assets")||!std::filesystem::is_directory(runtimeRoot/"ShaderPrecompiled")) throw std::runtime_error("Host runtime directory lacks SDK Assets or ShaderPrecompiled");
        donut::app::DeviceCreationParameters parameters;
        parameters.backBufferWidth=info.width; parameters.backBufferHeight=info.height;
        parameters.enableHeapDirectlyIndexed=true; parameters.enableRayTracingExtensions=true; parameters.enableComputeQueue=true;
        parameters.enableDebugRuntime=(info.flags&RLAB_HOST_DEBUG_VALIDATION)!=0; parameters.enableNvrhiValidationLayer=parameters.enableDebugRuntime;
        parameters.adapterIndex=int(adapterIndex(info.adapterLuid));
        trace("00.device.begin");
        manager=std::make_unique<HeadlessManager>(parameters);
        trace("01.device.end");
        auto device=manager->GetDevice();
        if (!device->queryFeatureSupport(nvrhi::Feature::RayTracingPipeline)||!device->queryFeatureSupport(nvrhi::Feature::RayQuery)||!device->queryFeatureSupport(nvrhi::Feature::HeapDirectlyIndexed)) throw std::runtime_error("Host device lacks required DXR/ray-query/bindless capability");
        native=device->getNativeObject(nvrhi::ObjectTypes::D3D12_Device);
        if (luidBits(native->GetAdapterLuid())!=adapter) throw std::runtime_error("Headless device did not select the requested OpenGL adapter");
        queue=device->getNativeQueue(nvrhi::ObjectTypes::D3D12_CommandQueue,nvrhi::CommandQueue::Graphics);
        check(native->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&dxReady)),"Create DX ready fence");
        check(native->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&glDone)),"Create GL done fence");
        check(native->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&sdkCompleted)),"Create SDK completion fence");
        check(native->CreateSharedHandle(dxReady.Get(),nullptr,GENERIC_ALL,nullptr,&dxReadyHandle.value),"Share DX ready fence");
        check(native->CreateSharedHandle(glDone.Get(),nullptr,GENERIC_ALL,nullptr,&glDoneHandle.value),"Share GL done fence");
        auto fs=std::make_shared<donut::vfs::RootFileSystem>();
        fs->mount("/ShaderPrecompiled/donut",runtimeRoot/"ShaderPrecompiled/framework/dxil");
        fs->mount("/ShaderPrecompiled/app",runtimeRoot/"ShaderPrecompiled/Rtxpt/dxil");
        fs->mount("/ShaderPrecompiled/nrd",runtimeRoot/"ShaderPrecompiled/nrd/dxil");
        fs->mount("/ShaderPrecompiled/omm",runtimeRoot/"ShaderPrecompiled/omm/dxil");
        shaders=std::make_shared<donut::engine::ShaderFactory>(device,fs,"/ShaderPrecompiled");
        commands.nonInteractive=true; commands.stopAnimations=true; commands.RealtimeAA=int(aa); commands.OverrideToRealtimeMode=true;
        commands.OverrideRadianceCacheMode=1; commands.OverrideSubSamplingMode=0; commands.OverrideDLSSMode=6; commands.AsyncPrefix=false;
        transfer=device->createCommandList();
        if (!transfer) throw std::runtime_error("Host transfer command list creation failed");
        ComPtr<ID3DBlob> bytecode,errors;
        auto hr=D3DCompile(depthShader,sizeof(depthShader)-1,"RendererLabDepth",nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&bytecode,&errors);
        if (FAILED(hr)) throw std::runtime_error(errors?std::string(static_cast<const char*>(errors->GetBufferPointer()),errors->GetBufferSize()):"Host depth compute compilation failed");
        depthCompute=device->createShader(nvrhi::ShaderDesc(nvrhi::ShaderType::Compute),bytecode->GetBufferPointer(),bytecode->GetBufferSize());
        if (!depthCompute) throw std::runtime_error("Host depth shader creation failed");
        nvrhi::BindingLayoutDesc layout; layout.visibility=nvrhi::ShaderType::Compute;
        layout.bindings={nvrhi::BindingLayoutItem::Texture_SRV(0),nvrhi::BindingLayoutItem::Texture_UAV(0),nvrhi::BindingLayoutItem::PushConstants(0,sizeof(DepthParameters))};
        depthLayout=device->createBindingLayout(layout);
        nvrhi::ComputePipelineDesc pipeline; pipeline.CS=depthCompute; pipeline.bindingLayouts={depthLayout};
        depthPipeline=device->createComputePipeline(pipeline);
        if (!depthLayout||!depthPipeline) throw std::runtime_error("Host depth pipeline creation failed");
        resize(info.width,info.height);
        trace("02.create.ready");
    }
    ~Host() noexcept {
        // No queued wait can depend on an unconsumed GL frame: render checks the
        // consumer fence before enqueuing reuse. Failed GL import can tear down.
        sdkFatalCanThrow=false;
        if (manager) {
            try {
                manager->GetDevice()->waitForIdle();
                finishSdkFrame();
#if DONUT_WITH_STREAMLINE
                // RR owns NGX resources and tags referring to renderer targets.
                // Release/unregister them before destroying those targets.
                donut::app::RendererLabHostStreamlineShutdown();
#endif
            } catch (const std::exception& error) {
                std::fprintf(stderr,"[RendererLabHost shutdown] %s\n",error.what());
                std::fflush(stderr);
            }
        }
        renderer.reset(); framebuffer=nullptr; color=nullptr; depth=nullptr; sdkOutput=nullptr;
        depthPipeline=nullptr; depthCompute=nullptr; depthLayout=nullptr; transfer=nullptr; shaders.reset();
        colorExportHandle.reset(); depthExportHandle.reset();
        dxReady.Reset(); glDone.Reset(); sdkCompleted.Reset(); manager.reset(); runtimeRoot.clear();
        sdkFatalCanThrow=true;
    }
    void resize(std::uint32_t w,std::uint32_t h) {
        if (!w||!h||w>16384||h>16384) throw std::runtime_error("Invalid host output dimensions");
        if (w==width&&h==height) return;
        if (renderer) { retireViewport(); renderer->BackBufferResizing(); }
        else manager->GetDevice()->waitForIdle();
        nvrhi::TextureDesc desc; desc.width=w; desc.height=h; desc.format=nvrhi::Format::RGBA8_UNORM;
        desc.initialState=nvrhi::ResourceStates::Common; desc.keepInitialState=true; desc.isRenderTarget=true;
        desc.sharedResourceFlags=nvrhi::SharedResourceFlags::Shared; desc.debugName="Minecraft host shared RGBA8";
        color=manager->GetDevice()->createTexture(desc);
        if (!color) throw std::runtime_error("Shared host color allocation failed");
        colorExportHandle.reset(static_cast<void*>(color->getNativeObject(nvrhi::ObjectTypes::SharedHandle)));
        desc.format=nvrhi::Format::R32_FLOAT; desc.isRenderTarget=false; desc.isUAV=true; desc.debugName="Minecraft host shared projected depth";
        depth=manager->GetDevice()->createTexture(desc);
        if (!depth) throw std::runtime_error("Shared host depth allocation failed");
        depthExportHandle.reset(static_cast<void*>(depth->getNativeObject(nvrhi::ObjectTypes::SharedHandle)));
        desc.format=nvrhi::Format::RGBA8_UNORM; desc.isRenderTarget=true; desc.isUAV=false;
        desc.sharedResourceFlags=nvrhi::SharedResourceFlags::None; desc.initialState=nvrhi::ResourceStates::RenderTarget;
        desc.debugName="SDK private headless framebuffer";
        sdkOutput=manager->GetDevice()->createTexture(desc);
        if (!sdkOutput) throw std::runtime_error("Private host framebuffer allocation failed");
        framebuffer=manager->GetDevice()->createFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(sdkOutput));
        if (!framebuffer) throw std::runtime_error("Private host framebuffer creation failed");
        width=w; height=h; ++generation; emptyCleared=false; manager->setOutput(sdkOutput,w,h);
        auto allocation=[&](nvrhi::ITexture* texture) {
            ID3D12Resource* resource=texture->getNativeObject(nvrhi::ObjectTypes::D3D12_Resource); auto d=resource->GetDesc();
            return native->GetResourceAllocationInfo(0,1,&d).SizeInBytes;
        };
        targets={}; targets.structSize=sizeof(targets); targets.abiVersion=RLAB_HOST_ABI_VERSION;
        targets.width=w; targets.height=h; targets.generation=generation; targets.adapterLuid=adapter;
        targets.colorResourceHandle=std::uint64_t(reinterpret_cast<std::uintptr_t>(colorExportHandle.value));
        targets.depthResourceHandle=std::uint64_t(reinterpret_cast<std::uintptr_t>(depthExportHandle.value));
        targets.colorAllocationBytes=allocation(color); targets.depthAllocationBytes=allocation(depth);
        targets.dxReadyFenceHandle=reinterpret_cast<std::uintptr_t>(dxReadyHandle.value); targets.glDoneFenceHandle=reinterpret_cast<std::uintptr_t>(glDoneHandle.value);
        targets.readyFenceValue=0; targets.colorDxgiFormat=DXGI_FORMAT_R8G8B8A8_UNORM; targets.depthDxgiFormat=DXGI_FORMAT_R32_FLOAT;
        if (!targets.colorResourceHandle||!targets.depthResourceHandle) throw std::runtime_error("NVRHI did not export real shared texture handles");
    }
    void apply(std::span<const std::byte> bytes) {
        thread(); scene.apply(bytes);
        // Inspect only record headers: camera-only streams avoid a scene reload.
        auto u32=[&](std::size_t at) { std::uint32_t x; std::memcpy(&x,bytes.data()+at,4); return x; };
        for (std::size_t at=0;at<bytes.size();) {
            auto flags=u32(at+8),count=u32(at+12),length=u32(at+24); std::size_t cursor=at+32;
            sceneDirty|=(flags&SceneState::FullReset)!=0;
            if (flags&SceneState::FullReset) { resetRequested=true; targets.readyFenceValue=0; }
            for (std::uint32_t i=0;i<count;++i) { auto op=u32(cursor),size=u32(cursor+4); if (op!=9) sceneDirty=true; cursor+=8+size; }
            at+=32+length;
        }
        if (!scene.hasCamera||scene.instances.empty()) targets.readyFenceValue=0;
        if (!scene.instances.empty()) emptyCleared=false;
    }
    int render(std::uint64_t,std::uint32_t w,std::uint32_t h) {
        thread();
        ++renderAttempts; trace("10.render.enter");
        // Polling a synchronization primitive is not image readback. Refuse to
        // enqueue a producer wait that could strand teardown after GL failure.
        if (readyValue&&glDone->GetCompletedValue()<readyValue) { trace("11.consumer.pending"); return RLAB_HOST_PENDING; }
        if (!finishSdkFrame()) { trace("11.sdkGpu.pending"); return RLAB_HOST_PENDING; }
        trace("12.resize.begin");
        resize(w,h);
        trace("13.resize.end");
        if (resetRequested) {
            retireViewport();
            renderer.reset(); manager->m_callbacks={};
            resetRequested=false; sceneDirty=true; emptyCleared=false;
        }
        if (!scene.hasCamera||scene.instances.empty()) {
            targets.readyFenceValue=0;
            if (!emptyCleared) {
                retireViewport(); renderer.reset(); manager->m_callbacks={};
                transfer->open();
                transfer->clearTextureFloat(color,nvrhi::AllSubresources,nvrhi::Color(0.f,0.f,0.f,1.f));
                transfer->clearTextureFloat(depth,nvrhi::AllSubresources,nvrhi::Color(1.f));
                transfer->setTextureState(color,nvrhi::AllSubresources,nvrhi::ResourceStates::Common);
                transfer->setTextureState(depth,nvrhi::AllSubresources,nvrhi::ResourceStates::Common);
                transfer->commitBarriers(); transfer->close(); manager->GetDevice()->executeCommandList(transfer);
                manager->GetDevice()->waitForIdle(); emptyCleared=true;
            }
            return RLAB_HOST_PENDING;
        }
        // Finish an accepted load before starting the next retained-state reload.
        // Animated textures/light deltas must not restart loading indefinitely.
        if (sceneDirty&&(!renderer||renderer->complete())) {
            trace("20.scene.begin");
            scene.validate();
            if (renderer) retireViewport();
            else manager->GetDevice()->waitForIdle();
            auto exported=exportGltf(scene,directory,{approximations});
            if (!renderer) {
                trace("21.intro.construct.begin");
                renderer=std::make_unique<HostIntro>(*manager,commands,aa);
                trace("22.intro.init.begin");
                renderer->Init(exported.sceneFile.string(),shaders);
                trace("23.intro.init.end");
            } else { trace("24.scene.reload.begin"); renderer->SetCurrentScene(exported.sceneFile.string(),true); trace("25.scene.reload.end"); }
            renderer->configure(); sceneDirty=false;
            trace("26.scene.end");
        }
#if DONUT_WITH_STREAMLINE
        auto& streamline=manager->GetStreamline();
#endif
        auto sdkFrame=manager->GetFrameIndex();
#if DONUT_WITH_STREAMLINE
        trace("30.streamline.simStart.begin");
        streamline.SimStart(*manager);
        trace("31.streamline.simStart.end");
#endif
        trace("32.animate.begin");
        renderer->Animate(1.f/60.f); renderer->camera(scene.camera);
        trace("33.animate.end");
#if DONUT_WITH_STREAMLINE
        trace("34.streamline.simEnd.begin");
        streamline.SimEnd(*manager); streamline.RenderStart(*manager);
        trace("35.streamline.renderStart.end");
#endif
        trace("36.callback.beforeRender.begin");
        if (manager->m_callbacks.beforeRender) manager->m_callbacks.beforeRender(*manager,sdkFrame);
        trace("37.sdk.render.begin");
        renderer->Render(framebuffer);
        trace("38.sdk.render.end");
        if (manager->m_callbacks.afterRender) manager->m_callbacks.afterRender(*manager,sdkFrame);
        trace("39.callback.afterRender.end");
#if DONUT_WITH_STREAMLINE
        streamline.RenderEnd(*manager);
        trace("40.streamline.renderEnd.end");
#endif
        if (manager->m_callbacks.beforePresent) manager->m_callbacks.beforePresent(*manager,sdkFrame);
        trace("41.callback.beforePresent.end");
#if DONUT_WITH_STREAMLINE
        streamline.PresentStart(*manager); streamline.PresentEnd(*manager);
        trace("42.streamline.presentMarkers.end");
#endif
        trace("43.callback.afterPresent.begin");
        if (manager->m_callbacks.afterPresent) manager->m_callbacks.afterPresent(*manager,sdkFrame);
        trace("44.callback.afterPresent.end");
        // A real headless GPU completion replaces the swapchain's frame pacing.
        // Streamline common GC may retire RR handles only after this work ends,
        // including loading frames that do not publish a Minecraft image.
        check(queue->Signal(sdkCompleted.Get(),++sdkFrameValue),"Signal SDK frame completion");
        sdkFramePending=true;
        checkSdkError();
        manager->advance(); manager->GetDevice()->runGarbageCollection();
        trace("45.garbageCollection.end");
        if (!renderer->complete()) { trace("46.sdk.pending"); return RLAB_HOST_PENDING; }
        if (renderer->depth()->getDesc().width!=w||renderer->depth()->getDesc().height!=h)
            throw std::runtime_error("Host SDK depth extent differs from its full-resolution GL interop contract");
        if (readyValue) check(queue->Wait(glDone.Get(),readyValue),"Wait for GL consumption");
        trace("50.publication.begin");
        transfer->open();
        transfer->copyTexture(color,nvrhi::TextureSlice(),renderer->color(),nvrhi::TextureSlice());
        auto p=renderer->projection();
        DepthParameters parameters{w,h,scene.camera.zNear,scene.camera.zFar,{p[2][2],p[3][2],p[2][3],p[3][3]}};
        nvrhi::BindingSetDesc bindings;
        bindings.bindings={nvrhi::BindingSetItem::Texture_SRV(0,renderer->depth()),nvrhi::BindingSetItem::Texture_UAV(0,depth),nvrhi::BindingSetItem::PushConstants(0,sizeof(parameters))};
        auto set=manager->GetDevice()->createBindingSet(bindings,depthLayout);
        nvrhi::ComputeState state; state.pipeline=depthPipeline; state.bindings={set}; transfer->setComputeState(state);
        transfer->setPushConstants(&parameters,sizeof(parameters)); transfer->dispatch((w+7)/8,(h+7)/8,1);
        transfer->setTextureState(color,nvrhi::AllSubresources,nvrhi::ResourceStates::Common);
        transfer->setTextureState(depth,nvrhi::AllSubresources,nvrhi::ResourceStates::Common);
        transfer->commitBarriers(); transfer->close(); manager->GetDevice()->executeCommandList(transfer);
        trace("51.publication.submitted");
        check(queue->Signal(dxReady.Get(),++readyValue),"Signal rendered DX12 frame"); targets.readyFenceValue=readyValue;
        trace("52.publication.signaled");
        return RLAB_HOST_OK;
    }
};
void errorText(char* buffer,std::uint32_t capacity,const char* message) {
    if (!buffer||!capacity) return; auto size=std::min<std::size_t>(capacity-1,std::strlen(message)); std::memcpy(buffer,message,size); buffer[size]=0;
}
template<class F> int invoke(char* error,std::uint32_t capacity,F&& call) {
    try { if (error&&capacity) error[0]=0; return call(); }
    catch (const std::exception& e) { errorText(error,capacity,e.what()); return RLAB_HOST_ERROR; }
    catch (...) { errorText(error,capacity,"Unknown renderer host failure"); return RLAB_HOST_ERROR; }
}
} // namespace renderer_lab::host

std::filesystem::path RendererLabHostLocalPath(const std::string& subfolder) {
    return renderer_lab::host::runtimeRoot/subfolder;
}
namespace donut::app {
// StreamlineIntegration is implemented inside donut::app, while the SDK's
// SampleCommon path helper is global. Both resolve the same explicit root.
std::filesystem::path RendererLabHostLocalPath(const std::string& subfolder) {
    return ::RendererLabHostLocalPath(subfolder);
}
}
extern "C" {
uint32_t RendererLabHostAbiVersion(void) { return RLAB_HOST_ABI_VERSION; }
int32_t RendererLabHostCreate(const RlabHostCreateInfo* info,void** result,char* error,uint32_t capacity) {
    return renderer_lab::host::invoke(error,capacity,[&] {
        if (!info||!result||info->structSize!=sizeof(*info)||info->abiVersion!=RLAB_HOST_ABI_VERSION||!info->runtimeDirectoryUtf8||!info->sceneDirectoryUtf8||!info->adapterLuid||info->aaMode>RLAB_HOST_AA_RR||(info->flags&~(RLAB_HOST_ALLOW_APPROXIMATIONS|RLAB_HOST_DEBUG_VALIDATION))) throw std::runtime_error("Invalid renderer host creation ABI or settings");
        *result=nullptr;
        if (renderer_lab::host::occupied.exchange(true)) throw std::runtime_error("Only one renderer host can own the SDK global state");
        try { *result=new renderer_lab::host::Host(*info); }
        catch (...) { renderer_lab::host::occupied=false; throw; }
        return RLAB_HOST_OK;
    });
}
int32_t RendererLabHostApply(void* host,const void* bytes,uint64_t count,char* error,uint32_t capacity) {
    return renderer_lab::host::invoke(error,capacity,[&] {
        if (!host||!bytes||count>std::uint64_t(SIZE_MAX)) throw std::runtime_error("Invalid host packet input");
        static_cast<renderer_lab::host::Host*>(host)->apply({static_cast<const std::byte*>(bytes),std::size_t(count)}); return RLAB_HOST_OK;
    });
}
int32_t RendererLabHostRender(void* host,uint64_t frame,uint32_t width,uint32_t height,char* error,uint32_t capacity) {
    return renderer_lab::host::invoke(error,capacity,[&] {
        if (!host) throw std::runtime_error("Null renderer host");
        return static_cast<renderer_lab::host::Host*>(host)->render(frame,width,height);
    });
}
int32_t RendererLabHostGetSharedTargets(void* host,RlabHostSharedTargets* targets,char* error,uint32_t capacity) {
    return renderer_lab::host::invoke(error,capacity,[&] {
        if (!host||!targets||targets->structSize!=sizeof(*targets)||targets->abiVersion!=RLAB_HOST_ABI_VERSION) throw std::runtime_error("Invalid shared-target ABI");
        auto& value=*static_cast<renderer_lab::host::Host*>(host); value.thread(); *targets=value.targets; return RLAB_HOST_OK;
    });
}
void RendererLabHostDestroy(void* host) {
    if (!host) return;
    auto* value=static_cast<renderer_lab::host::Host*>(host);
    if (value->owner!=std::this_thread::get_id()) return; // JNI enforces owner-thread destruction.
    delete value; renderer_lab::host::occupied=false;
}
}
