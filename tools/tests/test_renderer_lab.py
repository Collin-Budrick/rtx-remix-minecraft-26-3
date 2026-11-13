# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0

import copy
import json
from pathlib import Path

import pytest

from rtx_tools import renderer_lab
from rtx_tools.benchmark import runner


def test_protocol_pins_real_frame_quality_and_refuses_single_run() -> None:
    config = {"rendererLab": {"enabled": True, "backend": "remix"}, "render": {"fov": 90}}
    with pytest.raises(RuntimeError, match="exactly 3"):
        renderer_lab.apply_protocol(config, 1)
    renderer_lab.apply_protocol(config)
    assert config["warmupFrames"] == 600
    assert config["runs"] == 3
    assert config["render"] == {
        "fov": 90,
        "fullscreen": False,
        "overrideWidth": 2560,
        "overrideHeight": 1440,
        "renderDistance": 16,
        "simulationDistance": 12,
        "maxFps": 260,
        "vsync": False,
    }
    assert config["resourcePack"]["mode"] == "current"
    assert renderer_lab.jvm_args(config) == ["-Drtxremix.backend=remix", "-Drtxremix.backend.fallback=false"]


def test_unrecognized_backend_cannot_be_presented_as_implemented() -> None:
    with pytest.raises(RuntimeError, match="Unknown rendererLab.backend"):
        renderer_lab.jvm_args({"rendererLab": {"enabled": True, "backend": "imaginary-4x"}})


@pytest.fixture
def frozen_fixture(tmp_path: Path) -> tuple[dict, runner.BenchmarkSelection, Path]:
    world_dir = tmp_path / "fixture_v1.0.0"
    world = world_dir / "world"
    world.mkdir(parents=True)
    (world / "level.dat").write_bytes(b"frozen world")
    cameras = world_dir / "cameras"
    cameras.mkdir()
    camera = cameras / "benchmark.json"
    camera.write_text("{}", encoding="utf-8")
    game = tmp_path / "bench"
    packs = game / "resourcepacks"
    packs.mkdir(parents=True)
    (packs / "current.zip").write_bytes(b"current PBR pack")
    (game / "options.txt").write_text('resourcePacks:["vanilla","fabric","file/current.zip"]\n', encoding="utf-8")
    config = {"rendererLab": {"enabled": True, "fixtureLock": str(tmp_path / "lock.json")}}
    selection = runner.BenchmarkSelection("fixture", "benchmark", world_dir, world, cameras, camera, "v1.0.0")
    return config, selection, game


def test_fixture_lock_detects_resource_pack_and_world_changes(
    frozen_fixture: tuple[dict, runner.BenchmarkSelection, Path],
) -> None:
    config, selection, game = frozen_fixture
    renderer_lab.freeze(config, selection, game)
    before = (selection.world_path / "level.dat").read_bytes()
    first = renderer_lab.verify_fixture(config, selection, game)
    assert first["resourcePacks"][0]["name"] == "current.zip"
    assert (selection.world_path / "level.dat").read_bytes() == before
    pack = game / "resourcepacks" / "current.zip"
    pack.write_bytes(b"changed material")
    with pytest.raises(RuntimeError, match="changed"):
        renderer_lab.verify_fixture(config, selection, game)
    pack.write_bytes(b"current PBR pack")
    (selection.world_path / "level.dat").write_bytes(b"world edit")
    with pytest.raises(RuntimeError, match="changed"):
        renderer_lab.verify_fixture(config, selection, game)


def test_current_pack_mode_does_not_fetch_or_replace_packs(
    frozen_fixture: tuple[dict, runner.BenchmarkSelection, Path], monkeypatch: pytest.MonkeyPatch
) -> None:
    _, selection, game = frozen_fixture
    config = {"resourcePack": {"mode": "current"}}

    def refuse_copy(*args, **kwargs):
        pytest.fail("Current-profile resource packs must not be replaced.")

    monkeypatch.setattr(runner, "copy_resource_pack", refuse_copy)
    assert runner.resolve_art_pack(config) is None
    assert runner.stage_resource_packs(config, selection, game) == ["current.zip"]


