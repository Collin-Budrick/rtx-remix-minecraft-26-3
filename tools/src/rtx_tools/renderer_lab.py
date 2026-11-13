# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

"""Reproducible renderer experiments; requested backends are never evidence of implementation."""

from __future__ import annotations

import copy
import hashlib
import json
import math
from pathlib import Path
from typing import cast

from .benchmark import util

BACKENDS = {
    "remix": "Existing in-process Remix baseline",
    "scene-export": "Shared-scene capture while Remix still presents; not a replacement renderer",
    "native-hybrid": "RTXPT native experiment; requires observed native in-process presentation",
    "restir-pt": "ReSTIR PT experiment; requires observed native in-process presentation",
    "voxel": "Voxel research adapter; requires observed native in-process presentation",
    "gi12": "GI-1.2 research adapter; requires observed native in-process presentation",
}
STATUS_PREFIX = "RTX_BACKEND_STATUS "
NATIVE_FRAME_SOURCE = "D3D12 new ready-fence publications"
NATIVE_COUNT_KEYS = (
    "renderAttempts",
    "producedFrames",
    "pendingFrames",
    "unreadyFrames",
    "duplicateFrames",
    "failedFrames",
)


def enabled(config: dict) -> bool:
    return (config.get("rendererLab") or {}).get("enabled") is True


def requested_backend(config: dict) -> str:
    value = str((config.get("rendererLab") or {}).get("backend") or "remix")
    if value not in BACKENDS:
        raise RuntimeError(f"Unknown rendererLab.backend {value!r}; expected {', '.join(BACKENDS)}")
    return value


def jvm_args(config: dict) -> list[str]:
    if not enabled(config):
        return []
    return [f"-Drtxremix.backend={requested_backend(config)}", "-Drtxremix.backend.fallback=false"]


def apply_protocol(config: dict, runs: int = 3) -> None:
    if not enabled(config):
        return
    if runs != 3:
        raise RuntimeError("Renderer comparisons require exactly 3 runs; use --runs 3.")
    requested_backend(config)
    config["runs"] = 3
    config["warmupFrames"] = 600
    config.setdefault("render", {}).update(
        {
            "fullscreen": False,
            "overrideWidth": 2560,
            "overrideHeight": 1440,
            "renderDistance": 16,
            "simulationDistance": 12,
            "maxFps": 260,
            "vsync": False,
        }
    )
    config.setdefault("resourcePack", {})["mode"] = "current"


def current_resource_packs(game_dir: Path) -> list[str]:
    try:
        text = (game_dir / "options.txt").read_text(encoding="utf-8")
        values = [line.split(":", 1)[1] for line in text.splitlines() if line.startswith("resourcePacks:")]
        packs = json.loads(values[-1]) if values else ["vanilla", "fabric"]
    except (OSError, ValueError, IndexError) as exc:
        raise RuntimeError("Cannot freeze the isolated profile's current resource-pack order.") from exc
    if not isinstance(packs, list) or not all(isinstance(pack, str) for pack in packs):
        raise RuntimeError("The isolated profile resourcePacks option must be a JSON string array.")
    result = []
    for pack in packs:
        if pack in ("vanilla", "fabric"):
            continue
        if not pack.startswith("file/"):
            raise RuntimeError(
                f"Cannot reproduce enabled built-in resource pack {pack!r}; configure its equivalent first."
            )
        relative = pack.removeprefix("file/")
        path = (game_dir / "resourcepacks" / relative).resolve()
        if path.parent != (game_dir / "resourcepacks").resolve() or not path.exists():
            raise RuntimeError(f"Enabled resource pack {pack!r} is missing or outside resourcepacks/.")
        result.append(relative)
    return result


