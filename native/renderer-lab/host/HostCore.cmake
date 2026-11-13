# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
# Imported implementation stays in the ignored SDK; generate only the checked
# headless boundary adaptations in the SDK binary directory.
set(host_generated "${CMAKE_BINARY_DIR}/renderer-lab-host-generated")
file(MAKE_DIRECTORY "${host_generated}")
function(renderer_lab_write_generated path content)
    if(EXISTS "${path}")
        file(READ "${path}" previous_content)
        if("${previous_content}" STREQUAL "${content}")
            return()
        endif()
    endif()
    file(WRITE "${path}" "${content}")
endfunction()
file(READ "${CMAKE_SOURCE_DIR}/Rtxpt/SampleCommon/SampleCommon.cpp" host_common)
string(REPLACE "\r\n" "\n" host_common "${host_common}")
set(path_old [=[std::filesystem::path GetLocalPath(std::string subfolder)
{
    static std::filesystem::path oneChoice;
    // if( oneChoice.empty() )
    {
        std::filesystem::path candidateA = donut::app::GetDirectoryWithExecutable() / subfolder;
        std::filesystem::path candidateB = donut::app::GetDirectoryWithExecutable().parent_path() / subfolder;
        if (std::filesystem::exists(candidateA))
            oneChoice = candidateA;
        else
            oneChoice = candidateB;
    }
    return oneChoice;
}]=])
string(FIND "${host_common}" "${path_old}" path_position)
if(path_position LESS 0)
    message(FATAL_ERROR "Pinned SDK GetLocalPath headless adaptation anchor is missing")
endif()
string(REPLACE "${path_old}" [=[extern std::filesystem::path RendererLabHostLocalPath(const std::string& subfolder);
std::filesystem::path GetLocalPath(std::string subfolder)
{
    return RendererLabHostLocalPath(subfolder);
}]=] host_common "${host_common}")
string(FIND "${host_common}" "int ProgressBarStart(const char * windowText)" progress_start)
string(FIND "${host_common}" "void ProgressBarStop(" progress_end)
if(progress_start LESS 0 OR progress_end LESS progress_start)
    message(FATAL_ERROR "Pinned SDK progress boundary anchors are missing")
endif()
string(SUBSTRING "${host_common}" 0 ${progress_start} progress_prefix)
string(SUBSTRING "${host_common}" ${progress_end} -1 progress_suffix)
set(host_common "${progress_prefix}int ProgressBarStart(const char *) { return -1; } // Minecraft owns all windows.\n\n${progress_suffix}")
set(exit_old [=[void HelpersExitProcess(int exitCode)
{
    // Not std::exit: DLL detach with Streamline/NGX still initialized crashes in nvngx_dlssd. TerminateProcess skips
    // detach and static destructors (so no joinable progress bar threads either); only stdio needs flushing.
    fflush(nullptr);
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), (UINT)exitCode);
#endif
    std::_Exit(exitCode);
}]=])
string(FIND "${host_common}" "${exit_old}" exit_position)
if(exit_position LESS 0)
    message(FATAL_ERROR "Pinned SDK process-exit boundary anchor is missing")
endif()
string(REPLACE "${exit_old}" [=[void HelpersExitProcess(int exitCode)
{
    throw std::runtime_error("SDK requested renderer shutdown (code " + std::to_string(exitCode) + "); see the preceding SDK error");
}]=] host_common "${host_common}")
string(REPLACE "#include \"SampleCommon.h\"" "#include \"SampleCommon.h\"\n#include <stdexcept>" host_common "${host_common}")
renderer_lab_write_generated("${host_generated}/SampleCommon.cpp" "${host_common}")

file(READ "${CMAKE_SOURCE_DIR}/Rtxpt/Sample.cpp" host_sample)
string(REPLACE "\r\n" "\n" host_sample "${host_sample}")
set(title_call "    GetDeviceManager()->SetInformativeWindowTitle(windowTitle.c_str(), false, extraInfo.c_str());")
string(FIND "${host_sample}" "${title_call}" title_position)
if(title_position LESS 0)
    message(FATAL_ERROR "Pinned SDK window-title boundary anchor is missing")
