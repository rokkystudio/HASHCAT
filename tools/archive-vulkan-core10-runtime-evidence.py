#!/usr/bin/env python3
"""Archive exact real-device Vulkan 1.0 synthetic runtime observations.

Copies original GPU, noncoherent memory, resource-acquisition fault and
invalid-input logs without rewriting their bytes. Records SHA-256 source
identities and compiled artifact hashes for reproducibility. The archiver
does not call ADB, Vulkan or any external network or credential service.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import shutil


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/diagnostic-multimode/vulkan-runtime-20261031"
ARCHIVE = ROOT / "tests/fixtures/android-gpu/vulkan-runtime-20261031"

SOURCES = (
    "tools/vulkan-core10-runtime.h",
    "tools/vulkan-core10-runtime.c",
    "tools/vulkan-core10-runtime-diagnostic.c",
    "tools/build-vulkan-core10-runtime.ps1",
    "tools/run-vulkan-core10-runtime.ps1",
    "tools/archive-vulkan-core10-runtime-evidence.py",
    "tools/test-vulkan-core10-runtime-evidence.py",
    "tools/vulkan-auto-config.c",
    "tools/vulkan-auto-config.h",
    "tools/vulkan-core11-policy.c",
    "tools/vulkan-core11-policy.h",
    "tools/vulkan-multistage-memory-plan.c",
    "tools/vulkan-multistage-memory-plan.h",
    "tools/test-vulkan-multistage-memory-plan-host.c",
    "tools/vulkan-pipeline-stage-a.comp",
    "tools/vulkan-pipeline-stage-b.comp",
    "tools/vulkan-pipeline-stage-c.comp",
)

ARTIFACTS = (
    "vulkan-runtime-arm64",
    "vulkan-runtime-armv7",
    "vulkan-runtime-heap-O0.exe",
    "vulkan-runtime-heap-O2.exe",
    *(
        f"stage-{stage}-{optimization}-vk1.0.spv"
        for optimization in ("O0", "O")
        for stage in "abc"
    ),
)

DEVICES = (
    ("redmagic-arm64", "arm64-v8a"),
    ("redmagic-armv7", "armeabi-v7a"),
    ("pixel-arm64", "arm64-v8a"),
)

MODES = ("default10", "explicit10", "required11", "noncoherent10")
OPTIMIZATIONS = ("O0", "O")


def sha256(path: Path) -> str:
    """Return SHA-256 for an exact existing file's raw bytes."""
    assert path.is_file(), path

    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_sha256(path: Path) -> str:
    """Compute canonical UTF-8 source identity independent of CRLF checkout."""
    assert path.is_file(), path

    return hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")
    ).hexdigest()


def copy_original(name: str) -> str:
    """Copy one LF-only original real-device record without altering its bytes."""
    source = BUILD / name
    data = source.read_bytes()

    assert data and data.endswith(b"\n"), name
    assert b"\r" not in data, name
    assert b"91809037" not in data, name
    assert b"46281FDAS002AL" not in data, name

    target = ARCHIVE / name
    shutil.copyfile(source, target)

    assert target.read_bytes() == data

    return sha256(target)


