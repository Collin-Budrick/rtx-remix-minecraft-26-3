# Imported sources and licenses

The renderer adapters compile NVIDIA's original renderer implementations in their
dependency checkout. They do not replace their lighting shaders with device probes
or placeholder draws.

| Dependency | Pin | License / acquisition |
| --- | --- | --- |
| NVIDIA RTXPT | tag `v.1.9.0`, commit `303b408ab81790a5051abd7573830b7a4112eb8e` | [NVIDIA RTX SDK license](licenses/RTXPT-LICENSE.txt); `scripts/Prepare-Rtxpt.ps1` |
| RTXPT support assets | parent gitlink `a56be4ed65029d2733fd02f00f4027ffd541eda8` | Original asset notices retained; sparse checkout of `fonts`, `StandaloneTextures` and root notices |
| Donut, NRD, RTXDI-Library, RTXTF-Library, SHaRC, OMM, NVAPI, cxxopts | Exact recursive submodule pins from the RTXPT commit | Original dependency licenses retained in their checkouts |
| NVIDIA Streamline and DXC | SDK CMake's original pinned downloads | Original NVIDIA/Microsoft notices and redistribution terms apply |
| nlohmann/json | `v3.12.0`, commit `55f93686c01528224f448c19128836e7df245f72` | [MIT](licenses/nlohmann-json-LICENSE.MIT); original single header imported in `third_party/nlohmann/json.hpp` |

The imported JSON header SHA256 is
`aaf127c04cb31c406e5b04a63f1ae89369fccde6d8fa7cdda1ed4f32dfc5de63`.
Our adapters and packet/export code use Apache-2.0; this does not relicense the
NVIDIA dependencies. Distributing SDK code or binaries requires following their
respective license terms and including their notices. This tree does not vendor
proprietary DLLs or redistribute sample worlds.

The prepare script accepts an existing absolute generated scene filename before
the sample-scene lookup, honors the argument vector passed to
`SampleBaseApp::Init`, and gates precompiled DXR1.2 opacity-micromap permutations
on the corresponding native/SM6.9 feature path. Baseline builds retain the
SDK's ordinary non-DXR1.2 permutations and validation.

The host generates separate build-tree copies of the SDK boundary code to
resolve assets and Streamline DLL/plugins through the explicit runtime directory
and suppress window/progress operations. Original license headers remain in all
generated files. Lighting/denoising algorithm source stays the pinned upstream
implementation. The dependency directory is ignored; a fresh checkout is
reproducible through the pin-checked preparation script and CMake hooks.