def content_hash(path: Path) -> str:
    if not path.exists():
        raise RuntimeError(f"Frozen input is missing: {path}")
    digest = hashlib.sha256()
    entries = [path] if path.is_file() else sorted(path.rglob("*"))
    if any(entry.is_symlink() or entry.is_junction() for entry in entries):
        raise RuntimeError(f"Frozen inputs must contain regular files/directories, not links: {path}")
    files = [entry for entry in entries if entry.is_file()]
    for file in files:
        digest.update((file.name if path.is_file() else file.relative_to(path).as_posix()).encode())
        digest.update(b"\0")
        with file.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        digest.update(b"\0")
    return digest.hexdigest()


def fixture_snapshot(selection, game_dir: Path) -> dict:
    packs = current_resource_packs(game_dir)
    return {
        "schemaVersion": 1,
        "world": selection.world,
        "version": selection.version,
        "camera": selection.camera,
        "worldSha256": content_hash(selection.world_path),
        "cameraSha256": content_hash(selection.camera_path),
        "resourcePacks": [{"name": name, "sha256": content_hash(game_dir / "resourcepacks" / name)} for name in packs],
    }


def lock_path(config: dict) -> Path:
    raw = (config.get("rendererLab") or {}).get("fixtureLock") or ".codex/renderer-lab/fixture-lock.json"
    return util.project_path(raw)


def freeze(config: dict, selection, game_dir: Path, *, replace: bool = False) -> Path:
    path = lock_path(config)
    if path.exists() and not replace:
        raise RuntimeError(f"Fixture lock already exists: {path}; use --replace only for a new baseline series.")
    util.write_json(path, fixture_snapshot(selection, game_dir))
    return path


def verify_fixture(config: dict, selection, game_dir: Path) -> dict:
    try:
        expected = util.read_json(lock_path(config))
    except (OSError, ValueError) as exc:
        raise RuntimeError(
            "Freeze the fixture and current resource packs with renderer-lab freeze before measuring."
        ) from exc
    if expected != fixture_snapshot(selection, game_dir):
        raise RuntimeError("Frozen fixture or resource packs changed; restore them or begin a new baseline series.")
    return expected


def _integer(values: dict, key: str, *, minimum: int = 0) -> int:
    value = values.get(key)
    if not isinstance(value, int) or isinstance(value, bool) or value < minimum:
        raise RuntimeError(f"Native frame evidence requires integer {key} >= {minimum}.")
    return value


def native_frame_proof(evidence: object, record_frames: int) -> dict:
    if not isinstance(evidence, dict):
        raise RuntimeError("Native frame evidence is missing or malformed; presentation alone cannot prove real FPS.")
    evidence = cast(dict[str, object], evidence)
    frames = _integer({"recordFrames": record_frames}, "recordFrames", minimum=1)
    if _integer(evidence, "schema", minimum=1) != 1 or evidence.get("source") != NATIVE_FRAME_SOURCE:
        raise RuntimeError("Native frame evidence has an unsupported schema or counter source.")
    if evidence.get("complete") is not True or evidence.get("eligibleForRenderedFps") is not True:
        raise RuntimeError("Native frame evidence is incomplete or ineligible for real rendered FPS.")
    for key, expected in (
        ("capturedSamples", frames),
        ("samplesWithNewFrame", frames),
        ("samplesWithoutNewFrame", 0),
        ("samplesWithMultipleFrames", 0),
    ):
        if _integer(evidence, key) != expected:
            raise RuntimeError("Every recorded Minecraft sample must contain exactly one new native frame.")
    start, end, delta = (evidence.get(key) for key in ("start", "end", "delta"))
    if not isinstance(start, dict) or not isinstance(end, dict) or not isinstance(delta, dict):
        raise RuntimeError("Native frame evidence needs start/end snapshots and measured counter deltas.")
    if _integer(start, "session", minimum=1) != _integer(end, "session", minimum=1):
        raise RuntimeError("Native renderer session changed during the measurement window.")
    start_generation = _integer(start, "lastGeneration", minimum=-1)
    end_generation = _integer(end, "lastGeneration")
    start_fence = _integer(start, "lastReadyFenceValue")
    end_fence = _integer(end, "lastReadyFenceValue", minimum=1)
    if (
        end_generation < start_generation
        or (start_generation == -1 and start_fence != 0)
        or (end_generation == start_generation and end_fence <= start_fence)
    ):
        raise RuntimeError("Native ready-fence publications did not advance consistently during measurement.")
    for key in NATIVE_COUNT_KEYS:
        before = _integer(start, key)
        after = _integer(end, key)
        change = _integer(delta, key)
        expected = frames if key in {"renderAttempts", "producedFrames"} else 0
        if after - before != change or change != expected:
            raise RuntimeError(f"Native frame counter {key} disagrees with one rendered frame per sample.")
    return evidence


