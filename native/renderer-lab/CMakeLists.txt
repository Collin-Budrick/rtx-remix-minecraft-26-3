# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.24)
project(MinecraftRendererLab LANGUAGES CXX)

option(RENDERER_LAB_BUILD_RTXPT "Build the pinned DX12 path and raster-primary RTXPT applications" OFF)
set(RENDERER_LAB_RTXPT_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/.deps/RTXPT" CACHE PATH "Prepared RTXPT v.1.9.0 checkout")
set(RENDERER_LAB_RTXPT_BUILD "${CMAKE_CURRENT_BINARY_DIR}/rtxpt" CACHE PATH "Separate SDK build tree")

add_library(RendererLabScene STATIC src/ScenePacket.cpp src/GltfExport.cpp)
target_compile_features(RendererLabScene PUBLIC cxx_std_20)
target_include_directories(RendererLabScene PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(RendererLabScene PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/third_party")
target_compile_definitions(RendererLabScene PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
if(WIN32)
    target_link_libraries(RendererLabScene PUBLIC windowscodecs ole32)
endif()
set_target_properties(RendererLabScene PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
add_executable(RendererLabExport src/ExportMain.cpp)
target_link_libraries(RendererLabExport PRIVATE RendererLabScene)

if(RENDERER_LAB_BUILD_RTXPT)
    if(NOT WIN32)
        message(FATAL_ERROR "The experimental SDK adapters currently require Windows/DX12")
    endif()
    if(NOT EXISTS "${RENDERER_LAB_RTXPT_SOURCE}/.renderer-lab-prepared")
        message(FATAL_ERROR "Run scripts/Prepare-Rtxpt.ps1 before configuring the RTXPT adapters")
    endif()
    include(ExternalProject)
    # The SDK resolves several dependencies with CMAKE_SOURCE_DIR. Give it its own
    # configure, then inject our adapters after its real rendering targets exist.
    ExternalProject_Add(RendererLabRTXPT
        SOURCE_DIR "${RENDERER_LAB_RTXPT_SOURCE}"
        BINARY_DIR "${RENDERER_LAB_RTXPT_BUILD}"
        DOWNLOAD_COMMAND ""
        UPDATE_COMMAND ""
        CMAKE_ARGS
            "-DCMAKE_BUILD_TYPE=Release"
            "-DCMAKE_PROJECT_INCLUDE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/RtxptHook.cmake"
            "-DRENDERER_LAB_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DRTXPT_D3D_AGILITY_SDK_VERSION_NAME="
            "-DDONUT_WITH_DX11=OFF" "-DDONUT_WITH_VULKAN=OFF"
            "-DDONUT_WITH_KTX=OFF" "-DDONUT_WITH_STREAMLINE=ON"
            "-DRTXPT_LOCAL_CONFIG_ID=RendererLab"
        BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config Release --target RendererLabPath RendererLabHybrid RendererLabHost
        INSTALL_COMMAND ""
        BUILD_ALWAYS TRUE
        USES_TERMINAL_CONFIGURE TRUE USES_TERMINAL_BUILD TRUE)
endif()
