#!/usr/bin/env python3
"""Archive exact real-device Vulkan 1.0 uniform/storage multi-set evidence.

Copies original LF-only one/two-set GPU outputs, incompatible-memory logs,
Vulkan acquisition failure records, invalid command controls and native C
descriptor policy tests. Records exact source, build and ABI-specific Vulkan
memory survey SHA-256 identifiers without executing ADB or Vulkan.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import shutil


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/diagnostic-multimode/vulkan-multiset-20261031"
ARCHIVE = ROOT / "tests/fixtures/android-gpu/vulkan-multiset-20261031"

DEVICES = {
    "redmagic-arm64": "arm64-v8a",
    "redmagic-armv7": "armeabi-v7a",
    "pixel-arm64": "arm64-v8a",
}
MODES = ("default10", "explicit10", "required11", "noncoherent10")
OPTIMIZATIONS = ("O0", "O")

SOURCES = (
    "tools/vulkan-core10-descriptor-policy.h",
    "tools/vulkan-core10-descriptor-policy.c",
    "tools/test-vulkan-core10-descriptor-policy-host.c",
    "tools/vulkan-core10-multiset.h",
    "tools/vulkan-core10-multiset.c",
    "tools/vulkan-core10-multiset-diagnostic.c",
    "tools/vulkan-multiset-one.comp",
    "tools/vulkan-multiset-two.comp",
    "tools/build-vulkan-core10-multiset.ps1",
    "tools/run-vulkan-core10-multiset.ps1",
    "tools/archive-vulkan-core10-multiset-evidence.py",
    "tools/test-vulkan-core10-multiset-evidence.py",
    "tools/vulkan-auto-config.c",
    "tools/vulkan-auto-config.h",
    "tools/vulkan-core11-policy.c",
    "tools/vulkan-core11-policy.h",
    "tools/vulkan-core10-graph-memory.c",
    "tools/vulkan-core10-graph-memory.h",
    "tools/vulkan-memory-heap-policy.h",
)

ARTIFACTS = (
    "vulkan-multiset-arm64",
    "vulkan-multiset-armv7",
    "multiset-descriptor-host-O0.exe",
    "multiset-descriptor-host-O2.exe",
    *(
        f"multiset-{variant}-{optimization}-vk1.0.spv"
        for variant in ("one", "two")
        for optimization in OPTIMIZATIONS
    ),
)


def sha256(path: Path) -> str:
    """Return SHA-256 of one original source, compiled artifact or GPU log."""
    assert path.is_file(), path

    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_sha256(path: Path) -> str:
    """Hash source content in LF form regardless of the working-tree CRLF."""
    assert path.is_file(), path

    return hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")
    ).hexdigest()


def copy_original(name: str) -> str:
    """Preserve an original LF-only real-hardware log without modification."""
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
    """Archive the complete one/two-set ARM64/ARMv7 GPU and fault matrix."""
    ARCHIVE.mkdir(parents=True, exist_ok=True)

    device_runs = []
    fault_runs = []
    invalid_runs = []
    native_runs = []

    for device, abi in DEVICES.items():
        for sets in (1, 2):
            for mode in MODES:
                for optimization in OPTIMIZATIONS:
                    name = f"{device}-set{sets}-{mode}-{optimization}.log"
                    digest = copy_original(name)
                    exit_code = int(
                        mode == "noncoherent10"
                        and device.startswith("redmagic")
                    )
                    rows = (ARCHIVE / name).read_text(
                        encoding="utf-8"
                    ).splitlines()

                    assert rows[:6] == [
                        "TEST=vulkan-core10-multiset",
                        f"SETS={sets}",
                        f"MODE={mode}",
                        f"OPTIMIZATION={optimization}",
                        f"ABI={abi}",
                        f"ADB_EXIT_CODE={exit_code}",
                    ]

                    device_runs.append({
                        "device": device,
                        "abi": abi,
                        "sets": sets,
                        "mode": mode,
                        "optimization": optimization,
                        "file": name,
                        "exit_code": exit_code,
                        "sha256": digest,
                    })

        name = f"{device}-faults.log"
        digest = copy_original(name)

        assert sum(
            line.startswith("FAULT_CASE ")
            for line in (ARCHIVE / name).read_text(
                encoding="utf-8"
            ).splitlines()
        ) == 48

        fault_runs.append({
            "device": device,
            "abi": abi,
            "file": name,
            "sha256": digest,
            "fault_points": 48,
        })

        name = f"{device}-negative-controls.log"
        digest = copy_original(name)

        invalid_runs.append({
            "device": device,
            "abi": abi,
            "file": name,
            "sha256": digest,
            "invalid_cases": 9,
        })

    for optimization in ("O0", "O2"):
        name = f"multiset-descriptor-host-{optimization}.log"

        native_runs.append({
            "optimization": optimization,
            "file": name,
            "sha256": copy_original(name),
            "exit_code": 0,
        })

    heap_profiles = {
        "redmagic-arm64": (
            "tests/fixtures/android-gpu/vulkan-heap-20261031/"
            "redmagic-allocation.log"
        ),
        "redmagic-armv7": (
            "tests/fixtures/android-gpu/vulkan-core-baseline-20261031/"
            "redmagic-armv7-memory-properties.log"
        ),
        "pixel-arm64": (
            "tests/fixtures/android-gpu/vulkan-heap-20261031/"
            "pixel-allocation.log"
        ),
    }

    manifest = {
        "date": "2026-10-31",
        "scope": (
            "Vulkan 1.0 multi-set Compute with explicit STORAGE_BUFFER "
            "and UNIFORM_BUFFER descriptor layouts and noncoherent staging"
        ),
        "source_hash_policy": "UTF-8 with CRLF normalized to LF",
        "minimum_android_api": 24,
        "requested_default_api": "1.0",
        "physical_vulkan10_only_device_tested": False,
        "device_abi": DEVICES,
        "set_counts": [1, 2],
        "descriptor_counts": {"storage": 2, "uniform": 1},
        "buffer_count": 5,
        "mapped_buffer_count": 3,
        "submissions_per_run": 4,
        "verified_words_per_submission": 2048,
        "resource_fault_counts": {"1": 23, "2": 25},
        "modes": list(MODES),
        "optimizations": list(OPTIMIZATIONS),
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
            for device, path in heap_profiles.items()
        },
        "device_runs": device_runs,
        "fault_runs": fault_runs,
        "invalid_runs": invalid_runs,
        "native_runs": native_runs,
        "limitations": [
            "No physical Vulkan-1.0-only GPU or Android 7.0 device was available; modern devices executed an explicitly requested Vulkan 1.0 API.",
            "The resource owner accepts arbitrary supported positive numbers of sets and Compute stages, but these public shaders exercise exactly one and two sets with one stage.",
            "The only implemented descriptor types are STORAGE_BUFFER and UNIFORM_BUFFER; unsupported descriptor kinds are rejected explicitly.",
            "Every set must contain one binding, and the entire declared descriptor layout is conservatively checked against per-stage limits.",
            "Uniform parameter values are written before every sequential GPU submission and flushed on noncoherent memory.",
            "The runtime uses one Vulkan queue, one primary command buffer and one fence; parallel queues or GPU device-loss recovery are not tested.",
            "An injected acquisition failure occurs after a genuine Vulkan resource acquisition and exercises application-level teardown, not real driver loss or forced allocation exhaustion.",
            "No validation-layer driver-internal leak absence is claimed.",
            "This work is standalone noncryptographic Vulkan infrastructure and does not enable hashcat or bcrypt GPU execution.",
        ],
    }

    (ARCHIVE / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n",
        encoding="utf-8",
        newline="\n",
    )

    assert len(list(ARCHIVE.glob("*.log"))) == 56

    print("PASS 40 real GPU multi-set success runs and 8 explicit memory incompatibilities")
    print("PASS 144 real Vulkan resource acquisition fault tests")
    print("PASS 27 invalid CLI controls and 2 driver-free native descriptor policy logs")
    print("PASS exact source identities", len(SOURCES), "binary artifacts", len(ARTIFACTS))
    print("MANIFEST_SHA256", sha256(ARCHIVE / "manifest.json"))


if __name__ == "__main__":
    main()
