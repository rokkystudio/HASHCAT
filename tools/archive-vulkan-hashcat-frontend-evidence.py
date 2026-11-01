#!/usr/bin/env python3
"""Record static full Android HASHCAT Vulkan-discovery evidence identities.

Reads three already captured Android -I transcripts, full Android
ARM64/ARMv7 executable artifacts and exact source files. Stores their
SHA-256 provenance in a JSON manifest. Does not run HASHCAT, Android
ADB, Vulkan or any password-processing workload.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/diagnostic-multimode/vulkan-hashcat-frontend-20261101"
ARCHIVE = ROOT / "tests/fixtures/android-gpu/vulkan-hashcat-frontend-20261101"

DEVICES = {
    "redmagic-arm64": {
        "abi": "arm64-v8a",
        "opencl_vendor": "QUALCOMM",
        "gpu_name": "QUALCOMM Adreno(TM)",
        "loader_minor": 3,
    },
    "redmagic-armv7": {
        "abi": "armeabi-v7a",
        "opencl_vendor": "QUALCOMM",
        "gpu_name": "QUALCOMM Adreno(TM)",
        "loader_minor": 3,
    },
    "pixel-arm64": {
        "abi": "arm64-v8a",
        "opencl_vendor": "ARM",
        "gpu_name": "Mali-G715 r0p0",
        "loader_minor": 4,
    },
}

SOURCES = (
    "scripts/BUILD.ps1",
    "source/src/Makefile",
    "source/src/backend.c",
    "source/src/backend_vulkan.c",
    "source/include/backend_vulkan.h",
    "source/include/types.h",
    "tools/vulkan-auto-config.c",
    "tools/vulkan-auto-config.h",
    "tools/vulkan-core11-policy.c",
    "tools/vulkan-core11-policy.h",
    "tools/vulkan-core10-device-catalog.c",
    "tools/vulkan-core10-device-catalog.h",
    "tools/archive-vulkan-hashcat-frontend-evidence.py",
    "tools/test-vulkan-hashcat-frontend-evidence.py",
)


def sha256(path: Path) -> str:
    """Calculate SHA-256 over the complete binary or transcript bytes."""
    assert path.is_file(), path

    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_sha256(path: Path) -> str:
    """Hash source content after normalizing any Windows CRLF endings."""
    return hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")
    ).hexdigest()


def main() -> None:
    """Create a reproducible evidence manifest from already captured files."""
    assert ARCHIVE.is_dir(), ARCHIVE

    runs = []

    for name, properties in DEVICES.items():
        file = f"{name}-backend-info.log"
        path = ARCHIVE / file
        data = path.read_bytes()

        assert data and data.endswith(b"\n"), file
        assert b"\r" not in data, file
        assert b"91809037" not in data, file
        assert b"46281FDAS002AL" not in data, file

        lines = data.decode("utf-8").splitlines()
        assert lines[:4] == [
            "TEST=vulkan-hashcat-real-frontend",
            f"ABI={properties['abi']}",
            "COMMAND=-I",
            "ADB_EXIT_CODE=0",
        ], file
        assert lines[-1] == "HC_NATIVE_EXIT=0", file

        runs.append({
            "device": name,
            **properties,
            "file": file,
            "sha256": sha256(path),
            "vulkan_instance_lifecycles": 1,
            "hashcat_opencl_backend_retained": True,
            "vulkan_hash_scheduler_enabled": False,
        })

    binaries = {}

    for abi in ("arm64-v8a", "armeabi-v7a"):
        root = BUILD / abi
        exe = root / "hashcat"
        assert exe.is_file(), exe

        modules = sorted((root / "modules").glob("*.so"))
        bridges = sorted((root / "bridges").glob("*.so"))
        opencl = sorted(
            path for path in (root / "OpenCL").rglob("*") if path.is_file()
        )

        assert len(modules) == 581, abi
        assert len(bridges) == 4, abi
        assert len(opencl) == 1595, abi

        binaries[abi] = {
            "sha256": sha256(exe),
            "size_bytes": exe.stat().st_size,
            "modules": len(modules),
            "bridges": len(bridges),
            "opencl_files": len(opencl),
        }

    manifest = {
        "date": "2026-11-01",
        "scope": "Actual Android HASHCAT -I process with optional Vulkan 1.0 discovery",
        "build_flag": "ENABLE_VULKAN_DISCOVERY=1",
        "target_android_api": 24,
        "required_vulkan_api": "1.0",
        "full_hashcat_android_frontend_linked": True,
        "hashcat_vulkan_kernel_scheduling": False,
        "release_archives_modified": False,
        "physically_vulkan10_only_gpu_tested": False,
        "android_7_api24_device_tested": False,
        "unrar_disabled_for_api24": True,
        "shared_assets_from_executable_directory": True,
        "capture_source_commit": "32382a5487c470f1c4e2dfe4b96a58b307830c49",
        "source_hash_policy": "CRLF normalized to LF",
        "source_sha256_scope": (
            "Current checked-out source used for regression checking; "
            "the capture_source_commit identifies the original native build inputs"
        ),
        "capture_policy": (
            "Native HASHCAT stdout/stderr lines extracted from stored real "
            "ADB command transcripts, with enclosing host report headings "
            "removed and line endings normalized to LF. The exit marker "
            "comes from the native Android process. These are not claimed "
            "to be byte-for-byte unmodified original ADB streams."
        ),
        "source_sha256": {
            file: source_sha256(ROOT / file) for file in SOURCES
        },
        "binaries": binaries,
        "runs": runs,
        "limitations": [
            "The complete native executable was built; no Android APK was produced or modified.",
            "Only backend information (-I) and version reporting were tested on physical devices.",
            "Vulkan devices are discovered inside backend_ctx but are not scheduled as hashcat kernel devices.",
            "The observed normal GPU backend is OpenCL, not Vulkan Compute execution.",
            "Android 7/API24 system hardware and physically Vulkan-1.0-only GPUs were not available.",
            "Build outputs remain under ignored diagnostic directories; release zip archives and HASHCAT-APP are untouched.",
        ],
    }

    (ARCHIVE / "manifest.json").write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8", newline="\n",
    )

    assert len(list(ARCHIVE.glob("*.log"))) == 3

    print("PASS three actual Android HASHCAT -I Vulkan discovery transcripts")
    print("PASS two full Android ARM64/ARMv7 executables and 1162 modules")
    print("PASS SHA-256 source identities", len(SOURCES))
    print("MANIFEST_SHA256", sha256(ARCHIVE / "manifest.json"))


if __name__ == "__main__":
    main()
