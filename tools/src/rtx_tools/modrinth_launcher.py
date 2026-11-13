# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

"""Launch an explicitly adopted, isolated Modrinth instance without writing its app database.

The supported deep link carries instance/world only. Persistent JVM arguments
must be set in the app; we verify the new JVM actually received them. The
singleton app is never owned or terminated by this controller.
"""

from __future__ import annotations

import json
import ntpath
import os
import re
import sqlite3
import subprocess
import time
from dataclasses import dataclass
from pathlib import Path
from urllib.parse import quote, urlencode
from uuid import UUID

from . import client, install, winproc
from . import config as tools_config
from .benchmark import runner, util
from .client import GameProcess, LaunchBlocked, LaunchRequest

OWNERSHIP_FILE = ".rtx-tools-owner.json"
BENCH_NAME = "RTX Rewrite Bench"


def instance_id(value: str) -> str:
    try:
        return "local:" + str(UUID(value.removeprefix("local:")))
    except ValueError as exc:
        raise LaunchBlocked("modrinth.profileId must be a local Modrinth instance UUID.") from exc


def launch_url(profile_id: str, world: str | None = None) -> str:
    url = "modrinth://launch/instance/" + quote(instance_id(profile_id), safe=":")
    return url if world is None else url + "?" + urlencode({"singleplayer_world": world})


def normalized(path: Path | str) -> str:
    return ntpath.normpath(str(path)).rstrip("\\").casefold()


