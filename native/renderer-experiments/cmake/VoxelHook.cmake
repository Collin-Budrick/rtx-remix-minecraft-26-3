# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
if(PROJECT_NAME STREQUAL "VoxelRT" AND NOT DEFINED RENDERER_LAB_VOXEL_HOOK)
    set(RENDERER_LAB_VOXEL_HOOK TRUE)
    function(add_renderer_lab_voxel)
        get_target_property(vendor_sources VoxelRT SOURCES)
        set(sources "")
        foreach(source IN LISTS vendor_sources)
            if(NOT source STREQUAL "Main.cpp")
                list(APPEND sources "${CMAKE_SOURCE_DIR}/VoxelRT/${source}")
            endif()
        endforeach()
        add_executable(RendererLabVoxel ${sources}
            "${RENDERER_EXPERIMENT_ROOT}/voxel/VoxelMain.cpp"
            "${RENDERER_LAB_ROOT}/src/ScenePacket.cpp")
        target_include_directories(RendererLabVoxel PRIVATE
            "${CMAKE_SOURCE_DIR}/VoxelRT" "${RENDERER_LAB_ROOT}/include")
        target_link_libraries(RendererLabVoxel PRIVATE common_compile_opts havk FastNoise fastgltf::fastgltf bvh)
        if(MSVC)
            target_compile_options(RendererLabVoxel PRIVATE /Zi)
            target_link_options(RendererLabVoxel PRIVATE /DEBUG)
        endif()
        add_custom_command(TARGET RendererLabVoxel POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:RendererLabVoxel> $<TARGET_FILE_DIR:RendererLabVoxel>
            COMMAND_EXPAND_LISTS)
    endfunction()
    cmake_language(DEFER CALL add_renderer_lab_voxel)
endif()
