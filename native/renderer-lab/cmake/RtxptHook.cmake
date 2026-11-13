# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
# CMAKE_PROJECT_INCLUDE is invoked by dependency project() calls as well.
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR AND PROJECT_NAME STREQUAL "RTXPathTracing")
    function(renderer_lab_add_sdk_targets)
        # The SDK sample macro references variables from its Rtxpt directory.
        # Restore those explicitly in this deferred root-directory invocation.
        set(folder "Renderer lab")
        set(rtxpt_shaders rtxpt_shaders)
        add_library(RendererLabScene STATIC
            "${RENDERER_LAB_ROOT}/src/ScenePacket.cpp"
            "${RENDERER_LAB_ROOT}/src/GltfExport.cpp")
        target_compile_features(RendererLabScene PUBLIC cxx_std_20)
        target_include_directories(RendererLabScene PUBLIC "${RENDERER_LAB_ROOT}/include")
        target_include_directories(RendererLabScene PRIVATE "${RENDERER_LAB_ROOT}/third_party")
        target_compile_definitions(RendererLabScene PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
        target_link_libraries(RendererLabScene PUBLIC windowscodecs ole32)
        set_target_properties(RendererLabScene PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
        add_rtxpt_sample(RendererLabPath "${RENDERER_LAB_ROOT}/src/RtxptPathMain.cpp")
        add_rtxpt_sample(RendererLabHybrid "${RENDERER_LAB_ROOT}/src/RtxptHybridMain.cpp")
        foreach(target RendererLabPath RendererLabHybrid)
            target_include_directories(${target} PRIVATE "${RENDERER_LAB_ROOT}/src")
            target_link_libraries(${target} PRIVATE RendererLabScene)
            set_target_properties(${target} PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different
                    "${CMAKE_SOURCE_DIR}/Assets" "$<TARGET_FILE_DIR:${target}>/Assets")
        endforeach()
        include("${RENDERER_LAB_ROOT}/host/HostCore.cmake")
    endfunction()
    cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL renderer_lab_add_sdk_targets)
endif()
