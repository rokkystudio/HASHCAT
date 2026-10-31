#!/usr/bin/env python3
"""Validate archived Vulkan GPU executions against independent CPU references.

The validator checks the exact source checksums, eight recorded device logs,
driver metadata, exit codes, workgroup limits, and expected arithmetic
checksums. It reads archived evidence only and never starts Android or Vulkan.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import runpy


ROOT = Path(__file__).resolve().parents[1]
FIXTURES = (
    ROOT / "tests" / "fixtures" / "android-gpu" / "vulkan-compute-20261031"
)
MANIFEST = FIXTURES / "manifest.json"


def digest(path: Path) -> str:
    """Compute the exact SHA-256 of one required tracked source or log file."""
    assert path.is_file(), f"Required Vulkan evidence missing: {path}"

    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_source_identity(manifest: dict) -> None:
    """Check source digests and recorded standalone compilation artifacts."""
    assert manifest["observation_date"] == "2026-10-31"
    assert manifest["toolchain"]["host_target"] == "aarch64-linux-android26"
    assert manifest["toolchain"]["target_environment"] == "vulkan1.1"

    source_hashes = manifest["source_sha256"]

    assert set(source_hashes) == {
        "tools/vulkan-compute-diagnostic.c",
        "tools/vulkan-compute-diagnostic.comp",
        "tools/vulkan-compute-shared32k.comp",
    }

    for name, expected in source_hashes.items():
        assert re.fullmatch(r"[0-9a-f]{64}", expected)
        assert digest(ROOT / name) == expected, name

    artifacts = manifest["build_artifact_sha256"]

    assert set(artifacts) == {
        "vulkan-compute-diagnostic-arm64",
        "vulkan-compute-O0.spv",
        "vulkan-compute-O.spv",
        "vulkan-compute-shared32k-O0.spv",
        "vulkan-compute-shared32k-O.spv",
    }

    for digest_value in artifacts.values():
        assert re.fullmatch(r"[0-9a-f]{64}", digest_value)

    print("PASS Vulkan diagnostic source identity and five recorded build artifacts")


def verify_workload(manifest: dict) -> dict[str, str]:
    """Compute both public synthetic CPU digests and verify workload metadata."""
    workload = manifest["synthetic_workload"]

    assert workload["elements"] == 2048
    assert workload["u32_outputs"] == 8192
    assert workload["local_group_size"] == 32
    assert workload["group_count"] == 64
    assert workload["baseline_shared_bytes"] == 128
    assert workload["shared32k_shared_bytes"] == 32768
    assert workload["shared32k_cross_lane_reads_per_thread"] == 256

    reference = runpy.run_path(
        str(ROOT / "tools" / "test-vulkan-compute-reference.py")
    )
    expected_checksums = workload["expected_checksum_fnv1a64"]

    assert set(expected_checksums) == {"baseline", "shared32k"}

    for mode, value in expected_checksums.items():
        actual = reference["reference_checksum"](mode == "shared32k")
        assert actual == value, (
            f"CPU reference does not match archived {mode} checksum"
        )

    print("PASS independently computed baseline and 32 KiB CPU checksums")

    return expected_checksums


def verify_recorded_run(
    run: dict,
    device: dict,
    expected_checksums: dict[str, str],
) -> None:
    """Validate all six archived lines against exact device and shader data."""
    assert set(run) == {
        "device", "mode", "optimization", "file", "sha256", "exit_code",
    }
    assert run["mode"] in ("baseline", "shared32k")
    assert run["optimization"] in ("O0", "O")
    assert run["exit_code"] == 0

    filename = (
        f"{run['device']}-{run['mode']}-{run['optimization']}.log"
    )

    assert run["file"] == filename
    assert digest(FIXTURES / filename) == run["sha256"]

    lines = (FIXTURES / filename).read_text(
        encoding="utf-8"
    ).splitlines()

    assert len(lines) == 6, (filename, lines)
    assert lines == [
        f"MODE={run['mode']}",
        f"OPTIMIZATION={run['optimization']}",
        "ADB_EXIT_CODE=0",
        (
            f"VULKAN_DEVICE name={device['gpu']} "
            f"vendor={device['vendor_id']} "
            f"device={device['device_id']} "
            f"driver={device['driver_version']} "
            f"api={device['vulkan_api_version']} queue_family=0"
        ),
        (
            f"VULKAN_LIMITS workgroup_x="
            f"{device['max_compute_work_group_size_x']} "
            f"invocations={device['max_compute_work_group_invocations']} "
            f"shared_bytes={device['max_compute_shared_memory_size']}"
        ),
        (
            f"PASS Vulkan Compute mode={run['mode']}: "
            "2048 elements, 8192 correct 32-bit words; "
            f"FNV1A64={expected_checksums[run['mode']]}"
        ),
    ]

    print(
        f"PASS archived {run['device']} Vulkan "
        f"{run['mode']} -{run['optimization']}: "
        f"{expected_checksums[run['mode']]}"
    )


def main() -> None:
    """Validate all eight recorded real-device runs without executing a GPU."""
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))

    assert "adb_serial" not in manifest
    assert "no cryptographic workload" in manifest["scope"]

    verify_source_identity(manifest)
    checksums = verify_workload(manifest)
    devices = manifest["device_profiles"]

    assert set(devices) == {"redmagic", "pixel"}
    assert devices["redmagic"]["gpu"] == "Adreno (TM) 730"
    assert devices["pixel"]["gpu"] == "Mali-G715"

    for device in devices.values():
        assert device["max_compute_shared_memory_size"] == 32768

    runs = manifest["runs"]
    expected = {
        (device, mode, optimization)
        for device in ("redmagic", "pixel")
        for mode in ("baseline", "shared32k")
        for optimization in ("O0", "O")
    }

    assert len(runs) == len(expected)
    assert {
        (run["device"], run["mode"], run["optimization"])
        for run in runs
    } == expected
    assert {
        path.name for path in FIXTURES.glob("*.log")
    } == {run["file"] for run in runs}

    for run in runs:
        verify_recorded_run(
            run, devices[run["device"]], checksums
        )

    print("PASS eight archived GPU executions verified; NO GPU EXECUTION BY VALIDATOR")


if __name__ == "__main__":
    main()