def _validate_backend_status(proof: object, requested: str) -> dict:
    if not isinstance(proof, dict):
        raise RuntimeError("Backend status evidence is malformed.")
    proof = cast(dict[str, object], proof)
    if not isinstance(requested, str) or requested not in BACKENDS:
        raise RuntimeError("Backend status names an unknown requested renderer.")
    if proof.get("requested") != requested or proof.get("active") != requested or proof.get("fallback") is not False:
        raise RuntimeError(
            "Backend fell back or disagrees with the requested renderer; "
            "fallback runs cannot support renderer FPS claims."
        )
    if requested != "remix" and (
        proof.get("nativeInProcess") is not True
        or proof.get("presentation") not in ("native-dx12", "minecraft-opengl")
        or proof.get("sceneCapture") is not True
    ):
        raise RuntimeError("Standalone/export-only rendering is not an end-to-end Minecraft renderer measurement.")
    if proof.get("qualityComparable") is False:
        raise RuntimeError(
            "Renderer explicitly reports qualityComparable=false; manual review cannot override known gaps."
        )
    unsupported = proof.get("unsupportedFeatures", [] if requested == "remix" else None)
    if not isinstance(unsupported, list) or unsupported:
        raise RuntimeError(
            "Renderer reports unsupported features or malformed capability evidence; comparison is ineligible."
        )
    if proof.get("frameGeneration") is True:
        raise RuntimeError("Generated frames cannot support a real rendered-FPS comparison.")
    if requested != "remix" and (
        proof.get("qualityComparable") is not True or proof.get("frameGeneration") is not False
    ):
        raise RuntimeError(
            "Native renderer quality comparability and frame-generation-off require explicit runtime evidence."
        )
    return proof


def _timing_summary(values: object, frames: int) -> dict:
    if not isinstance(values, list) or len(values) != frames:
        raise RuntimeError("Full-frame timing sample count disagrees with native frame evidence.")
    for value in values:
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            raise RuntimeError("Full-frame timings must be positive finite numbers.")
        try:
            valid = value > 0 and math.isfinite(value)
        except OverflowError:
            valid = False
        if not valid:
            raise RuntimeError("Full-frame timings must be positive finite numbers.")
    return {"samples": frames, "minNanos": min(values), "maxNanos": max(values)}


def _validate_recorded_proof(proof: object, requested: str) -> None:
    status = _validate_backend_status(proof, requested)
    if requested == "remix":
        return
    frames = _integer(status, "recordFrames", minimum=1)
    native_frame_proof(status.get("nativeFrameEvidence"), frames)
    timing = status.get("fullFrameTiming")
    if not isinstance(timing, dict) or status.get("frameTimeSource") != "MinecraftClient.render":
        raise RuntimeError("Native comparisons require full MinecraftClient.render timing evidence.")
    if _integer(timing, "samples", minimum=1) != frames:
        raise RuntimeError("Full-frame timing sample count disagrees with native frame evidence.")
    summary = _timing_summary([timing.get("minNanos"), timing.get("maxNanos")], 2)
    if summary["minNanos"] != timing["minNanos"]:
        raise RuntimeError("Full-frame timing bounds are reversed.")