endif()
string(REPLACE "${title_call}" "    if (!GetDeviceManager()->GetDeviceParams().headlessDevice)\n        GetDeviceManager()->SetInformativeWindowTitle(windowTitle.c_str(), false, extraInfo.c_str());" host_sample "${host_sample}")
renderer_lab_write_generated("${host_generated}/Sample.cpp" "${host_sample}")

file(READ "${CMAKE_SOURCE_DIR}/Rtxpt/SampleCommon/ShaderCompilerUtils.cpp" host_compiler)
set(compiler_exe_path "donut::app::GetDirectoryWithExecutable()")
string(REGEX MATCHALL "donut::app::GetDirectoryWithExecutable\\(\\)" compiler_exe_paths "${host_compiler}")
list(LENGTH compiler_exe_paths compiler_exe_path_count)
if(NOT compiler_exe_path_count EQUAL 10)
    message(FATAL_ERROR "Pinned SDK runtime shader-compiler path anchors changed")
endif()
string(REPLACE "${compiler_exe_path}" "GetLocalPath(\"\")" host_compiler "${host_compiler}")
renderer_lab_write_generated("${host_generated}/ShaderCompilerUtils.cpp" "${host_compiler}")

get_target_property(host_sdk_sources RtxptCore SOURCES)
get_target_property(host_sdk_links RtxptCore LINK_LIBRARIES)
get_target_property(host_sdk_includes RtxptCore INCLUDE_DIRECTORIES)
get_target_property(host_sdk_definitions RtxptCore COMPILE_DEFINITIONS)
set(host_sources)
foreach(source IN LISTS host_sdk_sources)
    if(NOT source STREQUAL "${CMAKE_SOURCE_DIR}/Rtxpt/SampleCommon/SampleCommon.cpp" AND NOT source STREQUAL "${CMAKE_SOURCE_DIR}/Rtxpt/Sample.cpp" AND NOT source STREQUAL "${CMAKE_SOURCE_DIR}/Rtxpt/SampleCommon/ShaderCompilerUtils.cpp")
        list(APPEND host_sources "${source}")
    endif()