def proof(requested: str, *, active: str | None = None, native: bool = False, fallback: bool = False) -> dict:
    result: dict = {
        "requested": requested,
        "active": active or requested,
        "presentation": "minecraft-opengl" if native else "remix",
        "nativeInProcess": native,
        "sceneCapture": native or requested == "scene-export",
        "fallback": fallback,
    }
    if native:
        result.update({"qualityComparable": True, "unsupportedFeatures": [], "frameGeneration": False})
    return result


def native_benchmark(backend: str = "native-hybrid") -> dict:
    # Warmup may already have pending/failed attempts; only the measured window must be clean.
    start = {
        "renderAttempts": 100,
        "producedFrames": 80,
        "pendingFrames": 10,
        "unreadyFrames": 5,
        "duplicateFrames": 3,
        "failedFrames": 2,
        "lastGeneration": 2,
        "lastReadyFenceValue": 80,
        "session": 7,
    }
    end = {**start, "renderAttempts": 103, "producedFrames": 83, "lastReadyFenceValue": 83}
    return {
        "metadata": {
            "recordFrames": 3,
            "frameTimeSource": "MinecraftClient.render",
            "rendererBackend": proof(backend, native=True),
            "nativeFrameEvidence": {
                "schema": 1,
                "source": "D3D12 new ready-fence publications",
                "complete": True,
                "eligibleForRenderedFps": True,
                "capturedSamples": 3,
                "samplesWithNewFrame": 3,
                "samplesWithoutNewFrame": 0,
                "samplesWithMultipleFrames": 0,
                "start": start,
                "end": end,
                "delta": {key: end[key] - start[key] for key in renderer_lab.NATIVE_COUNT_KEYS},
            },
        },
        "perFrame": [16_000_000, 17_000_000, 15_000_000],
    }


def recorded_native_proof(benchmark: dict | None = None, backend: str = "native-hybrid") -> dict:
    return renderer_lab.backend_proof(
        renderer_lab.STATUS_PREFIX + json.dumps(proof(backend, native=True)),
        backend,
        benchmark=benchmark if benchmark is not None else native_benchmark(backend),
    )


@pytest.mark.parametrize("backend", ["native-hybrid", "restir-pt", "voxel", "gi12", "scene-export"])
def test_standalone_or_capture_only_backend_cannot_claim_full_frame_fps(backend: str) -> None:
    with pytest.raises(RuntimeError, match="not an end-to-end"):
        renderer_lab.backend_proof(renderer_lab.STATUS_PREFIX + json.dumps(proof(backend)), backend)


def test_silent_fallback_and_missing_status_cannot_claim_backend_fps() -> None:
    with pytest.raises(RuntimeError, match="fell back"):
        renderer_lab.backend_proof(
            renderer_lab.STATUS_PREFIX + json.dumps(proof("voxel", active="remix", fallback=True)), "voxel"
        )
    with pytest.raises(RuntimeError, match="No RTX_BACKEND_STATUS"):
        renderer_lab.backend_proof("Exported standalone native timings: 900 FPS", "native-hybrid")


def test_latest_native_status_supersedes_initializing_but_does_not_override_known_quality_gaps() -> None:
    initializing = proof("native-hybrid", active="initializing")
    native = proof("native-hybrid", native=True)
    log = "\n".join(renderer_lab.STATUS_PREFIX + json.dumps(status) for status in (initializing, native))
    assert renderer_lab.backend_proof(log, "native-hybrid", benchmark=native_benchmark())["presentation"] == (
        "minecraft-opengl"
    )
    native["qualityComparable"] = False
    log += "\n" + renderer_lab.STATUS_PREFIX + json.dumps(native)
    with pytest.raises(RuntimeError, match="qualityComparable=false"):
        renderer_lab.backend_proof(log, "native-hybrid")