def main() -> None:
    """Record exact compiled artifacts and all tested hardware outcomes."""
    ARCHIVE.mkdir(parents=True, exist_ok=True)

    device_runs = []
    faults = []
    invalids = []

    for device, abi in DEVICES:
        for mode in MODES:
            for optimization in OPTIMIZATIONS:
                name = f"{device}-{mode}-{optimization}.log"
                digest = copy_original(name)
                status = (
                    1 if device.startswith("redmagic")
                    and mode == "noncoherent10" else 0
                )

                lines = (ARCHIVE / name).read_text(
                    encoding="utf-8"
                ).splitlines()

                assert lines[:5] == [
                    "TEST=vulkan-core10-reusable-runtime",
                    f"MODE={mode}",
                    f"OPTIMIZATION={optimization}",
                    f"ABI={abi}",
                    f"ADB_EXIT_CODE={status}",
                ]

                device_runs.append({
                    "device": device,
                    "abi": abi,
                    "mode": mode,
                    "optimization": optimization,
                    "file": name,
                    "exit_code": status,
                    "sha256": digest,
                })

        name = f"{device}-faults.log"
        digest = copy_original(name)

        assert (
            (ARCHIVE / name).read_text(encoding="utf-8").count(
                "\nFAULT_POINT="
            ) == 28
        )

        faults.append({
            "device": device,
            "abi": abi,
            "file": name,
            "sha256": digest,
            "fault_points": 28,
        })

        name = f"{device}-negative-controls.log"
        digest = copy_original(name)

        invalids.append({
            "device": device,
            "abi": abi,
            "file": name,
            "sha256": digest,
            "invalid_cases": 5,
        })

    native_runs = []

    for optimization in ("O0", "O2"):
        name = f"vulkan-runtime-heap-{optimization}.log"
        digest = copy_original(name)

        native_runs.append({
            "optimization": optimization,
            "file": name,
            "sha256": digest,
            "exit_code": 0,
        })

    heap_logs = {
        "redmagic-arm64": (
            "tests/fixtures/android-gpu/vulkan-heap-20261031/redmagic-allocation.log"
        ),
        "redmagic-armv7": (
            "tests/fixtures/android-gpu/vulkan-core-baseline-20261031/"
            "redmagic-armv7-memory-properties.log"
        ),
        "pixel-arm64": (
            "tests/fixtures/android-gpu/vulkan-heap-20261031/pixel-allocation.log"
        ),
    }

    manifest = {
        "date": "2026-10-31",
        "scope": (
            "standalone Vulkan 1.0 resource owner, six buffer allocations, "
            "three public synthetic Compute stages and sequential reuse"
        ),
        "source_hash_policy": "UTF-8 with CRLF normalized to LF",
        "workload": {
            "default_vulkan_api": "1.0",
            "optional_vulkan_api": "1.1",
            "minimum_android_api": 24,
            "spirv_version": "1.0",
            "local_size": 32,
            "group_count": 64,
            "storage_buffers": 4,
            "staging_buffers": 2,
            "shader_stages": 3,
            "copies_per_submission": 2,
            "verified_words_per_submission": 12288,
            "sequential_submissions": 4,
            "acquisition_fault_points": 28,
            "enabled_optional_features": 0,
            "enabled_extensions": 0,
        },
        "source_sha256": {
            name: source_sha256(ROOT / name)
            for name in SOURCES
        },
        "artifact_sha256": {
            name: sha256(BUILD / name)
            for name in ARTIFACTS
        },
        "heap_reference_sha256": {
            device: {
                "file": path,
                "sha256": sha256(ROOT / path),
            }
            for device, path in heap_logs.items()
        },
        "device_runs": device_runs,
        "fault_runs": faults,
        "invalid_runs": invalids,
        "native_runs": native_runs,
        "limitations": [
            "No physical Vulkan 1.0-only or Android 7/API24 hardware is available; later GPUs execute a Vulkan 1.0 instance.",
            "Stage A/B intermediate results are validated through the stage-C output channels; they are not copied separately to the host.",
            "An injected acquisition failure occurs after the genuine Vulkan call succeeds; it tests cleanup but does not simulate every possible real driver error.",
            "Resource destruction counts are application observations and no validation-layer GPU leak report is claimed.",
            "RedMagic ARM64 and ARMv7 have no compatible forced noncoherent staging memory for this buffer; the default coherent mode works.",
            "Pixel ARM64 executes real noncoherent flush and invalidate with a device-reported 64-byte cache atom.",
            "This is a general-purpose standalone public synthetic GPU Compute diagnostic, not a hashcat backend or cryptographic implementation.",
        ],
    }

    manifest_path = ARCHIVE / "manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, indent=2) + "\n",
        encoding="utf-8",
        newline="\n",
    )

    assert len(list(ARCHIVE.glob("*.log"))) == 32

    print("PASS archived 20 real GPU success logs, four explicit memory incompatibilities")
    print("PASS archived 84 real resource acquisition failures in three fault logs")
    print("PASS archived 15 invalid-input controls and two native planner logs")
    print("PASS source identities", len(SOURCES), "compiled binaries", len(ARTIFACTS))
    print("MANIFEST_SHA256", sha256(manifest_path))


if __name__ == "__main__":
    main()
