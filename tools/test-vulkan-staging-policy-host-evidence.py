#!/usr/bin/env python3
"""Validate native-C Vulkan staging and compute policy tests without a GPU.

The verifier audits exact source and Windows executable identities, recorded
synthetic Vulkan property sets, deterministic memory decisions, cache-barrier
unit test results and the absence of Vulkan driver imports. It also rejects
manually corrupted matrix totals and noncoherent memory layouts.

All results derive from native CPU execution of the actual C policy functions.
This script neither uses ADB nor executes a GPU shader or hashcat.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = ROOT / "tests" / "fixtures" / "vulkan-host-policy-20261031"
MANIFEST = ARCHIVE / "manifest.json"
MODES = ("O0", "O2")

PASS_LINES = (
    "PASS_HOST_MEMORY_TYPES coherent noncoherent gpu_local bit31 invalid",
    "PASS_HOST_TRANSFER_MODES direct local_only mixed forced invalid",
    "PASS_HOST_EXHAUSTIVE_MATRIX layouts=64 masks=7 selections=448 transfers=6272",
    "PASS_HOST_MEMORY_PLANS atomic_alignment coherent disjoint overflow",
    "PASS_HOST_MAPPED_SYNC coherent flush invalidate full_range errors",
    "PASS_HOST_COMPUTE_CAPABILITIES queue limits requirements errors",
    "PASS_HOST_POLICY all six native-C test groups, no Vulkan driver",
)

EXACT_OUTCOMES = (
    "AUTO_TRANSFER mode=direct input_memory_type=2 output_memory_type=2 "
    "input_coherent=1 output_coherent=1",
    "AUTO_TRANSFER mode=staging input_gpu_type=0 output_gpu_type=0 "
    "input_visible=0 output_visible=0 forced=0",
    "AUTO_TRANSFER mode=staging input_gpu_type=0 output_gpu_type=1 "
    "input_visible=0 output_visible=1 forced=0",
    "AUTO_TRANSFER mode=staging input_gpu_type=0 output_gpu_type=1 "
    "input_visible=1 output_visible=1 forced=1",
    "REJECT_STORAGE_MEMORY input_bits=0x00000000 output_bits=0x00000006 "
    "input_result=-8 output_result=0",
    "AUTO_BUFFER index=0 offset=0 required=384 driver_alignment=64 "
    "effective_alignment=256 compatible_bits=0x00000002",
    "AUTO_BUFFER index=1 offset=512 required=257 driver_alignment=64 "
    "effective_alignment=256 compatible_bits=0x00000002",
    "AUTO_MEMORY type=1 coherent=0 bytes=769 compatible_bits=0x00000002 "
    "atom=256",
    "AUTO_BUFFER index=0 offset=0 required=384 driver_alignment=64 "
    "effective_alignment=64 compatible_bits=0x00000006",
    "AUTO_BUFFER index=1 offset=384 required=257 driver_alignment=64 "
    "effective_alignment=64 compatible_bits=0x00000006",
    "AUTO_MEMORY type=2 coherent=1 bytes=641 compatible_bits=0x00000006 "
    "atom=256",
    "REJECT_MEMORY_TYPE compatible_bits=0x00000001 "
    "need=HOST_VISIBLE fallback=staging-required",
    "REJECT_MEMORY_TYPE compatible_bits=0x00000000 "
    "need=HOST_VISIBLE fallback=staging-required",
    "REJECT_MEMORY_REQUIREMENTS index=0",
    "REJECT_MEMORY_REQUIREMENTS index=1",
    "AUTO_BUFFER index=0 offset=0 required=18446744073709551552 "
    "driver_alignment=64 effective_alignment=64 compatible_bits=0x00000006",
    "ACCEPT_COMPUTE index=0 queue_family=1 local_max=256 "
    "invocations_max=512 groups_max=1024 shared_max=16384 "
    "storage_range_max=65536 descriptor_max=8 non_coherent_atom=256",
    "REJECT_COMPUTE dimension=0 local=0 max_local=256 "
    "groups=64 max_groups=1024",
    "REJECT_COMPUTE dimension=0 local=257 max_local=256 "
    "groups=64 max_groups=1024",
    "REJECT_COMPUTE dimension=1 invocation_limit=512",
    "REJECT_COMPUTE dimension=0 local=32 max_local=256 "
    "groups=0 max_groups=1024",
    "REJECT_COMPUTE dimension=0 local=32 max_local=256 "
    "groups=1025 max_groups=1024",
    "REJECT_QUEUE no_queue_families",
    "REJECT_QUEUE no_compute_queue",
    "REJECT_VULKAN_API index=0 requires=1.0",
)


def sha256(path: Path) -> str:
    """Compute the digest of exactly one required source, binary or test log."""
    assert path.is_file(), f"Missing native Vulkan C policy evidence: {path}"

    return hashlib.sha256(path.read_bytes()).hexdigest()


def sha256_source_lf(path: Path) -> str:
    """Hash UTF-8 source text with CRLF line endings normalized to LF.

    Git may check out the same source text with CRLF on Windows. The
    manifest records canonical LF source content; native executable
    and archived log hashes continue to use their exact raw bytes.
    """
    assert path.is_file(), f"Missing native Vulkan C source: {path}"

    content = path.read_bytes().replace(b"\r\n", b"\n")

    return hashlib.sha256(content).hexdigest()


def validate_manifest(manifest: dict) -> None:
    """Require canonical C source identities and CPU-only execution."""
    assert manifest["date"] == "2026-10-31"
    assert manifest["scope"] == (
        "real native C Vulkan 1.0 capability and staging-memory functions "
        "tested on Windows CPU with mock driver entry points"
    )
    assert manifest["compiler"] == {
        "name": "MSYS2 MinGW GCC",
        "version": "16.2.0",
        "target": "x86_64-w64-mingw32",
        "optimization_modes": ["O0", "O2"],
        "flags": [
            "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-Wconversion", "-Wshadow",
        ],
    }
    assert manifest["source_hash_policy"] == "UTF-8 source with CRLF normalized to LF"
    assert manifest["test_matrix"] == {
        "valid_memory_property_classes": 4,
        "independent_memory_type_slots": 3,
        "property_layouts": 64,
        "nonempty_masks_per_buffer": 7,
        "direct_or_forced_staging_requests": 2,
        "memory_type_selection_cases": 448,
        "transfer_decision_cases": 6272,
        "native_test_groups": 6,
        "uses_gpu_driver": False,
    }

    expected_sources = {
        "tools/vulkan-auto-config.c",
        "tools/vulkan-auto-config.h",
        "tools/vulkan-staging-memory.c",
        "tools/vulkan-staging-memory.h",
        "tools/test-vulkan-staging-policy-host.c",
        "tools/run-vulkan-staging-policy-host.ps1",
    }

    assert set(manifest["source_sha256"]) == expected_sources

    for name, checksum in manifest["source_sha256"].items():
        assert re.fullmatch(r"[0-9a-f]{64}", checksum)
        assert sha256_source_lf(ROOT / name) == checksum, name

    historical = json.loads(
        (
            ROOT / "tests" / "fixtures" / "android-gpu"
            / "vulkan-staging-20261031" / "manifest.json"
        ).read_text(encoding="utf-8")
    )

    for name in (
        "tools/vulkan-auto-config.c",
        "tools/vulkan-auto-config.h",
        "tools/vulkan-staging-memory.c",
        "tools/vulkan-staging-memory.h",
    ):
        assert (
            manifest["source_sha256"][name]
            == historical["sources_sha256"][name]
        ), name

    assert manifest["imported_dlls"] == ["KERNEL32.dll", "msvcrt.dll"]
    assert not any(
        "vulkan" in library.lower() or "opencl" in library.lower()
        for library in manifest["imported_dlls"]
    )

    assert len(manifest["runs"]) == 2

    print("PASS original staging/config sources unchanged; six LF-canonical SHA-256 records")
    print("PASS Windows CPU-only policy test matrix and GPU driver import absence")


def verify_log_contents(lines: list[str], optimization: str) -> None:
    """Check exact C-tested memory outcomes and all six native test groups."""
    assert lines[:4] == [
        "TEST=vulkan-host-c-memory-policy",
        f"OPTIMIZATION={optimization}",
        "VULKAN_DRIVER_LINKED=0",
        "EXIT_CODE=0",
    ]
    assert lines[-1] == PASS_LINES[-1]

    passes = [line for line in lines if line.startswith("PASS_HOST_")]

    assert set(passes) == set(PASS_LINES)
    assert len(passes) == len(PASS_LINES)

    for item in EXACT_OUTCOMES:
        expected_count = (
            2 if item == "REJECT_QUEUE no_compute_queue" else 1
        )

        assert lines.count(item) == expected_count, (
            item, expected_count
        )

    assert len([
        line for line in lines if line.startswith("AUTO_TRANSFER")
    ]) == 4
    assert len([
        line for line in lines if line.startswith("AUTO_MEMORY")
    ]) == 2
    assert len([
        line for line in lines if line.startswith("REJECT_COMPUTE")
    ]) == 5
    assert len([
        line for line in lines if line.startswith("REJECT_STORAGE ")
    ]) == 4
    assert len([
        line for line in lines if line.startswith("REJECT_QUEUE")
    ]) == 3
    assert len([
        line for line in lines if line.startswith("REJECT_VULKAN_API")
    ]) == 1

    candidates = [
        line for line in lines if line.startswith("VULKAN_CANDIDATE")
    ]
    assert len(candidates) == 14

    for candidate in candidates:
        assert candidate.startswith(
            "VULKAN_CANDIDATE index=0 name=synthetic-vulkan-1.0 "
            "vendor=0x1111 device=0x2222 "
        )
        assert candidate.endswith("driver=0x00000000")

    assert all(
        ("api=1.0.0" in candidate or "api=0.0.0" in candidate)
        for candidate in candidates
    )

    assert "REJECT_STORAGE descriptors=0 max_descriptor_set=8 " in (
        " ".join(lines)
    )
    assert "REJECT_STORAGE descriptors=5 max_descriptor_set=8 " in (
        " ".join(lines)
    )


def verify_runs(manifest: dict) -> None:
    """Verify both optimization logs and exactly equal underlying C decisions."""
    assert {
        item["optimization"] for item in manifest["runs"]
    } == set(MODES)

    compared = None

    for record in manifest["runs"]:
        assert set(record) == {
            "optimization", "file", "sha256", "binary_sha256", "exit_code",
        }
        assert record["exit_code"] == 0
        assert re.fullmatch(r"[0-9a-f]{64}", record["binary_sha256"])
        name = f"vulkan-staging-policy-{record['optimization']}.log"

        assert record["file"] == name
        assert sha256(ARCHIVE / name) == record["sha256"]

        lines = (ARCHIVE / name).read_text(
            encoding="utf-8"
        ).splitlines()

        verify_log_contents(lines, record["optimization"])

        if compared is None:
            compared = lines[4:]
        else:
            assert compared == lines[4:], (
                "Compiler optimization changed policy behavior"
            )

        print(
            f"PASS native C -{record['optimization']}: "
            "448 memory selections, 6272 transfer decisions, "
            "mapped cache mocks and compute capability rejections"
        )


def verify_current_build(manifest: dict, directory: Path) -> None:
    """Verify the locally built Windows executables against their recorded SHA."""
    assert directory.is_dir()

    for record in manifest["runs"]:
        name = f"vulkan-staging-policy-{record['optimization']}.exe"

        assert sha256(directory / name) == record["binary_sha256"], name

    print("PASS both native Windows C executables match recorded artifact SHA-256")


def verify_negative_controls(manifest: dict) -> None:
    """Reject a falsified matrix total and incorrectly coherent allocation."""
    record = manifest["runs"][0]
    lines = (ARCHIVE / record["file"]).read_text(
        encoding="utf-8"
    ).splitlines()
    changed = list(lines)

    changed = [
        line.replace("transfers=6272", "transfers=6271")
        for line in changed
    ]

    assert changed != lines

    try:
        verify_log_contents(changed, record["optimization"])
    except AssertionError:
        print("PASS negative control: missing transfer combination rejected")
    else:
        raise AssertionError("Altered staging transfer total was accepted")

    changed = [
        line.replace("AUTO_MEMORY type=1 coherent=0", "AUTO_MEMORY type=1 coherent=1")
        for line in lines
    ]

    assert changed != lines

    try:
        verify_log_contents(changed, record["optimization"])
    except AssertionError:
        print("PASS negative control: wrong noncoherent cache flag rejected")
    else:
        raise AssertionError("Invalid host memory coherency was accepted")


def main() -> None:
    """Audit exact source, native CPU test logs and optional local executables."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--artifacts", type=Path,
        help="Directory containing two compiled Windows CPU test executables",
    )
    args = parser.parse_args()
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))

    assert "adb_serial" not in MANIFEST.read_text(encoding="utf-8")
    assert "device_profiles" not in manifest

    validate_manifest(manifest)
    verify_runs(manifest)

    if args.artifacts is not None:
        verify_current_build(
            manifest, args.artifacts.resolve(strict=True)
        )

    verify_negative_controls(manifest)

    assert {
        path.name for path in ARCHIVE.glob("*.log")
    } == {f"vulkan-staging-policy-{mode}.log" for mode in MODES}

    print(
        "PASS exact host-native Vulkan policy regressions; "
        "NO GPU DRIVER, ADB, CREDENTIALS OR CRYPTOGRAPHIC COMPUTE"
    )


if __name__ == "__main__":
    main()
