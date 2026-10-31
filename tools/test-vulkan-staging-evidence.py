#!/usr/bin/env python3
"""Audit Vulkan 1.0 direct/staging experiments and split-module provenance.

The archived GPU logs were captured with an earlier combined-library build.
This validator never equates that historical executable with the separately
compiled staging module. It checks exact per-device output, selected runtime
memory masks, CPU results and optional hashes of the new offline build.
No Android, Vulkan, credentials, hashcat or GPU execution is performed.
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
    / "vulkan-staging-20261031"
)
DEVICE_NAMES = ("redmagic", "pixel")
SHADER_MODES = ("O0", "O")
INPUT_MODES = ("auto", "staging")
POLICY = (
    "PASS_POLICY gpu_local_preference=0",
    "PASS_POLICY compatible_nonlocal_type=1",
    "PASS_POLICY noncoherent_staging_memory_type=1",
    "PASS_POLICY no_host_visible_staging_result=-8",
    "PASS_POLICY incompatible_gpu_memory_result=-8",
    "PASS_POLICY supported_direct_mapping=1",
    "PASS_POLICY device_local_only_selects_staging=1",
    "PASS_POLICY explicit_staging=1",
    "PASS_POLICY no_gpu_storage_type_result=-8",
)
BUFFER_PATTERN = re.compile(
    r"^BUFFER_REQUIREMENTS role=(gpu-input|gpu-output|staging-upload|"
    r"staging-download) required=(\d+) alignment=(\d+) "
    r"memory_bits=0x([0-9a-f]{8})$"
)
ALLOCATION_PATTERN = re.compile(
    r"^BUFFER_ALLOCATION role=(gpu-input|gpu-output|staging-upload|"
    r"staging-download) memory_type=(\d+) cpu_mapped=([01]) "
    r"coherent=([01]) bytes=(\d+)$"
)
GPU_PATTERN = re.compile(
    r"^PASS_VULKAN_TRANSFER mode=(direct|staging) device=(\d+) "
    r"queue=(\d+) words=2048 checksum=([0-9a-f]{16}) "
    r"uploads=(\d+) downloads=(\d+) gpu_dispatches=1$"
)
EXPECTED_INVALIDS = (
    "TEST=portable-vulkan1.0-staging",
    "INVALID_INPUT_CONTROLS=4",
    "INVALID no-arguments exit=2",
    "INVALID missing-shader exit=2",
    "INVALID unknown-mode exit=2",
    "INVALID wrong-flag exit=2",
)


def sha256(path: Path) -> str:
    """Return SHA-256 for an exact required source, artifact or GPU log."""
    assert path.is_file(), f"Missing synthetic Vulkan staging evidence: {path}"

    return hashlib.sha256(path.read_bytes()).hexdigest()


def scalar_checksum() -> str:
    """Calculate independent FNV-1a over the synthetic stage-A GPU output."""
    result = 14695981039346656037
    mask32 = (1 << 32) - 1
    mask64 = (1 << 64) - 1

    for index in range(2048):
        value = index * 1664525 & mask32
        affine = (value * 1664525 + 1013904223) & mask32
        rotate = ((value << 7) | (value >> 25)) & mask32
        output = ((affine ^ rotate) + index * 2654435761) & mask32

        for byte in struct.pack("<I", output):
            result = (result ^ byte) * 1099511628211 & mask64

    return f"{result:016x}"


def verify_manifest(manifest: dict) -> str:
    """Check declared build limits, exact code identities and GPU provenance."""
    assert manifest["date"] == "2026-10-31"
    assert manifest["scope"] == (
        "synthetic Vulkan 1.0 direct and staging GPU transfers; "
        "historical live results and separately compiled modular source"
    )
    assert manifest["toolchain"] == {
        "ndk": "29.0.14206865",
        "clang": "21.0.0",
        "target": "aarch64-linux-android26",
        "shader": "vulkan1.0",
    }

    original = json.loads(
        (
            ROOT / "tests" / "fixtures" / "android-gpu"
            / "vulkan-autoconfig-20261031" / "manifest.json"
        ).read_text(encoding="utf-8")
    )

    assert manifest["device_profiles"] == original["device_profiles"]

    sources = manifest["sources_sha256"]
    expected_sources = {
        "tools/vulkan-auto-config.c",
        "tools/vulkan-auto-config.h",
        "tools/vulkan-staging-memory.c",
        "tools/vulkan-staging-memory.h",
        "tools/vulkan-staging-diagnostic.c",
        "tools/build-vulkan-staging.ps1",
        "tools/run-vulkan-staging.ps1",
        "tools/vulkan-pipeline-stage-a.comp",
    }

    assert set(sources) == expected_sources

    for name, digest in sources.items():
        assert re.fullmatch(r"[0-9a-f]{64}", digest)
        assert sha256(ROOT / name) == digest, name

    for name in ("tools/vulkan-auto-config.c", "tools/vulkan-auto-config.h"):
        assert sources[name] == original["source_sha256"][name]

    build = manifest["modular_build_sha256"]

    assert set(build) == {
        "vulkan-staging-arm64",
        "stage-a-O0-vk1.0.spv",
        "stage-a-O-vk1.0.spv",
    }

    for name, digest in build.items():
        assert re.fullmatch(r"[0-9a-f]{64}", digest)

        if name.endswith(".spv"):
            assert digest == original["artifact_sha256"][name]

    historical = manifest["historical_device_executable"]

    assert historical == {
        "sha256": "8127abccf5beb7368877f1db2fef1b857b495e655ceac17e3c19738c5ff52f90",
        "bytes": 30368,
        "compiled_library_layout": "combined-universal-config-and-staging",
        "device_tested": True,
    }
    assert manifest["current_modular_build_hardware_tested"] is False
    assert historical["sha256"] != build["vulkan-staging-arm64"]

    oracle = scalar_checksum()

    assert oracle == "00d6d6e4837edebc"
    assert manifest["expected_output_fnv1a64"] == oracle
    assert manifest["elements"] == 2048
    assert manifest["bytes_per_buffer"] == 8192

    print("PASS original universal-config source identities preserved")
    print(
        "PASS exact source/build hashes and distinct historical "
        "versus modular executable identities"
    )
    print(f"PASS independent scalar stage-A output digest: {oracle}")

    return oracle


def verify_current_artifacts(manifest: dict, directory: Path) -> None:
    """Match all three locally built but not hardware-tested modular artifacts."""
    assert directory.is_dir()

    for name, digest in manifest["modular_build_sha256"].items():
        assert sha256(directory / name) == digest, name

    print("PASS three modular ARM64/SPIR-V artifact SHA-256 identities (offline)")


def verify_gpu_lines(
    lines: list[str],
    device: dict,
    expected_input: str,
    optimization: str,
    oracle: str,
) -> None:
    """Validate eight real-device run types without relying on a PASS alone."""
    assert lines[:4] == [
        "TEST=portable-vulkan1.0-staging",
        f"REQUESTED_MODE={expected_input}",
        f"OPTIMIZATION={optimization}",
        "ADB_EXIT_CODE=0",
    ]

    for entry in POLICY:
        assert lines.count(entry) == 1, entry

    candidates = [line for line in lines if line.startswith("VULKAN_CANDIDATE")]

    assert len(candidates) == 1
    assert f"name={device['gpu']}" in candidates[0]
    assert f"vendor={device['vendor_id']}" in candidates[0]
    assert f"device={device['device_id']}" in candidates[0]
    assert f"driver={device['driver_version']}" in candidates[0]
    assert f"api={device['vulkan_api_version']}" in candidates[0]

    accepts = [line for line in lines if line.startswith("ACCEPT_COMPUTE")]

    assert len(accepts) == 1
    assert "queue_family=0" in accepts[0]

    choices = [line for line in lines if line.startswith("AUTO_TRANSFER")]

    assert len(choices) == 4
    assert choices[0].startswith("AUTO_TRANSFER mode=direct ")
    assert choices[1].startswith("AUTO_TRANSFER mode=staging ")
    assert choices[2].startswith("AUTO_TRANSFER mode=staging ")

    selected_mode = "direct" if expected_input == "auto" else "staging"

    assert choices[3].startswith(f"AUTO_TRANSFER mode={selected_mode} ")

    buffer_names = ["gpu-input", "gpu-output"]

    if selected_mode == "staging":
        buffer_names.extend(["staging-upload", "staging-download"])

    requirements = [
        BUFFER_PATTERN.fullmatch(line)
        for line in lines if line.startswith("BUFFER_REQUIREMENTS")
    ]
    allocations = [
        ALLOCATION_PATTERN.fullmatch(line)
        for line in lines if line.startswith("BUFFER_ALLOCATION")
    ]

    assert len(requirements) == len(buffer_names)
    assert len(allocations) == len(buffer_names)
    assert all(entry is not None for entry in requirements)
    assert all(entry is not None for entry in allocations)

    for index, name in enumerate(buffer_names):
        actual_name, required, alignment, bits = requirements[index].groups()
        allocated_name, type_index, mapped, coherent, allocation = (
            allocations[index].groups()
        )

        assert actual_name == allocated_name == name
        assert int(required) >= 8192
        assert int(alignment) > 0
        assert int(alignment) & (int(alignment) - 1) == 0
        assert int(allocation) == int(required)
        assert int(bits, 16) & (1 << int(type_index))
        assert int(type_index) < 32
        assert int(coherent) in (0, 1)

        expected_mapped = (
            0 if selected_mode == "staging" and index < 2 else 1
        )
        assert int(mapped) == expected_mapped

    successes = [GPU_PATTERN.fullmatch(line) for line in lines
                 if line.startswith("PASS_VULKAN_TRANSFER")]

    assert len(successes) == 1 and successes[0] is not None
    mode, gpu_index, family, digest, upload, download = (
        successes[0].groups()
    )

    assert mode == selected_mode
    assert gpu_index == "0"
    assert family == "0"
    assert digest == oracle
    expected_copy = 1 if selected_mode == "staging" else 0
    assert int(upload) == int(download) == expected_copy


def verify_runs(manifest: dict, digest: str) -> None:
    """Validate eight original device run logs with their exact recorded hashes."""
    records = manifest["historical_gpu_runs"]

    assert len(records) == 8
    assert {
        (record["device"], record["requested_mode"], record["optimization"])
        for record in records
    } == {
        (device, mode, optimization)
        for device in DEVICE_NAMES
        for mode in INPUT_MODES
        for optimization in SHADER_MODES
    }

    for record in records:
        assert set(record) == {
            "device", "requested_mode", "selected_mode",
            "optimization", "file", "exit_code", "sha256",
        }
        assert record["exit_code"] == 0
        assert record["selected_mode"] == (
            "direct" if record["requested_mode"] == "auto" else "staging"
        )
        name = (
            f"{record['device']}-{record['requested_mode']}-"
            f"{record['optimization']}.log"
        )
        assert record["file"] == name
        assert sha256(ARCHIVE / name) == record["sha256"]

        lines = (ARCHIVE / name).read_text(encoding="utf-8").splitlines()

        verify_gpu_lines(
            lines,
            manifest["device_profiles"][record["device"]],
            record["requested_mode"], record["optimization"], digest,
        )
        print(
            f"PASS historical GPU {record['device']}: "
            f"{record['requested_mode']} -{record['optimization']}, "
            "2048 exact words and correct transfer/memory roles"
        )


def verify_invalids(manifest: dict) -> None:
    """Require four malformed CLI requests to fail on each historical device."""
    records = manifest["negative_controls"]

    assert len(records) == 2
    assert {entry["device"] for entry in records} == set(DEVICE_NAMES)

    for record in records:
        assert set(record) == {
            "device", "file", "controls", "sha256",
        }
        assert record["controls"] == 4
        assert record["file"] == f"{record['device']}-negative-controls.log"
        assert sha256(ARCHIVE / record["file"]) == record["sha256"]
        observed = (
            (ARCHIVE / record["file"]).read_text(encoding="utf-8").splitlines()
        )
        assert tuple(observed) == EXPECTED_INVALIDS

    print("PASS eight historical invalid-input controls, exit code 2")


def verify_negative_controls(manifest: dict, digest: str) -> None:
    """Reject one modified output digest and one incompatible memory type."""
    first = manifest["historical_gpu_runs"][0]
    original = (
        (ARCHIVE / first["file"]).read_text(encoding="utf-8").splitlines()
    )
    profile = manifest["device_profiles"][first["device"]]

    corrupted = list(original)
    corrupted[-1] = corrupted[-1].replace(digest, "0000000000000000")

    assert corrupted != original

    try:
        verify_gpu_lines(
            corrupted, profile, first["requested_mode"],
            first["optimization"], digest,
        )
    except AssertionError:
        print("PASS negative control: incorrect staged GPU checksum rejected")
    else:
        raise AssertionError("Incorrect GPU output was accepted")

    corrupted = list(original)
    allocation_line = next(
        line for line in original
        if line.startswith("BUFFER_ALLOCATION role=gpu-input ")
    )
    corrupted[corrupted.index(allocation_line)] = allocation_line.replace(
        "memory_type=6", "memory_type=31"
    ).replace("memory_type=0", "memory_type=31")

    assert corrupted != original

    try:
        verify_gpu_lines(
            corrupted, profile, first["requested_mode"],
            first["optimization"], digest,
        )
    except AssertionError:
        print("PASS negative control: incompatible GPU memory type rejected")
    else:
        raise AssertionError("Incompatible GPU memory type was accepted")


def main() -> None:
    """Validate archived GPU evidence and the separately compiled module."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--artifacts", type=Path,
        help="Path containing three locally built modular diagnostic artifacts",
    )
    args = parser.parse_args()
    manifest = json.loads(
        (ARCHIVE / "manifest.json").read_text(encoding="utf-8")
    )

    assert "adb_serial" not in (ARCHIVE / "manifest.json").read_text(
        encoding="utf-8"
    )

    checksum = verify_manifest(manifest)

    if args.artifacts is not None:
        verify_current_artifacts(
            manifest, args.artifacts.resolve(strict=True),
        )

    verify_runs(manifest, checksum)
    verify_invalids(manifest)
    verify_negative_controls(manifest, checksum)

    files = {
        record["file"]
        for record in (
            manifest["historical_gpu_runs"]
            + manifest["negative_controls"]
        )
    }
    assert {path.name for path in ARCHIVE.glob("*.log")} == files

    print(
        "PASS eight original GPU outputs and separately built modular source; "
        "modular binary GPU execution is NOT claimed"
    )


if __name__ == "__main__":
    main()
