# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param(
    [ValidateSet('All','Core','RTXPT','Capsaicin','VoxelRT','RTXDI')]
    [string]$Project = 'Core',
    [ValidateRange(1,32)][int]$Jobs = 8,
    [string]$SourceCache
)
$ErrorActionPreference = 'Stop'
$experimentRoot = Split-Path -Parent $PSScriptRoot
$repoRoot = [IO.Path]::GetFullPath((Join-Path $experimentRoot '../..'))
$labRoot = Join-Path $repoRoot 'native/renderer-lab'
$buildRoot = Join-Path $repoRoot 'build/renderer-experiments'
if (-not $SourceCache) { $SourceCache = Join-Path $repoRoot '.codex/vendor-src' }
$env:CMAKE_BUILD_PARALLEL_LEVEL = "$Jobs"
function Invoke-Native([string]$Executable, [string[]]$Arguments) {
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Executable failed with exit code $LASTEXITCODE" }
}
if ($Project -ne 'Core') {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $vsPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsPath) { throw 'Visual Studio C++ x64 build tools are required.' }
    Import-Module (Join-Path $vsPath 'Common7/Tools/Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
    $env:TARGET_CPU = 'AVX2'
}
$nativeArgs = @('-S',$experimentRoot,'-B',$buildRoot,'-G','Visual Studio 17 2022','-A','x64',
    "-DRENDERER_EXPERIMENTS_SOURCE_CACHE=$SourceCache",
    '-DRENDERER_EXPERIMENT_BUILD_GI12=OFF','-DRENDERER_EXPERIMENT_BUILD_VOXEL=OFF',
    '-DRENDERER_EXPERIMENT_BUILD_RESTIR_PT=OFF')
$targets = @('InstanceUpdateBatchTests','ScenePacketTests')
if ($Project -ne 'Core' -and $Project -ne 'RTXPT') {
    & (Join-Path $PSScriptRoot 'Prepare-Experiments.ps1') -Project $Project -SourceCache $SourceCache
    if (-not $?) { throw 'SDK source preparation failed.' }
    if ($Project -eq 'All' -or $Project -eq 'Capsaicin') {
        $nativeArgs += '-DRENDERER_EXPERIMENT_BUILD_GI12=ON'
        $targets += 'RendererLabGI12'
    }
    if ($Project -eq 'All' -or $Project -eq 'RTXDI') {
        $nativeArgs += '-DRENDERER_EXPERIMENT_BUILD_RESTIR_PT=ON'
        $targets += 'RendererLabReSTIRPT'
    }
    if ($Project -eq 'All' -or $Project -eq 'VoxelRT') {
        $nativeArgs += '-DRENDERER_EXPERIMENT_BUILD_VOXEL=ON'
        $targets += 'RendererLabVoxel'
    }
}
Invoke-Native cmake $nativeArgs
Invoke-Native cmake (@('--build',$buildRoot,'--config','Release','--parallel',"$Jobs",'--target') + $targets)
Invoke-Native ctest @('--test-dir',$buildRoot,'-C','Release','--output-on-failure')
$labBuild = Join-Path $repoRoot 'build/renderer-lab'
$withRtxpt = $Project -eq 'All' -or $Project -eq 'RTXPT'
if ($withRtxpt) {
    & (Join-Path $labRoot 'scripts/Prepare-Rtxpt.ps1')
    if (-not $?) { throw 'RTXPT source preparation failed.' }
}
Invoke-Native cmake @('-S',$labRoot,'-B',$labBuild,'-G','Visual Studio 17 2022','-A','x64',
    "-DRENDERER_LAB_BUILD_RTXPT=$(if ($withRtxpt) { 'ON' } else { 'OFF' })")
$labTargets = @('RendererLabExport')
if ($withRtxpt) { $labTargets += 'RendererLabRTXPT' }
Invoke-Native cmake (@('--build',$labBuild,'--config','Release','--parallel',"$Jobs",'--target') + $labTargets)