@pytest.mark.parametrize(
    ("field", "value", "message"),
    [
        ("unsupportedFeatures", ["transparent water is omitted"], "unsupported features"),
        ("unsupportedFeatures", "unknown", "unsupported features"),
        ("frameGeneration", True, "Generated frames"),
        ("qualityComparable", None, "explicit runtime evidence"),
    ],
)
def test_native_presentation_alone_does_not_prove_comparable_real_fps(field: str, value, message: str) -> None:
    native = proof("native-hybrid", native=True)
    native[field] = value
    with pytest.raises(RuntimeError, match=message):
        renderer_lab.backend_proof(renderer_lab.STATUS_PREFIX + json.dumps(native), "native-hybrid")


def test_missing_native_capability_list_cannot_mean_no_unsupported_features() -> None:
    native = proof("native-hybrid", native=True)
    native.pop("unsupportedFeatures")
    with pytest.raises(RuntimeError, match="unsupported features"):
        renderer_lab.backend_proof(renderer_lab.STATUS_PREFIX + json.dumps(native), "native-hybrid")


def completed_manifest(backend: str, *, native: bool = False) -> dict:
    config = {
        "rendererLab": {"enabled": True, "backend": backend},
        "_rendererLabFixture": {"worldSha256": "same", "resourcePacks": [{"name": "current.zip", "sha256": "same"}]},
        "_rendererLabProofs": [recorded_native_proof(backend=backend) if native else proof(backend) for _ in range(3)],
    }
    return {"rendererLab": renderer_lab.evidence(config), "outcome": {"status": "passed"}}


def test_manifest_records_no_speedup_claim_and_requires_visual_review() -> None:
    baseline = completed_manifest("remix")
    candidate = completed_manifest("native-hybrid", native=True)
    assert candidate["rendererLab"]["speedupClaim"] is None
    assert "stable motion" in (renderer_lab.comparison_problem(baseline, candidate) or "")
    for manifest in (baseline, candidate):
        manifest["rendererLab"]["visualReview"] = "accepted"
        manifest["rendererLab"]["runtimeQualityReview"] = "verified"
    assert renderer_lab.comparison_problem(baseline, candidate) is None
    altered = copy.deepcopy(candidate)
    altered["rendererLab"]["quality"]["frameGeneration"] = True
    assert "quality settings" in (renderer_lab.comparison_problem(baseline, altered) or "")
    altered = copy.deepcopy(candidate)
    altered["rendererLab"]["backendProofs"] = [proof("native-hybrid", active="remix", fallback=True)] * 3
    assert "fallback" in (renderer_lab.comparison_problem(baseline, altered) or "")


def test_three_successful_runs_are_required_for_comparison() -> None:
    baseline = completed_manifest("remix")
    candidate = completed_manifest("native-hybrid", native=True)
    for manifest in (baseline, candidate):
        manifest["rendererLab"]["visualReview"] = "accepted"
        manifest["rendererLab"]["runtimeQualityReview"] = "verified"
    candidate["rendererLab"]["backendProofs"].pop()
    assert "Three successful runs" in (renderer_lab.comparison_problem(baseline, candidate) or "")


@pytest.mark.parametrize("field,value", [("qualityComparable", False), ("unsupportedFeatures", ["water omitted"])])
def test_manual_visual_and_runtime_reviews_cannot_override_known_backend_gaps(field: str, value) -> None:
    baseline = completed_manifest("remix")
    candidate = completed_manifest("native-hybrid", native=True)
    for manifest in (baseline, candidate):
        manifest["rendererLab"]["visualReview"] = "accepted"
        manifest["rendererLab"]["runtimeQualityReview"] = "verified"
    for status in candidate["rendererLab"]["backendProofs"]:
        status[field] = value
    problem = renderer_lab.comparison_problem(baseline, candidate)
    assert problem is not None
    assert "qualityComparable=false" in problem or "unsupported features" in problem


