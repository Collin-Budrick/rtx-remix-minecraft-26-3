# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$labRoot = Split-Path -Parent $PSScriptRoot
$sdkPath = Join-Path $labRoot '.deps/RTXPT'
$sdkCommit = '303b408ab81790a5051abd7573830b7a4112eb8e'
$assetCommit = 'a56be4ed65029d2733fd02f00f4027ffd541eda8'
function Invoke-Git {
    param([Parameter(ValueFromRemainingArguments = $true)][string[]] $Arguments)
    & git @Arguments
    if ($LASTEXITCODE -ne 0) { throw "git failed (exit $LASTEXITCODE)" }
}
if (-not (Test-Path -LiteralPath (Join-Path $sdkPath '.git'))) {
    New-Item -ItemType Directory -Force (Split-Path -Parent $sdkPath) | Out-Null
    Invoke-Git @('clone', '--depth', '1', '--branch', 'v.1.9.0', 'https://github.com/NVIDIA-RTX/RTXPT.git', $sdkPath)
}
$actualCommit = (& git -C $sdkPath rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $actualCommit -ne $sdkCommit) { throw "Expected pinned RTXPT $sdkCommit; found $actualCommit" }
Invoke-Git @('-C', $sdkPath, 'submodule', 'update', '--init', '--recursive', '--depth', '1', '--',
    'External/Donut', 'External/Nrd', 'External/Omm', 'External/NVAPI', 'External/cxxopts',
    'External/Rtxdi', 'External/Rtxtf', 'External/SHARC')

# Fetch only SDK fonts and standalone support textures, not multi-GB sample worlds.
# The Assets gitlink is pinned by the parent commit; root license/readme files
# remain included by cone-mode sparse checkout.
$assetPath = Join-Path $sdkPath 'Assets'
if (-not (Test-Path -LiteralPath (Join-Path $assetPath '.git'))) {
    New-Item -ItemType Directory -Force $assetPath | Out-Null
    Invoke-Git @('-C', $assetPath, 'init')
    Invoke-Git @('-C', $assetPath, 'remote', 'add', 'origin', 'https://github.com/NVIDIA-RTX/RTXPT-Assets.git')
    Invoke-Git @('-C', $assetPath, 'sparse-checkout', 'init', '--cone')
    Invoke-Git @('-C', $assetPath, 'sparse-checkout', 'set', 'fonts', 'StandaloneTextures')
    Invoke-Git @('-C', $assetPath, 'fetch', '--filter=blob:none', '--depth', '1', 'origin', $assetCommit)
    Invoke-Git @('-C', $assetPath, 'checkout', '--detach', 'FETCH_HEAD')
}
$actualAssetCommit = (& git -C $assetPath rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or $actualAssetCommit -ne $assetCommit) { throw 'RTXPT Assets pin mismatch' }
New-Item -ItemType Directory -Force (Join-Path $assetPath 'EnvironmentMaps') | Out-Null

# These small integration edits are deliberately outside the vendored license
# text and rendering implementation. Exact source checks make version drift fail.
$samplePath = Join-Path $sdkPath 'Rtxpt/Sample.cpp'
$sampleText = [IO.File]::ReadAllText($samplePath)
$anchor = 'static std::string FindPreferredSceneCaseInsensitive(const std::vector<std::string>& available, const std::string& preferred)'
$marker = '// RendererLab: explicit generated scenes must never fall back to a sample world.'
if (-not $sampleText.Contains($marker)) {
    $start = $sampleText.IndexOf($anchor, [StringComparison]::Ordinal)
    if ($start -lt 0) { throw 'Pinned SDK scene resolver signature not found' }
    $brace = $sampleText.IndexOf('{', $start)
    $insertion = "`n    $marker`n    if (std::filesystem::path(preferred).is_absolute())`n    {`n        if (!std::filesystem::is_regular_file(preferred))`n            throw std::runtime_error(`"RendererLab scene does not exist: `" + preferred);`n        return preferred;`n    }`n"
    $sampleText = $sampleText.Insert($brace + 1, $insertion)
    [IO.File]::WriteAllText($samplePath, $sampleText, [Text.UTF8Encoding]::new($false))
}
$appPath = Join-Path $sdkPath 'Rtxpt/SampleCommon/SampleBaseApp.cpp'
$appText = [IO.File]::ReadAllText($appPath)
$oldCall = 'm_CmdLine.InitFromCommandLine(__argc, __argv)'
$newCall = 'm_CmdLine.InitFromCommandLine(argc, argv)'
if ($appText.Contains($oldCall)) {
    [IO.File]::WriteAllText($appPath, $appText.Replace($oldCall,$newCall), [Text.UTF8Encoding]::new($false))
} elseif (-not $appText.Contains($newCall)) { throw 'Pinned SDK command-line call not found' }
& (Join-Path $PSScriptRoot 'Patch-RtxptShaderConfig.ps1') -SdkPath $sdkPath
[IO.File]::WriteAllText((Join-Path $sdkPath '.renderer-lab-prepared'), "$sdkCommit`n$assetCommit`n", [Text.UTF8Encoding]::new($false))
Write-Output "Prepared NVIDIA RTXPT $sdkCommit with pinned support assets; no build was run."
