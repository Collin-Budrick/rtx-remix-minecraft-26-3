# SPDX-License-Identifier: Apache-2.0
# Injected into the unmodified, pinned Capsaicin project with CMAKE_PROJECT_INCLUDE.
if(PROJECT_NAME STREQUAL "Capsaicin" AND CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR
        AND NOT DEFINED RENDERER_LAB_GI12_HOOK)
    set(RENDERER_LAB_GI12_HOOK TRUE)
    function(add_renderer_lab_gi12)
        add_library(RendererLabGI12Scene STATIC
            "${RENDERER_LAB_ROOT}/src/ScenePacket.cpp"
            "${RENDERER_LAB_ROOT}/src/GltfExport.cpp")
        target_compile_features(RendererLabGI12Scene PUBLIC cxx_std_20)
        target_include_directories(RendererLabGI12Scene PUBLIC "${RENDERER_LAB_ROOT}/include")
        target_include_directories(RendererLabGI12Scene PRIVATE "${RENDERER_LAB_ROOT}/third_party")
        target_compile_definitions(RendererLabGI12Scene PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_link_libraries(RendererLabGI12Scene PUBLIC windowscodecs ole32)

        add_executable(RendererLabGI12App "${RENDERER_EXPERIMENT_ROOT}/gi12/Gi12Main.cpp")
        target_compile_features(RendererLabGI12App PRIVATE cxx_std_20)
        target_compile_definitions(RendererLabGI12App PRIVATE
            NOMINMAX WIN32_LEAN_AND_MEAN GLM_FORCE_XYZW_ONLY GLM_FORCE_DEPTH_ZERO_TO_ONE)
        target_include_directories(RendererLabGI12App PRIVATE "${RENDERER_LAB_ROOT}/third_party")
        target_link_libraries(RendererLabGI12App PRIVATE capsaicin RendererLabGI12Scene d3d12 dxgi)
        set_target_properties(RendererLabGI12App PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CAPSAICIN_RUNTIME_OUTPUT_DIRECTORY}"
            VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
        add_custom_command(TARGET RendererLabGI12App POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                $<TARGET_RUNTIME_DLLS:RendererLabGI12App> $<TARGET_FILE_DIR:RendererLabGI12App>
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${CMAKE_SOURCE_DIR}/LICENSE" "$<TARGET_FILE_DIR:RendererLabGI12App>/Capsaicin-LICENSE.MIT"
            COMMAND_EXPAND_LISTS)
    endfunction()
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL add_renderer_lab_gi12)
endif()
