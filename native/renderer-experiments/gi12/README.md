<!-- SPDX-License-Identifier: Apache-2.0 -->
# GI-1.2 standalone adapter

`RendererLabGI12App` calls the pinned Capsaicin SDK's original GI-1.2 renderer.
It uses a true offscreen DX12 gfx context, creates no window and consumes the
same RSLB ScenePacket as the other renderer experiments. It exports geometry,
PBR textures and posed instances through `RendererLabScene`, then adds glTF
punctual lights with the photometric conversion required by gfx. A captured
HDR/EXR dome is supplied through Capsaicin's environment API. It never selects
a demo scene, demo environment or alternate named renderer.

The parent ExternalProject must inject `cmake/Gi12Hook.cmake` through
`CMAKE_PROJECT_INCLUDE`, set `RENDERER_EXPERIMENT_ROOT` and `RENDERER_LAB_ROOT`,
and build **RendererLabGI12App**. The hook links the existing `capsaicin` and
`gfx` targets and retains the upstream MIT notice beside the executable.
Original SDK dependency and binary redistribution terms continue to apply.

Controller invocation, after building:

```powershell
& build/renderer-experiments/gi12/bin/Release/RendererLabGI12App.exe `
  --scene .codex/renderer-lab/captured-frame.rslb `
  --output .codex/renderer-lab/gi12-new-run `
  --sdk-source .codex/vendor-src/Capsaicin `
  --adapter 'RTX 4080 SUPER' --width 2560 --height 1440 `
  --warmup 600 --frames 180 --runs 3 --aa none --allow-approximations
```

The output directory must be new. Adapter selection must match exactly one
hardware GPU and is verified against the created device's LUID; no integrated
GPU or software fallback is permitted. Each run reinitializes Capsaicin,
freezes animation, applies the captured position/forward/up/FOV/near/far,
warms up independently and records native-resolution frames without VSync or
frame generation. Anti-aliasing defaults to `none`, and `--aa fsr2` fails before
graphics initialization. The pinned SDK's FSR2 path still produces spatially
incorrect fixture output after corrected input dimensions and distinct
input/output resources. Its cause remains unresolved; these changes do not
certify FSR2. Successful no-AA manifests record this known quality gap. Spatial
and temporal anti-aliasing parity remain unproven.

Each run emits raw GPU frame times and CPU render/submit times, percentiles,
scene counts, `final.exr`, `final.jpeg` and the SDK's camera dump. GPU samples
use raw D3D12 timestamp query pairs on gfx's command queue: gfx's convenience
duration API applies smoothing and is unsuitable for frame percentiles.
Captures and readback occur outside the measured GPU interval. Device/scene,
camera, triangle-count, timestamp or capture failures fail the experiment.

`--allow-approximations` is explicit because finite-radius lights, angular
sun size, unsupported analytical lights, missing HDR domes and material
extensions can differ from Remix. All exporter and adapter warnings are
recorded. The flag does not pass the quality gate.

The manifest is always `standalone-static-scene`, `endToEndMinecraft=false`,
`qualityGatePassed=false` and `speedupClaim=null`. It does not measure live
Minecraft world extraction, dynamic entities/world edits, a moving camera
path, GL/UI interop or Distant Horizons. Transparency/refraction, rough
reflections, PBR, dome/light semantics and stable motion need separate visual
validation. These timings cannot establish the requested 3-4x real Minecraft
FPS improvement.
