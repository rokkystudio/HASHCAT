#!/usr/bin/env python3
"""Verify archived Vulkan shared-allocation GPU output and memory error paths.

The validator checks the exact source and compiled binary hashes, Vulkan
buffer alignment and memory-type identities, intermediate GPU output
checksums against a separate scalar CPU oracle, and synthetic host failures.
It never invokes Android, Vulkan, GPU compilation, or hashcat.
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
    / "vulkan-suballocation-20261031"
)
MANIFEST = FIXTURES / "manifest.json"
BUFFER_NAMES = ("input", "stage-a", "stage-b", "output")
MODES = ("O0", "O")
DEVICES = ("redmagic", "pixel")
FAULT_LINES = (
    "TEST=shared-allocation",
    "SIMULATED_ERRORS=3",
    "INVALID_INPUTS=3",
    "SIMULATED memory-type exit=42 vkresult=-8 allocations_freed=0 buffers_bound=0",
    "SIMULATED allocation exit=43 vkresult=-2 allocations_freed=0 buffers_bound=0",
    "SIMULATED device-lost exit=44 vkresult=-4 allocations_freed=1 buffers_bound=4",
    "INVALID invalid-simulation exit=2",
    "INVALID missing-shader exit=2",
    "INVALID missing-simulate-flag exit=2",
)
LAYOUT_PATTERN = re.compile(
    r"^BUFFER_LAYOUT name=(input|stage-a|stage-b|output) "
    r"bytes=(\d+) required=(\d+) alignment=(\d+) "
    r"offset=(\d+) end=(\d+) memory_type_bits=(0x[0-9a-f]{8})$"
)


def sha256(path: Path) -> str:
    """Compute SHA-256 of exactly one required source, binary or GPU log."""
    assert path.is_file(), f"Vulkan suballocation evidence missing: {path}"

    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_source_identity(manifest: dict) -> None:
    """Check tracked sources, shader provenance and all seven binary hashes."""
    assert manifest["observation_date"] == "2026-10-31"
    assert manifest["scope"] == (
        "real 3-stage Vulkan arithmetic with four storage buffers bound "
        "into one allocation; three simulated host error paths; "
        "no hashcat, password or cryptographic GPU work"
    )
    assert manifest["toolchain"] == {
        "android_ndk": "29.0.14206865",
        "native_compiler": "Clang 21.0.0",
        "shader_compiler": "glslc shaderc v2022.3",
        "target": "aarch64-linux-android26",
        "spirv_target": "vulkan1.1",
    }

    sources = manifest["source_sha256"]
    expected_sources = {
        "tools/vulkan-suballocation-diagnostic.c",
        "tools/build-vulkan-suballocation-diagnostic.ps1",
        "tools/run-vulkan-suballocation-diagnostic.ps1",
        "tools/test-vulkan-multistage-reference.py",
        *(
            f"tools/vulkan-pipeline-stage-{stage}.comp"
            for stage in ("a", "b", "c")
        ),
    }

    assert set(sources) == expected_sources

    for name, checksum in sources.items():
        assert re.fullmatch(r"[0-9a-f]{64}", checksum)
        assert sha256(ROOT / name) == checksum, name

    binaries = manifest["build_artifact_sha256"]
    expected_binaries = {
        "vulkan-suballocation-arm64",
        *(
            f"stage-{stage}-{optimization}.spv"
            for stage in ("a", "b", "c")
            for optimization in MODES
        ),
    }

    assert set(binaries) == expected_binaries

    for checksum in binaries.values():
        assert re.fullmatch(r"[0-9a-f]{64}", checksum)

    previous = json.loads(
        (
            ROOT / "tests" / "fixtures" / "android-gpu"
            / "vulkan-pipeline-20261031" / "manifest.json"
        ).read_text(encoding="utf-8")
    )

    assert manifest["device_profiles"] == previous["device_profiles"]

    for name, checksum in binaries.items():
        if name.endswith(".spv"):
            assert checksum == previous["build_artifact_sha256"][name]

    print("PASS exact Vulkan/CPU source identity, driver profiles and six original shaders")


def verify_build_artifacts(manifest: dict, directory: Path) -> None:
    """Require locally rebuilt ARM64 and SPIR-V binaries to match archived runs."""
    assert directory.is_dir()

    for name, checksum in manifest["build_artifact_sha256"].items():
        assert sha256(directory / name) == checksum, name

    print("PASS all seven local ARM64/SPIR-V artifacts match actual GPU evidence")


def verify_workload(manifest: dict) -> dict[str, str]:
    """Validate allocation constraints and independent synthetic stage digests."""
    workload = manifest["workload"]

    assert workload == {
        "elements": 2048,
        "workgroups": 64,
        "local_size": 32,
        "compute_stages": 3,
        "number_of_buffers": 4,
        "number_of_vk_device_memory_allocations": 1,
        "one_allocation_limit_bytes": 1048576,
        "buffer_sizes_bytes": [8192, 8192, 8192, 32768],
        "expected_buffer_offsets_bytes": [0, 8192, 16384, 24576],
        "expected_buffer_ends_bytes": [8192, 16384, 24576, 57344],
        "expected_checksums": workload["expected_checksums"],
        "gpu_words_checked_per_run": 12288,
    }

    reference = runpy.run_path(
        str(ROOT / "tools" / "test-vulkan-multistage-reference.py")
    )
    expected = reference["reference"]()

    assert workload["expected_checksums"] == expected

    assert manifest["error_handling"] == {
        "simulated_failure_codes": {
            "memory-type": 42, "allocation": 43, "device-lost": 44,
        },
        "simulated_vkresults": {
            "memory-type": -8, "allocation": -2, "device-lost": -4,
        },
        "actual_device_loss_caused": False,
        "device_lost_injection_after_successful_fence_wait": True,
        "invalid_argument_codes": {
            "invalid-simulation": 2,
            "missing-shader": 2,
            "missing-simulate-flag": 2,
        },
    }

    print("PASS one-allocation safety bound and independently computed GPU stage digests")

    return expected


def verify_gpu_lines(
    lines: list[str],
    device: dict,
    memory: dict,
    optimization: str,
    checksums: dict[str, str],
) -> None:
    """Check exact GPU device metadata, four disjoint ranges and output words."""
    assert lines[:5] == [
        "TEST=shared-allocation",
        f"OPTIMIZATION={optimization}",
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

    assert len(lines) == 13, (len(lines), lines)

    expected_sizes = (8192, 8192, 8192, 32768)
    expected_offsets = (0, 8192, 16384, 24576)
    cursor = 0
    common_types = 0xFFFFFFFF

    for index, name in enumerate(BUFFER_NAMES):
        match = LAYOUT_PATTERN.fullmatch(lines[5 + index])

        assert match is not None, (name, lines[5 + index])
        groups = match.groups()
        assert groups[0] == name

        size, required, alignment, offset, end = (
            int(value) for value in groups[1:6]
        )
        bits = int(groups[6], 16)

        assert size == expected_sizes[index]
        assert required >= size
        assert alignment == memory["required_alignment_bytes"]
        assert alignment > 0 and alignment & (alignment - 1) == 0
        assert offset >= cursor
        assert offset % alignment == 0
        assert offset == expected_offsets[index]
        assert end == offset + required
        assert end <= 1048576
        assert groups[6] == memory["memory_type_bits"]

        common_types &= bits
        cursor = end

    assert cursor == memory["memory_allocation_bytes"] == 57344
    assert common_types == int(memory["memory_type_bits"], 16)
    assert common_types & (1 << memory["selected_memory_type"])

    assert lines[9] == (
        f"MEMORY_PLAN bytes={cursor} "
        f"memory_type={memory['selected_memory_type']} "
        f"buffers=4 compatible_bits={memory['memory_type_bits']}"
    )
    assert lines[10] == (
        "MEMORY_ALLOCATION allocations=1 "
        f"bytes={cursor} bound_buffers=4 "
        f"memory_type={memory['selected_memory_type']}"
    )
    assert lines[11] == (
        "PASS Vulkan 3-stage pipeline: 2048 elements, "
        "12288 correct 32-bit words; "
        f"stageA={checksums['stage_a']} "
        f"stageB={checksums['stage_b']} "
        f"output={checksums['output']}"
    )
    assert lines[12] == (
        "CLEANUP_SHARED_MEMORY allocations_freed=1 buffers_bound=4 "
        "simulated_device_lost=0 final_status=0"
    )


def verify_device_runs(manifest: dict, checksums: dict[str, str]) -> None:
    """Validate all four real GPU executions and actual shared memory layouts."""
    runs = manifest["gpu_runs"]

    assert len(runs) == 4
    assert {
        (entry["device"], entry["optimization"]) for entry in runs
    } == {(device, mode) for device in DEVICES for mode in MODES}

    for entry in runs:
        assert set(entry) == {
            "device", "optimization", "file", "exit_code", "sha256",
        }
        assert entry["exit_code"] == 0

        name = f"{entry['device']}-{entry['optimization']}.log"

        assert entry["file"] == name
        assert sha256(FIXTURES / name) == entry["sha256"]

        lines = (FIXTURES / name).read_text(encoding="utf-8").splitlines()

        verify_gpu_lines(
            lines,
            manifest["device_profiles"][entry["device"]],
            manifest["memory_profiles"][entry["device"]],
            entry["optimization"],
            checksums,
        )

        print(
            f"PASS actual GPU {entry['device']} -{entry['optimization']}: "
            "4 correctly aligned buffers, 1 Vulkan allocation, "
            "12288 correct intermediate/final values"
        )


def verify_error_controls(manifest: dict) -> None:
    """Check explicit simulated error exits and malformed input rejections."""
    runs = manifest["error_control_runs"]

    assert len(runs) == 2
    assert {entry["device"] for entry in runs} == set(DEVICES)

    for entry in runs:
        assert set(entry) == {
            "device", "file", "simulated_errors", "invalid_arguments",
            "sha256",
        }
        assert entry["simulated_errors"] == 3
        assert entry["invalid_arguments"] == 3
        name = f"{entry['device']}-negative-controls.log"

        assert entry["file"] == name
        assert sha256(FIXTURES / name) == entry["sha256"]

        lines = (FIXTURES / name).read_text(encoding="utf-8").splitlines()

        assert tuple(lines) == FAULT_LINES, (name, lines)

        print(
            f"PASS {entry['device']}: three simulated Vulkan errors, "
            "three invalid CLI arguments and checked cleanup"
        )


def verify_negative_control(
    manifest: dict, checksums: dict[str, str],
) -> None:
    """Reject a Vulkan storage binding whose offset violates its alignment."""
    entry = manifest["gpu_runs"][0]
    lines = (FIXTURES / entry["file"]).read_text(
        encoding="utf-8"
    ).splitlines()
    modified = list(lines)
    modified[6] = modified[6].replace(
        "offset=8192", "offset=8193"
    )

    assert modified != lines

    try:
        verify_gpu_lines(
            modified,
            manifest["device_profiles"][entry["device"]],
            manifest["memory_profiles"][entry["device"]],
            entry["optimization"],
            checksums,
        )
    except AssertionError:
        print("PASS negative control: misaligned Vulkan buffer binding rejected")
    else:
        raise AssertionError("Misaligned Vulkan memory was accepted by validator")


def main() -> None:
    """Read archived GPU results and optionally validate local build artifacts."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--artifacts", type=Path,
        help="Directory containing the seven locally rebuilt Vulkan artifacts",
    )
    args = parser.parse_args()
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))

    assert "adb_serial" not in MANIFEST.read_text(encoding="utf-8")
    assert set(manifest["device_profiles"]) == set(DEVICES)
    assert set(manifest["memory_profiles"]) == set(DEVICES)

    verify_source_identity(manifest)

    if args.artifacts is not None:
        verify_build_artifacts(manifest, args.artifacts.resolve(strict=True))

    checksums = verify_workload(manifest)
    verify_device_runs(manifest, checksums)
    verify_error_controls(manifest)
    verify_negative_control(manifest, checksums)

    assert {path.name for path in FIXTURES.glob("*.log")} == {
        *(entry["file"] for entry in manifest["gpu_runs"]),
        *(entry["file"] for entry in manifest["error_control_runs"]),
    }

    print(
        "PASS four real Vulkan shared-allocation GPU results and "
        "six simulated error/input controls; "
        "NO GPU EXECUTION BY OFFLINE VALIDATOR"
    )


if __name__ == "__main__":
    main()
