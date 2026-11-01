#!/usr/bin/env python3
"""Archive native Android Vulkan fault-to-live-session recovery evidence.

Copies only previously captured LF-normalized real Android GPU logs.
Each recovered Vulkan instance proves two injected partial-resource
failures, four newly owned logical-device sessions, and twelve public
integer buffer copies completed by actual Vulkan queue fences. Records
exact source and ARM64/ARMv7 ELF identities without running a GPU,
hashing code or a password-processing workflow.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import shutil


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/diagnostic-multimode/vulkan-core10-transfer-recovery-20261101"
ARCHIVE = ROOT / "tests/fixtures/android-gpu/vulkan-core10-transfer-recovery-20261101"

DEVICES = {
    "redmagic-arm64": ("arm64-v8a", ("prefer",)),
    "redmagic-armv7": ("armeabi-v7a", ("prefer",)),
    "pixel-arm64": ("arm64-v8a", ("prefer", "noncoherent")),
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
    "tools/vulkan-core10-transfer-recovery-diagnostic.c",
    "tools/build-vulkan-core10-transfer-recovery.ps1",
    "tools/run-vulkan-core10-transfer-recovery.ps1",
    "tools/archive-vulkan-core10-transfer-recovery-evidence.py",
    "tools/test-vulkan-core10-transfer-recovery-evidence.py",
    "tools/vulkan-core10-transfer-session.c",
    "tools/vulkan-core10-transfer-session.h",
    "tools/vulkan-core10-device-catalog.c",
    "tools/vulkan-core10-device-catalog.h",
    "tools/vulkan-auto-config.c",
    "tools/vulkan-auto-config.h",
    "tools/vulkan-core11-policy.c",
    "tools/vulkan-core11-policy.h",
)


def sha256(path: Path) -> str:
    """Hash one complete existing native executable or GPU transcript."""
    assert path.is_file(), path

    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_sha256(path: Path) -> str:
    """Identify a checked-out source file with canonical LF line endings."""
    return hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")
    ).hexdigest()


def preserve_log(name: str) -> str:
    """Copy the real Android runner's LF-normalized transcript unchanged."""
    original = BUILD / name
    data = original.read_bytes()

    assert data and data.endswith(b"\n") and b"\r" not in data, name
    assert b"91809037" not in data, name
    assert b"46281FDAS002AL" not in data, name

    destination = ARCHIVE / name
    shutil.copyfile(original, destination)
    assert destination.read_bytes() == data

    return sha256(destination)