def test_native_proof_retains_measured_window_and_full_frame_timing_without_warmup_veto() -> None:
    benchmark = native_benchmark()
    observed = recorded_native_proof(benchmark)
    assert observed["nativeFrameEvidence"] == benchmark["metadata"]["nativeFrameEvidence"]
    assert observed["recordFrames"] == 3
    assert observed["frameTimeSource"] == "MinecraftClient.render"
    assert observed["fullFrameTiming"] == {"samples": 3, "minNanos": 15_000_000, "maxNanos": 17_000_000}
    benchmark["metadata"]["nativeFrameEvidence"]["start"]["pendingFrames"] = 0
    assert observed["nativeFrameEvidence"]["start"]["pendingFrames"] == 10


def test_native_comparison_requires_benchmark_metadata_even_after_successful_native_status() -> None:
    with pytest.raises(RuntimeError, match="benchmark result metadata"):
        renderer_lab.backend_proof(
            renderer_lab.STATUS_PREFIX + json.dumps(proof("native-hybrid", native=True)), "native-hybrid"
        )


@pytest.mark.parametrize("value", [None, [], "unverified"])
def test_missing_or_malformed_native_window_is_rejected(value) -> None:
    benchmark = native_benchmark()
    benchmark["metadata"]["nativeFrameEvidence"] = value
    with pytest.raises(RuntimeError, match="missing or malformed"):
        recorded_native_proof(benchmark)


@pytest.mark.parametrize(
    ("field", "value"),
    [
        ("schema", True),
        ("schema", 2),
        ("source", "render attempts"),
        ("complete", False),
        ("eligibleForRenderedFps", "true"),
        ("eligibleForRenderedFps", False),
        ("capturedSamples", 2),
        ("samplesWithNewFrame", 2),
        ("samplesWithoutNewFrame", 1),
        ("samplesWithMultipleFrames", 1),
        ("start", None),
        ("end", []),
        ("delta", "3"),
    ],
)
def test_native_window_requires_complete_exactly_one_new_frame_per_recorded_sample(field: str, value) -> None:
    benchmark = native_benchmark()
    benchmark["metadata"]["nativeFrameEvidence"][field] = value
    with pytest.raises(RuntimeError, match="[Nn]ative|recorded Minecraft"):
        recorded_native_proof(benchmark)


@pytest.mark.parametrize(
    ("snapshot", "field", "value"),
    [
        ("end", "session", 8),
        ("start", "session", True),
        ("end", "lastGeneration", 1),
        ("end", "lastReadyFenceValue", 80),
        ("start", "lastGeneration", -2),
        ("end", "producedFrames", 84),
        ("delta", "renderAttempts", 4),
        ("delta", "producedFrames", 2),
        ("delta", "failedFrames", -1),
        ("end", "unreadyFrames", 6),
        ("end", "duplicateFrames", 4),
    ],
)
def test_native_window_reconciles_snapshots_session_fences_and_counter_deltas(snapshot: str, field: str, value) -> None:
    benchmark = native_benchmark()
    benchmark["metadata"]["nativeFrameEvidence"][snapshot][field] = value
    with pytest.raises(RuntimeError, match="[Nn]ative"):
        recorded_native_proof(benchmark)


def test_counter_deltas_cannot_hide_pending_or_failed_render_attempts() -> None:
    for field in ("pendingFrames", "unreadyFrames", "duplicateFrames", "failedFrames"):
        benchmark = native_benchmark()
        window = benchmark["metadata"]["nativeFrameEvidence"]
        window["end"][field] += 1
        window["delta"][field] = 1
        with pytest.raises(RuntimeError, match=field):
            recorded_native_proof(benchmark)


def test_new_native_resource_generation_does_not_require_an_old_fence_sequence() -> None:
    benchmark = native_benchmark()
    end = benchmark["metadata"]["nativeFrameEvidence"]["end"]
    end.update({"lastGeneration": 3, "lastReadyFenceValue": 2})
    assert recorded_native_proof(benchmark)["nativeFrameEvidence"]["end"]["lastGeneration"] == 3


