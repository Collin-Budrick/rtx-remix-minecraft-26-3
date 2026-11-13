# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
[CmdletBinding()]
param(
    [ValidateSet('All','Capsaicin','VoxelRT','RTXDI')]
    [string]$Project = 'All',
    [string]$SourceCache
)
$ErrorActionPreference = 'Stop'
$experimentRoot = Split-Path -Parent $PSScriptRoot
$repoRoot = [IO.Path]::GetFullPath((Join-Path $experimentRoot '../..'))
if (-not $SourceCache) { $SourceCache = Join-Path $repoRoot '.codex/vendor-src' }
New-Item -ItemType Directory -Force -Path $SourceCache | Out-Null
$pins = Get-Content -LiteralPath (Join-Path $experimentRoot 'dependencies.json') -Raw | ConvertFrom-Json

function Invoke-Git([string[]]$Arguments) {
    & git @Arguments
    if ($LASTEXITCODE -ne 0) { throw "Git failed with exit code $LASTEXITCODE" }
}
function Replace-Checked([string]$Path, [string]$Before, [string]$After) {
    $content = [IO.File]::ReadAllText($Path).Replace("`r`n", "`n")
    if ($content.Contains($After)) { return }
    if (-not $content.Contains($Before)) { throw "Pinned source adaptation no longer matches: $Path" }
    [IO.File]::WriteAllText($Path, $content.Replace($Before, $After), [Text.UTF8Encoding]::new($false))
}

