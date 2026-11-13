# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

import json
import os
import sqlite3
from dataclasses import dataclass
from pathlib import Path
from typing import cast

import pytest

from rtx_tools import client, winproc
from rtx_tools import config as tools_config
from rtx_tools import modrinth_launcher as modrinth
from rtx_tools.benchmark import runner, util
from rtx_tools.client import GameProcess, LaunchBlocked, LaunchRequest

SOURCE = "local:11111111-1111-4111-8111-111111111111"
BENCH = "local:22222222-2222-4222-8222-222222222222"


@pytest.fixture
def isolated_modrinth(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> tuple[Path, dict]:
    root = tmp_path / "ModrinthApp"
    for name in ("Fabric 26.3", modrinth.BENCH_NAME):
        (root / "profiles" / name / "mods").mkdir(parents=True)
    with sqlite3.connect(root / "app.db") as connection:
        connection.executescript(
            "CREATE TABLE instances(id TEXT,path TEXT,name TEXT,applied_content_set_id TEXT);"
            "CREATE TABLE instance_content_sets(id TEXT,game_version TEXT,loader TEXT);"
            "INSERT INTO instance_content_sets VALUES('fabric','26.3','fabric');"
        )
        connection.executemany(
            "INSERT INTO instances VALUES(?,?,?,'fabric')",
            [(SOURCE, "Fabric 26.3", "Fabric 26.3"), (BENCH, modrinth.BENCH_NAME, modrinth.BENCH_NAME)],
        )
    config = {
        "launch": {"launcher": "modrinth"},
        "modrinth": {"dataDir": str(root), "profileId": BENCH, "sourceProfileId": SOURCE},
    }
    monkeypatch.setattr(tools_config, "load_config", lambda: config)
    monkeypatch.setattr(runner, "java_process_command_lines", lambda **kwargs: [])
    monkeypatch.setattr(winproc, "short_path", lambda path: None)
    monkeypatch.setattr(modrinth.ModrinthLauncher, "_executable", lambda self: tmp_path / "Modrinth App.exe")
    return root, config


def test_supported_deep_link_encodes_world_and_rejects_nonlocal_identity() -> None:
    assert modrinth.launch_url(BENCH, "Frozen world & water") == (
        f"modrinth://launch/instance/{BENCH}?singleplayer_world=Frozen+world+%26+water"
    )
    with pytest.raises(LaunchBlocked):
        modrinth.launch_url("server:123")


def test_read_only_metadata_and_adoption_leave_source_untouched(isolated_modrinth: tuple[Path, dict]) -> None:
    root, _ = isolated_modrinth
    original = root / "profiles" / "Fabric 26.3"
    (original / "options.txt").write_text("unchanged", encoding="utf-8")
    before = (root / "app.db").read_bytes()
    launcher = modrinth.ModrinthLauncher()
    with pytest.raises(LaunchBlocked, match="adopt"):
        launcher.check()
    launcher.adopt()
    launcher.check()
    assert (root / "app.db").read_bytes() == before
    assert (original / "options.txt").read_text(encoding="utf-8") == "unchanged"
    assert not (original / modrinth.OWNERSHIP_FILE).exists()
    assert json.loads((launcher.game_dir / modrinth.OWNERSHIP_FILE).read_text())["profileId"] == BENCH


def test_source_profile_cannot_be_adopted(isolated_modrinth: tuple[Path, dict]) -> None:
    _, config = isolated_modrinth
    config["modrinth"]["profileId"] = SOURCE
    with pytest.raises(LaunchBlocked, match="source gameplay profile"):
        modrinth.ModrinthLauncher()


def test_instance_identity_and_display_name_do_not_determine_its_profile_directory(
    isolated_modrinth: tuple[Path, dict],
) -> None:
    root, _ = isolated_modrinth
    path = root / "profiles" / BENCH.removeprefix("local:")
    (root / "profiles" / modrinth.BENCH_NAME).rename(path)
    with sqlite3.connect(root / "app.db") as connection:
        connection.execute("UPDATE instances SET path=? WHERE id=?", (path.name, BENCH))
    launcher = modrinth.ModrinthLauncher()
    assert launcher.game_dir == path
    launcher.adopt()
    launcher.check()


def test_shared_profile_files_are_rejected_before_staging(isolated_modrinth: tuple[Path, dict]) -> None:
    root, _ = isolated_modrinth
    source = root / "profiles" / "Fabric 26.3" / "options.txt"
    source.write_text("source config", encoding="utf-8")
    launcher = modrinth.ModrinthLauncher()
    os.link(source, launcher.game_dir / "options.txt")
    with pytest.raises(LaunchBlocked, match="shared writable content"):
        launcher.adopt()
    assert source.read_text(encoding="utf-8") == "source config"
    assert not (launcher.game_dir / modrinth.OWNERSHIP_FILE).exists()


def test_adoption_rejects_live_selected_game_but_not_live_source(
    isolated_modrinth: tuple[Path, dict], monkeypatch: pytest.MonkeyPatch
) -> None:
    root, _ = isolated_modrinth
    source = root / "profiles" / "Fabric 26.3"
    launcher = modrinth.ModrinthLauncher()
    processes = [(42960, f'javaw --gameDir "{source}"')]
    monkeypatch.setattr(runner, "java_process_command_lines", lambda **kwargs: processes)
    launcher.adopt()
    processes.append((1234, f'javaw --gameDir "{launcher.game_dir}"'))
    with pytest.raises(LaunchBlocked, match="Close Minecraft"):
        launcher.check()


def test_adoption_does_not_transfer_checkout_ownership(
    isolated_modrinth: tuple[Path, dict], monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    launcher = modrinth.ModrinthLauncher()
    launcher.adopt()
    monkeypatch.setattr(util, "PROJECT_DIR", tmp_path / "different-checkout")
    with pytest.raises(LaunchBlocked, match="another checkout"):
        launcher.adopt()


def test_process_matching_requires_exact_gamedir(monkeypatch: pytest.MonkeyPatch, tmp_path: Path) -> None:
    monkeypatch.setattr(winproc, "short_path", lambda path: None)
    bench = tmp_path / "profiles" / modrinth.BENCH_NAME
    processes = [
        (1, f'java --gameDir "{bench}"'),
        (2, f'java --gameDir "{bench} Extra"'),
        (3, f'java -cp "{bench}/mods/a.jar" --gameDir "{tmp_path / "Fabric 26.3"}"'),
    ]
    assert [pid for pid, _ in modrinth.matching_games(bench, processes)] == [1]
    matches = runner.find_checkout_java_processes(processes, project_dir=tmp_path / "checkout", game_dir=bench)
    assert [pid for pid, _ in matches] == [1]


def test_gamedir_like_text_inside_a_library_argument_never_establishes_ownership(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    monkeypatch.setattr(winproc, "short_path", lambda path: None)
    bench = tmp_path / "profiles" / modrinth.BENCH_NAME
    user = tmp_path / "profiles" / "Fabric 26.3"
    processes = [
        (42960, f'java -cp "library --gameDir {bench}" --gameDir "{user}"'),
        (1234, f'java -cp "{user}/library.jar" --gameDir "{bench}"'),
        (5678, f'java --gameDir "{bench}" --gameDir "{user}"'),
    ]
    assert [pid for pid, _ in modrinth.matching_games(bench, processes)] == [1234]
    assert modrinth.process_game_dir(f'java --gameDir "{bench}') is None
    assert modrinth.process_game_dir(f'java --gameDir "{bench}" --gameDir') is None
    alias = f'java --gameDir "{bench / "nested" / ".."}"'
    assert modrinth.process_game_dir(alias) == modrinth.normalized(bench)


def test_windows_argument_quoting_round_trips_library_and_jvm_values() -> None:
    import subprocess

    expected = [
        r"C:\Program Files\Java\bin\javaw.exe",
        r"-Doutput=C:\Game Path\profile.jfr",
        '-Dlabel=a"b',
        "",
        "--gameDir",
        "C:\\Bench Path\\",
    ]
    assert modrinth.windows_arguments(subprocess.list2cmdline(expected)) == expected


def test_game_java_version_and_architecture_are_verified_without_running_java(tmp_path: Path) -> None:
    executable = tmp_path / "jdk" / "bin" / "javaw.exe"
    executable.parent.mkdir(parents=True)
    release = executable.parent.parent / "release"
    release.write_text('JAVA_VERSION="25.0.2"\nOS_ARCH="amd64"\n', encoding="utf-8")
    modrinth.validate_java(f'"{executable}" --gameDir bench', "win64")
    release.write_text('JAVA_VERSION="21.0.2"\nOS_ARCH="amd64"\n', encoding="utf-8")
    with pytest.raises(LaunchBlocked, match="other than Java 25"):
        modrinth.validate_java(f'"{executable}"', "win64")


@dataclass
class OwnedGame:
    pid: int
    code: int | None = None
    ended: bool = False

    def poll(self) -> int | None:
        return self.code

    def terminate(self) -> None:
        self.ended = True
        self.code = 1

    def kill(self) -> None:
        self.terminate()

    def wait(self, timeout: float | None = None) -> int:
        return self.code or 0


def test_cleanup_ends_only_retained_controller_processes(monkeypatch: pytest.MonkeyPatch) -> None:
    user_game = OwnedGame(42960)
    owned = OwnedGame(1234)
    monkeypatch.setattr(runner, "OWNED_PROCESSES", {id(owned): cast(GameProcess, owned)})

    def refuse_taskkill(*args: str) -> None:
        pytest.fail(f"Cleanup must use an owned process handle, never taskkill: {args}")

    monkeypatch.setattr(runner, "run_taskkill", refuse_taskkill)
    runner.kill_leftovers()
    assert owned.ended
    assert not user_game.ended
    assert runner.OWNED_PROCESSES == {}


def test_missing_persistent_jvm_args_stop_only_new_game_without_printing_command(
    isolated_modrinth: tuple[Path, dict], monkeypatch: pytest.MonkeyPatch
) -> None:
    _, config = isolated_modrinth
    config["launch"]["jvmArgs"] = ["-Drtxremix.backend=voxel"]
    launcher = modrinth.ModrinthLauncher()
    launcher.adopt()
    monkeypatch.setattr(launcher, "check", lambda **kwargs: None)
    snapshots = iter([[(42960, "user-game")], [(1234, f'java --gameDir "{launcher.game_dir}" --accessToken SECRET')]])
    monkeypatch.setattr(runner, "java_process_command_lines", lambda **kwargs: next(snapshots))
    launched: list[list[str]] = []
    monkeypatch.setattr(modrinth.subprocess, "Popen", lambda args, **kwargs: launched.append(args))
    game = OwnedGame(1234)
    monkeypatch.setattr(winproc, "PidProcess", lambda pid: game)
    with pytest.raises(LaunchBlocked, match="missing required arguments") as error:
        launcher.start(LaunchRequest(arch="win64", env={}, startup_seconds=1))
    assert "SECRET" not in str(error.value)
    assert game.ended
    assert launched[0][1] == modrinth.launch_url(BENCH)


def test_modrinth_benchmark_config_restores_original_bytes_on_failure(
    isolated_modrinth: tuple[Path, dict], monkeypatch: pytest.MonkeyPatch
) -> None:
    launcher = modrinth.ModrinthLauncher()
    launcher.adopt()
    client.use(launcher)
    target = launcher.game_dir / "rtx.conf"
    original = b"# custom profile\r\nrtx.someOption = 7\r\n"
    target.write_bytes(original)
    monkeypatch.setattr(runner, "gradle_env", lambda: {})
    with pytest.raises(RuntimeError, match="measurement failed"):
        with runner.benchmark_rtx_config_env():
            assert b"rtx.dlfg.enable = false" in target.read_bytes()
            raise RuntimeError("measurement failed")
    assert target.read_bytes() == original
    assert not (launcher.game_dir / "benchmark" / "startup-rtx.conf.backup").exists()
