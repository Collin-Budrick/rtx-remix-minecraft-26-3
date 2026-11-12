# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

[CmdletBinding()]
param(
    [ValidateSet('Deploy', 'Test', 'Package', 'Tools')]
    [string]$Mode = 'Deploy',
    [string]$GameDir,
    [string]$JdkPath,
    [string]$TestFilter,
    [switch]$Native
)

$ErrorActionPreference = 'Stop'
$repoRoot = $PSScriptRoot
$localDir = Join-Path $repoRoot '.codex'
$modId = 'rtx-remix-renderer'
$timer = [Diagnostics.Stopwatch]::StartNew()

if ($Native -and $Mode -ne 'Deploy') { throw '-Native applies only to Deploy.' }
if ($TestFilter -and $Mode -ne 'Test') { throw '-TestFilter applies only to Test.' }
if (-not $GameDir -and $env:APPDATA) { $GameDir = Join-Path $env:APPDATA 'ModrinthApp\profiles\Fabric 26.3' }
if ($Mode -eq 'Deploy') {
    if (-not $GameDir -or -not (Test-Path -LiteralPath $GameDir -PathType Container)) {
        throw 'Set -GameDir to an existing Modrinth profile. See docs/MODRINTH_DEVELOPMENT.md for initial setup.'
    }
    $GameDir = (Resolve-Path -LiteralPath $GameDir).ProviderPath.TrimEnd('\', '/')
    $modsDir = Join-Path $GameDir 'mods'
    if (-not (Test-Path -LiteralPath $modsDir -PathType Container)) { throw "Existing mods directory required: $modsDir" }
}

Add-Type -AssemblyName System.IO.Compression.FileSystem

function Test-Jdk25([string]$Path) {
    if (-not $Path -or -not (Test-Path -LiteralPath (Join-Path $Path 'bin\java.exe')) -or
            -not (Test-Path -LiteralPath (Join-Path $Path 'include\jni.h'))) { return $false }
    $release = Join-Path $Path 'release'
    return (Test-Path -LiteralPath $release) -and
        [bool](Select-String -LiteralPath $release -Pattern '^JAVA_VERSION="25(?:[.\-+"]|$)' -Quiet)
}

function Find-Jdk25([string]$RequestedPath) {
    if ($RequestedPath) {
        if (-not (Test-Jdk25 $RequestedPath)) { throw "-JdkPath must name a JDK 25 containing JNI headers: $RequestedPath" }
        return (Resolve-Path -LiteralPath $RequestedPath).ProviderPath
    }
    if (Test-Jdk25 $env:JAVA_HOME) { return (Resolve-Path -LiteralPath $env:JAVA_HOME).ProviderPath }

    $roots = @()
    $gradleHome = $env:GRADLE_USER_HOME
    if (-not $gradleHome -and $env:USERPROFILE) { $gradleHome = Join-Path $env:USERPROFILE '.gradle' }
    if ($gradleHome) { $roots += Join-Path $gradleHome 'jdks' }
    if ($env:MISE_DATA_DIR) { $roots += Join-Path $env:MISE_DATA_DIR 'installs\java' }
    if ($env:LOCALAPPDATA) { $roots += Join-Path $env:LOCALAPPDATA 'mise\installs\java' }
    if ($env:ProgramFiles) {
        foreach ($vendor in @('Eclipse Adoptium', 'Microsoft', 'Java')) { $roots += Join-Path $env:ProgramFiles $vendor }
    }
    foreach ($root in ($roots | Select-Object -Unique)) {
        if (-not (Test-Path -LiteralPath $root -PathType Container)) { continue }
        foreach ($candidate in (Get-ChildItem -LiteralPath $root -Directory)) {
            if (Test-Jdk25 $candidate.FullName) { return $candidate.FullName }
            foreach ($nested in (Get-ChildItem -LiteralPath $candidate.FullName -Directory)) {
                if (Test-Jdk25 $nested.FullName) { return $nested.FullName }
            }
        }
    }
    throw 'JDK 25 was not found. Install a matching x64 JDK and pass -JdkPath or set JAVA_HOME.'
}

function Find-Uv {
    $candidates = @()
    $existing = Get-Command uv -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($existing) { $candidates += $existing.Source }
    $candidates += Join-Path $localDir 'tools\uv\uv.exe'
    foreach ($candidate in ($candidates | Select-Object -Unique)) {
        if (-not (Test-Path -LiteralPath $candidate -PathType Leaf)) { continue }
        $versionText = & $candidate --version 2>$null
        if ($LASTEXITCODE -eq 0 -and $versionText -match '^uv (\d+\.\d+\.\d+)' -and
                [version]$Matches[1] -ge [version]'0.13.0') { return $candidate }
    }
    throw 'Tools mode requires uv 0.13 or newer on PATH, or at .codex/tools/uv/uv.exe. No tools were installed.'
}

function Assert-GameStopped([string]$Directory = $GameDir) {
    if (-not $Directory -or -not (Test-Path -LiteralPath $Directory -PathType Container)) { return }
    $Directory = (Resolve-Path -LiteralPath $Directory).ProviderPath.TrimEnd('\', '/')
    $desktopSession = (Get-Process -Id $PID).SessionId
    $processes = Get-CimInstance Win32_Process -Filter "Name = 'java.exe' OR Name = 'javaw.exe'"
    foreach ($process in $processes) {
        if (-not $process.CommandLine) {
            if ($process.SessionId -eq $desktopSession) {
                throw "Cannot inspect Java process $($process.ProcessId). Close Minecraft before continuing."
            }
            continue
        }
        $command = $process.CommandLine.Replace('/', '\')
        if ($command.IndexOf($Directory.Replace('/', '\'), [StringComparison]::OrdinalIgnoreCase) -ge 0) {
            throw "A Java client is using $Directory (PID $($process.ProcessId)). Close it before continuing."
        }
    }
}

function Assert-MatchingRuntime {
    $source = Join-Path $repoRoot 'remix-runtime\win64\d3d9.dll'
    $target = Join-Path $GameDir 'remix-sdk\d3d9.dll'
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw 'Source runtime missing. Run .\dev.ps1 -Mode Package first. See docs/MODRINTH_DEVELOPMENT.md.'
    }
    if (-not (Test-Path -LiteralPath $target -PathType Leaf) -or
            (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne
            (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash) {
        throw 'The profile runtime is missing or differs from the source SDK. Install and launch a matching runtime-bundled package before Deploy. See docs/MODRINTH_DEVELOPMENT.md.'
    }
}

function Get-FabricModId([string]$JarPath) {
    $archive = [IO.Compression.ZipFile]::OpenRead($JarPath)
    try {
        $entry = $archive.GetEntry('fabric.mod.json')
        if ($null -eq $entry) { return $null }
        $reader = [IO.StreamReader]::new($entry.Open())
        try { return ($reader.ReadToEnd() | ConvertFrom-Json).id }
        finally { $reader.Dispose() }
    }
    finally { $archive.Dispose() }
}

function Copy-Verified([string]$Source, [string]$Destination) {
    $expected = (Get-FileHash -LiteralPath $Source -Algorithm SHA256).Hash
    Copy-Item -LiteralPath $Source -Destination $Destination -Force
    $actual = (Get-FileHash -LiteralPath $Destination -Algorithm SHA256).Hash
    if ($actual -ne $expected) { throw "SHA-256 verification failed: $Destination" }
    return $expected
}

function Invoke-Logged([string]$Executable, [string[]]$CommandArguments) {
    Add-Content -LiteralPath $logPath -Value ("Command: $Executable " + ($CommandArguments -join ' '))
    $previousErrorAction = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $Executable @CommandArguments 2>&1 | Tee-Object -FilePath $logPath -Append
        $commandExitCode = $LASTEXITCODE
    }
    finally { $ErrorActionPreference = $previousErrorAction }
    if ($commandExitCode -ne 0) { throw "$Executable failed with exit code $commandExitCode. See $logPath" }
}

$previousEnvironment = @{}
foreach ($name in @('JAVA_HOME', 'PATH', 'UV_PYTHON_INSTALL_DIR', 'UV_CACHE_DIR')) {
    $previousEnvironment[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
}
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$logDir = Join-Path $localDir 'testing'
New-Item -ItemType Directory -Path $logDir -Force | Out-Null
$logPath = Join-Path $logDir "dev-$stamp-$Mode.log"
$operationTarget = if ($Mode -eq 'Deploy') { $GameDir } else { $repoRoot }
Set-Content -LiteralPath $logPath -Value "Mode: $Mode`nTarget: $operationTarget"

Push-Location -LiteralPath $repoRoot
try {
    if ($Mode -eq 'Tools') {
        $uvExecutable = Find-Uv
        $env:PATH = "$(Split-Path -Parent $uvExecutable);$env:PATH"
    }
    else {
        $resolvedJdk = Find-Jdk25 $JdkPath
        $env:JAVA_HOME = $resolvedJdk
        $env:PATH = "$resolvedJdk\bin;$env:PATH"
        Assert-GameStopped (Join-Path $repoRoot 'run')
        if ($Mode -eq 'Deploy') { Assert-GameStopped }
    }
    $env:UV_PYTHON_INSTALL_DIR = Join-Path $localDir 'python'
    $env:UV_CACHE_DIR = Join-Path $localDir 'uv-cache'

    switch ($Mode) {
        'Test' {
            $gradleArgs = @('test', '-Parch=win64', '--console=plain')
            if ($TestFilter) { $gradleArgs += @('--tests', $TestFilter) }
            Invoke-Logged '.\gradlew.bat' $gradleArgs
        }
        'Package' {
            Invoke-Logged '.\gradlew.bat' @('build', 'package', '-Parch=win64', '--console=plain')
        }
        'Tools' {
            Invoke-Logged '.\tools\rtx.bat' @('check', '--tests')
        }
        'Deploy' {
            Assert-MatchingRuntime
            $gradleArgs = @('jar', '-x', 'setupRuntime', '-Parch=win64', '--console=plain')
            if ($Native) { $gradleArgs = @('compileNativeWin64') + $gradleArgs }
            Invoke-Logged '.\gradlew.bat' $gradleArgs

            $jar = Get-ChildItem -LiteralPath (Join-Path $repoRoot 'build\libs') -File |
                Where-Object { $_.Name -like 'rtx-remix-renderer*.jar' -and $_.Name -notlike '*-sources.jar' } |
                Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
            if ($null -eq $jar -or (Get-FabricModId $jar.FullName) -ne $modId) {
                throw 'No built RTX Remix mod JAR was found.'
            }

            $oldJars = @(Get-ChildItem -LiteralPath $modsDir -File -Filter '*.jar' |
                Where-Object { (Get-FabricModId $_.FullName) -eq $modId })
            $destination = Join-Path $modsDir 'rtx-remix-renderer-dev.jar'
            if ((Test-Path -LiteralPath $destination) -and $destination -notin $oldJars.FullName) {
                throw "The deployment filename belongs to another mod: $destination"
            }
            foreach ($oldJar in $oldJars) {
                if ($oldJar.Attributes -band [IO.FileAttributes]::ReparsePoint) {
                    throw "Refusing to replace a linked mod JAR: $($oldJar.FullName)"
                }
            }

            $bridgeSource = Join-Path $repoRoot 'build\native\win64\remixbridge.dll'
            $runtimeDir = Join-Path $GameDir 'remix-sdk'
            $bridgeDestination = Join-Path $runtimeDir 'remixbridge.dll'
            if ($Native) {
                if (-not (Test-Path -LiteralPath $runtimeDir -PathType Container)) { throw 'Stage the pinned runtime before native deployment.' }
                if ((Get-Item -LiteralPath $runtimeDir).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Refusing to modify a linked runtime directory.' }
                if (-not (Test-Path -LiteralPath $bridgeSource -PathType Leaf)) { throw "Native bridge missing: $bridgeSource" }
                if ((Test-Path -LiteralPath $bridgeDestination) -and ((Get-Item -LiteralPath $bridgeDestination).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Refusing to replace a linked native bridge.' }
            }

            $backupDir = Join-Path $localDir "backups\deploy\$stamp"
            New-Item -ItemType Directory -Path $backupDir -Force | Out-Null
            $backups = [Collections.Generic.List[object]]::new()
            foreach ($oldJar in $oldJars) {
                $backup = Join-Path $backupDir $oldJar.Name
                $hash = Copy-Verified $oldJar.FullName $backup
                $backups.Add([pscustomobject]@{ Original = $oldJar.FullName; Backup = $backup; SHA256 = $hash })
            }
            $bridgeExisted = $Native -and (Test-Path -LiteralPath $bridgeDestination)
            if ($bridgeExisted) {
                $backup = Join-Path $backupDir 'remixbridge.dll'
                $hash = Copy-Verified $bridgeDestination $backup
                $backups.Add([pscustomobject]@{ Original = $bridgeDestination; Backup = $backup; SHA256 = $hash })
            }
            ConvertTo-Json -InputObject $backups.ToArray() -Depth 3 | Set-Content -LiteralPath (Join-Path $backupDir 'manifest.json') -Encoding UTF8

            Assert-GameStopped
            $bridgeTouched = $false
            try {
                $deployedHash = Copy-Verified $jar.FullName $destination
                if ($Native) {
                    $bridgeTouched = $true
                    $null = Copy-Verified $bridgeSource $bridgeDestination
                }
                foreach ($oldJar in $oldJars) {
                    if ($oldJar.FullName -ne $destination) { Remove-Item -LiteralPath $oldJar.FullName }
                }
            }
            catch {
                $deploymentError = $_
                try {
                    if (Test-Path -LiteralPath $destination) { Remove-Item -LiteralPath $destination -Force }
                    if ($bridgeTouched -and -not $bridgeExisted -and (Test-Path -LiteralPath $bridgeDestination)) { Remove-Item -LiteralPath $bridgeDestination -Force }
                    foreach ($backup in $backups) { $null = Copy-Verified $backup.Backup $backup.Original }
                }
                catch { throw "Deployment and rollback failed. Preserve $backupDir. Rollback: $($_.Exception.Message). Original: $($deploymentError.Exception.Message)" }
                throw $deploymentError
            }
            "Deployed: $destination`nSHA256: $deployedHash`nBackup: $backupDir" |
                Tee-Object -FilePath $logPath -Append
        }
    }
    "Completed $Mode in $([Math]::Round($timer.Elapsed.TotalSeconds, 1)) seconds.`nTarget: $operationTarget`nLog: $logPath" |
        Tee-Object -FilePath $logPath -Append
}
finally {
    foreach ($name in $previousEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($name, $previousEnvironment[$name], 'Process')
    }
    Pop-Location
}