foreach ($entry in $pins.PSObject.Properties) {
    $name = $entry.Name
    if ($Project -ne 'All' -and $Project -ne $name) { continue }
    $pin = $entry.Value
    $source = Join-Path $SourceCache $name
    if (-not (Test-Path -LiteralPath (Join-Path $source '.git'))) {
        if (Test-Path -LiteralPath $source) { throw "Source directory already exists without Git metadata: $source" }
        Invoke-Git @('clone','--filter=blob:none','--no-checkout',$pin.url,$source)
        Invoke-Git @('-C',$source,'checkout','--detach',$pin.commit)
    }
    $head = (& git -C $source rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or $head -ne $pin.commit) { throw "Expected $name at $($pin.commit), found $head" }
    if ($pin.submodules.Count) {
        Invoke-Git (@('-C',$source,'submodule','update','--init','--recursive') + @($pin.submodules))
    }
    if ($name -eq 'Capsaicin') {
        Replace-Checked (Join-Path $source 'src/scene_viewer/main_shared.h') 'defaultRenderer       = "GI-1.1"' 'defaultRenderer       = "GI-1.2"'
        Replace-Checked (Join-Path $source 'src/scene_viewer/main_shared.cpp') `
            'externalSceneLoaded = loadScene(externalScene);' `
            'externalSceneLoaded = loadScene(externalScene); if (!externalSceneLoaded) return false;'

        $fsr = Join-Path $source 'src/core/src/render_techniques/fsr/fsr.cpp'
        $renderSizeBefore = @'
        if (options_.fsr_version == static_cast<uint8_t>(RenderOptions::FSRVersion::FSR2))
        {
            // FSR2 requires render size to be doubled
            dispatchUpscale.renderSize.width  = renderDimensions.x * 2;
            dispatchUpscale.renderSize.height = renderDimensions.y * 2;
        }
        else
        {
            dispatchUpscale.renderSize.width  = renderDimensions.x;
            dispatchUpscale.renderSize.height = renderDimensions.y;
        }
'@
        $renderSizeAfter = @'
        // FidelityFX renderSize describes the actual input resource dimensions.
        dispatchUpscale.renderSize.width  = renderDimensions.x;
        dispatchUpscale.renderSize.height = renderDimensions.y;
'@
        Replace-Checked $fsr $renderSizeBefore $renderSizeAfter

        $nativeInputBefore = @'
    GfxTexture const &colourAOV = capsaicin.getSharedTexture("Color");
'@
        $nativeInputAfter = @'
    GfxTexture const &colourAOV = capsaicin.getSharedTexture("Color");
    GfxTexture inputAOV = colourAOV;
    if (!usesScaling)
    {
        // Keep native-scale input distinct from the output UAV; post passes use Color.
        if (!native_color_input_ || native_color_input_.getWidth() != renderDimensions.x
            || native_color_input_.getHeight() != renderDimensions.y
            || native_color_input_.getFormat() != colourAOV.getFormat())
        {
            gfxDestroyTexture(gfx_, native_color_input_);
            native_color_input_ = gfxCreateTexture2D(
                gfx_, renderDimensions.x, renderDimensions.y, colourAOV.getFormat());
            if (!native_color_input_)
            {
                GFX_PRINTLN("Error: Failed to allocate distinct native-scale FSR input");
                return;
            }
        }
        if (gfxCommandCopyTexture(gfx_, native_color_input_, colourAOV) != kGfxResult_NoError
            || gfxTextureSetResourceState(gfx_, native_color_input_, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
                != kGfxResult_NoError)
        {
            GFX_PRINTLN("Error: Failed to prepare distinct native-scale FSR input");
            return;
        }
        inputAOV = native_color_input_;
    }
'@
        Replace-Checked $fsr $nativeInputBefore $nativeInputAfter
        Replace-Checked $fsr `
            '        dispatchUpscale.color                      = ToFfxApiResource(capsaicin, colourAOV, !usesScaling);' `
            '        dispatchUpscale.color                      = ToFfxApiResource(capsaicin, inputAOV, false);'
        Replace-Checked (Join-Path $source 'src/core/src/render_techniques/fsr/fsr.h') `
            '    GfxTexture          exposure_;' `
            "    GfxTexture          exposure_;`n    GfxTexture          native_color_input_;"
        Replace-Checked $fsr `
            "    gfxDestroyTexture(gfx_, exposure_);`n    exposure_ = {};" `
            "    gfxDestroyTexture(gfx_, native_color_input_);`n    native_color_input_ = {};`n    gfxDestroyTexture(gfx_, exposure_);`n    exposure_ = {};"
    }
    if ($name -eq 'VoxelRT') {
        $voxelize = Join-Path $source 'src/VoxelRT/Voxelize.cpp'
        Replace-Checked $voxelize 'if (texCoords->type != fastgltf::AccessorType::Vec2)' 'if (texCoords && texCoords->type != fastgltf::AccessorType::Vec2)'
        Replace-Checked $voxelize 'glm::vec3(FLT_MAX), glm::vec3(FLT_MIN)' 'glm::vec3(FLT_MAX), glm::vec3(-FLT_MAX)'
        Replace-Checked (Join-Path $source 'src/VoxelRT/VoxelMap.h') "struct VoxelMap {`n" "struct VoxelMap {`n    glm::vec3 importedOrigin{}, importedScale{1}, importedCenter{};`n"
        Replace-Checked $voxelize 'center.y = 0;' "center.y = 0;`n    importedOrigin = bounds[0]; importedScale = scale; importedCenter = center;"
        Replace-Checked $voxelize `
            'static swr::StbImage LoadImage(const fastgltf::Asset& asset, const fastgltf::DataSource& data)' `
            'static swr::StbImage LoadImage(const fastgltf::Asset& asset, const fastgltf::DataSource& data, const std::filesystem::path& directory)'
        Replace-Checked $voxelize 'swr::StbImage::Load(uri->uri.fspath().string())' 'swr::StbImage::Load((directory / uri->uri.fspath()).string())'
        Replace-Checked $voxelize 'LoadImage(asset, asset.images[imageIndex].data)' 'LoadImage(asset, asset.images[imageIndex].data, std::filesystem::path(modelPath).parent_path())'
        Replace-Checked $voxelize 'if (textures.contains(imageIndex)) continue;' 'if (textures.contains(imageIndex)) { materialTextures.push_back(&textures.at(imageIndex)); continue; }'
        Replace-Checked (Join-Path $source 'src/VoxelRT/Render/RendererDistField.cpp') `
            'cmds.Clear(*StorageImage, { ~0u });' 'cmds.Clear(*StorageImage, { .uint32 = { ~0u, ~0u, ~0u, ~0u } });'
        # Unused CPU renderers must not load relative-path assets before main can
        # select the explicit SDK directory for this standalone diagnostic.
        $cpu = Join-Path $source 'src/VoxelRT/Render/RendererCPU.cpp'
        Replace-Checked $cpu `
            'static swr::HdrTexture2D _skyBox = swr::texutil::LoadCubemapFromPanoramaHDR("assets/skyboxes/evening_road_01_puresky_4k.hdr");' `
            'static swr::HdrTexture2D& CpuSkyBox() { static auto texture = swr::texutil::LoadCubemapFromPanoramaHDR("assets/skyboxes/evening_road_01_puresky_4k.hdr"); return texture; }'
        Replace-Checked $cpu 'static VBlueNoise _blueNoise = VBlueNoise();' `
            'static VBlueNoise& CpuBlueNoise() { static VBlueNoise noise; return noise; }'
        Replace-Checked $cpu '_skyBox.SampleCube' 'CpuSkyBox().SampleCube'
        Replace-Checked $cpu '_blueNoise.Sample' 'CpuBlueNoise().Sample'
        $images = Join-Path $source 'src/LibHavk/SwRast/ImageHelpers.cpp'
        Replace-Checked $images `
            'auto tex = RgbaTexture2D((uint32_t)width, (uint32_t)height, mipLevels, 1);' `
            'if (!pixels) throw std::runtime_error("Cannot load voxel image: " + std::string(path)); auto tex = RgbaTexture2D((uint32_t)width, (uint32_t)height, mipLevels, 1);'
        Replace-Checked $images `
            'auto tex = HdrTexture2D((uint32_t)width, (uint32_t)height, mipLevels, 1);' `
            'if (!pixels) throw std::runtime_error("Cannot load voxel HDR: " + std::string(path)); auto tex = HdrTexture2D((uint32_t)width, (uint32_t)height, mipLevels, 1);'
        # The SIMD sampler requires >=4x4 POT images; upsample tiny/NPOT glTF
        # images with nearest sampling so they retain their full UV extent.
        $smallBefore = @'
            swr::RgbaTexture2D tex(image.Width, image.Height, 4, 1);
            tex.SetPixels(image.Data.get(), image.Width, 0);
'@
        $smallAfter = @'
            const auto width = std::max(8u, std::bit_ceil(image.Width));
            const auto height = std::max(8u, std::bit_ceil(image.Height));
            std::vector<uint32_t> expanded(size_t(width) * height);
            for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
                const auto sourcePixel = size_t(y * image.Height / height) * image.Width + x * image.Width / width;
                std::memcpy(&expanded[size_t(y) * width + x], image.Data.get() + sourcePixel * 4, 4);
            }
            swr::RgbaTexture2D tex(width, height, 4, 1);
            tex.SetPixels(expanded.data(), width, 0);
'@
        Replace-Checked $voxelize $smallBefore $smallAfter
        Replace-Checked $voxelize 'swr::RgbaTexture2D emptyTex(4, 4, 1, 1);' 'swr::RgbaTexture2D emptyTex(8, 8, 1, 1);'
        Replace-Checked $voxelize 'PaletteBuilder palette;' 'PaletteBuilder palette; std::vector<uint32_t> white(64, 0xffffffffu); emptyTex.SetPixels(white.data(), 8, 0); palette.AddColor(0xffffffffu);'
        # Material index zero represents air in VoxelRT; no actual color may use it.
        Replace-Checked $voxelize 'Palette[i] = Material{' 'Palette[i + 1] = Material{'
        Replace-Checked $voxelize 'uint32_t paletteIdx = palette.FindIndex((uint32_t)colors[0]);' 'uint32_t paletteIdx = palette.FindIndex((uint32_t)colors[0]) + 1;'
        Replace-Checked (Join-Path $source 'src/VoxelRT/Render/RendererTree64.cpp') 'return layerNodes[0].second;' 'return layerNodes.empty() ? CompressedNode{} : layerNodes[0].second;'
    }
    [IO.File]::WriteAllText((Join-Path $source '.renderer-lab-prepared'), ($pin | ConvertTo-Json -Depth 3))
    Write-Output "$name $head prepared; upstream license retained at $source"
}