@pytest.mark.parametrize("frames", [0, True, 3.0, "3"])
def test_recorded_frame_count_must_be_a_positive_integer(frames) -> None:
    benchmark = native_benchmark()
    benchmark["metadata"]["recordFrames"] = frames
    with pytest.raises(RuntimeError, match="recordFrames"):
        recorded_native_proof(benchmark)


@pytest.mark.parametrize(
    "samples",
    [[], [1, 2], [1, 0, 2], [1, float("nan"), 2], [1, float("inf"), 2], [1, True, 2]],
)
def test_native_window_requires_matching_positive_finite_full_frame_timings(samples) -> None:
    benchmark = native_benchmark()
    benchmark["perFrame"] = samples
    with pytest.raises(RuntimeError, match="Full-frame timing"):
        recorded_native_proof(benchmark)


def test_standalone_timings_or_a_stale_recorded_backend_cannot_pass_a_native_status() -> None:
    benchmark = native_benchmark()
    benchmark["metadata"]["frameTimeSource"] = "standalone DX12 GPU timestamps"
    with pytest.raises(RuntimeError, match="MinecraftClient.render"):
        recorded_native_proof(benchmark)
    benchmark = native_benchmark()
    benchmark["metadata"]["rendererBackend"]["qualityComparable"] = False
    with pytest.raises(RuntimeError, match="qualityComparable=false"):
        recorded_native_proof(benchmark)
    benchmark = native_benchmark()
    benchmark["metadata"]["rendererBackend"]["presentation"] = "native-dx12"
    with pytest.raises(RuntimeError, match="identity disagrees"):
        recorded_native_proof(benchmark)


@pytest.mark.parametrize("missing", [True, False])
def test_manual_reviews_cannot_override_missing_or_ineligible_stored_frame_window(missing: bool) -> None:
    baseline = completed_manifest("remix")
    candidate = completed_manifest("native-hybrid", native=True)
    for manifest in (baseline, candidate):
        manifest["rendererLab"]["visualReview"] = "accepted"
        manifest["rendererLab"]["runtimeQualityReview"] = "verified"
    for status in candidate["rendererLab"]["backendProofs"]:
        if missing:
            status.pop("nativeFrameEvidence")
        else:
            status["nativeFrameEvidence"]["eligibleForRenderedFps"] = False
    assert "Native frame evidence" in (renderer_lab.comparison_problem(baseline, candidate) or "")


def test_comparison_revalidates_stored_timing_count_and_counter_deltas() -> None:
    baseline = completed_manifest("remix")
    candidate = completed_manifest("native-hybrid", native=True)
    for manifest in (baseline, candidate):
        manifest["rendererLab"]["visualReview"] = "accepted"
        manifest["rendererLab"]["runtimeQualityReview"] = "verified"
    altered = copy.deepcopy(candidate)
    altered["rendererLab"]["backendProofs"][1]["fullFrameTiming"]["samples"] = 2
    assert "timing sample count" in (renderer_lab.comparison_problem(baseline, altered) or "")
    altered = copy.deepcopy(candidate)
    altered["rendererLab"]["backendProofs"][2]["nativeFrameEvidence"]["delta"]["producedFrames"] = 1
    assert "producedFrames" in (renderer_lab.comparison_problem(baseline, altered) or "")


@pytest.mark.parametrize("field", ["producedFrames", "lastReadyFenceValue", "session"])
def test_native_window_rejects_omitted_snapshot_fields(field: str) -> None:
    benchmark = native_benchmark()
    benchmark["metadata"]["nativeFrameEvidence"]["end"].pop(field)
    with pytest.raises(RuntimeError, match=field):
        recorded_native_proof(benchmark)


def test_malformed_backend_presentation_is_rejected_without_accepting_its_frame_window() -> None:
    native = proof("native-hybrid", native=True)
    native["presentation"] = ["minecraft-opengl"]
    with pytest.raises(RuntimeError, match="not an end-to-end"):
        renderer_lab.backend_proof(
            renderer_lab.STATUS_PREFIX + json.dumps(native), "native-hybrid", benchmark=native_benchmark()
        )