endforeach()
add_library(RendererLabHostCore STATIC ${host_sources} "${host_generated}/SampleCommon.cpp" "${host_generated}/Sample.cpp" "${host_generated}/ShaderCompilerUtils.cpp")
target_include_directories(RendererLabHostCore PUBLIC ${host_sdk_includes} "${CMAKE_SOURCE_DIR}/Rtxpt/SampleCommon")
target_link_libraries(RendererLabHostCore PUBLIC ${host_sdk_links})
target_compile_definitions(RendererLabHostCore PUBLIC ${host_sdk_definitions})
set_target_properties(RendererLabHostCore PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
add_dependencies(RendererLabHostCore rtxpt_shaders nrd_shaders omm_shaders ShaderDynamicAssets ShaderDynamicAssets_CopyAlways)

add_library(RendererLabHost SHARED "${RENDERER_LAB_ROOT}/host/RendererLabHost.cpp")
target_include_directories(RendererLabHost PRIVATE "${RENDERER_LAB_ROOT}/host")
target_link_libraries(RendererLabHost PRIVATE RendererLabHostCore RendererLabScene d3dcompiler dxgi d3d12)
if(DONUT_WITH_STREAMLINE)
    # Donut resolves sl.interposer.dll beside the process executable, which is
    # java.exe for the host. Compile its original integration as a direct object
    # with an explicit DLL/plugin path; the corresponding static-library archive
    # member is then not extracted. No original Donut source is changed.
    file(READ "${CMAKE_SOURCE_DIR}/External/Donut/src/app/streamline/StreamlineIntegration.cpp" host_streamline)
    string(REPLACE "\r\n" "\n" host_streamline "${host_streamline}")
    set(sl_path_old [=[static std::wstring GetSlInterposerDllLocation()
{
    wchar_t path[PATH_MAX] = { 0 };
#ifdef _WIN32
    if (GetModuleFileNameW(nullptr, path, dim(path)) == 0)
        return std::wstring();
#else // _WIN32
#error Unsupported platform for GetSlInterposerDllLocation!
#endif // _WIN32

    auto basePath = std::filesystem::path(path).parent_path();
    auto dllPath = basePath.wstring().append(L"\\sl.interposer.dll");
    return dllPath;
}]=])
    string(FIND "${host_streamline}" "${sl_path_old}" sl_path_position)
    if(sl_path_position LESS 0)
        message(FATAL_ERROR "Pinned Donut Streamline module-path adaptation anchor is missing")
    endif()
    string(REPLACE "${sl_path_old}" [=[extern std::filesystem::path RendererLabHostLocalPath(const std::string& subfolder);
static std::wstring GetSlInterposerDllLocation()
{
    return (RendererLabHostLocalPath("") / "sl.interposer.dll").wstring();
}]=] host_streamline "${host_streamline}")
    set(sl_preferences "    sl::Preferences pref;")
    string(FIND "${host_streamline}" "${sl_preferences}" sl_preferences_position)
    if(sl_preferences_position LESS 0)
        message(FATAL_ERROR "Pinned Donut Streamline plugin-path adaptation anchor is missing")
    endif()
    string(REPLACE "${sl_preferences}" [=[    sl::Preferences pref;
    const std::wstring hostPluginRoot = RendererLabHostLocalPath("").wstring();
    const wchar_t* hostPluginPaths[] = { hostPluginRoot.c_str() };
    pref.pathsToPlugins = hostPluginPaths;
    pref.numPathsToPlugins = 1;]=] host_streamline "${host_streamline}")
    set(sl_fg_request "        sl::kFeatureDLSS_G,")
    set(sl_fg_capability "    m_dlssgAvailable = sl::Result::eOk == slIsFeatureSupported(sl::kFeatureDLSS_G, adapterInfo), \"slIsFeatureSupported_DLSSG\";")
    string(FIND "${host_streamline}" "${sl_fg_request}" sl_fg_request_position)
    string(FIND "${host_streamline}" "${sl_fg_capability}" sl_fg_capability_position)
    if(sl_fg_request_position LESS 0 OR sl_fg_capability_position LESS 0)
        message(FATAL_ERROR "Pinned Donut headless frame-generation boundary anchors are missing")
    endif()
    string(REPLACE "${sl_fg_request}" "        // Minecraft owns presentation; headless frame generation is unavailable." host_streamline "${host_streamline}")
    string(REPLACE "${sl_fg_capability}" "    m_dlssgAvailable = false; // No host swapchain and no generated frames." host_streamline "${host_streamline}")
    include("${RENDERER_LAB_ROOT}/host/AdaptStreamline.cmake")
    renderer_lab_write_generated("${host_generated}/StreamlineIntegration.cpp" "${host_streamline}")
    target_sources(RendererLabHost PRIVATE "${host_generated}/StreamlineIntegration.cpp")
    get_target_property(host_donut_includes donut_app INCLUDE_DIRECTORIES)
    get_target_property(host_donut_definitions donut_app COMPILE_DEFINITIONS)
    target_include_directories(RendererLabHost PRIVATE ${host_donut_includes})
    target_compile_definitions(RendererLabHost PRIVATE ${host_donut_definitions})
endif()
set_target_properties(RendererLabHost PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
if(MSVC)
    target_compile_options(RendererLabHostCore PRIVATE /Zi)
    target_compile_options(RendererLabHost PRIVATE /Zi)
    target_link_options(RendererLabHost PRIVATE /DEBUG)
endif()
add_custom_command(TARGET RendererLabHost POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_directory_if_different "${CMAKE_SOURCE_DIR}/Assets" "$<TARGET_FILE_DIR:RendererLabHost>/Assets")
