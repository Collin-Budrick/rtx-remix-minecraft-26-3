# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
# Applied only to the generated, licensed Donut implementation. A headless
# renderer has no swapchain proxy to retire Streamline's per-frame resources.
function(renderer_lab_replace_streamline old replacement)
    string(FIND "${host_streamline}" "${old}" position)
    if(position LESS 0)
        message(FATAL_ERROR "Pinned Donut headless Streamline lifecycle anchor is missing: ${old}")
    endif()
    string(REPLACE "${old}" "${replacement}" host_streamline "${host_streamline}")
    set(host_streamline "${host_streamline}" PARENT_SCOPE)
endfunction()

renderer_lab_replace_streamline("#include <StreamlineIntegration.h>" [=[#include <StreamlineIntegration.h>
#include <stdexcept>
#include <vector>
// slShutdown unloads feature plugins while this host DLL remains pinned. The
// stock inline helpers cache their function addresses forever; resolve each
// invocation so a second host session cannot call an unloaded RR plugin.
#undef SL_FEATURE_FUN_IMPORT_STATIC
#define SL_FEATURE_FUN_IMPORT_STATIC(feature, func) \
    PFun_##func* s_##func{}; \
    { const sl::Result result = slGetFeatureFunction(feature, #func, (void*&)s_##func); \
      if (result != sl::Result::eOk) return result; }
]=])

renderer_lab_replace_streamline("StreamlineIntegration& StreamlineIntegration::Get()" [=[static bool rendererLabStreamlinePoisoned = false;

// These are the common plugin's manual hooks, exported by the pinned Streamline
// SDK. A null swapchain is supported by presentCommon; this advances resource
// retirement and recycles eValidUntilPresent tags without an OS presentation.
// The host calls it only after completion of that SDK frame's GPU work.
void RendererLabHostStreamlineFrameEnd()
{
    using Present = HRESULT (*)(IDXGISwapChain*, UINT, UINT, bool&);
    using AfterPresent = HRESULT (*)(UINT);
    Present present = nullptr;
    AfterPresent afterPresent = nullptr;
    if (!successCheck(slGetFeatureFunction(sl::kFeatureCommon, "slHookPresent", (void*&)present), "host common present hook") ||
        !successCheck(slGetFeatureFunction(sl::kFeatureCommon, "slHookAfterPresent", (void*&)afterPresent), "host common after-present hook"))
        throw std::runtime_error("Streamline common headless frame hooks are unavailable");
    bool skip = false;
    if (FAILED(present(nullptr, 0, 0, skip)) || FAILED(afterPresent(0)))
        throw std::runtime_error("Streamline common headless frame retirement failed");
}

static bool rendererLabReleaseViewport(const sl::ViewportHandle& viewport, bool rr, bool sr)
{
    bool clean = true;
    // RR resize defers old NGX handles by three frames. The caller has waited
    // for all device queues; retire old handles while RR/NGX still exist.
    try { for (int frame = 0; frame < 4; ++frame) RendererLabHostStreamlineFrameEnd(); }
    catch (const std::exception& error) { log::error("%s", error.what()); clean = false; }
    const sl::BufferType types[] = {
        sl::kBufferTypeDepth, sl::kBufferTypeMotionVectors,
        sl::kBufferTypeScalingInputColor, sl::kBufferTypeScalingOutputColor,
        sl::kBufferTypeHUDLessColor, sl::kBufferTypeBackbuffer,
        sl::kBufferTypeAlbedo, sl::kBufferTypeSpecularAlbedo,
        sl::kBufferTypeNormals, sl::kBufferTypeNormalRoughness,
        sl::kBufferTypeRoughness, sl::kBufferTypeSpecularHitDistance,
        sl::kBufferTypeSpecularMotionVectors
    };
    std::vector<sl::ResourceTag> tags;
    for (auto type : types) tags.emplace_back(nullptr, type, sl::ResourceLifecycle::eValidUntilPresent);
    clean &= successCheck(slSetTag(viewport, tags.data(), uint32_t(tags.size()), nullptr), "host clear viewport tags");
    auto freeViewport = [&](sl::Feature feature, const char* name)
    {
        const auto result = slFreeResources(feature, viewport);
        // No viewport is allocated until the first evaluation of that feature.
        if (result != sl::Result::eErrorInvalidParameter)
            clean &= successCheck(result, name);
    };
#if STREAMLINE_FEATURE_DLSS_RR && STREAMLINE_HAS_DLSS_RR
    if (rr) freeViewport(sl::kFeatureDLSS_RR, "host free DLSS-RR viewport");
#endif
#if STREAMLINE_FEATURE_DLSS_SR
    if (sr) freeViewport(sl::kFeatureDLSS, "host free DLSS-SR viewport");
#endif
    try { for (int frame = 0; frame < 4; ++frame) RendererLabHostStreamlineFrameEnd(); }
    catch (const std::exception& error) { log::error("%s", error.what()); clean = false; }
    return clean;
}

void RendererLabHostStreamlineReleaseViewport()
{
    auto& integration = StreamlineIntegration::Get();
    if (!rendererLabReleaseViewport(sl::ViewportHandle(0), integration.IsDLSSRRAvailable(), integration.IsDLSSAvailable()))
        throw std::runtime_error("Streamline viewport cleanup failed before releasing the renderer targets");
}

void RendererLabHostStreamlineShutdown()
{
    StreamlineIntegration::Get().Shutdown();
}

StreamlineIntegration& StreamlineIntegration::Get()]=])

renderer_lab_replace_streamline([=[    if (m_slInitialized)
    {
        log::info("StreamlineIntegration is already initialised.");]=] [=[    if (rendererLabStreamlinePoisoned)
        throw std::runtime_error("A previous Streamline initialization/shutdown failed; restart Minecraft/the JVM before recreating the native host");
    if (m_slInitialized)
    {
        log::info("StreamlineIntegration is already initialised.");]=])
renderer_lab_replace_streamline([=[    m_slInitialized = successCheck(slInit(pref, SDK_VERSION), "slInit");]=] [=[    m_slInitialized = successCheck(slInit(pref, SDK_VERSION), "slInit");
    rendererLabStreamlinePoisoned = !m_slInitialized;]=])

string(FIND "${host_streamline}" "void StreamlineIntegration::Shutdown()" shutdown_start)
string(FIND "${host_streamline}" "void StreamlineIntegration::SetViewport(" shutdown_end)
if(shutdown_start LESS 0 OR shutdown_end LESS shutdown_start)
    message(FATAL_ERROR "Pinned Donut headless Streamline shutdown boundaries are missing")
endif()
math(EXPR shutdown_length "${shutdown_end} - ${shutdown_start}")
string(SUBSTRING "${host_streamline}" ${shutdown_start} ${shutdown_length} shutdown_old)
renderer_lab_replace_streamline("${shutdown_old}" [=[void StreamlineIntegration::Shutdown()
{
    // DeviceManager also invokes Shutdown, including partial-create cleanup.
    if (!m_slInitialized) return;
    bool clean = true;
    try
    {
        if (m_device)
        {
            m_device->waitForIdle();
            clean &= rendererLabReleaseViewport(m_viewport, m_dlssrrAvailable, m_dlssAvailable);
            m_device->waitForIdle();
        }
    }
    catch (const std::exception& error) { log::error("%s", error.what()); clean = false; }
    clean &= successCheck(slShutdown(), "slShutdown");
    m_slInitialized = false;
    rendererLabStreamlinePoisoned |= !clean;
    m_currentFrame = nullptr;
    m_device = nullptr;
    m_viewport = sl::ViewportHandle(0);
    m_dlssAvailable = m_nisAvailable = m_deepdvcAvailable = false;
    m_dlssgAvailable = m_dlssrrAvailable = m_reflexAvailable = m_pclAvailable = false;
    if (!clean) log::error("Streamline teardown failed; native-host recreation requires a JVM restart.");
}

]=])
