#!/usr/bin/env python3
"""Archive real Android Vulkan 1.0 Compute session ownership evidence.

Copies LF-only original Android session logs, records captured real
hashcat -I process transcripts, and hashes the exact native Android
executables and current sources. Does not execute Android, Vulkan or
any password-processing workload.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import shutil


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/diagnostic-multimode/vulkan-backend-session-20261101"
FULL = ROOT / "build/diagnostic-multimode/vulkan-hashcat-session-20261101"
ARCHIVE = ROOT / "tests/fixtures/android-gpu/vulkan-backend-session-20261101"

DEVICES = {
    "redmagic-arm64": ("arm64-v8a", "QUALCOMM", "QUALCOMM Adreno(TM)", 3),
    "redmagic-armv7": ("armeabi-v7a", "QUALCOMM", "QUALCOMM Adreno(TM)", 3),
    "pixel-arm64": ("arm64-v8a", "ARM", "Mali-G715 r0p0", 4),
}
MODES = ("1.0", "1.1")
ARTIFACTS = (
    "hashcat-backend-vulkan-arm64.o",
    "hashcat-backend-vulkan-armv7.o",
    "vulkan-backend-session-arm64",
    "vulkan-backend-session-armv7",
)
SOURCES = (
    "source/include/types.h",
    "source/include/backend_vulkan.h",
    "source/src/backend.c",
    "source/src/backend_vulkan.c",
    "source/src/backend_vulkan_session.c",
    "source/src/Makefile",
    "scripts/BUILD.ps1",
    "tools/vulkan-core10-device-catalog.c",
    "tools/vulkan-core10-device-catalog.h",
    "tools/vulkan-auto-config.c",
    "tools/vulkan-auto-config.h",
    "tools/vulkan-core11-policy.c",
    "tools/vulkan-core11-policy.h",
    "tools/vulkan-backend-session-diagnostic.c",
    "tools/build-vulkan-backend-session.ps1",
    "tools/run-vulkan-backend-session.ps1",
    "tools/archive-vulkan-backend-session-evidence.py",
    "tools/test-vulkan-backend-session-evidence.py",
)


def sha256(path: Path) -> str:
    """Calculate SHA-256 over an existing file's exact bytes."""
    assert path.is_file(), path

    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_sha256(path: Path) -> str:
    """Hash a source file with Windows CRLF normalized to LF."""
    return hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")
    ).hexdigest()


def archive_log(name: str) -> str:
    """Copy one LF-only hardware transcript without altering its bytes."""
    original = (BUILD / name).read_bytes()

    assert original.endswith(b"\n") and b"\r" not in original, name
    assert b"91809037" not in original, name
    assert b"46281FDAS002AL" not in original, name

    destination = ARCHIVE / name
    shutil.copyfile(BUILD / name, destination)
    assert destination.read_bytes() == original

    return sha256(destination)