def backend_proof(log: str, requested: str, *, benchmark: dict | None = None) -> dict:
    statuses = []
    for line in log.splitlines():
        if STATUS_PREFIX not in line:
            continue
        try:
            value = json.loads(line.split(STATUS_PREFIX, 1)[1])
        except ValueError:
            continue
        if isinstance(value, dict):
            statuses.append(value)
    if not statuses:
        raise RuntimeError("No RTX_BACKEND_STATUS evidence was recorded; this run cannot identify its renderer.")
    status = copy.deepcopy(_validate_backend_status(statuses[-1], requested))
    if requested == "remix":
        return status
    metadata = benchmark.get("metadata") if isinstance(benchmark, dict) else None
    if not isinstance(metadata, dict):
        raise RuntimeError("Native frame evidence requires benchmark result metadata.")
    recorded = _validate_backend_status(metadata.get("rendererBackend"), requested)
    for key in ("active", "presentation", "nativeInProcess", "sceneCapture", "fallback"):
        if recorded.get(key) != status.get(key):
            raise RuntimeError("Recorded renderer identity disagrees with the observed backend status.")
    frames = _integer(metadata, "recordFrames", minimum=1)
    status["nativeFrameEvidence"] = copy.deepcopy(native_frame_proof(metadata.get("nativeFrameEvidence"), frames))
    status["recordFrames"] = frames
    status["frameTimeSource"] = metadata.get("frameTimeSource")
    assert isinstance(benchmark, dict)
    status["fullFrameTiming"] = _timing_summary(benchmark.get("perFrame"), frames)
    _validate_recorded_proof(status, requested)
    return status


def evidence(config: dict) -> dict | None:
    if not enabled(config):
        return None
    return {
        "schemaVersion": 1,
        "scope": "end-to-end-minecraft",
        "requestedBackend": requested_backend(config),
        "capabilityDescriptions": BACKENDS,
        "backendProofs": config.get("_rendererLabProofs", []),
        "fixture": config.get("_rendererLabFixture"),
        "quality": {
            "evidenceType": "requested-settings",
            "width": 2560,
            "height": 1440,
            "renderDistance": 16,
            "simulationDistance": 12,
            "runs": 3,
            "warmupFrames": 600,
            "frameGeneration": False,
            "vsync": False,
            "fpsLimit": "unlimited (Minecraft option 260)",
            "fov": (config.get("render") or {}).get("fov", 70),
            "modSettings": (config.get("render") or {}).get("modSettings") or {},
        },
        "visualReview": (config.get("rendererLab") or {}).get("visualReview") or "pending",
        "runtimeQualityReview": (config.get("rendererLab") or {}).get("runtimeQualityReview") or "pending",
        "speedupClaim": None,
    }


def comparison_problem(first: dict, second: dict) -> str | None:
    labs = [first.get("rendererLab"), second.get("rendererLab")]
    if not any(labs):
        return None
    for run, lab in zip((first, second), labs, strict=True):
        if not isinstance(lab, dict) or lab.get("scope") != "end-to-end-minecraft":
            return "Both runs must have end-to-end renderer-lab evidence."
        proofs = lab.get("backendProofs") or []
        if not isinstance(proofs, list) or len(proofs) != 3 or (run.get("outcome") or {}).get("status") != "passed":
            return "Three successful runs with observed backend proof are required."
        for proof in proofs:
            try:
                _validate_recorded_proof(proof, lab["requestedBackend"])
            except (RuntimeError, KeyError) as error:
                return f"Renderer comparison evidence rejected: {error}"
    for lab in labs:
        assert isinstance(lab, dict)
        if lab.get("visualReview") != "accepted":
            return "Comparable visuals and stable motion must be reviewed before a renderer speedup claim."
        if lab.get("runtimeQualityReview") != "verified":
            return "Actual frame generation, VSync, distance settings and GPU selection require runtime verification."
    first_lab, second_lab = labs
    assert isinstance(first_lab, dict)
    assert isinstance(second_lab, dict)
    if first_lab["fixture"] != second_lab["fixture"] or first_lab["quality"] != second_lab["quality"]:
        return "Fixture hashes, resource-pack order and quality settings must match."
    return None
