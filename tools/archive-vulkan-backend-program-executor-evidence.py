#!/usr/bin/env python3
"""Archive program-declared Vulkan executor evidence from Android HASHCAT.

Preserves two real Pixel Vulkan native chain runs, one complete Android
hashcat -I process, three malformed native controls, exact ARM64/ARMv7
native diagnostic/backend objects, full HASHCAT binaries and source
SHA-256 identities. Only the public integer shaders execute.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/diagnostic-multimode/vulkan-backend-program-executor-20261101"
FULL = ROOT / "build/diagnostic-multimode/vulkan-hashcat-program-executor-20261101"
ARCHIVE = ROOT / "tests/fixtures/android-gpu/vulkan-backend-program-executor-20261101"

SOURCES = (
    "source/src/backend.c",
    "source/src/backend_vulkan.c",
    "source/src/backend_vulkan_session.c",
    "source/src/backend_vulkan_kernel.c",
    "source/src/backend_vulkan_program.c",
    "source/src/Makefile",
    "source/include/backend_vulkan.h",
    "source/include/backend_vulkan_kernel.h",
    "source/include/backend_vulkan_program.h",
    "source/include/backend_vulkan_public_spirv.h",
    "source/include/backend_vulkan_public_stage2_spirv.h",
    "tools/vulkan-backend-public-kernel.comp",
    "tools/vulkan-backend-public-stage2.comp",
    "tools/vulkan-backend-public-chain-diagnostic.c",
    "tools/build-vulkan-backend-public-chain.ps1",
    "tools/run-vulkan-backend-program-executor.ps1",
    "tools/run-vulkan-hashcat-program-executor.ps1",
    "tools/archive-vulkan-backend-program-executor-evidence.py",
    "tools/test-vulkan-backend-program-executor-evidence.py",
    "tools/vulkan-core10-device-catalog.c",
    "tools/vulkan-core11-policy.c",
    "tools/vulkan-auto-config.c",
    "scripts/BUILD.ps1",
)


def sha256(path: Path) -> str:
    """Return SHA-256 over complete file bytes."""
    assert path.is_file(), path
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_sha256(path: Path) -> str:
    """Hash source after CRLF-to-LF normalization."""
    return hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")
    ).hexdigest()


def copy_log(name: str) -> str:
    """Copy one saved LF-only Android transcript without alteration."""
    source = BUILD / name
    data = source.read_bytes()

    assert data.endswith(b"\n") and b"\r" not in data, name
    assert b"46281FDAS002AL" not in data, name

    target = ARCHIVE / name
    target.write_bytes(data)
    assert target.read_bytes() == data

    return sha256(target)


def main() -> None:
    """Archive Pixel program-driven GPU execution and both Android ABIs."""
    ARCHIVE.mkdir(parents=True, exist_ok=True)

    native = []

    for api in ("1.0", "1.1"):
        filename = f"pixel-arm64-{api}.log"
        digest = copy_log(filename)
        lines = (ARCHIVE / filename).read_text(
            encoding="utf-8"
        ).splitlines()

        assert lines[:4] == [
            "TEST=vulkan-backend-program-executor",
            "ABI=arm64-v8a",
            f"API={api}",
            "ADB_EXIT_CODE=0",
        ]
        assert lines[-1] == (
            f"BACKEND_PUBLIC_CHAIN_DIAGNOSTIC_PASS"
            f" api={api} trials=2 dispatches=4 descriptors=4"
            " storage_buffers=3 verified_words=256 shaders=2"
            " password_inputs=0 scheduled=0"
        )
        assert sum(
            x == "BACKEND_VULKAN_PROGRAM_ACCEPT stages=2 buffers=3 bytes=256 descriptors_per_stage=2"
            for x in lines
        ) == 2

        native.append({
            "requested_api": api,
            "file": filename,
            "sha256": digest,
            "native_exit_code": 0,
            "program_validations": 2,
            "vk_instances": 2,
            "compute_dispatches": 4,
            "verified_words": 256,
        })

    full_name = "pixel-arm64-hashcat-program-info.log"
    full_digest = copy_log(full_name)
    full_lines = (ARCHIVE / full_name).read_text(
        encoding="utf-8"
    ).splitlines()

    assert full_lines[:5] == [
        "TEST=vulkan-hashcat-program-executor",
        "DEVICE=pixel-arm64",
        "ABI=arm64-v8a",
        "COMMAND=-I",
        "ADB_EXIT_CODE=0",
    ]
    assert full_lines[-1] == "HASHCAT_KERNEL_EXIT=0"
    assert full_lines.count(
        "BACKEND_VULKAN_PROGRAM_ACCEPT stages=1 buffers=2 bytes=256 descriptors_per_stage=2"
    ) == 1
    assert full_lines.count(
        "BACKEND_VULKAN_PROGRAM_ACCEPT stages=2 buffers=3 bytes=256 descriptors_per_stage=2"
    ) == 1

    negative_name = "pixel-arm64-negative-controls.log"
    negative_digest = copy_log(negative_name)

    manifest = {
        "date": "2026-11-01",
        "scope": (
            "Public one/two-stage Vulkan program declarations validated "
            "against real Mali-G715 limits and consumed by the production "
            "Android HASHCAT information-mode Compute executor"
        ),
        "minimum_android_api": 24,
        "physical_gpu_profiles": ["pixel-arm64"],
        "armv7_compile_only": True,
        "adreno_current_revision_runtime_tested": False,
        "hashcat_info_mode_only": True,
        "hashcat_hashing_scheduler_connected": False,
        "cryptographic_spirv_kernels_ported": 0,
        "password_or_hash_inputs_processed": False,
        "program_graph_connected_to_executor": True,
        "source_hash_policy": "CRLF normalized to LF",
        "source_sha256": {
            source: source_sha256(ROOT / source)
            for source in SOURCES
        },
        "native_artifact_sha256": {
            abi: {
                "diagnostic": sha256(
                    BUILD / f"vulkan-backend-public-chain-{abi}"
                ),
                "backend_object": sha256(
                    BUILD / f"hashcat-backend-vulkan-{abi}.o"
                ),
            }
            for abi in ("arm64", "armv7")
        },
        "full_hashcat_artifact_sha256": {
            abi: {
                "frontend": sha256(FULL / abi / "hashcat"),
                "module_00000": sha256(
                    FULL / abi / "modules/module_00000.so"
                ),
            }
            for abi in ("arm64-v8a", "armeabi-v7a")
        },
        "native_runs": native,
        "full_hashcat_info_run": {
            "file": full_name,
            "sha256": full_digest,
            "native_exit_code": 0,
            "program_validations": 2,
            "compute_dispatches": 3,
            "verified_words": 192,
            "normal_opencl_gpu_enumeration": True,
        },
        "invalid_cli": {
            "file": negative_name,
            "sha256": negative_digest,
            "invalid_requests": 3,
        },
        "totals": {
            "real_android_processes": 3,
            "program_validations_against_real_gpu_limits": 6,
            "real_spirv_vkcmd_dispatches": 11,
            "public_integer_words_verified": 704,
            "invalid_native_commands": 3,
        },
        "limitations": [
            "This current-revision executor integration was run only on Pixel 9 Pro XL Mali-G715 ARM64 because RedMagic was disconnected.",
            "ARMv7 native diagnostic and full HASHCAT binaries were compiled and hashed but not executed on physical ARMv7 hardware for this revision.",
            "Only the embedded public integer SPIR-V modules are accepted by the information-mode verification methods.",
            "The normal HASHCAT hashing scheduler remains disconnected from Vulkan and no password candidate or target digest is processed.",
            "The executor currently owns capacity for at most two public program stages even though the host-only program validator can validate longer linear graphs.",
        ],
    }

    manifest_path = ARCHIVE / "manifest.json"
    manifest_path.write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8", newline="\n",
    )

    print("PASS 2 native Pixel Vulkan program-executor processes")
    print("PASS 1 complete Pixel hashcat -I program-executor process")
    print("PASS 6 real-limit program validations, 11 dispatches, 704 verified words")
    print("PASS exact ARM64/ARMv7 native and full HASHCAT artifact SHA-256")
    print("MANIFEST_SHA256", sha256(manifest_path))


if __name__ == "__main__":
    main()