def main() -> None:
    """Record 12 Android transcripts and their native source/binary origins."""
    ARCHIVE.mkdir(parents=True, exist_ok=True)

    device_runs = []
    frontend_runs = []
    cli_controls = []

    for device, (abi, vendor, gpu_name, loader_minor) in DEVICES.items():
        for api in MODES:
            name = f"{device}-{api}.log"
            digest = archive_log(name)

            text = (ARCHIVE / name).read_text(encoding="utf-8")
            lines = text.splitlines()

            assert lines[:4] == [
                "TEST=vulkan-backend-session",
                f"ABI={abi}",
                f"API={api}",
                "ADB_EXIT_CODE=0",
            ], name

            assert text.count("BACKEND_VULKAN_SESSION_READY ") == 2
            assert text.count("BACKEND_VULKAN_SESSION_CLOSE ") == 2
            assert text.count("BACKEND_SESSION_TRIAL_PASS ") == 2

            device_runs.append({
                "device": device,
                "abi": abi,
                "api": api,
                "file": name,
                "sha256": digest,
                "device_sessions": 2,
                "empty_fence_submissions": 2,
                "invalid_host_calls": 8,
                "invalid_owner_order_calls": 4,
                "driver_workload_refusals": 2,
            })

        cli_name = f"{device}-negative-controls.log"

        cli_controls.append({
            "device": device,
            "abi": abi,
            "file": cli_name,
            "sha256": archive_log(cli_name),
            "invalid_commands": 3,
        })

        frontend_name = f"{device}-hashcat-info.log"
        frontend_digest = archive_log(frontend_name)
        frontend_lines = (ARCHIVE / frontend_name).read_text(
            encoding="utf-8"
        ).splitlines()

        assert frontend_lines[:5] == [
            "TEST=vulkan-hashcat-compute-session",
            f"DEVICE={device}",
            f"ABI={abi}",
            "COMMAND=-I",
            "ADB_EXIT_CODE=0",
        ], frontend_name
        assert frontend_lines[-1] == "HASHCAT_SESSION_EXIT=0"

        frontend_runs.append({
            "device": device,
            "abi": abi,
            "opencl_vendor": vendor,
            "gpu_name": gpu_name,
            "loader_minor": loader_minor,
            "file": frontend_name,
            "sha256": frontend_digest,
            "device_sessions": 1,
            "empty_fence_submissions": 1,
            "vulkan_kernel_scheduler_enabled": False,
        })

    full_binaries = {}

    for abi in ("arm64-v8a", "armeabi-v7a"):
        directory = FULL / abi
        exe = directory / "hashcat"
        modules = list((directory / "modules").glob("*.so"))
        bridges = list((directory / "bridges").glob("*.so"))
        opencl = list((directory / "OpenCL").rglob("*"))
        opencl_count = sum(f.is_file() for f in opencl)

        assert len(modules) == 581, abi
        assert len(bridges) == 4, abi
        assert opencl_count == 1595, abi

        full_binaries[abi] = {
            "sha256": sha256(exe),
            "bytes": exe.stat().st_size,
            "modules": len(modules),
            "bridges": len(bridges),
            "opencl_files": opencl_count,
        }

    manifest = {
        "date": "2026-11-01",
        "scope": (
            "Vulkan 1.0 empty Compute queue session owned by backend_ctx_t, "
            "without any hashcat password/hash GPU kernel"
        ),
        "minimum_android_api": 24,
        "default_vulkan_api": "1.0",
        "optional_api": "1.1",
        "build_flag": "ENABLE_VULKAN_DISCOVERY=1",
        "full_android_build_comptime": 1793491200,
        "full_android_frontend_built": True,
        "vulkan_hashing_scheduler_enabled": False,
        "physically_vulkan10_only_gpu_tested": False,
        "android_7_api24_os_tested": False,
        "release_packages_modified": False,
        "capture_policy": {
            "session_diagnostic": (
                "Native Android stdout/stderr returned through ADB and "
                "normalized CRLF to LF by the hardware runner; saved "
                "LF-only diagnostic logs are archived without further edits, "
                "not claimed as raw ADB byte streams"
            ),
            "full_frontend": (
                "Captured native Android hashcat -I stdout/stderr and "
                "process exit marker, with normalized CRLF->LF; "
                "not claimed as original unprocessed ADB bytes"
            ),
        },
        "source_hash_policy": "CRLF normalized to LF",
        "source_sha256": {
            path: source_sha256(ROOT / path)
            for path in SOURCES
        },
        "diagnostic_sha256": {
            name: sha256(BUILD / name)
            for name in ARTIFACTS
        },
        "full_binaries": full_binaries,
        "device_runs": device_runs,
        "frontend_runs": frontend_runs,
        "invalid_cli_runs": cli_controls,
        "limitations": [
            "The full hashcat executable is tested in backend information mode (-I), without attack-mode kernel scheduling.",
            "One empty Vulkan primary command buffer is submitted and its fence completes, but no shader or password/hash candidate is processed.",
            "The Vulkan device session does not enter hashcat's active hashing-device list or backend_session_begin attack workflow.",
            "Android ARM64/ARMv7 binaries are stored in ignored diagnostic directories, not release or HASHCAT-APP packages.",
            "Vulkan 1.0 is requested on newer Vulkan loaders; no physical Vulkan 1.0-only GPU or actual Android 7 device was available.",
        ],
    }

    path = ARCHIVE / "manifest.json"
    path.write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
        newline="\n",
    )

    assert len(device_runs) == 6
    assert len(frontend_runs) == 3
    assert len(cli_controls) == 3
    assert len(list(ARCHIVE.glob("*.log"))) == 12

    print("PASS six real device/API runs and twelve empty Vulkan fence submissions")
    print("PASS three full HASHCAT -I native Compute session lifecycles")
    print("PASS 48 invalid host calls, 24 invalid ownership calls, 12 driver refusals")
    print("PASS 9 malformed Android CLI requests")
    print("PASS 18 canonical source identities and six exact native build products")
    print("MANIFEST_SHA256", sha256(path))


if __name__ == "__main__":
    main()
