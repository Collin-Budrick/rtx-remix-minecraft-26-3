# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
# Generates a narrowly adapted SDK implementation in the build directory. The
# source implementation and its NVIDIA license remain in the ignored SDK tree.
file(READ "${sdk_source}/App/SceneRenderer.cpp" replay_source)
string(REPLACE "\r\n" "\n" replay_source "${replay_source}")
string(SHA256 renderer_source_hash "${replay_source}")
if(NOT renderer_source_hash STREQUAL "477154f9fb76b6be9564fda987a78712e5e6f731b0a14c9149438b58312e04b3")
    message(FATAL_ERROR "RTXDI SceneRenderer.cpp does not match the pinned v3.2.0 source")
endif()
macro(restir_replace old_text new_text)
    string(FIND "${replay_source}" "${old_text}" position)
    if(position LESS 0)
        message(FATAL_ERROR "Pinned RTXDI scene adaptation anchor missing: ${old_text}")
    endif()
    string(REPLACE "${old_text}" "${new_text}" replay_source "${replay_source}")
endmacro()
restir_replace("#include \"SceneRenderer.h\"" "#include \"SceneRenderer.h\"\n#include \"RestirReplay.h\"\n#include <stdexcept>")
restir_replace([=[    std::filesystem::path mediaPath = app::GetDirectoryWithExecutable().parent_path() / "Assets/Media";
    if (!std::filesystem::exists(mediaPath))
    {
        mediaPath = app::GetDirectoryWithExecutable().parent_path().parent_path() / "Assets/Media";
        if (!std::filesystem::exists(mediaPath))
        {
            log::error("Couldn't locate the 'Assets/Media' folder.");
            return false;
        }
    }]=] [=[    const std::filesystem::path mediaPath = renderer_lab::replayConfig().directory;]=])
restir_replace([=[    // std::filesystem::path scenePath = "/Assets/Media/bistro-rtxdi.scene.json";
    std::filesystem::path scenePath;
    switch (m_args.sceneAsset)
    {
    case SceneAsset::Arcade:
        scenePath = "/Assets/Media/Arcade/Arcade.gltf";
        break;
    case SceneAsset::Bistro:
        scenePath = "/Assets/Media/bistro-rtxdi.scene.json";
        break;
    default:
        log::warning("Loading unrecognized scene enum, defaulting to BistroMirror scene");
    case SceneAsset::BistroMirror:
        scenePath = "/Assets/Media/bistro_with_mirrors-rtxdi.scene.json";
        break;
    case SceneAsset::CornellBox:
        scenePath = "/Assets/Media/CornellBox/CornellBox.gltf";
        break;
    }]=] [=[    const std::filesystem::path scenePath = "/Assets/Media/world-restir.scene.json";]=])
restir_replace("m_sunLight->angularSize = 1.f;" "m_sunLight->angularSize = 1.f;\n        m_sunLight->irradiance = 0.f; // No invented sun in a captured skyless scene.")
restir_replace([=[    m_ui.environmentMapDirty = 2;
    m_ui.environmentMapIndex = 0;]=] [=[    auto& replayMaps = m_scene->GetEnvironmentMaps();
    replayMaps.clear();
    replayMaps.push_back("");
    const auto& replayEnvironment = renderer_lab::replayConfig().environmentPath;
    if (!replayEnvironment.empty()) replayMaps.push_back(replayEnvironment);
    m_ui.environmentMapIndex = replayEnvironment.empty() ? -1 : 1;
    m_ui.environmentMapDirty = replayEnvironment.empty() ? 0 : 2;]=])
restir_replace([=[                // Failed to load the file: revert to the procedural map and remove this file from the list.
                m_environmentMap = nullptr;
                environmentMaps.erase(environmentMaps.begin() + m_ui.environmentMapIndex);
                m_ui.environmentMapIndex = 0;]=] [=[                throw std::runtime_error("Captured dome texture failed to load; procedural-sky fallback is disabled");]=])
restir_replace("        m_sunLight->irradiance = (m_ui.environmentMapIndex > 0) ? 0.f : 1.f;" "        // Preserve every captured directional light's irradiance.")
restir_replace("        m_sunLight->irradiance = 0.f;\n    }\n}\n\nvoid SceneRenderer::EnforceFpsLimit" "        // Preserve captured directional lighting when the dome is absent.\n    }\n}\n\nvoid SceneRenderer::EnforceFpsLimit")
restir_replace("perspProjD3DStyleReverse(radians(m_ui.verticalFov), aspectRatio, 0.01f)" "perspProjD3DStyleReverse(radians(m_ui.verticalFov), aspectRatio, renderer_lab::replayConfig().scene.camera.zNear)")
# Both SDK fallback sites must reject an unavailable requested feature instead.
restir_replace("m_ui.denoiserMode = DenoiserMode::NRD_REBLUR;" "throw std::runtime_error(\"Requested DLSS-RR is unavailable; replay rejected instead of falling back to NRD\");")
restir_replace("        m_ui.aaMode = AntiAliasingMode::TAA;" "        throw std::runtime_error(\"Requested DLSS-SR is unavailable; replay rejected instead of falling back to TAA\");")
restir_replace("    ProcessScreenshotFrame();" "    ProcessScreenshotFrame();\n    renderer_lab::recordGpuFrame(m_ui, m_profiler, GetDevice(), m_renderTargets->LdrColor, m_renderFrameIndex, GetDeviceManager());")
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/renderer-lab-generated")
set(replay_renderer "${CMAKE_BINARY_DIR}/renderer-lab-generated/SceneRenderer.cpp")
file(WRITE "${replay_renderer}" "${replay_source}")
