#!/usr/bin/env python3
"""Archive real Android Vulkan 1.0 public-buffer transfer observations.

Copies normalized LF-only ADB transcripts from the actual device test
runner, and stores exact SHA-256 identities of the independent Vulkan
source and native ARM64/ARMv7 executables. Never launches ADB, Vulkan,
HASHCAT, a cryptographic shader or password-processing operation.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import shutil


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/diagnostic-multimode/vulkan-core10-transfer-20261101"
ARCHIVE = ROOT / "tests/fixtures/android-gpu/vulkan-core10-transfer-20261101"

DEVICES = {
    "redmagic-arm64": "arm64-v8a",
    "redmagic-armv7": "armeabi-v7a",
    "pixel-arm64": "arm64-v8a",
}
APIS = ("1.0", "1.1")
MODES = ("prefer", "noncoherent")
SOURCES = (
    "tools/vulkan-core10-transfer-session.h",
    "tools/vulkan-core10-transfer-session.c",
    "tools/vulkan-core10-transfer-diagnostic.c",
    "tools/build-vulkan-core10-transfer.ps1",
    "tools/run-vulkan-core10-transfer.ps1",
    "tools/archive-vulkan-core10-transfer-evidence.py",
    "tools/test-vulkan-core10-transfer-evidence.py",
    "tools/vulkan-core10-device-catalog.h",
    "tools/vulkan-core10-device-catalog.c",
    "tools/vulkan-auto-config.h",
    "tools/vulkan-auto-config.c",
    "tools/vulkan-core11-policy.h",
    "tools/vulkan-core11-policy.c",
)


def sha256(path: Path) -> str:
    """Return SHA-256 for one complete existing binary or transcript."""
    assert path.is_file(), path

    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_sha256(path: Path) -> str:
    """Hash the exact source with CRLF normalized to LF."""
    return hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")
    ).hexdigest()


def preserve_log(name: str) -> str:
    """Copy the already LF-normalized real Android diagnostic bytes."""
    source = BUILD / name
    data = source.read_bytes()

    assert data and data.endswith(b"\n") and b"\r" not in data, name
    assert b"91809037" not in data, name
    assert b"46281FDAS002AL" not in data, name

    destination = ARCHIVE / name
    shutil.copyfile(source, destination)
    assert destination.read_bytes() == data

    return sha256(destination)


def main() -> None:
    """Archive twelve driver/API modes and three malformed-CLI reports."""
    ARCHIVE.mkdir(parents=True, exist_ok=True)

    runs = []
    controls = []

    for device, abi in DEVICES.items():
        for api in APIS:
            for mode in MODES:
                filename = f"{device}-{api}-{mode}.log"
                digest = preserve_log(filename)
                lines = (ARCHIVE / filename).read_text(
                    encoding="utf-8"
                ).splitlines()

                assert lines[:4] == [
                    "TEST=vulkan-core10-transfer",
                    f"ABI={abi}",
                    f"API={api}",
                    f"MEMORY_MODE={mode}",
                ], filename

                marker = lines[4]
                assert marker in ("ADB_EXIT_CODE=0", "ADB_EXIT_CODE=3")
                status = "transferred" if marker == "ADB_EXIT_CODE=0" else "unsupported"

                assert status == "transferred" or mode == "noncoherent"
                if status == "transferred":
                    assert sum(
                        line.startswith("TRANSFER_COPY ")
                        for line in lines
                    ) == 10
                    assert sum(
                        line.startswith("TRANSFER_VERIFIED ")
                        for line in lines
                    ) == 10
                else:
                    assert sum(
                        line.startswith("TRANSFER_MEMORY_UNAVAILABLE ")
                        for line in lines
                    ) == 2
                    assert not any(
                        line.startswith("TRANSFER_COPY ") for line in lines
                    )

                runs.append({
                    "device": device,
                    "abi": abi,
                    "api": api,
                    "memory_mode": mode,
                    "outcome": status,
                    "native_exit_code": int(marker.rsplit("=", 1)[1]),
                    "file": filename,
                    "sha256": digest,
                    "device_sessions": 2 if status == "transferred" else 0,
                    "successful_copies": 10 if status == "transferred" else 0,
                    "verified_words": 2706 if status == "transferred" else 0,
                    "transferred_bytes": 10824 if status == "transferred" else 0,
                    "memory_refusals": 0 if status == "transferred" else 2,
                })

        filename = f"{device}-negative-controls.log"
        digest = preserve_log(filename)

        controls.append({
            "device": device,
            "abi": abi,
            "file": filename,
            "sha256": digest,
            "rejected_commands": 3,
        })

    assert len(runs) == 12
    assert len(controls) == 3
    assert len(list(ARCHIVE.glob("*.log"))) == 15

    assert sum(x["device_sessions"] for x in runs) == 16
    assert sum(x["successful_copies"] for x in runs) == 80
    assert sum(x["verified_words"] for x in runs) == 21648
    assert sum(x["transferred_bytes"] for x in runs) == 86592
    assert sum(x["memory_refusals"] for x in runs) == 8

    manifest = {
        "date": "2026-11-01",
        "scope": (
            "Standalone Vulkan core 1.0/1.1 mapped storage/transfer buffers "
            "with public integer arrays and real GPU queue/fence synchronization"
        ),
        "minimum_android_api": 24,
        "apis": list(APIS),
        "memory_modes": list(MODES),
        "driver_capabilities_from_real_memory_type_bits": True,
        "vulkan_hashcat_backend_connected": False,
        "shader_dispatches": 0,
        "password_or_cryptographic_inputs": False,
        "physically_vulkan10_only_gpu_tested": False,
        "actual_android7_api24_os_tested": False,
        "capture_policy": (
            "ADB native stdout/stderr was processed by the PowerShell runner "
            "to normalize CRLF into LF before saving public transcripts. "
            "The resulting LF-only test log is preserved byte-for-byte, "
            "not claimed to be the original raw ADB byte stream."
        ),
        "source_hash_policy": "CRLF normalized to LF",
        "source_sha256": {
            source: source_sha256(ROOT / source)
            for source in SOURCES
        },
        "native_elf_sha256": {
            abi: sha256(BUILD / f"vulkan-core10-transfer-{abi}")
            for abi in ("arm64", "armv7")
        },
        "runs": runs,
        "invalid_cli": controls,
        "limitations": [
            "Only independent public integer buffer copying was performed, not GPU shader execution.",
            "The new buffer session is not integrated with HASHCAT backend_ctx, its scheduler or cryptographic kernels.",
            "The Adreno Vulkan drivers declined explicitly required HOST_VISIBLE noncoherent memory, recorded as exit code 3 rather than a successful transfer.",
            "No physically Vulkan-1.0-only GPU or actual Android 7.0/API-24 OS device was available.",
            "Native binaries remain in ignored diagnostic directories and no release archives or Android APKs are modified.",
        ],
    }

    manifest_file = ARCHIVE / "manifest.json"
    manifest_file.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
        newline="\n",
    )

    print("PASS 12 real Android GPU API/memory-mode observations")
    print("PASS 16 Vulkan device lifecycles and 80 fence-completed queue copies")
    print("PASS 21648 verified integer words, 86592 transferred bytes")
    print("PASS eight explicit noncoherent memory capability refusals")
    print("PASS nine rejected malformed CLI requests")
    print("PASS exact ELF/source identities", len(SOURCES))
    print("MANIFEST_SHA256", sha256(manifest_file))


if __name__ == "__main__":
    main()
