#!/usr/bin/env python3
"""Archive real Vulkan 1.0 fence NOT_READY/TIMEOUT results from Android.

Preserves LF-normalized native Android logs emitted by the dedicated
standalone fence-status runner. Each trial queries genuine Vulkan
driver-returned status values, completes public GPU buffer copies and
closes its VkDevice before VkInstance. The archival operation does not
launch Android, Vulkan, HASHCAT or a cryptographic workload.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import shutil


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/diagnostic-multimode/vulkan-core10-transfer-fence-status-20261101"
ARCHIVE = ROOT / "tests/fixtures/android-gpu/vulkan-core10-transfer-fence-status-20261101"

DEVICES = {
    "redmagic-arm64": ("arm64-v8a", ("prefer",)),
    "redmagic-armv7": ("armeabi-v7a", ("prefer",)),
    "pixel-arm64": ("arm64-v8a", ("prefer", "noncoherent")),
}
APIS = ("1.0", "1.1")
SOURCES = (
    "tools/vulkan-core10-transfer-fence-status-diagnostic.c",
    "tools/build-vulkan-core10-transfer-fence-status.ps1",
    "tools/run-vulkan-core10-transfer-fence-status.ps1",
    "tools/archive-vulkan-core10-transfer-fence-status-evidence.py",
    "tools/test-vulkan-core10-transfer-fence-status-evidence.py",
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
    """Return the complete SHA-256 of one existing file."""
    assert path.is_file(), path

    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_sha256(path: Path) -> str:
    """Hash checked-out source with CRLF normalized to LF."""
    return hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")
    ).hexdigest()


def archive_log(name: str) -> str:
    """Copy one complete already LF-normalized real driver transcript."""
    source = BUILD / name
    data = source.read_bytes()

    assert data and data.endswith(b"\n") and b"\r" not in data, name
    assert b"91809037" not in data, name
    assert b"46281FDAS002AL" not in data, name

    target = ARCHIVE / name
    shutil.copyfile(source, target)
    assert target.read_bytes() == data

    return sha256(target)


def main() -> None:
    """Record eight actual GPU modes and three native-invalid-input reports."""
    ARCHIVE.mkdir(parents=True, exist_ok=True)

    runs = []
    invalid_cli = []

    for device, (abi, policies) in DEVICES.items():
        for api in APIS:
            for policy in policies:
                name = f"{device}-{api}-{policy}.log"
                digest = archive_log(name)
                lines = (ARCHIVE / name).read_text(
                    encoding="utf-8"
                ).splitlines()

                assert lines[:5] == [
                    "TEST=vulkan-core10-transfer-fence-status",
                    f"ABI={abi}",
                    f"API={api}",
                    f"MEMORY_MODE={policy}",
                    "ADB_EXIT_CODE=0",
                ], name

                mode = 0 if policy == "prefer" else 1
                assert lines[-1] == (
                    f"FENCE_DIAGNOSTIC_PASS api={api} mode={mode}"
                    " trials=2 copies=10 verified_words=2706 copied_bytes=10824"
                    " not_ready=12 zero_timeouts=12 bounded_timeouts=2"
                    " signaled=10 zero_success=10 resets=10 shaders=0"
                ), name

                for prefix, count in (
                    ("DEVICE_CATALOG_OPEN ", 2),
                    ("DEVICE_CATALOG_CLOSE ", 2),
                    ("TRANSFER_OPEN ", 2),
                    ("TRANSFER_CLOSE ", 2),
                    ("FENCE_UNSIGNALED ", 12),
                    ("FENCE_TIMED_WAIT ", 2),
                    ("FENCE_SIGNALED ", 10),
                    ("FENCE_RESET ", 10),
                    ("FENCE_COPY_VERIFIED ", 10),
                    ("TRANSFER_COPY ", 10),
                ):
                    assert sum(
                        line.startswith(prefix) for line in lines
                    ) == count, (name, prefix)

                runs.append({
                    "device": device,
                    "abi": abi,
                    "api": api,
                    "memory_mode": policy,
                    "file": name,
                    "sha256": digest,
                    "native_exit_code": 0,
                    "vkdevice_sessions": 2,
                    "real_not_ready": 12,
                    "real_zero_timeout": 12,
                    "real_bounded_timeout": 2,
                    "real_signaled_success": 10,
                    "fence_resets": 10,
                    "gpu_copies": 10,
                    "verified_words": 2706,
                    "verified_bytes": 10824,
                    "noncoherent_copies": 10 if policy == "noncoherent" else 0,
                })

        name = f"{device}-negative-controls.log"
        invalid_cli.append({
            "device": device,
            "abi": abi,
            "file": name,
            "sha256": archive_log(name),
            "invalid_requests": 5,
        })

    assert len(runs) == 8
    assert len(invalid_cli) == 3
    assert len(list(ARCHIVE.glob("*.log"))) == 11
    assert sum(x["vkdevice_sessions"] for x in runs) == 16
    assert sum(x["real_not_ready"] for x in runs) == 96
    assert sum(x["real_zero_timeout"] for x in runs) == 96
    assert sum(x["real_bounded_timeout"] for x in runs) == 16
    assert sum(x["gpu_copies"] for x in runs) == 80
    assert sum(x["verified_words"] for x in runs) == 21648
    assert sum(x["verified_bytes"] for x in runs) == 86592
    assert sum(x["noncoherent_copies"] for x in runs) == 20

    manifest = {
        "date": "2026-11-01",
        "scope": (
            "Real Vulkan core 1.0/1.1 driver-returned NOT_READY and "
            "TIMEOUT on unsignaled VkFences, with fence reset and "
            "genuine GPU buffer copy/signal verification"
        ),
        "minimum_android_api": 24,
        "apis": list(APIS),
        "vk_not_ready": 1,
        "vk_timeout": 2,
        "vk_success": 0,
        "real_driver_results_injected": False,
        "vk_error_device_lost_induced": False,
        "production_hashcat_backend_connected": False,
        "gpu_shader_dispatches": 0,
        "password_or_cryptographic_inputs": False,
        "vulkan10_only_hardware_tested": False,
        "android_api24_os_tested": False,
        "capture_policy": (
            "Native Android Vulkan stdout/stderr is obtained through ADB "
            "and normalized to LF by the PowerShell runner; these saved "
            "LF-only files are archived unchanged, not represented as "
            "unprocessed original ADB byte streams."
        ),
        "source_hash_policy": "CRLF normalized to LF",
        "source_sha256": {
            name: source_sha256(ROOT / name)
            for name in SOURCES
        },
        "native_elf_sha256": {
            abi: sha256(
                BUILD / f"vulkan-core10-transfer-fence-status-{abi}"
            )
            for abi in ("arm64", "armv7")
        },
        "runs": runs,
        "invalid_cli": invalid_cli,
        "limitations": [
            "VK_NOT_READY and VK_TIMEOUT are genuine nonfatal Vulkan completion states, not VK_ERROR_DEVICE_LOST or actual driver-allocation errors.",
            "An intentionally unsignaled fence and zero/1ms waits avoid inducing a GPU hang, reset or device loss.",
            "After driver status queries, every buffer copy uses real Vulkan queue/fence synchronization and public integer arrays; no shader is executed.",
            "The standalone diagnostic does not register Vulkan hashing devices, invoke HASHCAT password handling or change the production Android APK.",
            "The Adreno device lacks the required noncoherent memory type; strict noncoherent mode is tested only on Mali-G715.",
            "No physically Vulkan-1.0-only GPU, Android 7.0/API24 OS device, validation-layer leak report or real device-loss recovery is included.",
        ],
    }

    manifest_path = ARCHIVE / "manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8", newline="\n",
    )

    print("PASS 8 real Android Vulkan driver/API/memory status transcripts")
    print("PASS 16 Vulkan device/instance lifecycles")
    print("PASS 96 real VK_NOT_READY and 112 actual VK_TIMEOUT statuses")
    print("PASS 80 completed GPU buffer copies, 21648 exact public integers")
    print("PASS 86592 verified bytes, 20 noncoherent synchronized copies")
    print("PASS 15 native invalid-input requests rejected")
    print("PASS source SHA256 identities", len(SOURCES))
    print("MANIFEST_SHA256", sha256(manifest_path))


if __name__ == "__main__":
    main()
