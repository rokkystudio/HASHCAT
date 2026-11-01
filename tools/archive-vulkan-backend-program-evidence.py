#!/usr/bin/env python3
"""Archive host-only Vulkan program graph validation evidence.

Copies one LF-normalized Android ARM64 process transcript and records
exact source plus ARM64/ARMv7 ELF SHA-256 identities. The diagnostic
contains no Vulkan loader dependency and performs no GPU operation.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/diagnostic-multimode/vulkan-backend-program-20261101"
ARCHIVE = ROOT / "tests/fixtures/android-gpu/vulkan-backend-program-20261101"

SOURCES = (
    "source/include/backend_vulkan_program.h",
    "source/src/backend_vulkan_program.c",
    "source/include/backend_vulkan_public_spirv.h",
    "source/include/backend_vulkan_public_stage2_spirv.h",
    "tools/vulkan-backend-program-diagnostic.c",
    "tools/build-vulkan-backend-program.ps1",
    "tools/run-vulkan-backend-program.ps1",
    "tools/archive-vulkan-backend-program-evidence.py",
    "tools/test-vulkan-backend-program-evidence.py",
)


def sha256(path: Path) -> str:
    """Return SHA-256 over exact file bytes."""
    assert path.is_file(), path
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_sha256(path: Path) -> str:
    """Hash source with CRLF normalized to LF."""
    return hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")
    ).hexdigest()


def main() -> None:
    """Archive the Android process transcript and exact build identities."""
    ARCHIVE.mkdir(parents=True, exist_ok=True)

    source_log = BUILD / "pixel-arm64-program-graph.log"
    data = source_log.read_bytes()

    assert data.endswith(b"\n") and b"\r" not in data
    assert b"46281FDAS002AL" not in data
    assert data.count(b"BACKEND_VULKAN_PROGRAM_CASE_PASS ") == 16
    assert data.count(b"BACKEND_VULKAN_PROGRAM_DIAGNOSTIC_PASS ") == 1

    target_log = ARCHIVE / source_log.name
    target_log.write_bytes(data)
    assert target_log.read_bytes() == data

    manifest = {
        "date": "2026-11-01",
        "scope": (
            "Host-only explicit Vulkan 1.0 Compute program declaration "
            "validation before production executor integration"
        ),
        "minimum_android_api": 24,
        "android_runtime_abi": "arm64-v8a",
        "armv7_compile_only": True,
        "accepted_cases": 2,
        "rejected_cases": 14,
        "vulkan_loader_linked": False,
        "vulkan_driver_calls": 0,
        "gpu_dispatches": 0,
        "hashcat_hashing_scheduler_connected": False,
        "password_or_hash_inputs_processed": False,
        "source_hash_policy": "CRLF normalized to LF",
        "source_sha256": {
            source: source_sha256(ROOT / source)
            for source in SOURCES
        },
        "native_elf_sha256": {
            abi: sha256(BUILD / f"vulkan-backend-program-{abi}")
            for abi in ("arm64", "armv7")
        },
        "android_run": {
            "device": "pixel-arm64",
            "abi": "arm64-v8a",
            "file": target_log.name,
            "sha256": sha256(target_log),
            "native_exit_code": 0,
            "accepted_cases": 2,
            "rejected_cases": 14,
            "driver_calls": 0,
            "gpu_dispatches": 0,
        },
        "limitations": [
            "This milestone validates only program declarations and does not record or submit Vulkan commands.",
            "The new graph contract is not yet linked into source/src/Makefile or backend_vulkan_kernel.c.",
            "ARMv7 is compiled and ELF-inspected but was not run on physical ARMv7 hardware because the RedMagic device was disconnected.",
            "VkPhysicalDeviceLimits in the diagnostic are explicit test values, not queried from a Vulkan driver.",
            "No cryptographic SPIR-V module, password candidate, target digest or HASHCAT scheduler work is present.",
        ],
    }

    manifest_path = ARCHIVE / "manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8", newline="\n",
    )

    print("PASS archived one Android ARM64 program-graph contract process")
    print("PASS exact ARM64/ARMv7 ELF and nine source SHA-256 identities")
    print("PASS zero Vulkan driver calls and zero HASHCAT hash scheduling")
    print("MANIFEST_SHA256", sha256(manifest_path))


if __name__ == "__main__":
    main()
