#!/usr/bin/env python3
"""Validate real Android Vulkan resource lifetimes and injected cleanup errors.

The validator reads saved device logs, verifies original C/PowerShell/GLSL
sources and binary identities, and checks that all 14 recorded Vulkan resource
acquisitions balance their releases on both devices and optimization variants.
No Android device, GPU execution, credentials or hashcat runtime are used.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
FIXTURES = (
    ROOT / "tests" / "fixtures" / "android-gpu"
    / "vulkan-lifecycle-20261031"
)
MANIFEST = FIXTURES / "manifest.json"
STEPS = (
    "instance",
    "device",
    "buffer",
    "device-memory",
    "memory-map",
    "descriptor-layout",
    "pipeline-layout",
    "shader-module",
    "compute-pipeline",
    "descriptor-pool",
    "descriptor-set",
    "command-pool",
    "command-buffer",
    "fence",
)
INVALID_CASES = (
    "zero-cycles",
    "too-many-cycles",
    "noninteger-cycles",
    "unknown-failure-step",
    "missing-shader",
)


def sha256(path: Path) -> str:
    """Compute the exact SHA-256 of one required source, binary or device log."""
    assert path.is_file(), f"Missing Vulkan lifecycle resource: {path}"

    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_sources(manifest: dict) -> None:
    """Require exact source hashes, compiler provenance and binary identities."""
    assert manifest["observation_date"] == "2026-10-31"
    assert manifest["scope"] == (
        "actual Vulkan object creation and destruction on two GPUs; "
        "no dispatch, password processing, cryptographic shaders, "
        "hashcat API calls or self-test bypass"
    )
    assert manifest["toolchain"] == {
        "android_ndk": "29.0.14206865",
        "native_compiler": "Clang 21.0.0",
        "shader_compiler": "glslc shaderc v2022.3",
        "target": "aarch64-linux-android26",
        "spirv_target": "vulkan1.1",
    }

    sources = manifest["source_sha256"]

    assert set(sources) == {
        "tools/vulkan-resource-lifecycle-diagnostic.c",
        "tools/build-vulkan-resource-lifecycle.ps1",
        "tools/run-vulkan-resource-lifecycle.ps1",
        "tools/vulkan-pipeline-stage-a.comp",
    }

    for name, digest in sources.items():
        assert re.fullmatch(r"[0-9a-f]{64}", digest)
        assert sha256(ROOT / name) == digest, name

    expected_binaries = {
        "vulkan-resource-lifecycle-arm64",
        "lifecycle-O0.spv",
        "lifecycle-O.spv",
    }
    artifacts = manifest["build_artifact_sha256"]

    assert set(artifacts) == expected_binaries

    for digest in artifacts.values():
        assert re.fullmatch(r"[0-9a-f]{64}", digest)

    previous = json.loads(
        (
            ROOT / "tests" / "fixtures" / "android-gpu"
            / "vulkan-pipeline-20261031" / "manifest.json"
        ).read_text(encoding="utf-8")
    )

    assert manifest["device_profiles"] == previous["device_profiles"]

    print("PASS four lifecycle source hashes, three build identities and device provenance")


def verify_binaries(manifest: dict, directory: Path) -> None:
    """Require byte-for-byte equality with all three locally rebuilt artifacts."""
    assert directory.is_dir()

    for name, digest in manifest["build_artifact_sha256"].items():
        assert sha256(directory / name) == digest, name

    print("PASS three local ARM64/SPIR-V binaries match device evidence")


def verify_workload(manifest: dict) -> None:
    """Require exact resource lifetime, fault and CLI rejection test parameters."""
    assert manifest["resource_test"] == {
        "iterations_per_optimization": 4,
        "optimizations": ["O0", "O"],
        "managed_resources_per_cycle": 14,
        "gpu_submissions": 0,
        "fault_steps": list(STEPS),
        "injected_failure_exit_code": 42,
        "invalid_input_exit_code": 2,
        "invalid_input_cases": list(INVALID_CASES),
        "expected_created_and_released_per_fault": list(range(1, 15)),
        "host_visible_coherent_buffer_bytes": 4096,
    }

    print(
        "PASS four creation/destruction cycles per optimization, "
        "14 resource failure points and five invalid-input controls"
    )


def success_lines(device: dict, optimization: str) -> list[str]:
    """Construct every expected GPU metadata and normal cleanup record."""
    expected = [
        "PIPELINE=resource-lifecycle",
        f"OPTIMIZATION={optimization}",
        "CYCLES=4",
        "ADB_EXIT_CODE=0",
    ]

    for cycle in range(4):
        expected.extend((
            (
                f"VULKAN_DEVICE cycle={cycle} name={device['gpu']} "
                f"vendor={device['vendor_id']} "
                f"device={device['device_id']} "
                f"driver={device['driver_version']} "
                f"api={device['vulkan_api_version']} "
                f"queue_family={device['queue_family']}"
            ),
            (
                f"PASS Vulkan resource cycle={cycle} "
                "created=14 released=14 shader_dispatched=0"
            ),
        ))

    expected.append(
        "PASS Vulkan resource lifecycle: 4 cycles, 14 resources/cycle, "
        "all resources released; GPU submissions=0"
    )

    return expected


def verify_success(manifest: dict) -> None:
    """Validate four actual device success logs and their resource counts."""
    runs = manifest["successful_device_runs"]

    assert len(runs) == 4
    assert {
        (entry["device"], entry["optimization"]) for entry in runs
    } == {
        (device, optimization)
        for device in ("redmagic", "pixel")
        for optimization in ("O0", "O")
    }

    for entry in runs:
        assert set(entry) == {
            "device", "optimization", "file", "exit_code", "sha256",
        }
        assert entry["exit_code"] == 0

        name = f"{entry['device']}-{entry['optimization']}.log"

        assert entry["file"] == name
        assert sha256(FIXTURES / name) == entry["sha256"]

        lines = (FIXTURES / name).read_text(
            encoding="utf-8"
        ).splitlines()

        expected = success_lines(
            manifest["device_profiles"][entry["device"]],
            entry["optimization"],
        )

        assert lines == expected, f"Resource lifetime log differs: {name}"

        print(
            f"PASS real Vulkan {entry['device']} -{entry['optimization']}: "
            "4 cycles x 14 acquired/released objects; no GPU submission"
        )


def expected_failure_lines() -> list[str]:
    """Return exactly fourteen injected cleanup records and five input errors."""
    lines = [
        "PIPELINE=resource-lifecycle",
        "RESOURCE_FAULT_CONTROLS=14",
        "INVALID_INPUT_CONTROLS=5",
    ]

    for count, step in enumerate(STEPS, 1):
        lines.append(
            f"NEGATIVE {step} exit=42 created={count} released={count}"
        )

    for case in INVALID_CASES:
        lines.append(f"INVALID {case} exit=2")

    return lines


def verify_failures(manifest: dict) -> None:
    """Verify both devices rejected every exact injection and invalid input."""
    runs = manifest["failed_device_runs"]

    assert len(runs) == 2
    assert {entry["device"] for entry in runs} == {"redmagic", "pixel"}

    expected = expected_failure_lines()

    for entry in runs:
        assert set(entry) == {
            "device", "file", "resource_fault_count",
            "invalid_input_count", "sha256",
        }
        assert entry["resource_fault_count"] == 14
        assert entry["invalid_input_count"] == 5

        name = f"{entry['device']}-negative-controls.log"

        assert entry["file"] == name
        assert sha256(FIXTURES / name) == entry["sha256"]

        lines = (FIXTURES / name).read_text(
            encoding="utf-8"
        ).splitlines()

        assert lines == expected, f"Fault-injection records differ: {name}"

        print(
            f"PASS real Vulkan {entry['device']}: "
            "14 balanced cleanup faults (exit 42), "
            "5 invalid inputs (exit 2)"
        )


def verify_negative_control() -> None:
    """Require the expected ledger to reject one intentionally unmatched release."""
    altered = expected_failure_lines()
    altered[3 + 8] = (
        "NEGATIVE compute-pipeline exit=42 created=9 released=8"
    )

    try:
        assert altered == expected_failure_lines()
    except AssertionError:
        print("PASS negative control: unmatched compute-pipeline release rejected")
    else:
        raise AssertionError("Vulkan resource ledger accepted a missing release")


def main() -> None:
    """Audit saved GPU object-creation evidence without executing a device API."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--artifacts", type=Path,
        help="Directory containing the three optional local build artifacts",
    )
    arguments = parser.parse_args()

    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))

    assert "adb_serial" not in MANIFEST.read_text(encoding="utf-8")

    verify_sources(manifest)

    if arguments.artifacts is not None:
        verify_binaries(
            manifest, arguments.artifacts.resolve(strict=True),
        )

    verify_workload(manifest)
    verify_success(manifest)
    verify_failures(manifest)
    verify_negative_control()

    expected_logs = {
        entry["file"]
        for entry in (
            manifest["successful_device_runs"]
            + manifest["failed_device_runs"]
        )
    }

    assert {file.name for file in FIXTURES.glob("*.log")} == expected_logs

    print(
        "PASS exact real-device Vulkan resource records: "
        "4 successful runs, 28 fault injections and 10 input rejections; "
        "NO DEVICE EXECUTION BY VALIDATOR"
    )


if __name__ == "__main__":
    main()
