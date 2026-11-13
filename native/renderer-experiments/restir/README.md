# ReSTIR PT Enhanced scene replay

`RendererLabReSTIRPT` compiles the pinned RTXDI 3.2 FullSample's existing renderer
and shaders against the same RSLB snapshot and glTF export as the other native
experiments. It selects ReSTIR PT with temporal/spatial resampling and footprint
reconnection. The original SDK GPU profiler must observe initial, temporal and
spatial PT passes before a replay result is accepted. Frame generation is absent.

The CMake hook generates a minimally adapted `SceneRenderer.cpp` in the build
directory, preserving its original NVIDIA license header. The input implementation
SHA256 is checked before applying scene/camera/dome/sun and measurement boundary
changes. The SDK source stays in the ignored, commit-pinned dependency checkout;
no SDK implementation is copied into this tracked tree. Lighting shaders,
reservoir resampling and denoisers remain the upstream implementations.

The controller's external project must pass:

```text
-DCMAKE_PROJECT_INCLUDE=<absolute>/native/renderer-experiments/cmake/RestirHook.cmake
-DRENDERER_EXPERIMENT_ROOT=<absolute>/native/renderer-experiments
-DRENDERER_LAB_ROOT=<absolute>/native/renderer-lab
```

Build SDK target `RendererLabReSTIRPT`. Its executable is under the SDK build's
`bin` directory with its shaders and original NRD/DLSS runtime dependencies.

```powershell
RendererLabReSTIRPT.exe --packet capture.rslb --export-dir exports/restir --warmup 120 --frames 120 --aa rr --allow-approximations
```

`--aa` accepts `rr`, `nrd`, `taa`, `none`. Requested RR fails explicitly if
unsupported. SDK quality and ReSTIR sampling arguments pass through, but the
adapter owns scene, camera, resolution, indirect mode and frame schedule. The
snapshot fixes the camera and disables animation; exposure advances at a fixed
1/60 step. Internal and output resolution are identical. The SDK runs in its own
DX12 window and uses its real screenshot readback to write `frame.png`.

`gpu-frames.csv` contains SDK NVRHI timer-query values after warm-up, with the
two-bank query history drained before exit. `replay-result.json` identifies the
actual selected mode, observed PT GPU work and scope. It excludes live Minecraft
scene transfer, input, HUD and presentation integration, and therefore cannot
prove an end-to-end Minecraft FPS improvement. Review `scene.manifest.json` for
all explicitly accepted material/light approximations before comparing visuals.

One captured dome is currently supported. Multiple domes fail rather than
silently dropping lighting. Source materials, emissive meshes, captured analytic
lights and their supported geometry come from the shared exporter; complex
Remix material semantics remain explicit interchange limits.
