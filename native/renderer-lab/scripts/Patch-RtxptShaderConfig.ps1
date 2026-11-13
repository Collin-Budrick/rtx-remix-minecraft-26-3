# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param([Parameter(Mandatory = $true)][string] $SdkPath)
$ErrorActionPreference = 'Stop'
$sdkCommit = (& git -C $SdkPath rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $sdkCommit -ne '303b408ab81790a5051abd7573830b7a4112eb8e') { throw 'RTXPT shader adaptation requires the pinned v.1.9.0 commit' }
$cmakePath = Join-Path $SdkPath 'Rtxpt/CMakeLists.txt'
$cmakeText = [IO.File]::ReadAllText($cmakePath).Replace("`r`n", "`n")
$marker = '# RendererLab: compile only feature-compatible DXR opacity-micromap variants.'
if (-not $cmakeText.Contains($marker)) {
    $anchor = "donut_compile_shaders(`n    TARGET rtxpt_shaders`n    CONFIG " + '${CMAKE_CURRENT_SOURCE_DIR}/shaders.cfg'
    if (-not $cmakeText.Contains($anchor)) { throw 'Pinned RTXPT shader configuration anchor is missing' }
    $adaptation = @'
# RendererLab: compile only feature-compatible DXR opacity-micromap variants.
# The upstream cfg includes OMM=1 permutations even without Agility, whose
# baseline profile is SM6.5. AllocateRayQuery2 requires the SM6.9/DXR1.2 path.
set(RTXPT_SHADER_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/shaders.cfg")
if(NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP)
    if(NOT RTXPT_D3D_AGILITY_SDK_VERSION OR RTXPT_D3D_AGILITY_SDK_VERSION LESS 619)
        message(FATAL_ERROR "DXR1.2 opacity micromaps require Agility 1.619+ and Shader Model 6.9")
    endif()
else()
    file(READ "${RTXPT_SHADER_CONFIG}" RTXPT_BASELINE_SHADER_CONFIG)
    string(REPLACE "\r\n" "\n" RTXPT_BASELINE_SHADER_CONFIG "${RTXPT_BASELINE_SHADER_CONFIG}")
    string(REGEX REPLACE "[^\n]*-DNVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP=1[^\n]*(\n|$)" "" RTXPT_BASELINE_SHADER_CONFIG "${RTXPT_BASELINE_SHADER_CONFIG}")
    set(RTXPT_SHADER_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/shaders-baseline.cfg")
    file(WRITE "${RTXPT_SHADER_CONFIG}" "${RTXPT_BASELINE_SHADER_CONFIG}")
endif()

donut_compile_shaders(
    TARGET rtxpt_shaders
    CONFIG ${RTXPT_SHADER_CONFIG}
'@
    $cmakeText = $cmakeText.Replace($anchor, $adaptation.TrimEnd("`r", "`n"))
    [IO.File]::WriteAllText($cmakePath, $cmakeText, [Text.UTF8Encoding]::new($false))
}
# ShaderMake resolves input filenames relative to its configuration file. Keep
# the filtered configuration beside the original in this ignored SDK checkout.
$oldBaselinePath = 'set(RTXPT_SHADER_CONFIG "${CMAKE_CURRENT_BINARY_DIR}/shaders-baseline.cfg")'
$newBaselinePath = 'set(RTXPT_SHADER_CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/shaders-baseline.cfg")'
if ($cmakeText.Contains($oldBaselinePath)) {
    $cmakeText = $cmakeText.Replace($oldBaselinePath, $newBaselinePath)
    [IO.File]::WriteAllText($cmakePath, $cmakeText, [Text.UTF8Encoding]::new($false))
}
$writeMarker = '# RendererLab: preserve generated shader configuration timestamps when unchanged.'
if (-not $cmakeText.Contains($writeMarker)) {
    $oldBaselineWrite = '    file(WRITE "${RTXPT_SHADER_CONFIG}" "${RTXPT_BASELINE_SHADER_CONFIG}")'
    if (-not $cmakeText.Contains($oldBaselineWrite)) { throw 'Pinned baseline shader configuration write anchor is missing' }
    $newBaselineWrite = @'
    # RendererLab: preserve generated shader configuration timestamps when unchanged.
    set(RTXPT_PREVIOUS_BASELINE_SHADER_CONFIG "")
    if(EXISTS "${RTXPT_SHADER_CONFIG}")
        file(READ "${RTXPT_SHADER_CONFIG}" RTXPT_PREVIOUS_BASELINE_SHADER_CONFIG)
    endif()
    if(NOT "${RTXPT_PREVIOUS_BASELINE_SHADER_CONFIG}" STREQUAL "${RTXPT_BASELINE_SHADER_CONFIG}")
        file(WRITE "${RTXPT_SHADER_CONFIG}" "${RTXPT_BASELINE_SHADER_CONFIG}")
    endif()
'@
    $cmakeText = $cmakeText.Replace($oldBaselineWrite, $newBaselineWrite.TrimEnd("`r", "`n"))
    [IO.File]::WriteAllText($cmakePath, $cmakeText, [Text.UTF8Encoding]::new($false))
}
function Replace-PinnedShaderText {
    param([string] $RelativePath, [string] $OldText, [string] $NewText, [int] $ExpectedCount)
    $shaderPath = Join-Path $SdkPath $RelativePath
    $shaderText = [IO.File]::ReadAllText($shaderPath).Replace("`r`n", "`n")
    $count = [regex]::Matches($shaderText, [regex]::Escape($OldText)).Count
    if ($count -eq 0 -and $shaderText.Contains($NewText)) { return }
    if ($count -ne $ExpectedCount) { throw "Pinned shader adaptation anchor mismatch: $RelativePath ($count, expected $ExpectedCount)" }
    [IO.File]::WriteAllText($shaderPath, $shaderText.Replace($OldText, $NewText), [Text.UTF8Encoding]::new($false))
}
$oldBridgeFlag = @'
// I think this is obsolete now with NVAPI OMM's gone?
#if NVRHI_D3D12_WITH_DXR12_OPACITY_MICROMAP // this doesn't seem to work?
#define RTXPT_FLAG_ALLOW_OPACITY_MICROMAPS 0 //RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS
#else
#define RTXPT_FLAG_ALLOW_OPACITY_MICROMAPS 0
#endif
'@
$newBridgeFlag = @'
// RendererLab: match the SDK IntroBridge shader-profile feature guard. The
// native DXR1.2 build selects SM6.9; baseline shaders must use RayQuery v1.
#if (__SHADER_TARGET_MAJOR > 6) || (__SHADER_TARGET_MAJOR == 6 && __SHADER_TARGET_MINOR >= 9)
#define RTXPT_FLAG_ALLOW_OPACITY_MICROMAPS RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS
#else
#define RTXPT_FLAG_ALLOW_OPACITY_MICROMAPS 0
#endif
'@
Replace-PinnedShaderText 'Rtxpt/Shaders/PathTracerBridge.hlsli' $oldBridgeFlag.TrimEnd("`r", "`n") $newBridgeFlag.TrimEnd("`r", "`n") 1
$oldRayFlag = ', RAYQUERY_FLAG_ALLOW_OPACITY_MICROMAPS>'
$newRayFlag = ', RTXPT_FLAG_ALLOW_OPACITY_MICROMAPS>'
Replace-PinnedShaderText 'Rtxpt/Shaders/PathTracerBridge.hlsli' $oldRayFlag $newRayFlag 1
Replace-PinnedShaderText 'Rtxpt/Shaders/PathTracerBridgeDonut.hlsli' $oldRayFlag $newRayFlag 2
Replace-PinnedShaderText 'Rtxpt/RTXDI/RtxdiApplicationBridge.hlsli' $oldRayFlag $newRayFlag 1
# ShaderMake prints normalized paths but previously tested raw recursive include
# strings. Repeated ../ segments can exceed Win32 MAX_PATH although the real
# file exists. Normalize before lookup and cache access, preserving include roots.
$oldDependencyEntry = "static bool GetHierarchicalUpdateTime(const fs::path& file, list<fs::path>& callStack, fs::file_time_type& outTime) {"
$newDependencyEntry = "static bool GetHierarchicalUpdateTime(const fs::path& inputFile, list<fs::path>& callStack, fs::file_time_type& outTime) {`n    const fs::path file = inputFile.lexically_normal(); // RendererLab: normalize before filesystem access."
$shaderMakeSource = 'External/Donut/ShaderMake/ShaderMake/ShaderMake.cpp'
Replace-PinnedShaderText $shaderMakeSource $oldDependencyEntry $newDependencyEntry 1
Replace-PinnedShaderText $shaderMakeSource 'fs::path includeFile = path / includeName;' 'fs::path includeFile = (path / includeName).lexically_normal();' 1
Replace-PinnedShaderText $shaderMakeSource 'includeFile = includePath / includeName;' 'includeFile = (includePath / includeName).lexically_normal();' 1
Write-Output 'Prepared feature-gated RTXPT shader permutations; validation remains enabled.'
