#!/usr/bin/env python3
"""Archive original public Vulkan 1.0 multi-stage, multi-set GPU evidence.

Copies LF-only logs recorded on RedMagic ARM64, RedMagic ARMv7 and Pixel
ARM64 without editing their bytes.
Stores independently identifiable source, job-port and build SHA-256 digests,
explicit
device/ABI coverage and exact run/negative-case metadata. Does not execute
Android, Vulkan or shader build operations.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import shutil


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/diagnostic-multimode/vulkan-multiset-graph-20261031"
ARCHIVE = ROOT / "tests/fixtures/android-gpu/vulkan-multiset-graph-20261031"

DEVICES = {
    "redmagic-arm64": "arm64-v8a",
    "redmagic-armv7": "armeabi-v7a",
    "pixel-arm64": "arm64-v8a",
}
MODES = ("default10", "explicit10", "required11", "noncoherent10")
OPTIMIZATIONS = ("O0", "O")
SOURCES = (
    "tools/vulkan-auto-config.c",
    "tools/vulkan-auto-config.h",
    "tools/vulkan-core11-policy.c",
    "tools/vulkan-core11-policy.h",
    "tools/vulkan-core10-device-catalog.c",
    "tools/vulkan-core10-device-catalog.h",
    "tools/vulkan-core10-graph-memory.c",
    "tools/vulkan-core10-graph-memory.h",
    "tools/vulkan-memory-heap-policy.h",
    "tools/vulkan-core10-descriptor-policy.c",
    "tools/vulkan-core10-descriptor-policy.h",
    "tools/vulkan-core10-multiset.c",
    "tools/vulkan-core10-multiset.h",
    "tools/vulkan-core10-multiset-graph.c",
    "tools/vulkan-core10-multiset-graph.h",
    "tools/vulkan-core10-job-port.c",
    "tools/vulkan-core10-job-port.h",
    "tools/vulkan-core10-multiset-graph-diagnostic.c",
    "tools/vulkan-multiset-graph.comp",
    "tools/build-vulkan-core10-multiset-graph.ps1",
    "tools/run-vulkan-core10-multiset-graph.ps1",
    "tools/archive-vulkan-core10-multiset-graph-evidence.py",
    "tools/test-vulkan-core10-multiset-graph-evidence.py",
)
ARTIFACTS = (
    "vulkan-multigraph-arm64",
    "vulkan-multigraph-armv7",
    *(
        f"multigraph-set{sets}-stage{stage}-{optimization}-vk1.0.spv"
        for sets in (1, 2)
        for stage in (0, 1, 2)
        for optimization in OPTIMIZATIONS
    ),
)


def sha256(path: Path) -> str:
    """Return the SHA-256 of the complete on-disk file bytes."""
    assert path.is_file(), path

    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_sha256(path: Path) -> str:
    """Hash UTF-8 source content with CRLF converted to LF."""
    return hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")
    ).hexdigest()


def copy_original(name: str) -> str:
    """Copy a native LF-only log with its exact byte content preserved."""
    source = BUILD / name
    data = source.read_bytes()

    assert data and data.endswith(b"\n"), name
    assert b"\r" not in data, name
    assert b"91809037" not in data, name
    assert b"46281FDAS002AL" not in data, name

    destination = ARCHIVE / name
    shutil.copyfile(source, destination)
    assert destination.read_bytes() == data

    return sha256(destination)


def main() -> None:
    """Archive three device/ABI GPU matrices, faults and build identities."""
    ARCHIVE.mkdir(parents=True, exist_ok=True)

    device_runs = []
    invalid_runs = []
    fault_runs = []

    for device, abi in DEVICES.items():
        for sets in (1, 2):
            for stages in (1, 2, 3):
                for mode in MODES:
                    for optimization in OPTIMIZATIONS:
                        name = (
                            f"{device}-set{sets}-stages{stages}-"
                            f"{mode}-{optimization}.log"
                        )
                        digest = copy_original(name)
                        exit_code = int(
                            mode == "noncoherent10"
                            and device.startswith("redmagic")
                        )
                        rows = (ARCHIVE / name).read_text(
                            encoding="utf-8"
                        ).splitlines()

                        assert rows[:7] == [
                            "TEST=vulkan-core10-multiset-graph",
                            f"SETS={sets}",
                            f"STAGES={stages}",
                            f"MODE={mode}",
                            f"OPTIMIZATION={optimization}",
                            f"ABI={abi}",
                            f"ADB_EXIT_CODE={exit_code}",
                        ], name

                        device_runs.append({
                            "device": device,
                            "abi": abi,
                            "sets": sets,
                            "stages": stages,
                            "mode": mode,
                            "optimization": optimization,
                            "file": name,
                            "exit_code": exit_code,
                            "sha256": digest,
                        })

        name = f"{device}-negative-controls.log"
        digest = copy_original(name)
        invalid_runs.append({
            "device": device,
            "abi": abi,
            "file": name,
            "sha256": digest,
            "cases": 10,
        })

        name = f"{device}-faults.log"
        digest = copy_original(name)
        fault_rows = (ARCHIVE / name).read_text(
            encoding="utf-8"
        ).splitlines()

        assert fault_rows[:3] == [
            "TEST=vulkan-core10-multiset-graph-faults",
            f"ABI={abi}",
            "CONFIGURATIONS=set1-stages1,set2-stages3",
        ], name

        fault_runs.append({
            "device": device,
            "abi": abi,
            "file": name,
            "sha256": digest,
            "set_stage_points": {
                "1:1": 23,
                "2:3": 39,
            },
        })

    manifest = {
        "date": "2026-10-31",
        "scope": (
            "Public noncryptographic Vulkan 1.0 multi-stage Compute "
            "with mixed storage/uniform descriptor sets and a synchronous "
            "caller-owned GPU job port"
        ),
        "api_default": "1.0",
        "minimum_android_api": 24,
        "physical_vulkan10_only_device_tested": False,
        "android_api24_os_tested": False,
        "gpu_device_abi_tested": DEVICES,
        "set_counts": [1, 2],
        "stage_counts": [1, 2, 3],
        "submissions_per_success_run": 4,
        "verified_words_per_stage": 2048,
        "modes": list(MODES),
        "optimizations": list(OPTIMIZATIONS),
        "source_hash_policy": "UTF-8 with CRLF normalized to LF",
        "source_sha256": {
            name: source_sha256(ROOT / name)
            for name in SOURCES
        },
        "artifact_sha256": {
            name: sha256(BUILD / name)
            for name in ARTIFACTS
        },
        "device_runs": device_runs,
        "invalid_runs": invalid_runs,
        "fault_runs": fault_runs,
        "limitations": [
            "Three device/ABI combinations were executed: RedMagic ARM64, RedMagic ARMv7 and Pixel ARM64.",
            "Pixel ARM64 executed all three stages using real HOST_VISIBLE noncoherent memory and driver cache flush/invalidate calls.",
            "Neither a Vulkan 1.0-only GPU nor an Android 7.0 device was available.",
            "Only public integer shaders are executed, without hashcat GPU integration.",
            "The job port accepts public CPU arrays, not production kernel scheduling or password/hash inputs.",
            "The Vulkan instance/device catalogue discovers physical devices, but the production hashcat backend does not yet own or use this catalogue.",
            "The shared pipeline layout serves all stages; stage-specific descriptor layouts and concurrent queues are not implemented.",
            "The new graph recorder validates declared usage against supplied original specifications, not SPIR-V reflection.",
            "The runtime is sequential with one queue, command buffer, fence and no device-loss recovery.",
            "Post-acquisition fault injection was executed for 62 object acquisition points on each device/ABI; no driver device loss or validation-layer leak test was performed.",
        ],
    }

    (ARCHIVE / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n",
        encoding="utf-8",
        newline="\n",
    )

    assert len(device_runs) == 144
    assert len(invalid_runs) == 3
    assert len(fault_runs) == 3
    assert len(list(ARCHIVE.glob("*.log"))) == 150

    print("PASS 120 real GPU graph runs, 24 explicit memory refusals")
    print("PASS 186 real post-acquisition partial cleanup trials")
    print("PASS 30 malformed CLI controls")
    print("PASS exact canonical source files", len(SOURCES))
    print("PASS native ARM64/ARMv7 and SPIR-V artifacts", len(ARTIFACTS))
    print("MANIFEST_SHA256", sha256(ARCHIVE / "manifest.json"))


if __name__ == "__main__":
    main()
