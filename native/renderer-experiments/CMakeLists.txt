# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.30)
project(MinecraftRendererExperiments LANGUAGES CXX)

include(CTest)
set(RENDERER_LAB_ROOT "${CMAKE_CURRENT_LIST_DIR}/../renderer-lab")
set(RENDERER_EXPERIMENTS_SOURCE_CACHE "${CMAKE_CURRENT_LIST_DIR}/../../.codex/vendor-src"
    CACHE PATH "Pinned, prepared upstream source checkouts")
option(RENDERER_EXPERIMENT_BUILD_GI12 "Build the imported GI-1.2 renderer" OFF)
option(RENDERER_EXPERIMENT_BUILD_RESTIR_PT "Build the imported ReSTIR PT Enhanced renderer" OFF)
option(RENDERER_EXPERIMENT_BUILD_VOXEL "Build the imported brick/Tree64 renderer adapter" OFF)

add_executable(InstanceUpdateBatchTests tests/InstanceUpdateBatchTests.cpp)
target_compile_features(InstanceUpdateBatchTests PRIVATE cxx_std_17)
target_include_directories(InstanceUpdateBatchTests PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../rtxremix-java/native")
add_test(NAME InstanceUpdateBatchContract COMMAND InstanceUpdateBatchTests)

add_executable(ScenePacketTests tests/ScenePacketTests.cpp "${RENDERER_LAB_ROOT}/src/ScenePacket.cpp")
target_compile_features(ScenePacketTests PRIVATE cxx_std_20)
target_include_directories(ScenePacketTests PRIVATE "${RENDERER_LAB_ROOT}/include")
add_test(NAME ScenePacketTransactions COMMAND ScenePacketTests)

if(RENDERER_EXPERIMENT_BUILD_GI12 OR RENDERER_EXPERIMENT_BUILD_RESTIR_PT OR RENDERER_EXPERIMENT_BUILD_VOXEL)
    if(NOT WIN32)
        message(FATAL_ERROR "The current comparison applications target Windows x64")
    endif()
    include(ExternalProject)
endif()

function(require_prepared name)
    if(NOT EXISTS "${RENDERER_EXPERIMENTS_SOURCE_CACHE}/${name}/.renderer-lab-prepared")
        message(FATAL_ERROR "Run scripts/Prepare-Experiments.ps1 -Project ${name} before building this experiment")
    endif()
endfunction()

if(RENDERER_EXPERIMENT_BUILD_GI12)
    require_prepared(Capsaicin)
    ExternalProject_Add(RendererLabGI12
        SOURCE_DIR "${RENDERER_EXPERIMENTS_SOURCE_CACHE}/Capsaicin"
        BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/gi12"
        DOWNLOAD_COMMAND "" UPDATE_COMMAND ""
        CMAKE_ARGS -DCAPSAICIN_DOWNLOAD_TEST_MEDIA=OFF -DCAPSAICIN_PREPARE_TEST_MEDIA_SCENES=OFF
            "-DCMAKE_PROJECT_INCLUDE=${CMAKE_CURRENT_LIST_DIR}/cmake/Gi12Hook.cmake"
            "-DRENDERER_EXPERIMENT_ROOT=${CMAKE_CURRENT_LIST_DIR}"
            "-DRENDERER_LAB_ROOT=${RENDERER_LAB_ROOT}"
        BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config Release --target RendererLabGI12App
        INSTALL_COMMAND "" BUILD_ALWAYS TRUE
        USES_TERMINAL_CONFIGURE TRUE USES_TERMINAL_BUILD TRUE)
endif()

if(RENDERER_EXPERIMENT_BUILD_RESTIR_PT)
    require_prepared(RTXDI)
    find_program(RENDERER_LAB_FXC fxc REQUIRED
        HINTS "$ENV{WindowsSdkVerBinPath}/x64" "$ENV{WindowsSdkDir}/bin/$ENV{WindowsSDKVersion}/x64")
    ExternalProject_Add(RendererLabReSTIRPT
        SOURCE_DIR "${RENDERER_EXPERIMENTS_SOURCE_CACHE}/RTXDI"
        BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/restir-pt"
        DOWNLOAD_COMMAND "" UPDATE_COMMAND ""
        CMAKE_ARGS -DDONUT_WITH_VULKAN=OFF -DDONUT_WITH_DX12=ON -DDONUT_WITH_DLSS=ON
            -DSHADERMAKE_FIND_FXC=OFF
            "-DSHADERMAKE_FXC_PATH=${RENDERER_LAB_FXC}"
            "-DCMAKE_PROJECT_INCLUDE=${CMAKE_CURRENT_LIST_DIR}/cmake/RestirHook.cmake"
            "-DRENDERER_EXPERIMENT_ROOT=${CMAKE_CURRENT_LIST_DIR}"
            "-DRENDERER_LAB_ROOT=${RENDERER_LAB_ROOT}"
        BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --config Release --target RendererLabReSTIRPT
        INSTALL_COMMAND "" BUILD_ALWAYS TRUE
        USES_TERMINAL_CONFIGURE TRUE USES_TERMINAL_BUILD TRUE)
endif()

if(RENDERER_EXPERIMENT_BUILD_VOXEL)
    require_prepared(VoxelRT)
    find_program(RENDERER_LAB_CLANG_CL clang-cl REQUIRED HINTS "$ENV{ProgramFiles}/LLVM/bin")
    ExternalProject_Add(RendererLabVoxel
        SOURCE_DIR "${RENDERER_EXPERIMENTS_SOURCE_CACHE}/VoxelRT/src"
        BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/voxel"
        DOWNLOAD_COMMAND "" UPDATE_COMMAND ""
        CMAKE_GENERATOR Ninja
        CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release
            "-DCMAKE_CXX_COMPILER=${RENDERER_LAB_CLANG_CL}"
            "-DCMAKE_C_COMPILER=${RENDERER_LAB_CLANG_CL}"
            "-DCMAKE_PROJECT_INCLUDE=${CMAKE_CURRENT_LIST_DIR}/cmake/VoxelHook.cmake"
            "-DRENDERER_EXPERIMENT_ROOT=${CMAKE_CURRENT_LIST_DIR}"
            "-DRENDERER_LAB_ROOT=${RENDERER_LAB_ROOT}"
        BUILD_COMMAND "${CMAKE_COMMAND}" --build <BINARY_DIR> --target RendererLabVoxel
        INSTALL_COMMAND "" BUILD_ALWAYS TRUE
        USES_TERMINAL_CONFIGURE TRUE USES_TERMINAL_BUILD TRUE)
endif()
