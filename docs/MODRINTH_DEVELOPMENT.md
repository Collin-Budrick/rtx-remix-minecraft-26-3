<!-- SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved. -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# Developing With Modrinth

`dev.ps1` builds and deploys to an existing Modrinth profile on Windows x64.
It does not start a launcher or Minecraft. Run the commands below from the
repository root, then use **Play** in Modrinth to test the result.

## First Setup

Install a JDK 25 and the Visual Studio 2022 C++ build tools described in
[Getting Started](GETTING_STARTED.md#prerequisites). The helper uses `-JdkPath`
when supplied, then a JDK 25 in `JAVA_HOME`, then checks the usual Gradle
toolchain, mise, Eclipse Adoptium, Microsoft, and Java installation folders.
It requires JNI headers and does not install a JDK.

Create a separate Modrinth profile for **Minecraft 26.3**, **Fabric Loader
0.19.5 or newer**, and **Fabric API 0.165.0+26.3**. Select **Java 25** in the
profile's Java settings. The default profile directory is
`%APPDATA%\ModrinthApp\profiles\Fabric 26.3`; pass `-GameDir` if Modrinth uses
another name or data directory. The directory and its `mods` folder must
already exist.

Build the full runtime-bundled package before deploying a lightweight JAR:

```powershell
.\dev.ps1 -Mode Package

# If the JDK is installed elsewhere:
.\dev.ps1 -Mode Package -JdkPath 'C:\path\to\jdk-25'
```

The first build downloads the pinned Remix SDK and runtime textures through
Packman and compiles the JNI bridge. It needs network access and leaves the
verified runtime-bundled JAR in `build/distributions/`.

With Minecraft closed, copy that packaged JAR into the profile's `mods`
directory, alongside Fabric API. Keep only one RTX Remix JAR in the profile.
Launch this profile once through Modrinth so the packaged JAR extracts its
native runtime, textures, and configuration, then close Minecraft. This step
must precede deployment of a lightweight development JAR. Use the profile's
own `mods` folder; no installer targeting the ordinary `.minecraft` directory
is needed.

Back up an existing profile before this initial package launch: a full native
extraction replaces mod-owned SDK files, `rtx.conf`, and shipped runtime
textures. Existing authored files under `rtx-remix/` are preserved. A separate
test profile avoids changing an established play setup.

## Edit, Deploy, Play

Close Minecraft, run the helper, then click **Play** in Modrinth:

```powershell
# Java and resource edits: build and deploy the lightweight JAR
.\dev.ps1

# JNI edits: also compile and deploy remix-sdk/remixbridge.dll
.\dev.ps1 -Native

# Select another existing Modrinth profile
.\dev.ps1 -GameDir 'C:\path\to\Modrinth\profiles\RTX Test'
```

Deploy runs `jar -x setupRuntime -Parch=win64`, leaving the already installed
native payload in place. It first compares the source and profile `d3d9.dll`
SHA-256 hashes and stops if the runtime is absent or differs. The source SDK
must match the checkout's pin. When the manifest changes, refresh the staged
SDK with the repository's supported command before rebuilding a package:

```powershell
# Requires uv 0.13 or newer on PATH; this refreshes the win64 SDK pin.
.\tools\rtx.bat sdk packman --arch win64
.\dev.ps1 -Mode Package
```

Install and launch that new packaged JAR once to update the profile's runtime,
then resume lightweight deploys. A failed runtime hash check is a reason to
complete this bootstrap, not to bypass the check.

The helper identifies RTX JARs by the `fabric.mod.json` mod ID and replaces
them with `rtx-remix-renderer-dev.jar`. It checks for Java processes using the
target directory before building and again before changing profile files.
Replaced JARs and any replaced JNI bridge are copied to
`.codex/backups/deploy/<timestamp>/`, checked by SHA-256, and recorded in a
manifest. Deployment restores those backups if a copy or replacement fails.

Deployment leaves other mods, worlds, options, configuration, runtime textures,
and extraction markers alone. `-Native` replaces only the JNI bridge in
addition to the JAR. Logs and elapsed times are under `.codex/testing/`.
Generated backups, logs, Python installations, and uv caches remain in ignored
`.codex/`; the helper restores the previous shell environment and directory.

## Checks And Packages

These modes do not require a local Modrinth profile:

```powershell
.\dev.ps1 -Mode Test
.\dev.ps1 -Mode Test -TestFilter '*TerrainStateTest'
.\dev.ps1 -Mode Package
.\dev.ps1 -Mode Tools
```

`Test` runs the JVM suite. `Package` runs the full build, JVM tests, packaging,
and package-content verification. Neither deploys to the profile. `Tools`
runs the locked Python tooling checks and tests; it requires uv 0.13 or newer
on `PATH`, or a suitable local copy at `.codex/tools/uv/uv.exe`. The helper
does not install uv. The existing tooling may download its required Python
and dependencies on first use.

Gradle modes stop if a client is using this checkout's `run` directory.
Deployment also stops if the selected Modrinth profile is running.

Builds and JVM tests do not validate JNI initialization or rendering. Verify
those by launching the Modrinth profile, joining a test world, and checking
`/rtx status` for initialized renderer, available SDK, loaded chunks, and
retained pages. The runtime success log is
`RTX Remix renderer initialized successfully!`; the differently capitalized
mod-initialization message is not runtime proof. Capture a screenshot for
visual rendering changes.

Publish the runtime-bundled JAR from `build/distributions/` only after package
verification and the relevant in-game checks. The lightweight development JAR
depends on the prepared profile and is not a standalone release. Keep local
profiles, runtimes, credentials, logs, and backup files out of Git commits.
