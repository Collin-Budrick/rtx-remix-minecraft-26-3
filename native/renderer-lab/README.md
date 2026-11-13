# Native Minecraft renderer lab

Experimental Windows DX12 applications render a captured Minecraft scene using
the pinned NVIDIA RTXPT implementation. `RendererLabPath` uses the SDK's advanced
path tracer. `RendererLabHybrid` uses the SDK Intro sample's raster G-buffer,
ReSTIR direct lighting, ray traced GI/reflections and SHaRC (step 8). Both retain
the SDK's NRD/Streamline Ray Reconstruction integrations and disable frame
generation. Requested DLSS modes fail explicitly when unavailable.

These two applications render snapshots in separate native windows. RSLB packet decoding
retains delta upserts/removals, but these applications load the captured state
once. Neither application claims a measured performance improvement. Full scene
transfer, acceleration-structure cost, animation, HUD, live updates and visual
parity must be measured before using it to select a replacement renderer.

`RendererLabHost.dll` adds an experimental in-process headless integration for
Minecraft's render thread. It creates the same-adapter DX12 device and runs the
original Intro step 8 renderer into private offscreen targets. Only completed
frames are copied into shared RGBA8 and projected R32F targets, returned with
real Win32 resource/fence handles through [the versioned C ABI](host/RendererLabHost.h).
Minecraft keeps its SDL/OpenGL window, input, composite, HUD, lens and weather.
The host never changes process CWD or starts a GLFW/progress window. Runtime
assets and shaders resolve from explicit VFS/absolute paths.

The consumer waits for `dxReady`, composites both top-left-oriented images, then
signals `glDone`. A producer that sees an unconsumed image returns `PENDING`
without enqueueing a wait that could deadlock failed-import teardown. Resizes
increment the allocation generation; a new allocation has ready value zero
until a complete image is published. Release imported GL objects before resize
or destroy. Apply acknowledges CPU-retained scene packets independently of GPU
readiness. Camera-only packets update the pose directly; other deltas currently
coalesce into real SDK scene reloads. This correctness prototype can stall while
reloading dynamic scenes and is not a performance result or a visual parity claim.

The JNI integration pins the host DLL for the JVM's lifetime. `Destroy` still
releases renderer, device, textures and fence resources; it does not unload the
module. NVIDIA's linked Tracy profiler owns process-level worker threads whose
destruction during `FreeLibrary` would wait under the Windows loader lock.
Restart Minecraft/the JVM before replacing native host code. Java-level host
sessions may be destroyed and recreated in the same JVM, including after resize.
Numbered native stage traces require the debug-validation flag; genuine SDK and
host error messages remain enabled during normal runs.

The headless path advances Streamline's common frame retirement only after the
SDK's GPU completion fence. Resize, scene replacement and shutdown release both
RR and SR viewports and clear their tags before their input textures disappear.
A failed Streamline teardown blocks another host session with a JVM-restart
error instead of attempting to reuse partly destroyed SDK state.

## Build (controller owned)

Run from an x64 MSVC developer PowerShell with CMake 4.1+ and Git available:

```powershell
pwsh -NoProfile -File native/renderer-lab/scripts/Prepare-Rtxpt.ps1
cmake -S native/renderer-lab -B native/renderer-lab/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DRENDERER_LAB_BUILD_RTXPT=ON
cmake --build native/renderer-lab/build --target RendererLabExport RendererLabRTXPT
```

The SDK remains a separate CMake project because it resolves dependencies relative
to `CMAKE_SOURCE_DIR`. A deferred CMake hook adds the two adapters to its existing
shader, runtime and renderer targets. Executables and SDK shader/runtime files
are placed under `.deps/RTXPT/bin` according to the SDK's configuration layout.
The default disables the experimental Agility SDK/DXR 1.2 requirement and uses
the SDK's standard DX12 path, suitable for an RTX 4080 SUPER. SDK feature checks
still determine real device/driver compatibility. See [dependency pins and
licenses](DEPENDENCIES.md).
The controller can also build SDK target `RendererLabHost` in the separate SDK
build tree. The host compiles a separate SDK core with checked, generated asset
lookup and no-window adaptations; no vendored implementation is copied into the
tracked source tree.

For just the common decoder/exporter, omit the prepare script and configure with
`-DRENDERER_LAB_BUILD_RTXPT=OFF`. Other prototypes can link `RendererLabScene`
and include `renderer_lab/ScenePacket.h` and `renderer_lab/GltfExport.h`.

The explicit GPU diagnostic exercises the real Java/JNI host and production GL
imports on a deterministic scene, including color/depth readback, two-fence
handshakes, resize, host recreation and writable-buffer validation:

```powershell
.\gradlew.bat rendererLabInteropSmoke -Parch=win64
```

Build the host first. `-PrendererLabRuntime=C:\absolute\SDK\bin` overrides its
runtime directory. The hidden context must select the NVIDIA GPU; the host
rejects cross-GPU and non-NVIDIA adapter requests. This opt-in diagnostic does
not launch Minecraft or edit Modrinth profiles. Images and a proof manifest
are written to a new `build/renderer-lab/interop-<timestamp>/` directory.
Passing it does not establish in-game quality or FPS.
`-PrendererLabAa=3` requests Ray Reconstruction without fallback; modes `0`,
`1`, and `2` select none, TAA, and DLSS-SR. Internal resolution stays native.

## Render a real capture

```powershell
RendererLabExport.exe capture.rslb exports/world --allow-approximations
RendererLabPath.exe --packet capture.rslb --export-dir exports/path --aa rr --frames 600 --nonInteractive --perfLog exports/path/sdk-gpu.csv
RendererLabHybrid.exe --packet capture.rslb --export-dir exports/hybrid --aa rr --frames 600 --nonInteractive
```

The applications also accept `--allow-approximations` when the snapshot contains
features glTF cannot represent. Without it, unsupported features stop the export.
Never use a warning-bearing capture as a visual parity result without reviewing
`scene.manifest.json`. `--aa` accepts `none`, `taa`, `sr`, `rr`; SDK arguments such
as `--overrideDLSSMode 3`, `--noSER`, debug validation and capture options pass
through. Scene, realtime mode and AA are owned by the adapter.

The exporter writes actual indexed triangles, transforms, CPU-posed skinning,
PNG/HDR textures, PBR materials, tint material variants, cutout alpha tests and a
Donut `.scene.json` wrapper. Camera position/direction/up and FOV come from the
packet. The captured sun, spherical lights and texture dome are imported;
unsupported area-light shapes and extended shaping are reported. Source material
and light metadata are preserved in the warning manifest.

Known interchange limits include nonuniform vertex tint within a triangle,
texture-factor/complex blend extensions, Remix water/refraction and subsurface
models, height/tangent maps, some file-backed roughness/metallic packing, dome
blending, and GPU transform extensions. Approximation is always explicit.
The glTF's HDR/DDS URI support is intended for Donut-based SDK loaders; generic
glTF tools may require converting those images first.

`renderer-started.json` records the actual selected AA mode and FG-off state after
loading. `render-run.json` records render-call count and wall time, not GPU time.
Advanced path GPU timings come from the SDK's real `--perfLog` queries; the hybrid
uses its existing GPU profiler. Warm-up, stable view, equal output/internal
resolution and quality, GPU/CPU profiling, captured image comparison and live
transfer costs remain required for a 3–4x real rendered FPS claim.