def windows_arguments(command: str) -> list[str]:
    """Decode Windows argv quoting without interpreting shell syntax or exposing credentials."""
    arguments = []
    offset = 0
    while offset < len(command):
        while offset < len(command) and command[offset] in " \t":
            offset += 1
        if offset == len(command):
            break
        argument = []
        quoted = False
        while offset < len(command):
            slashes = 0
            while offset < len(command) and command[offset] == "\\":
                slashes += 1
                offset += 1
            if offset < len(command) and command[offset] == '"':
                argument.extend("\\" * (slashes // 2))
                if slashes % 2:
                    argument.append('"')
                elif quoted and offset + 1 < len(command) and command[offset + 1] == '"':
                    argument.append('"')
                    offset += 1
                else:
                    quoted = not quoted
                offset += 1
                continue
            argument.extend("\\" * slashes)
            if offset == len(command) or (not quoted and command[offset] in " \t"):
                break
            argument.append(command[offset])
            offset += 1
        arguments.append("".join(argument))
        if quoted:
            # An incomplete command line cannot establish process ownership.
            return []
    return arguments


def process_game_dir(command: str) -> str | None:
    arguments = windows_arguments(command)
    positions = [index for index, argument in enumerate(arguments) if argument == "--gameDir"]
    if len(positions) != 1 or positions[0] + 1 == len(arguments):
        return None
    return normalized(arguments[positions[0] + 1])


def validate_java(command: str, arch: str) -> None:
    arguments = windows_arguments(command)
    if not arguments:
        raise LaunchBlocked("Cannot identify the benchmark JVM executable.")
    release = Path(arguments[0]).parent.parent / "release"
    try:
        text = release.read_text(encoding="utf-8")
    except OSError as exc:
        raise LaunchBlocked(
            "Cannot verify the benchmark JVM; select an installed Java 25 runtime in the duplicate."
        ) from exc
    if re.search(r'^JAVA_VERSION="25(?:[.\-+"]|$)', text, re.MULTILINE) is None:
        raise LaunchBlocked("The benchmark duplicate launched a JVM other than Java 25.")
    architecture = re.search(r'^OS_ARCH="([^"]+)"', text, re.MULTILINE)
    accepted = {"amd64", "x86_64"} if arch == "win64" else {"aarch64", "arm64"}
    if architecture is None or architecture.group(1) not in accepted:
        raise LaunchBlocked("The duplicate's Java architecture does not match the selected native runtime.")


def matching_games(game_dir: Path, processes: list[tuple[int, str]]) -> list[tuple[int, str]]:
    paths = {normalized(game_dir)}
    if short := winproc.short_path(game_dir):
        paths.add(normalized(short))
    return [(pid, command) for pid, command in processes if process_game_dir(command) in paths]


def data_directory(config: dict) -> Path:
    raw = (config.get("modrinth") or {}).get("dataDir")
    if raw:
        return util.project_path(raw)
    if not os.environ.get("APPDATA"):
        raise LaunchBlocked("Set modrinth.dataDir to the existing ModrinthApp data directory.")
    return Path(os.environ["APPDATA"]) / "ModrinthApp"


@dataclass(frozen=True)
class Profile:
    id: str
    path: Path
    name: str
    game_version: str
    loader: str


def profiles(data_dir: Path) -> list[Profile]:
    """Read only identity and content-set metadata, never account/token tables."""
    database = data_dir / "app.db"
    try:
        connection = sqlite3.connect(database.resolve().as_uri() + "?mode=ro", uri=True, timeout=3)
        try:
            connection.execute("PRAGMA query_only=ON")
            rows = connection.execute(
                "SELECT i.id, i.path, i.name, c.game_version, c.loader FROM instances i "
                "LEFT JOIN instance_content_sets c ON c.id=i.applied_content_set_id"
            ).fetchall()
        finally:
            connection.close()
    except sqlite3.Error as exc:
        raise LaunchBlocked(f"Cannot read Modrinth instance metadata from {database}: {exc}") from exc
    result = []
    root = (data_dir / "profiles").resolve()
    for identity, relative, name, version, loader in rows:
        raw_path = root / relative
        if raw_path.is_symlink() or raw_path.is_junction():
            continue
        path = raw_path.resolve()
        if path.parent != root:
            continue
        result.append(Profile(identity, path, name, version or "", loader or ""))
    return result


class ModrinthLauncher:
    name = "Modrinth"
    startup_config_in_profile = True

    def __init__(self) -> None:
        self.game_dir = Path()
        self.refresh_layout()

    @property
    def config(self) -> dict:
        return tools_config.load_config()

    @property
    def data_dir(self) -> Path:
        return data_directory(self.config)

    def selected_profile(self) -> Profile:
        config = self.config
        identity = instance_id(tools_config.require_setting(config, "modrinth.profileId"))
        source = instance_id(tools_config.require_setting(config, "modrinth.sourceProfileId"))
        if identity == source:
            raise LaunchBlocked("The source gameplay profile cannot be a tools target; duplicate it in Modrinth first.")
        candidates = [profile for profile in profiles(self.data_dir) if profile.id == identity]
        if len(candidates) != 1:
            raise LaunchBlocked("The selected Modrinth instance was not found; select its new local UUID.")
        profile = candidates[0]
        if profile.name != BENCH_NAME:
            raise LaunchBlocked(f"Only the dedicated Modrinth duplicate named {BENCH_NAME!r} may be adopted.")
        if profile.game_version != "26.3" or profile.loader != "fabric" or not (profile.path / "mods").is_dir():
            raise LaunchBlocked(
                "The benchmark duplicate must be an installed Minecraft 26.3 Fabric profile with mods/."
            )
        source_paths = [profile.path for profile in profiles(self.data_dir) if profile.id == source]
        if len(source_paths) != 1 or profile.path == source_paths[0] or profile.path.is_symlink():
            raise LaunchBlocked("Cannot establish separation from the source gameplay profile.")
        return profile

    def refresh_layout(self) -> None:
        self.game_dir = self.selected_profile().path

    def describe(self) -> str:
        return f"Modrinth: {self.game_dir.name} (isolated instance; app account)"

    def _refuse_running(self) -> None:
        if matching_games(self.game_dir, runner.java_process_command_lines(required=True)):
            raise LaunchBlocked(f"Close Minecraft for {self.game_dir.name} before staging or launching it.")

    def _refuse_shared_payload(self) -> None:
        # An app duplicate can share managed content. Never overwrite a link
        # that could also be the source profile's configuration or native file.
        roots = [self.game_dir / name for name in ("mods", "config", "textures", "remix-sdk", "rtx-remix")]
        roots += [self.game_dir / name for name in ("options.txt", "rtx.conf", "user.conf")]
        for root in roots:
            entries = [root, *root.rglob("*")] if root.is_dir() else [root]
            for entry in entries:
                if entry.is_symlink() or entry.is_junction() or (entry.is_file() and entry.stat().st_nlink > 1):
                    raise LaunchBlocked(
                        f"The benchmark duplicate contains shared writable content: {entry}. "
                        "Create an independent copy of that file/directory in the duplicate before adopting it."
                    )

    def _owner(self) -> dict:
        return {
            "schemaVersion": 1,
            "project": str(util.PROJECT_DIR.resolve()),
            "profileId": self.selected_profile().id,
            "sourceProfileId": instance_id(tools_config.require_setting(self.config, "modrinth.sourceProfileId")),
        }

    def adopt(self) -> None:
        """Explicit controller command after duplication through the app UI; never clones worlds or writes app.db."""
        self.refresh_layout()
        self._refuse_running()
        self._refuse_shared_payload()
        target = self.game_dir / OWNERSHIP_FILE
        if target.exists():
            self.check(interactive=False)
            return
        with target.open("x", encoding="utf-8") as stream:
            json.dump(self._owner(), stream, indent=2)
            stream.write("\n")

    def check(self, *, interactive: bool = True) -> None:
        del interactive
        self.refresh_layout()
        try:
            owner = json.loads((self.game_dir / OWNERSHIP_FILE).read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            raise LaunchBlocked(
                "Duplicate the profile in Modrinth, configure its UUID, then run modrinth adopt."
            ) from exc
        if owner != self._owner():
            raise LaunchBlocked("This Modrinth duplicate belongs to another checkout or instance; refusing to stage.")
        self._executable()
        self._refuse_running()
        self._refuse_shared_payload()

    def _executable(self) -> Path:
        raw = (self.config.get("modrinth") or {}).get("executable")
        executable = (
            util.project_path(raw)
            if raw
            else Path(os.environ.get("LOCALAPPDATA", "")) / "Modrinth App" / "Modrinth App.exe"
        )
        if os.name != "nt" or not executable.is_file():
            raise LaunchBlocked("Set modrinth.executable to the installed Windows Modrinth App executable.")
        return executable

    def prepare(self, arch: str, *, build: bool) -> None:
        self.check(interactive=False)
        if build:
            runner.run_gradle_build(arch, ci=False)
        self.check(interactive=False)
        if install.install_development_build(util.PROJECT_DIR, game_dir=self.game_dir, arch=arch) != 0:
            raise LaunchBlocked("Cannot stage the development build into the isolated Modrinth instance.")

    def required_jvm_args(self, request: LaunchRequest) -> list[str]:
        configured = (self.config.get("launch") or {}).get("jvmArgs") or []
        if not isinstance(configured, list) or not all(isinstance(value, str) for value in configured):
            raise LaunchBlocked("launch.jvmArgs must be an array of JVM argument strings.")
        from . import renderer_lab

        args = [*configured, *client.preset_jvm_args(request.preset), *renderer_lab.jvm_args(self.config)]
        if request.jfr_path is not None:
            depth = request.tracy_java_max_depth
            args += client.benchmark_jvm_args(
                request.jfr_path, runner.tracy_java_depth(self.config) if depth is None else depth
            )
        return args

    def start(self, request: LaunchRequest) -> GameProcess:
        self.check(interactive=False)
        required = self.required_jvm_args(request)
        before = {pid for pid, _ in runner.java_process_command_lines(required=True)}
        # No env= here: an already-running singleton cannot inherit it. The benchmark
        # context stages/restores startup RTX options in this owned profile instead.
        subprocess.Popen(
            [str(self._executable()), launch_url(self.selected_profile().id, request.quick_play_world)],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        timeout = request.startup_seconds
        deadline = time.monotonic() + (
            runner.benchmark_startup_stall_seconds(self.config) if timeout is None else timeout
        )
        while time.monotonic() < deadline:
            matches = matching_games(self.game_dir, runner.java_process_command_lines(required=True))
            fresh = [(pid, command) for pid, command in matches if pid not in before]
            if len(fresh) > 1:
                raise LaunchBlocked("Multiple new JVMs target the benchmark instance; refusing ambiguous ownership.")
            if fresh:
                pid, command = fresh[0]
                game = winproc.PidProcess(pid)
                actual_arguments = {arg.replace("\\", "/") for arg in windows_arguments(command)}
                missing = [arg for arg in required if arg.replace("\\", "/") not in actual_arguments]
                if missing:
                    game.terminate()
                    raise LaunchBlocked(
                        "The new benchmark JVM is missing required arguments. Set these in the duplicate's "
                        "Modrinth Java options (not global options), then retry:\n" + "\n".join(missing)
                    )
                try:
                    validate_java(command, request.arch)
                except LaunchBlocked:
                    game.terminate()
                    raise
                return game
            time.sleep(0.5)
        raise LaunchBlocked("Modrinth did not start a new JVM for the isolated instance before the startup timeout.")

    def log_files(self, since_ns: int) -> list[Path]:
        candidates = [
            self.game_dir / "logs" / "latest.log",
            self.game_dir / "rtx-remix" / "logs" / "remix-dxvk.log",
            *sorted(self.game_dir.glob("crash-reports/crash-*-client.txt")),
            *sorted(self.game_dir.glob("hs_err_pid*.log")),
        ]
        return [path for path in candidates if path.is_file() and path.stat().st_mtime_ns > since_ns]
