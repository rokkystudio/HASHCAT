#!/usr/bin/env python3
"""Verify real-device Vulkan 1.0 capability negotiation and synthetic GPU output.

The validator reads the original Adreno and Mali execution logs, exact source
and build identities, independently calculated 32-bit compute output, runtime
workgroup/device limits, aligned memory requirements, explicit capability
rejections and invalid CLI input results. It never invokes ADB or Vulkan.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct


ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = (
    ROOT / "tests" / "fixtures" / "android-gpu"
    / "vulkan-autoconfig-20261031"
)
MANIFEST = ARCHIVE / "manifest.json"

DEVICES = ("redmagic", "pixel")
MODES = ("O0", "O")
REJECTIONS = (
    "zero_local_size",
    "zero_workgroups",
    "missing_descriptors",
    "shared_memory_limit",
)
EXPECTED_CLI_FAILURES = (
    "TEST=portable-vulkan1.0-auto-config",
    "INVALID_INPUT_CONTROLS=2",
    "NEGATIVE no-shader-argument exit=2",
    "NEGATIVE missing-shader-file exit=2",
)
BUFFER_PATTERN = re.compile(
    r"^AUTO_BUFFER index=(\d+) offset=(\d+) required=(\d+) "
    r"driver_alignment=(\d+) effective_alignment=(\d+) "
    r"compatible_bits=(0x[0-9a-f]{8})$"
)
MEMORY_PATTERN = re.compile(
    r"^AUTO_MEMORY type=(\d+) coherent=([01]) bytes=(\d+) "
    r"compatible_bits=(0x[0-9a-f]{8}) atom=(\d+)$"
)
ACCEPT_PATTERN = re.compile(
    r"^ACCEPT_COMPUTE index=(\d+) queue_family=(\d+) "
    r"local_max=(\d+) invocations_max=(\d+) groups_max=(\d+) "
    r"shared_max=(\d+) storage_range_max=(\d+) descriptor_max=(\d+) "
    r"non_coherent_atom=(\d+)$"
)
CANDIDATE_PATTERN = re.compile(
    r"^VULKAN_CANDIDATE index=(\d+) name=(.+) "
    r"vendor=(0x[0-9a-f]+) device=(0x[0-9a-f]+) "
    r"api=(\d+\.\d+\.\d+) driver=(0x[0-9a-f]+)$"
)


def sha256(path: Path) -> str:
    """Hash the exact bytes of one required source, binary or evidence log."""
    assert path.is_file(), f"Missing portable Vulkan evidence: {path}"

    return hashlib.sha256(path.read_bytes()).hexdigest()


def expected_stage_a_checksum() -> str:
    """Compute the public shader output with independent scalar Python."""
    mask32 = (1 << 32) - 1
    mask64 = (1 << 64) - 1
    fnv = 14695981039346656037

    for index in range(2048):
        value = index * 1664525 & mask32
        affine = (value * 1664525 + 1013904223) & mask32
        rotated = ((value << 7) | (value >> 25)) & mask32
        output = ((affine ^ rotated) + index * 2654435761) & mask32

        for byte in struct.pack("<I", output):
            fnv = (fnv ^ byte) * 1099511628211 & mask64

    return f"{fnv:016x}"


def verify_provenance(manifest: dict) -> None:
    """Match all tracked runtime sources, toolchain details and SPIR-V hashes."""
    assert manifest["date"] == "2026-10-31"
    assert manifest["scope"] == (
        "Vulkan 1.0 synthetic compute using API-discovered device, "
        "queue and memory; no hashcat, cryptographic shader or password input"
    )
    assert manifest["toolchain"] == {
        "ndk": "29.0.14206865",
        "target": "aarch64-linux-android26",
        "native_clang": "21.0.0",
        "shader_target": "vulkan1.0",
        "shaderc": "glslc",
    }

    sources = manifest["source_sha256"]
    names = {
        "tools/vulkan-auto-config.h",
        "tools/vulkan-auto-config.c",
        "tools/vulkan-auto-config-diagnostic.c",
        "tools/build-vulkan-auto-config.ps1",
        "tools/run-vulkan-auto-config.ps1",
        *(
            f"tools/vulkan-pipeline-stage-{stage}.comp"
            for stage in ("a", "b", "c")
        ),
    }

    assert set(sources) == names

    for name, expected in sources.items():
        assert re.fullmatch(r"[0-9a-f]{64}", expected)
        assert sha256(ROOT / name) == expected, name

    binaries = manifest["artifact_sha256"]
    names = {
        "vulkan-autoconfig-arm64",
        *(
            f"stage-{stage}-{mode}-vk1.0.spv"
            for stage in ("a", "b", "c")
            for mode in MODES
        ),
    }

    assert set(binaries) == names

    for expected in binaries.values():
        assert re.fullmatch(r"[0-9a-f]{64}", expected)

    print("PASS eight exact source files and seven recorded build identities")


def verify_build_artifacts(manifest: dict, directory: Path) -> None:
    """Validate all seven locally built ARM64 and Vulkan 1.0 SPIR-V artifacts."""
    assert directory.is_dir()

    for name, expected in manifest["artifact_sha256"].items():
        assert sha256(directory / name) == expected, name

    print("PASS actual local ARM64 executable and six Vulkan 1.0 SPIR-V hashes")


def verify_workload(manifest: dict) -> str:
    """Verify explicit shader requirements and a bytewise scalar GPU reference."""
    expected = expected_stage_a_checksum()
    workload = manifest["workload"]

    assert workload == {
        "vulkan_minimum": "1.0",
        "local_size": [32, 1, 1],
        "workgroups": [64, 1, 1],
        "storage_buffer_descriptors": 4,
        "max_storage_buffer_bytes": 32768,
        "shared_memory_bytes": 128,
        "buffer_payload_bytes": [8192, 8192, 8192, 32768],
        "gpu_output_words": 2048,
        "cpu_mapped_words": 14336,
        "expected_gpu_fnv1a64": expected,
        "device_capability_rejections": list(REJECTIONS),
        "mock_memory_policy_checks": [
            "preferred_coherent_type",
            "fallback_noncoherent_type",
            "noncoherent_atom",
            "coherent_offset_1",
            "unsupported_memory_error",
            "invalid_requirements_error",
            "overflow_error",
        ],
    }
    assert expected == "00d6d6e4837edebc"

    print(f"PASS independent Python GPU stage-A oracle: FNV-1a-64={expected}")

    return expected


def verify_gpu_log(
    lines: list[str],
    mode: str,
    device: dict,
    memory: dict,
    expected_checksum: str,
) -> None:
    """Check exact runtime limits, negotiated buffer ranges and output checksum."""
    assert lines[:3] == [
        "TEST=portable-vulkan1.0-auto-config",
        f"OPTIMIZATION={mode}",
        "ADB_EXIT_CODE=0",
    ]
    assert "PASS_POLICY preferred_coherent_type=2" in lines
    assert "PASS_POLICY fallback_noncoherent_type=1" in lines
    assert "PASS_POLICY noncoherent_atom=256 offset_1=512 allocation=769" in lines
    assert "PASS_POLICY coherent_offset_1=384" in lines
    assert "PASS_POLICY unsupported_memory_error=-8" in lines
    assert "PASS_POLICY invalid_requirements_error=-3" in lines
    assert "PASS_POLICY overflow_error=-2" in lines
    assert (
        "WORKLOAD_PROFILE vulkan_min=1.0 local_x=32 groups_x=64 "
        "descriptors=4 largest_range=32768 shared_bytes=128"
    ) in lines

    candidates = [
        CANDIDATE_PATTERN.fullmatch(line)
        for line in lines if line.startswith("VULKAN_CANDIDATE")
    ]

    assert len(candidates) == 5
    assert all(candidate is not None for candidate in candidates)

    for candidate in candidates:
        index, name, vendor, physical, api, driver = candidate.groups()

        assert index == "0"
        assert name == device["gpu"]
        assert vendor == device["vendor_id"]
        assert physical == device["device_id"]
        assert api == device["vulkan_api_version"]
        assert driver == device["driver_version"]

    accepts = [
        ACCEPT_PATTERN.fullmatch(line)
        for line in lines if line.startswith("ACCEPT_COMPUTE")
    ]

    assert len(accepts) == 1 and accepts[0] is not None
    values = tuple(int(value) for value in accepts[0].groups())
    (
        index, family, local_max, invocation_max, groups_max, shared_max,
        storage_max, descriptor_max, atom,
    ) = values

    assert index == 0
    assert family == device["queue_family"]
    assert local_max >= 32
    assert invocation_max >= 32
    assert groups_max >= 64
    assert shared_max >= 128
    assert storage_max >= 32768
    assert descriptor_max >= 4
    assert atom == memory["non_coherent_atom_size"]
    assert atom > 0 and (atom & (atom - 1)) == 0

    for item in REJECTIONS:
        assert lines.count(
            f"PASS_CAPABILITY_REJECTION {item} vkresult=-8"
        ) == 1

    assert len([
        line for line in lines if line.startswith("PASS_CAPABILITY_REJECTION")
    ]) == 4
    assert len([
        line for line in lines if line.startswith("REJECT_COMPUTE")
    ]) == 2
    assert len([
        line for line in lines if line.startswith("REJECT_STORAGE")
    ]) == 2

    plans = [
        MEMORY_PATTERN.fullmatch(line)
        for line in lines if line.startswith("AUTO_MEMORY")
    ]

    assert len(plans) == 3 and all(plan is not None for plan in plans)
    assert plans[0].groups() == ("1", "0", "769", "0x00000002", "256")
    assert plans[1].groups() == ("2", "1", "641", "0x00000006", "256")

    selected = plans[2].groups()
    selected_type, coherent, size, bits, actual_atom = selected

    assert int(selected_type) == memory["memory_type_index"]
    assert coherent == str(memory["coherent"])
    assert int(size) == memory["allocation_bytes"]
    assert bits == memory["compatible_type_bits"]
    assert int(actual_atom) == atom
    assert int(bits, 16) & (1 << int(selected_type))

    ranges = [
        BUFFER_PATTERN.fullmatch(line)
        for line in lines if line.startswith("AUTO_BUFFER")
    ]

    assert len(ranges) == 9
    assert all(item is not None for item in ranges)

    expected_sizes = (8192, 8192, 8192, 32768)
    end = 0
    intersection = 0xFFFFFFFF

    for buffer, entry in enumerate(ranges[-4:]):
        index, offset, required, driver_alignment, alignment, compatible = (
            entry.groups()
        )
        offset = int(offset)
        required = int(required)
        driver_alignment = int(driver_alignment)
        alignment = int(alignment)

        assert int(index) == buffer
        assert required >= expected_sizes[buffer]
        assert driver_alignment > 0
        assert (driver_alignment & (driver_alignment - 1)) == 0
        assert alignment >= driver_alignment
        assert (alignment & (alignment - 1)) == 0
        assert offset >= end
        assert offset % driver_alignment == 0
        assert offset % alignment == 0
        assert offset + required <= memory["allocation_bytes"]

        if coherent == "0":
            assert alignment >= atom

        end = offset + required
        intersection &= int(compatible, 16)

    assert end == memory["allocation_bytes"]
    assert intersection == int(memory["compatible_type_bits"], 16)

    assert lines[-2] == (
        "PASS_VULKAN_GPU_STAGE device_index=0 local_size=32 "
        "groups=64 words=2048 memory_type="
        f"{selected_type} coherent={coherent} "
        f"fnv1a64={expected_checksum}"
    )
    assert lines[-1] == (
        "PASS_VULKAN_AUTO_CONFIG device_index=0 queue_family="
        f"{family} memory_type={selected_type} coherent={coherent} "
        f"allocation_bytes={size} buffers=4 verified_cpu_words=14336 "
        "gpu_dispatches=1 verified_gpu_words=2048"
    )


def verify_runs(manifest: dict, expected_checksum: str) -> None:
    """Verify both shader optimizations actually executed on two Vulkan GPUs."""
    runs = manifest["runs"]

    assert len(runs) == 4
    assert {
        (record["device"], record["optimization"])
        for record in runs
    } == {(device, mode) for device in DEVICES for mode in MODES}

    for record in runs:
        assert set(record) == {
            "device", "optimization", "file", "exit_code", "sha256",
        }
        assert record["exit_code"] == 0

        name = f"{record['device']}-{record['optimization']}.log"

        assert record["file"] == name
        assert sha256(ARCHIVE / name) == record["sha256"]

        lines = (ARCHIVE / name).read_text(encoding="utf-8").splitlines()

        verify_gpu_log(
            lines,
            record["optimization"],
            manifest["device_profiles"][record["device"]],
            manifest["memory_profiles"][record["device"]],
            expected_checksum,
        )

        print(
            f"PASS {record['device']} -{record['optimization']}: "
            "real Vulkan 1.0 GPU compute, validated device limits, "
            "14336 mapped words and 2048 GPU output words"
        )


def verify_rejected_inputs(manifest: dict) -> None:
    """Require each device to reject missing SPIR-V and absent arguments."""
    records = manifest["negative_control_runs"]

    assert len(records) == 2
    assert {entry["device"] for entry in records} == set(DEVICES)

    for entry in records:
        assert set(entry) == {
            "device", "file", "expected_errors", "sha256",
        }
        assert entry["expected_errors"] == 2
        assert entry["file"] == f"{entry['device']}-negative-controls.log"
        assert sha256(ARCHIVE / entry["file"]) == entry["sha256"]

        observed = tuple(
            (ARCHIVE / entry["file"]).read_text(
                encoding="utf-8"
            ).splitlines()
        )

        assert observed == EXPECTED_CLI_FAILURES

    print("PASS four real-device invalid-argument tests, exit code 2")


def verify_negative_controls(
    manifest: dict,
    checksum: str,
) -> None:
    """Reject one tampered GPU checksum and one unaligned buffer offset."""
    first = manifest["runs"][0]
    lines = (ARCHIVE / first["file"]).read_text(
        encoding="utf-8"
    ).splitlines()
    identity = manifest["device_profiles"][first["device"]]
    memory = manifest["memory_profiles"][first["device"]]

    changed = list(lines)
    changed[-2] = changed[-2].replace(
        checksum, "0000000000000000"
    )

    assert changed != lines

    try:
        verify_gpu_log(
            changed, first["optimization"], identity, memory, checksum,
        )
    except AssertionError:
        print("PASS negative control: incorrect GPU output checksum rejected")
    else:
        raise AssertionError("Incorrect GPU checksum accepted by validator")

    changed = list(lines)
    original = next(
        line for line in changed
        if line.startswith("AUTO_BUFFER index=1 ")
        and "required=8192" in line
    )
    changed[changed.index(original)] = original.replace(
        "offset=8192", "offset=8193"
    )

    try:
        verify_gpu_log(
            changed, first["optimization"], identity, memory, checksum,
        )
    except AssertionError:
        print("PASS negative control: unaligned Vulkan buffer rejected")
    else:
        raise AssertionError("Unaligned buffer accepted by validator")


def main() -> None:
    """Audit archived Vulkan computation and optionally local compiled binaries."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--artifacts", type=Path,
        help="Exact directory containing seven locally rebuilt Vulkan artifacts",
    )
    args = parser.parse_args()
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))

    assert "adb_serial" not in MANIFEST.read_text(encoding="utf-8")
    assert set(manifest["device_profiles"]) == set(DEVICES)
    assert set(manifest["memory_profiles"]) == set(DEVICES)

    verify_provenance(manifest)

    if args.artifacts is not None:
        verify_build_artifacts(
            manifest, args.artifacts.resolve(strict=True)
        )

    checksum = verify_workload(manifest)
    verify_runs(manifest, checksum)
    verify_rejected_inputs(manifest)
    verify_negative_controls(manifest, checksum)

    expected_logs = {
        entry["file"] for entry in (
            manifest["runs"] + manifest["negative_control_runs"]
        )
    }

    assert {path.name for path in ARCHIVE.glob("*.log")} == expected_logs

    print(
        "PASS four real Vulkan 1.0 GPU runs, exact runtime capability checks "
        "and four invalid-input controls; NO DEVICE EXECUTION BY VALIDATOR"
    )


if __name__ == "__main__":
    main()
