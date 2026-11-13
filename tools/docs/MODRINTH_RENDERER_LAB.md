<!-- SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Modrinth Renderer Lab

This workflow uses an isolated Modrinth profile. The existing gameplay profile,
its running process, worlds, packs and settings remain outside the tools target.
Prism remains the upstream default when no launcher override is configured.

## Prepare The Duplicate

In Modrinth App, duplicate the existing Fabric 26.3 profile and name the duplicate
**RTX Rewrite Bench**. Use the app's duplication/export-import UI; do not copy
database records. Wait for installation to finish and close only the duplicate's
game. Select Java 25 in the duplicate's settings. The tools do not install Java,
modify accounts, clone app records, or launch the source profile.

List local instance identities without reading account or token tables:

```powershell
.\tools\rtx.bat modrinth profiles
```

Create the ignored `tools/config.local.json`, substituting the two identities
reported above. The target and source must be distinct existing local instances.
The target's display name must be `RTX Rewrite Bench`. The app database's
`instances.path` selects its direct child directory under `profiles/`; that
directory may use a UUID rather than the display name. Local instance UUIDs,
legacy folder names and display names are separate identifiers.

```json
{
  "launch": { "launcher": "modrinth" },
  "modrinth": {
    "profileId": "local:<duplicate-uuid>",
    "sourceProfileId": "local:<source-uuid>"
  },
  "rendererLab": {
    "enabled": true,
    "backend": "remix",
    "fixtureLock": ".codex/renderer-lab/fixture-lock.json",
    "visualReview": "pending",
    "runtimeQualityReview": "pending"
  },
  "render": { "preset": "Custom" }
}
```

Optional `modrinth.dataDir` and `modrinth.executable` override the existing app
data and executable locations. Defaults are `%APPDATA%\ModrinthApp` and
`%LOCALAPPDATA%\Modrinth App\Modrinth App.exe`. Modrinth App 0.23.3's instance
metadata schema is read using SQLite `mode=ro` and `query_only`; schema drift
fails preflight instead of writing an alternative format. No database writes
or credential queries are used.

Explicitly adopt the duplicate for this checkout:

```powershell
.\tools\rtx.bat modrinth adopt
.\tools\rtx.bat modrinth jvm-args
```

Adoption writes only `.rtx-tools-owner.json` in the validated duplicate. Another
checkout cannot silently adopt it. Writable mod/config/runtime payloads must
be independent files: symlinks, junctions and hardlinks there are rejected so
staging cannot alter the source through shared files. If the app shares those
files, replace them with independent copies inside the duplicate first.
Copy the printed arguments into the
duplicate's Java argument settings in Modrinth, with quoting around paths that
contain spaces. Keep its normal recommended heap/tuning arguments. These
persistent arguments include the JFR destination, Java phase tracing and the
requested backend with fallback disabled. Re-run `jvm-args` after changing a
backend or preset.

