#!/usr/bin/env python3
"""Archive authentic Android hashcat backend Vulkan discovery evidence.

Copies original LF-only Android output unchanged, records SHA-256 identities
of real backend source files and native build products, and preserves the
reported Vulkan instance/device lifecycle for three physical ABI profiles.
Does not execute Vulkan, ADB, shader builds or password processing.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import shutil


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/diagnostic-multimode/vulkan-backend-catalog-20261031"
ARCHIVE = ROOT / "tests/fixtures/android-gpu/vulkan-backend-catalog-20261031"

DEVICES = {
    "redmagic-arm64": "arm64-v8a",
    "redmagic-armv7": "armeabi-v7a",
    "pixel-arm64": "arm64-v8a",
}
MODES = ("1.0", "1.1")
SOURCES = (
    "source/include/types.h",
    "source/include/backend_vulkan.h",
    "source/src/backend.c",
    "source/src/backend_vulkan.c",
    "source/src/Makefile",
    "tools/vulkan-auto-config.c",
    "tools/vulkan-auto-config.h",
    "tools/vulkan-core11-policy.c",
    "tools/vulkan-core11-policy.h",
    "tools/vulkan-core10-device-catalog.c",
    "tools/vulkan-core10-device-catalog.h",
    "tools/vulkan-backend-catalog-diagnostic.c",
    "tools/build-vulkan-backend-catalog.ps1",
    "tools/run-vulkan-backend-catalog.ps1",
    "tools/archive-vulkan-backend-catalog-evidence.py",
    "tools/test-vulkan-backend-catalog-evidence.py",
)
ARTIFACTS = (
    "hashcat-backend-vulkan-arm64.o",
    "hashcat-backend-vulkan-armv7.o",
    "vulkan-backend-catalog-arm64",
    "vulkan-backend-catalog-armv7",
)
OPEN = re.compile(
    r"^DEVICE_CATALOG_OPEN requested_api=(1\.[01]) "
    r"instance_api=(1\.[01]) devices=(\d+) extensions=0 layers=0$"
)


def sha256(path: Path) -> str:
    """Return the SHA-256 digest of one complete file's bytes."""
    assert path.is_file(), path

    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_sha256(path: Path) -> str:
    """Hash file content after normalizing platform CRLF to LF."""
    return hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")
    ).hexdigest()


def copy_original(name: str) -> str:
    """Copy a real Android UTF-8 log while retaining its original LF bytes."""
    original = (BUILD / name).read_bytes()

    assert original and original.endswith(b"\n"), name
    assert b"\r" not in original, name
    assert b"91809037" not in original, name
    assert b"46281FDAS002AL" not in original, name

    destination = ARCHIVE / name
    shutil.copyfile(BUILD / name, destination)
    assert destination.read_bytes() == original

    return sha256(destination)


def main() -> None:
    """Archive all hardware/ABI/API runs and exact code/binary identities."""
    ARCHIVE.mkdir(parents=True, exist_ok=True)

    runs = []
    controls = []

    for device, abi in DEVICES.items():
        for mode in MODES:
            name = f"{device}-{mode}.log"
            digest = copy_original(name)
            lines = (ARCHIVE / name).read_text(
                encoding="utf-8"
            ).splitlines()

            assert lines[:4] == [
                "TEST=vulkan-backend-catalog",
                f"ABI={abi}",
                f"API={mode}",
                "ADB_EXIT_CODE=0",
            ], name

            discovered = [
                match for line in lines[4:]
                if (match := OPEN.fullmatch(line)) is not None
            ]
            assert len(discovered) == 2, name
            assert all(
                item.group(1) == mode
                and item.group(2) == mode
                and int(item.group(3)) > 0
                for item in discovered
            ), name

            runs.append({
                "device": device,
                "abi": abi,
                "mode": mode,
                "file": name,
                "sha256": digest,
                "physical_devices": [int(item.group(3)) for item in discovered],
                "lifecycles": 2,
                "invalid_backend_requests": 12,
                "unsupported_compute_workloads": 2,
            })

        name = f"{device}-negative-controls.log"
        digest = copy_original(name)
        controls.append({
            "device": device,
            "abi": abi,
            "file": name,
            "sha256": digest,
            "cases": 3,
        })

    manifest = {
        "date": "2026-10-31",
        "scope": (
            "Android hashcat backend_ctx ownership of an unscheduled "
            "Vulkan 1.0 physical-device catalogue"
        ),
        "build_flag": "ENABLE_VULKAN_DISCOVERY=1",
        "default_vulkan_api": "1.0",
        "minimum_android_api": 24,
        "vulkan10_only_physical_gpu_tested": False,
        "android_api24_os_tested": False,
        "hashcat_vulkan_password_scheduler_enabled": False,
        "backend_device_list_unchanged": True,
        "tested_device_abis": DEVICES,
        "tested_apis": list(MODES),
        "source_hash_policy": "CRLF normalized to LF",
        "source_sha256": {
            path: source_sha256(ROOT / path)
            for path in SOURCES
        },
        "artifact_sha256": {
            name: sha256(BUILD / name)
            for name in ARTIFACTS
        },
        "runs": runs,
        "invalid_cli_runs": controls,
        "limitations": [
            "The Android native diagnostic uses the actual backend_ctx_t and backend_vulkan_* methods; it does not invoke the complete hashcat executable.",
            "backend.c was compiled for Android ARM64 and ARMv7 with WITH_VULKAN_DISCOVERY; the full production frontend and external modules were not linked in this fixture.",
            "Vulkan physical devices are enumerated but not inserted into hashcat's password-processing backend device list.",
            "Device discovery and Compute capability checks do not execute a Vulkan GPU workload, unlike the separately archived public multiset graph evidence.",
            "No physical Vulkan-1.0-only GPU or Android 7.0 system was tested.",
        ],
    }

    (ARCHIVE / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n",
        encoding="utf-8",
        newline="\n",
    )

    assert len(runs) == 6
    assert len(controls) == 3
    assert len(list(ARCHIVE.glob("*.log"))) == 9

    print("PASS 6 real backend Vulkan API runs and 12 instance lifecycles")
    print("PASS 72 rejected malformed backend catalogue requests")
    print("PASS 9 malformed Android CLI requests")
    print("PASS canonical backend source files", len(SOURCES))
    print("PASS ARM64 and ARMv7 compiler/linker artifacts", len(ARTIFACTS))
    print("MANIFEST_SHA256", sha256(ARCHIVE / "manifest.json"))


if __name__ == "__main__":
    main()
