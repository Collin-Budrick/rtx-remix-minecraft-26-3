<!-- SPDX-License-Identifier: Apache-2.0 -->
# Renderer experiments

These adapters import existing renderer implementations and consume the shared
Minecraft ScenePacket. They are research tools, separate from Gradle/release
packaging and from the integrated Minecraft benchmark. A selectable backend or
successful build is not proof of complete rendering or a performance gain.

## Imports and builds

The preparation scripts verify exact revisions, initialize required submodules
and apply checked compatibility changes. Original renderer code and licenses
remain in ignored dependency checkouts. [dependencies.json](dependencies.json)
and [RTXPT preparation](../renderer-lab/scripts/Prepare-Rtxpt.ps1) own the pins:

| Import | Revision | License and role |
| --- | --- | --- |
| [Capsaicin](https://github.com/GPUOpen-LibrariesAndSDKs/Capsaicin) | `v1.3`, `914b91596cd119eda85fbc1d3c7ee6ac391b1452` | MIT; original GI-1.2 renderer |
| [RTXDI](https://github.com/NVIDIA-RTX/RTXDI) | `v3.2.0`, `fdbd9b36dc55827b67a03f926e78cfad9b1c1c57` | NVIDIA RTX SDK terms; original FullSample ReSTIR PT |
| [VoxelRT](https://github.com/dubiousconst282/VoxelRT) | `alt-renderers`, `e605812840a9f6abbadf84a436f8602e2972ee43` | MIT; original brick and Tree64 traversal |
| [RTXPT](https://github.com/NVIDIA-RTX/RTXPT) | `v.1.9.0`, `303b408ab81790a5051abd7573830b7a4112eb8e` | NVIDIA RTX SDK terms; advanced path tracer and Intro step 8 hybrid |

Dependency assets and binaries retain their own terms; the renderer's top-level
license does not replace them. See [RTXPT dependency notices](../renderer-lab/DEPENDENCIES.md).
Capsaicin/RTXDI/VoxelRT use `.codex/vendor-src/`; RTXPT uses
`native/renderer-lab/.deps/RTXPT/`. No demo worlds are required for a capture.

Use Windows x64, PowerShell 7, Git, VS 2022 C++ tools/Windows SDK and CMake 4.1+.
VoxelRT additionally needs LLVM `clang-cl`, Ninja and its Vulkan build/runtime
dependencies. RTXDI needs Windows SDK `fxc`; NVIDIA SDK feature/driver checks
still apply. Builds may fetch dependencies and compile substantial shader sets.

From the repository root, with the relevant test client closed:

```powershell
pwsh -NoProfile -File native/renderer-experiments/scripts/Build-Experiments.ps1 -Project Core
pwsh -NoProfile -File native/renderer-experiments/scripts/Build-Experiments.ps1 -Project All -Jobs 8
```

`Core` builds the instance-update and transactional scene-packet tests plus the common exporter. `All`
prepares/builds all imported adapters, including the RTXPT host, and runs the
native contract tests. Use `-Project Capsaicin`, `RTXDI`, `VoxelRT` or `RTXPT`
to limit a build. Preparation alone is
`native/renderer-experiments/scripts/Prepare-Experiments.ps1 -Project Capsaicin`; RTXPT has its separate
preparation script linked above. These commands do not launch Minecraft.

## Capture and replay

For automated Minecraft runs, use the explicitly adopted Modrinth
`RTX Rewrite Bench` duplicate in [Modrinth Renderer Lab](../../tools/docs/MODRINTH_RENDERER_LAB.md).
Preserve the source gameplay profile. Select `-Drtxremix.backend=scene-export`
in the duplicate's persistent Java options: Remix continues rendering while
the shared scene is retained. In a loaded world, `/rtx lab snapshot` writes
`<gameDir>/renderer-lab/scene.rslb`; `/rtx backend` reports actual backend state.
For capture without typing, set `-Drtxremix.sceneExportAfterFrames=600` and an
absolute `-Drtxremix.sceneExportPath=...` before launch. Wait for the successful
`RTX_SCENE_CAPTURE` log entry and inspect its completeness flag and counts.

The common exporter decodes retained updates and writes real geometry, posed
instances, textures, PBR materials and warning metadata. Use a frozen full
snapshot for these static replays. Unsupported interchange features fail unless
approximations are explicitly enabled; review `scene.manifest.json` afterward.

Example commands after a build, substituting a real capture and new outputs:

```powershell
& build/renderer-lab/Release/RendererLabExport.exe capture.rslb .codex/renderer-lab/export --allow-approximations
& build/renderer-experiments/gi12/bin/Release/RendererLabGI12App.exe `
  --scene capture.rslb --output .codex/renderer-lab/gi12-new `
  --sdk-source .codex/vendor-src/Capsaicin --adapter 'RTX 4080 SUPER' `
  --warmup 600 --frames 180 --runs 3 --aa none --allow-approximations
& build/renderer-experiments/restir-pt/bin/RendererLabReSTIRPT.exe `
  --packet capture.rslb --export-dir .codex/renderer-lab/restir-new `
  --warmup 600 --frames 180 --aa rr --allow-approximations
& build/renderer-experiments/voxel/RendererLabVoxel.exe `
  --packet capture.rslb --gltf .codex/renderer-lab/export/world.gltf `
  --sdk-source .codex/vendor-src/VoxelRT --output .codex/renderer-lab/voxel-new `
  --backend brick --width 2560 --height 1440 --warmup 600 --frames 180 --allow-approximations
```

Voxel also accepts `--backend tree64`. GI-1.2 uses a true offscreen DX12 context,
raw GPU timestamp pairs and EXR/JPEG captures; see [its runner guide](gi12/README.md).
Its current FSR2 path is explicitly rejected because fixture output is wrong;
no-AA diagnostics do not establish anti-aliasing or motion parity.
ReSTIR PT reuses the SDK profiler, native window and captured scene/camera.
Voxel emits tracing timestamps, a manifest and an albedo image, with a hidden
window by default. Verify the actual GPU, output/internal dimensions and capture
contents; a timing file alone cannot establish a valid visual workload.
See [the RTXPT guide](../renderer-lab/README.md) for path/hybrid replay commands
and the separate SDK binary/runtime layout.

## Current integration boundary

`remix` remains the default Minecraft backend. `scene-export` still presents
Remix. GI-1.2, ReSTIR PT and voxel are standalone adapters; Java mode names do
not supply an in-process implementation. Disabling fallback rejects those
unavailable modes instead of labelling Remix as the requested renderer.

The separately built `RendererLabHost` connects `native-hybrid` to Minecraft's
render thread. It renders the original SDK hybrid offscreen on the same DX12
adapter, shares color/depth/fences with GL, and keeps Minecraft's SDL window,
input, HUD, lens and weather. Its latest `RTX_BACKEND_STATUS` becomes active
only after real rendering and shared-target import succeed; presentation is
truthfully `minecraft-opengl`. Host DLL/runtime selection and shared-target
ownership are documented in [the host guide](../renderer-lab/README.md).

This host explicitly reports `qualityComparable=false`. Conf layers,
water/refraction/SSS, area-light/material semantics and sky calibration are
incomplete or unreviewed; non-camera scene changes currently reload the SDK
scene and can hitch. Voxel quantizes materials and uses upstream test lighting:
it has no PBR/transparency parity, animated-update integration or triangle
fallback. A mixed voxel/triangle renderer is future work.

All standalone outputs exclude live Minecraft transfer, animation, UI and
presentation costs. They cannot pass the end-to-end quality gate. The Modrinth
harness requires three real-frame runs, 600 warmup frames, frozen fixtures and
current packs, 1440p, distances 16/12, FG/VSync off and uncapped rendering.
Known false quality flags or unsupported features veto comparison even after
manual review. No adapter, host activation, build or diagnostic timing here
establishes comparable stable visuals or a 3-4x real Minecraft FPS improvement.