The supported external command is
`modrinth://launch/instance/local:<uuid>?singleplayer_world=<encoded-name>`.
See the official [handler](https://github.com/modrinth/code/blob/main/packages/app-lib/src/api/handler.rs)
and [shortcut generation](https://github.com/modrinth/code/blob/main/apps/app/src/api/shortcuts/mod.rs).
It carries neither JVM arguments nor a replacement process environment. An
already-running singleton cannot inherit per-request settings. The adapter
checks the newly discovered JVM's required arguments and stops only that new
owned game if they are absent. It never terminates the Modrinth app.

Use the app UI to remove conflicting custom environment overrides such as
`DXVK_RTX_CONFIG_FILE` or `RTX_ENABLE_ASYNC_COMPILATION` from the duplicate/app
before benchmarking. A benchmark temporarily appends blocking-compilation and
frame-generation-off settings to the duplicate's `rtx.conf`, preserving the
original bytes in `benchmark/startup-rtx.conf.backup` and restoring them after
the run. An interrupted-run backup blocks subsequent measurement until it has
been reviewed/restored. Existing `user.conf` opinions masking those settings
are rejected. The original gameplay profile is never changed.

## Freeze And Measure

Choose the world/version and camera using the normal
[fixture workflow](BENCHMARK_FIXTURES.md). Import a copy rather than modifying
a live save. Enable the intended current resource packs in the duplicate.
Then freeze the fixture content, camera content and ordered resource-pack hashes:

```powershell
.\tools\rtx.bat renderer-lab capabilities
.\tools\rtx.bat renderer-lab freeze
.\tools\rtx.bat run --runs 3 --no-build --arch win64 --dry-run
.\tools\rtx.bat run --runs 3 --no-build --arch win64
```

`--no-build` requires an already built matching development JAR/JNI/runtime.
Omit it to build. The selected launcher stages only its adopted duplicate.
Each run copies the frozen world to `saves/rtx-benchmark`, uses the existing
benchmark file trigger and deterministic camera, and resets that staged copy.
Current resource packs are retained in their order, without a Packman art-pack
download or replacement. Changed world/camera/pack hashes reject the run.
`renderer-lab freeze --replace` starts a new baseline series deliberately.

Renderer-lab mode pins three runs, 600 warmup frames, 2560x1440 windowed output,
render distance 16, simulation distance 12, VSync off and Minecraft's uncapped
260 option. Frame generation is disabled through benchmark startup settings;
generated frames never count toward the target. Keep foreground focus and
other GPU workloads consistent. The existing benchmark display validation,
degenerate-frame rejection, screenshots, frame percentiles, JFR and optional
Tracy capture still apply.

## Evidence And Backend Boundaries

Accepted selector names are `remix`, `scene-export`, `native-hybrid`,
`restir-pt`, `voxel` and `gi12`. A selectable name is not proof of a renderer.
`scene-export` still presents Remix; standalone native SDK timings measure a
different workload and cannot establish Minecraft FPS improvements.

Each measured run must log `RTX_BACKEND_STATUS` JSON with `requested`, `active`,
`presentation`, `nativeInProcess` and `fallback`. Requested/active must agree
with no fallback. A replacement additionally needs native in-process rendering
and a supported presentation path. `minecraft-opengl` means DX12 renders
offscreen and Minecraft's SDL/OpenGL window presents the shared image and HUD;
it does not mean a second DX12 window presents Minecraft.

Replacement evidence must explicitly report `qualityComparable=true`,
`unsupportedFeatures=[]` and `frameGeneration=false`. An explicit false quality
flag or a nonempty unsupported-feature list vetoes comparison even after human
visual/runtime reviews. The current in-process prototype can demonstrate native
rendering while still failing this quality contract. Unsupported backends,
Remix fallback and standalone/export-only results fail the full-frame gate.
Raw SDK experiments must be reported with their own standalone scope.

The legacy `MinecraftClient.render` timing label brackets Minecraft 26.3's
`Minecraft.runTick` HEAD through RETURN. It includes ticks/tasks, rendering,
GPU command submission and the SDL/Remix presentation call when the surface
is acquired, including any blocking there. It measures CPU wall time rather
than GPU duration or monitor scanout. Keep the measured client visible; a
native window with no new rendered frame fails the per-sample fence gate.

Each replacement run also needs a complete `metadata.nativeFrameEvidence`
window from the benchmark result, sourced from D3D12 new ready-fence publications.
The same native renderer session must produce exactly one new ready image for
each recorded `MinecraftClient.render` sample. Start/end counters must reconcile
with their deltas, with no pending, unready, duplicate or failed attempts inside
that window. Warmup counters are outside this gate. The full-frame timing array
must contain the requested number of positive finite samples. Native presentation
without this window, including repeated display of an old shared image, cannot
establish real rendered FPS.

The result manifest records fixture hashes, requested quality and observed
backend status plus each run's native frame window and timing count/bounds,
leaving `speedupClaim` empty. Comparison rechecks that evidence; manual reviews
cannot override a missing or ineligible frame window. It does not automatically certify
visual quality, runtime GPU selection, frame-generation state, motion stability
or the 3-4x target. Review screenshots and motion sequences for equivalent PBR,
rough reflections, transparency, entities, sky/water and temporal stability.
Record reviews on the exact measured run after inspecting it:

```powershell
.\tools\rtx.bat renderer-lab review <RUN_ID> --visual-accepted --runtime-verified --note "Compared captures and motion; verified GPU, FG, VSync and distances"
```

The switches are human attestations, not automatic verifiers. Check actual
frame generation, VSync, distances and GPU selection separately from the visual
review. Requested settings alone cannot certify those runtime facts. `compare` refuses
renderer-lab comparisons without matching
fixtures/quality, three successful backend-proven runs and accepted visual
and runtime-quality reviews. A build, capability description or offline render
is never FPS proof.

Cleanup uses only retained game/capture process handles from the current
controller invocation. Other Java games, IDEs and Modrinth remain running.
For reliable timings the GPU must still be otherwise idle.
