#!/usr/bin/env python3
"""Verify real GPU command reuse logs against independent synthetic CPU stages.

This validator reads the archived Adreno and Mali execution logs, seven
source identities, six negative-control outcomes and seven compiled binary
hashes. It checks the first incorrect intermediate result with deterministic
per-submission CPU expectations and never starts ADB, Vulkan or hashcat.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import runpy


ROOT = Path(__file__).resolve().parents[1]
FIXTURES = (
    ROOT / "tests" / "fixtures" / "android-gpu"
    / "vulkan-resubmit-20261031"
)
MANIFEST = FIXTURES / "manifest.json"

FAILURE_LINES = (
    "PIPELINE=three-stage-command-reuse",
    "ERROR_CONTROLS=4",
    "NEGATIVE zero-submissions exit=2",
    "NEGATIVE too-many-submissions exit=2",
    "NEGATIVE non-numeric-submissions exit=2",
    "NEGATIVE missing-spirv-stage exit=2",
)


def sha256(path: Path) -> str:
    """Return SHA-256 of a required source, native binary, shader or log."""
    assert path.is_file(), f"Required Vulkan replay evidence missing: {path}"

    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_sources(manifest: dict) -> None:
    """Require source identity, toolchain selection and seven artifact digests."""
    assert manifest["observation_date"] == "2026-10-31"
    assert manifest["scope"] == (
        "actual Vulkan compute on public synthetic 32-bit data; "
        "no hashcat, password hashes, cryptographic GPU kernels "
        "or self-test bypass"
    )
    toolchain = manifest["toolchain"]

    assert toolchain == {
        "android_ndk": "29.0.14206865",
        "native_compiler": "Clang 21.0.0",
        "shader_compiler": "glslc shaderc v2022.3",
        "target": "aarch64-linux-android26",
        "spirv_target": "vulkan1.1",
    }

    sources = manifest["source_sha256"]
    expected = {
        "tools/vulkan-resubmit-diagnostic.c",
        "tools/build-vulkan-resubmit-diagnostic.ps1",
        "tools/run-vulkan-resubmit-diagnostic.ps1",
        "tools/test-vulkan-resubmit-reference.py",
        "tools/vulkan-pipeline-stage-a.comp",
        "tools/vulkan-pipeline-stage-b.comp",
        "tools/vulkan-pipeline-stage-c.comp",
    }

    assert set(sources) == expected

    for name, checksum in sources.items():
        assert re.fullmatch(r"[0-9a-f]{64}", checksum)
        assert sha256(ROOT / name) == checksum, name

    artifacts = manifest["build_artifact_sha256"]
    expected_binaries = {
        "vulkan-resubmit-arm64",
        *(
            f"stage-{stage}-{optimization}.spv"
            for stage in ("a", "b", "c")
            for optimization in ("O0", "O")
        ),
    }

    assert set(artifacts) == expected_binaries

    for checksum in artifacts.values():
        assert re.fullmatch(r"[0-9a-f]{64}", checksum)

    print("PASS seven exact Vulkan replay source identities and binary digests")


def verify_binaries(manifest: dict, directory: Path) -> None:
    """Verify all seven locally compiled artifacts against observed GPU runs."""
    assert directory.is_dir()

    for name, checksum in manifest["build_artifact_sha256"].items():
        assert sha256(directory / name) == checksum, name

    print("PASS seven local Android ARM64/SPIR-V binaries match device evidence")


def verify_workload(manifest: dict) -> tuple[dict[str, str], ...]:
    """Recompute all submission-specific stage checksums from scalar Python."""
    workload = manifest["workload"]

    assert workload == {
        "elements": 2048,
        "words_per_submission": 12288,
        "submissions_per_shader_optimization": 4,
        "optimizations": ["O0", "O"],
        "number_of_dispatches_per_submission": 3,
        "single_recorded_command_buffer": True,
        "command_buffer_one_time_submit_flag": False,
        "fence_reset_after_each_completed_submission": True,
        "host_input_changes_per_submission": True,
        "host_to_compute_barrier": 1,
        "compute_to_compute_barriers": 2,
        "compute_to_host_barrier": 1,
        "expected_checksums_by_submission": workload["expected_checksums_by_submission"],
    }

    reference = runpy.run_path(
        str(ROOT / "tools" / "test-vulkan-resubmit-reference.py")
    )
    expected = tuple(
        reference["submission_checksums"](index) for index in range(4)
    )

    assert list(expected) == workload["expected_checksums_by_submission"]
    assert expected == reference["EXPECTED"]
    assert len({entry["output"] for entry in expected}) == 4

    print(
        "PASS independent CPU oracle: four distinct 12288-word submissions "
        "(49152 words/optimization)"
    )

    return expected


def verify_run(
    run: dict, device: dict, expected: tuple[dict[str, str], ...],
) -> None:
    """Check every archived device result line including all replay stages."""
    assert set(run) == {
        "device", "optimization", "file", "exit_code", "sha256",
    }
    assert run["device"] in ("redmagic", "pixel")
    assert run["optimization"] in ("O0", "O")
    assert run["exit_code"] == 0

    name = f"{run['device']}-{run['optimization']}.log"

    assert run["file"] == name
    assert sha256(FIXTURES / name) == run["sha256"]

    lines = (FIXTURES / name).read_text(
        encoding="utf-8"
    ).splitlines()

    expected_lines = [
        "PIPELINE=three-stage-command-reuse",
        f"OPTIMIZATION={run['optimization']}",
        "SUBMISSIONS=4",
        "ADB_EXIT_CODE=0",
        (
            f"VULKAN_DEVICE name={device['gpu']} "
            f"vendor={device['vendor_id']} "
            f"device={device['device_id']} "
            f"driver={device['driver_version']} "
            f"api={device['vulkan_api_version']} "
            f"queue_family={device['queue_family']}"
        ),
        (
            "VULKAN_LIMITS workgroup_x=1024 invocations=1024 "
            f"shared_bytes={device['shared_memory_limit']}"
        ),
    ]

    for submission, entry in enumerate(expected):
        expected_lines.append(
            f"PASS Vulkan replay submission={submission}: "
            "2048 elements, 12288 correct 32-bit words; "
            f"stageA={entry['stage_a']} "
            f"stageB={entry['stage_b']} "
            f"output={entry['output']}"
        )

    expected_lines.append(
        "PASS Vulkan command reuse: 4 sequential submissions, "
        "3 dispatches/submission, 12288 verified words/submission"
    )

    assert lines == expected_lines, (
        f"Archived {name} differs from the CPU reference or GPU metadata"
    )

    print(
        f"PASS real GPU {run['device']} -{run['optimization']}: "
        "4 distinct re-submissions, 12 dispatches, 49152 verified words"
    )


def verify_negative_controls(manifest: dict) -> None:
    """Require each device to reject three invalid counts and absent SPIR-V."""
    entries = manifest["rejected_inputs"]

    assert len(entries) == 2
    assert {entry["device"] for entry in entries} == {"redmagic", "pixel"}

    for entry in entries:
        assert set(entry) == {
            "device", "file", "error_count", "expected_exit_code", "sha256",
        }
        assert entry["error_count"] == 4
        assert entry["expected_exit_code"] == 2
        assert entry["file"] == f"{entry['device']}-negative-controls.log"
        assert sha256(FIXTURES / entry["file"]) == entry["sha256"]

        lines = (FIXTURES / entry["file"]).read_text(
            encoding="utf-8"
        ).splitlines()

        assert tuple(lines) == FAILURE_LINES

        print(f"PASS {entry['device']}: four expected input failures, exit 2")


def verify_negative_control(
    manifest: dict, expected: tuple[dict[str, str], ...],
) -> None:
    """Require an altered stage-B CPU reference to invalidate recorded output."""
    first = manifest["real_gpu_runs"][0]
    altered = [dict(entry) for entry in expected]
    altered[2]["stage_b"] = "0000000000000000"

    try:
        verify_run(
            first, manifest["device_profiles"][first["device"]],
            tuple(altered),
        )
    except AssertionError:
        print("PASS validator rejects incorrect stage B during submission 2")
    else:
        raise AssertionError("Evidence validator accepted incorrect GPU stage B")


def main() -> None:
    """Validate all replayed-device records and optional exact local binaries."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--artifacts", type=Path,
        help="Explicit path to seven locally built ARM64 and SPIR-V files",
    )
    arguments = parser.parse_args()
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))

    assert "adb_serial" not in MANIFEST.read_text(encoding="utf-8")

    verify_sources(manifest)

    if arguments.artifacts is not None:
        verify_binaries(
            manifest, arguments.artifacts.resolve(strict=True)
        )

    expected = verify_workload(manifest)
    devices = manifest["device_profiles"]

    assert set(devices) == {"redmagic", "pixel"}
    assert devices["redmagic"]["gpu"] == "Adreno (TM) 730"
    assert devices["pixel"]["gpu"] == "Mali-G715"

    for device in devices.values():
        assert device["queue_family"] == 0
        assert device["shared_memory_limit"] == 32768

    runs = manifest["real_gpu_runs"]

    assert len(runs) == 4
    assert {
        (entry["device"], entry["optimization"]) for entry in runs
    } == {
        (device, optimization)
        for device in ("redmagic", "pixel")
        for optimization in ("O0", "O")
    }
    assert {path.name for path in FIXTURES.glob("*.log")} == {
        *(entry["file"] for entry in runs),
        *(entry["file"] for entry in manifest["rejected_inputs"]),
    }

    for entry in runs:
        verify_run(entry, devices[entry["device"]], expected)

    verify_negative_controls(manifest)
    verify_negative_control(manifest, expected)

    print(
        "PASS all four real GPU replay logs, both device failure controls, "
        "and independent per-stage CPU results; NO GPU EXECUTION BY VALIDATOR"
    )


if __name__ == "__main__":
    main()
