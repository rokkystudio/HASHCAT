#!/usr/bin/env python3
"""Audit archived real-device three-stage Vulkan runs against CPU arithmetic.

The validator checks original GLSL/C/Python source digests, build provenance,
device identity, buffer/dispatch metadata, exact recorded logs and independent
per-stage CPU checksums. It never starts Vulkan, ADB, hashcat or a GPU kernel.
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
    / "vulkan-pipeline-20261031"
)
MANIFEST = FIXTURES / "manifest.json"


def sha256(path: Path) -> str:
    """Hash the exact bytes of one required source or archived GPU log."""
    assert path.is_file(), f"Missing Vulkan pipeline evidence: {path}"

    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_sources(manifest: dict) -> None:
    """Require unchanged tracked synthetic sources and seven build digests."""
    assert manifest["observation_date"] == "2026-10-31"
    assert "no password" in manifest["scope"]
    assert manifest["toolchain"]["target"] == "aarch64-linux-android26"
    assert manifest["toolchain"]["spirv_target"] == "vulkan1.1"

    source_hashes = manifest["source_sha256"]
    expected_sources = {
        "tools/vulkan-multistage-diagnostic.c",
        "tools/vulkan-pipeline-stage-a.comp",
        "tools/vulkan-pipeline-stage-b.comp",
        "tools/vulkan-pipeline-stage-c.comp",
        "tools/test-vulkan-multistage-reference.py",
    }

    assert set(source_hashes) == expected_sources

    for name, expected_digest in source_hashes.items():
        assert re.fullmatch(r"[0-9a-f]{64}", expected_digest)
        assert sha256(ROOT / name) == expected_digest, name

    expected_binaries = {
        "vulkan-multistage-arm64",
        *(
            f"stage-{stage}-{optimization}.spv"
            for stage in ("a", "b", "c")
            for optimization in ("O0", "O")
        ),
    }
    binary_hashes = manifest["build_artifact_sha256"]

    assert set(binary_hashes) == expected_binaries

    for digest in binary_hashes.values():
        assert re.fullmatch(r"[0-9a-f]{64}", digest)

    print(
        "PASS original three-stage C/GLSL/Python sources and seven "
        "recorded ARM64/SPIR-V build identities"
    )


def verify_build_artifacts(manifest: dict, directory: Path) -> None:
    """Check all seven explicitly supplied local binaries against the log manifest."""
    assert directory.is_dir(), f"Vulkan build directory missing: {directory}"

    binaries = manifest["build_artifact_sha256"]

    for name, expected in binaries.items():
        path = directory / name
        observed = sha256(path)

        assert observed == expected, (
            f"Vulkan build artifact differs from archived execution: {path}"
        )

    print("PASS seven locally compiled ARM64/SPIR-V binaries match actual GPU logs")


def verify_workload(manifest: dict) -> dict[str, str]:
    """Calculate each public CPU stage independently and verify dispatch facts."""
    workload = manifest["workload"]

    assert workload["element_count"] == 2048
    assert workload["workgroup_count"] == 64
    assert workload["local_size"] == 32
    assert workload["dispatch_count"] == 3
    assert workload["storage_buffers"] == 4
    assert workload["intermediate_words_per_stage"] == 2048
    assert workload["final_words"] == 8192
    assert workload["verified_words_per_run"] == 12288
    assert workload["compute_to_compute_barriers"] == 2
    assert workload["compute_to_host_barriers"] == 1

    oracle = runpy.run_path(
        str(ROOT / "tools" / "test-vulkan-multistage-reference.py")
    )
    calculated = oracle["reference"]()
    expected = workload["expected_stage_checksums"]

    assert set(expected) == {"stage_a", "stage_b", "output"}
    assert calculated == expected, (
        f"Stage-by-stage Python CPU oracle differs: {calculated} vs {expected}"
    )

    print("PASS independent CPU oracle: 2048 + 2048 + 8192 synthetic words")

    return expected


def verify_run(
    run: dict,
    device: dict,
    reference: dict[str, str],
) -> None:
    """Check a single actual-device execution log and all three stage digests."""
    assert set(run) == {
        "device", "optimization", "file", "exit_code", "sha256",
    }
    assert run["device"] in ("redmagic", "pixel")
    assert run["optimization"] in ("O0", "O")
    assert run["exit_code"] == 0

    name = f"{run['device']}-{run['optimization']}.log"

    assert run["file"] == name
    assert sha256(FIXTURES / name) == run["sha256"]

    lines = (FIXTURES / name).read_text(encoding="utf-8").splitlines()

    assert lines == [
        "PIPELINE=three-stage",
        f"OPTIMIZATION={run['optimization']}",
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
        (
            "PASS Vulkan 3-stage pipeline: 2048 elements, "
            "12288 correct 32-bit words; "
            f"stageA={reference['stage_a']} "
            f"stageB={reference['stage_b']} "
            f"output={reference['output']}"
        ),
    ], (name, lines)

    print(
        f"PASS archived {run['device']} -{run['optimization']}: "
        f"stageA={reference['stage_a']} stageB={reference['stage_b']} "
        f"output={reference['output']}"
    )


def verify_negative_control(manifest: dict, reference: dict[str, str]) -> None:
    """Require the evidence checker to reject an incorrect intermediate stage."""
    entry = manifest["runs"][0]
    device = manifest["device_profiles"][entry["device"]]
    modified = dict(reference)
    modified["stage_b"] = "0000000000000000"

    try:
        verify_run(entry, device, modified)
    except AssertionError:
        print("PASS negative control: incorrect intermediate stage B rejected")
    else:
        raise AssertionError(
            "Vulkan pipeline evidence checker accepted incorrect stage B"
        )


def main() -> None:
    """Verify four GPU archives and optionally seven exact local build outputs."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--artifacts", type=Path,
        help="Explicit directory of the seven locally compiled Vulkan artifacts",
    )
    args = parser.parse_args()
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))

    assert "adb_serial" not in MANIFEST.read_text(encoding="utf-8")
    assert manifest["scope"] == (
        "real Vulkan compute execution using public synthetic integer data only; "
        "no password, hashcat or cryptographic GPU code"
    )

    verify_sources(manifest)

    if args.artifacts is not None:
        verify_build_artifacts(manifest, args.artifacts.resolve(strict=True))

    reference = verify_workload(manifest)
    devices = manifest["device_profiles"]

    assert set(devices) == {"redmagic", "pixel"}
    assert devices["redmagic"]["gpu"] == "Adreno (TM) 730"
    assert devices["pixel"]["gpu"] == "Mali-G715"

    for device in devices.values():
        assert device["shared_memory_limit"] == 32768
        assert device["queue_family"] == 0

    runs = manifest["runs"]
    expected_runs = {
        (device, optimization)
        for device in ("redmagic", "pixel")
        for optimization in ("O0", "O")
    }

    assert len(runs) == len(expected_runs)
    assert {
        (run["device"], run["optimization"])
        for run in runs
    } == expected_runs
    assert {
        path.name for path in FIXTURES.glob("*.log")
    } == {run["file"] for run in runs}

    for run in runs:
        verify_run(run, devices[run["device"]], reference)

    verify_negative_control(manifest, reference)

    print(
        "PASS four real GPU three-stage logs and intermediate checksums; "
        "NO DEVICE EXECUTION BY VALIDATOR"
    )


if __name__ == "__main__":
    main()
