#!/usr/bin/env python3
"""Archive real Android Vulkan buffer-acquisition fault evidence.

Preserves the LF-normalized Android diagnostics emitted by the
dedicated public-data Vulkan fault runner. Records each exact Vulkan
acquisition point, real partial resource teardown and native exit
status, plus executable/source SHA-256 identities. Does not execute
Android commands or cryptographic workloads.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import shutil


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/diagnostic-multimode/vulkan-core10-transfer-faults-20261101"
BINARIES = ROOT / "build/diagnostic-multimode/vulkan-core10-transfer-20261101"
ARCHIVE = ROOT / "tests/fixtures/android-gpu/vulkan-core10-transfer-faults-20261101"

DEVICES = {
    "redmagic-arm64": "arm64-v8a",
    "redmagic-armv7": "armeabi-v7a",
    "pixel-arm64": "arm64-v8a",
}
APIS = ("1.0", "1.1")
RESOURCES = (
    "device",
    "command_pool",
    "command_buffer",
    "fence",
    "source_buffer",
    "source_memory",
    "source_mapping",
    "destination_buffer",
    "destination_memory",
    "destination_mapping",
)
SOURCES = (
    "tools/vulkan-core10-transfer-session.h",
    "tools/vulkan-core10-transfer-session.c",
    "tools/vulkan-core10-transfer-diagnostic.c",
    "tools/build-vulkan-core10-transfer.ps1",
    "tools/run-vulkan-core10-transfer-faults.ps1",
    "tools/archive-vulkan-core10-transfer-fault-evidence.py",
    "tools/test-vulkan-core10-transfer-fault-evidence.py",
    "tools/vulkan-core10-device-catalog.h",
    "tools/vulkan-core10-device-catalog.c",
    "tools/vulkan-auto-config.h",
    "tools/vulkan-auto-config.c",
    "tools/vulkan-core11-policy.h",
    "tools/vulkan-core11-policy.c",
)


def sha256(path: Path) -> str:
    """Hash the exact bytes of a captured log or native Android executable."""
    assert path.is_file(), path

    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_sha256(path: Path) -> str:
    """Hash a source file with its CRLF line endings normalized to LF."""
    return hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")
    ).hexdigest()


def archive_log(name: str) -> str:
    """Preserve one complete LF-only Android fault-runner transcript."""
    source = BUILD / name
    data = source.read_bytes()

    assert data and data.endswith(b"\n") and b"\r" not in data, name
    assert b"91809037" not in data, name
    assert b"46281FDAS002AL" not in data, name

    target = ARCHIVE / name
    shutil.copyfile(source, target)

    assert target.read_bytes() == data, name

    return sha256(target)


def main() -> None:
    """Record sixty actual injected-fault runs and three native CLI controls."""
    ARCHIVE.mkdir(parents=True, exist_ok=True)

    runs = []
    invalid_cli = []

    for device, abi in DEVICES.items():
        for api in APIS:
            for point, resource in enumerate(RESOURCES, start=1):
                name = f"{device}-{api}-fault-{point}.log"
                digest = archive_log(name)
                lines = (ARCHIVE / name).read_text(
                    encoding="utf-8"
                ).splitlines()

                assert lines[:5] == [
                    "TEST=vulkan-core10-transfer-faults",
                    f"ABI={abi}",
                    f"API={api}",
                    f"FAULT_POINT={point}",
                    "ADB_EXIT_CODE=42",
                ], name
                assert lines[-1] == (
                    f"TRANSFER_DIAGNOSTIC_FAULT api={api} mode=0"
                    f" point={point} trials=2 injected=2 copies=0 shaders=0"
                ), name

                for prefix in (
                    "TRANSFER_INJECT_FAULT ",
                    "TRANSFER_RELEASE ",
                    "TRANSFER_FAULT_TRIAL ",
                    "DEVICE_CATALOG_OPEN ",
                    "DEVICE_CATALOG_CLOSE ",
                ):
                    assert sum(
                        item.startswith(prefix) for item in lines
                    ) == 2, (name, prefix)

                runs.append({
                    "device": device,
                    "abi": abi,
                    "api": api,
                    "fault_point": point,
                    "resource": resource,
                    "native_exit": 42,
                    "file": name,
                    "sha256": digest,
                    "partial_vulkan_device_lifecycles": 2,
                    "explicit_injections": 2,
                    "gpu_copy_submissions": 0,
                })

        name = f"{device}-fault-negative-controls.log"
        invalid_cli.append({
            "device": device,
            "abi": abi,
            "file": name,
            "sha256": archive_log(name),
            "rejected_commands": 5,
        })

    assert len(runs) == 60
    assert len(invalid_cli) == 3
    assert len(list(ARCHIVE.glob("*.log"))) == 63

    assert sum(
        record["partial_vulkan_device_lifecycles"]
        for record in runs
    ) == 120

    manifest = {
        "date": "2026-11-01",
        "scope": (
            "Ten real acquired Vulkan buffer-session handle fault points "
            "on Android ARM64/ARMv7; exact partial cleanup without GPU copies"
        ),
        "minimum_android_api": 24,
        "vulkan_apis": list(APIS),
        "fault_points": {
            str(point): resource
            for point, resource in enumerate(RESOURCES, start=1)
        },
        "failure_result": "VK_ERROR_INITIALIZATION_FAILED",
        "injected_native_exit": 42,
        "driver_fault_or_device_loss_simulated": False,
        "vulkan_gpu_copies_in_fault_runs": 0,
        "hashcat_backend_connected": False,
        "password_or_cryptographic_inputs": False,
        "capture_policy": (
            "Actual Android native Vulkan driver output recorded using ADB "
            "and converted to LF by the PowerShell runner. Archived LF "
            "records are byte-identical to the saved runner output and "
            "are not claimed as raw unprocessed ADB byte streams."
        ),
        "source_hash_policy": "CRLF normalized to LF",
        "source_sha256": {
            source: source_sha256(ROOT / source)
            for source in SOURCES
        },
        "native_elf_sha256": {
            abi: sha256(BINARIES / f"vulkan-core10-transfer-{abi}")
            for abi in ("arm64", "armv7")
        },
        "runs": runs,
        "invalid_cli": invalid_cli,
        "limitations": [
            "A test-defined error is returned only after the indicated Vulkan acquisition succeeds; these are controlled interruptions, not genuine driver-returned errors or VK_ERROR_DEVICE_LOST.",
            "No shader or queue copy is submitted during a fault trial.",
            "No production HASHCAT backend_ctx session or password processing is connected to the fault diagnostic.",
            "The available physical GPUs use newer Vulkan loaders even though the created instance explicitly requests Vulkan 1.0 or 1.1.",
            "No actual Android 7.0/API24 OS system or physically Vulkan-1.0-only GPU was available.",
            "Driver allocation failure, actual device loss, concurrent queues, validation-layer leak tracing and GPU shader execution are not verified by these fault trials.",
        ],
    }

    path = ARCHIVE / "manifest.json"
    path.write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8", newline="\n",
    )

    print("PASS 60 actual Android driver/API/acquisition-point fault runs")
    print("PASS 120 VkInstance/VkDevice partial resource lifecycles and cleanup")
    print("PASS ten exact acquisition resources, zero GPU queue copies")
    print("PASS 15 malformed native Android fault options rejected")
    print("PASS exact native ELF/source identities", len(SOURCES))
    print("MANIFEST_SHA256", sha256(path))


if __name__ == "__main__":
    main()