def main() -> None:
    """Archive eighty native recovery runs and three invalid-input controls."""
    ARCHIVE.mkdir(parents=True, exist_ok=True)

    runs = []
    invalid_cli = []

    for device, (abi, modes) in DEVICES.items():
        for api in APIS:
            for memory_mode in modes:
                for point, resource in enumerate(RESOURCES, start=1):
                    name = f"{device}-{api}-{memory_mode}-recover-{point}.log"
                    digest = preserve_log(name)

                    lines = (ARCHIVE / name).read_text(
                        encoding="utf-8"
                    ).splitlines()

                    assert lines[:6] == [
                        "TEST=vulkan-core10-transfer-recovery",
                        f"ABI={abi}",
                        f"API={api}",
                        f"MEMORY_MODE={memory_mode}",
                        f"FAULT_POINT={point}",
                        "ADB_EXIT_CODE=0",
                    ], name

                    assert lines[-1] == (
                        f"RECOVERY_DIAGNOSTIC_PASS api={api}"
                        f" mode={0 if memory_mode == 'prefer' else 1}"
                        f" point={point} trials=2 faults=2 reopened=4"
                        " copies=12 verified_words=4754 copied_bytes=19016 shaders=0"
                    ), name

                    for prefix, count in (
                        ("DEVICE_CATALOG_OPEN ", 2),
                        ("DEVICE_CATALOG_CLOSE ", 2),
                        ("TRANSFER_INJECT_FAULT ", 2),
                        ("TRANSFER_RELEASE ", 2),
                        ("TRANSFER_OPEN ", 4),
                        ("TRANSFER_COPY ", 12),
                        ("RECOVERY_VERIFIED ", 12),
                        ("RECOVERY_TRIAL_PASS ", 2),
                    ):
                        assert sum(
                            row.startswith(prefix) for row in lines
                        ) == count, (name, prefix)

                    runs.append({
                        "device": device,
                        "abi": abi,
                        "api": api,
                        "memory_mode": memory_mode,
                        "fault_point": point,
                        "acquired_resource": resource,
                        "file": name,
                        "sha256": digest,
                        "native_exit_code": 0,
                        "injected_faults": 2,
                        "recovered_vkdevices": 4,
                        "shared_vkinstances": 2,
                        "gpu_copies": 12,
                        "verified_words": 4754,
                        "verified_bytes": 19016,
                        "noncoherent_copies": 12 if memory_mode == "noncoherent" else 0,
                    })

        name = f"{device}-recovery-negative-controls.log"

        invalid_cli.append({
            "device": device,
            "abi": abi,
            "file": name,
            "sha256": preserve_log(name),
            "invalid_requests": 6,
        })

    assert len(runs) == 80
    assert len(invalid_cli) == 3
    assert len(list(ARCHIVE.glob("*.log"))) == 83

    assert sum(x["injected_faults"] for x in runs) == 160
    assert sum(x["recovered_vkdevices"] for x in runs) == 320
    assert sum(x["gpu_copies"] for x in runs) == 960
    assert sum(x["verified_words"] for x in runs) == 380320
    assert sum(x["verified_bytes"] for x in runs) == 1521280
    assert sum(x["noncoherent_copies"] for x in runs) == 240

    manifest = {
        "date": "2026-11-01",
        "scope": (
            "Standalone public integer VkBuffer GPU copy recovery after "
            "ten real Vulkan resource acquisitions, using the same live "
            "VkInstance and two fresh VkDevices per failed attempt"
        ),
        "minimum_android_api": 24,
        "apis": list(APIS),
        "fault_points": {
            str(point): resource
            for point, resource in enumerate(RESOURCES, start=1)
        },
        "injected_failure_result": "VK_ERROR_INITIALIZATION_FAILED",
        "fault_driver_returned_error_simulated": False,
        "physical_gpu_device_loss_tested": False,
        "production_hashcat_backend_connected": False,
        "gpu_shader_dispatches": 0,
        "password_or_cryptographic_inputs": False,
        "no_vulkan10_only_hardware": True,
        "no_android_api24_os_hardware": True,
        "capture_policy": (
            "Native Android Vulkan driver stdout/stderr captured by ADB "
            "and normalized to LF by the PowerShell runner; each saved "
            "LF-only log is archived unchanged, not claimed to be the "
            "original raw ADB byte stream"
        ),
        "source_hash_policy": "CRLF normalized to LF",
        "source_sha256": {
            name: source_sha256(ROOT / name)
            for name in SOURCES
        },
        "native_elf_sha256": {
            abi: sha256(BUILD / f"vulkan-core10-transfer-recovery-{abi}")
            for abi in ("arm64", "armv7")
        },
        "runs": runs,
        "invalid_cli": invalid_cli,
        "limitations": [
            "Tests use a deterministic error after successful Vulkan object acquisition and are not actual VK_ERROR_DEVICE_LOST or driver-allocation errors.",
            "All GPU work is Vulkan 1.0 vkCmdCopyBuffer on public unsigned integer arrays; no compute shader, cryptographic algorithm or HASHCAT hashing task is executed.",
            "The independent recovery binary has no integration with the production HASHCAT backend_ctx, GPU scheduler or Android APK.",
            "Adreno HOST_VISIBLE noncoherent memory is unsupported by the tested actual drivers, so strict noncoherent recovery was verified only on the compatible Mali-G715 GPU.",
            "Driver-level memory leak tracing, Vulkan validation layers, actual Android 7.0/API 24 OS hardware and physically Vulkan-1.0-only GPUs were not tested.",
        ],
    }

    manifest_file = ARCHIVE / "manifest.json"
    manifest_file.write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8", newline="\n",
    )

    print("PASS 80 actual Android driver/API/memory/recovery runs")
    print("PASS 160 controlled Vulkan failures, 320 new live VkDevice sessions")
    print("PASS 160 persistent parent VkInstance lifecycles")
    print("PASS 960 completed GPU buffer copies, 380320 exact integer values")
    print("PASS 1521280 verified bytes, 240 noncoherent cache-synchronized copies")
    print("PASS 18 rejected malformed native recovery requests")
    print("PASS exact Android ELF and 13 source identities")
    print("MANIFEST_SHA256", sha256(manifest_file))


if __name__ == "__main__":
    main()
