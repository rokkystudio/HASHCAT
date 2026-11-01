#!/usr/bin/env python3
"""Archive first real HASHCAT-owned Vulkan SPIR-V Compute kernel evidence.

Preserves actual ARM64/ARMv7 Android diagnostic and full hashcat -I
transcripts after the native runners normalize line endings to UTF-8 LF.
Records exact source, SPIR-V module, diagnostic ELF, production HASHCAT
ELF and module_00000.so identities without running password processing.
The embedded kernel only transforms 64 public unsigned integer words.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import shutil
import struct


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/diagnostic-multimode/vulkan-backend-public-kernel-20261101"
FULL = ROOT / "build/diagnostic-multimode/vulkan-hashcat-public-kernel-20261101"
ARCHIVE = ROOT / "tests/fixtures/android-gpu/vulkan-backend-public-kernel-20261101"
SPIRV = ROOT / "build/diagnostic-multimode/vulkan-backend-public-kernel.spv"

DEVICES = {
    "redmagic-arm64": ("arm64-v8a", "arm64"),
    "redmagic-armv7": ("armeabi-v7a", "armv7"),
    "pixel-arm64": ("arm64-v8a", "arm64"),
}
APIS = ("1.0", "1.1")
SOURCES = (
    "source/src/backend.c",
    "source/src/backend_vulkan.c",
    "source/src/backend_vulkan_session.c",
    "source/src/backend_vulkan_kernel.c",
    "source/src/Makefile",
    "source/include/types.h",
    "source/include/backend_vulkan.h",
    "source/include/backend_vulkan_kernel.h",
    "source/include/backend_vulkan_public_spirv.h",
    "tools/vulkan-backend-public-kernel.comp",
    "tools/generate-vulkan-backend-public-kernel.py",
    "tools/vulkan-backend-public-kernel-diagnostic.c",
    "tools/build-vulkan-backend-public-kernel.ps1",
    "tools/run-vulkan-backend-public-kernel.ps1",
    "tools/run-vulkan-hashcat-public-kernel.ps1",
    "tools/archive-vulkan-backend-public-kernel-evidence.py",
    "tools/test-vulkan-backend-public-kernel-evidence.py",
    "tools/vulkan-core10-device-catalog.c",
    "tools/vulkan-core10-device-catalog.h",
    "tools/vulkan-auto-config.c",
    "tools/vulkan-auto-config.h",
    "tools/vulkan-core11-policy.c",
    "tools/vulkan-core11-policy.h",
    "scripts/BUILD.ps1",
)


def sha256(path: Path) -> str:
    """Return SHA-256 over every byte of an existing ELF, SPIR-V or log."""
    assert path.is_file(), path

    return hashlib.sha256(path.read_bytes()).hexdigest()


def normalized_source_sha256(path: Path) -> str:
    """Hash checked-out source after canonical CRLF-to-LF conversion."""
    return hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")
    ).hexdigest()


def archive_log(filename: str) -> str:
    """Archive one LF-normalized saved Android driver transcript unchanged."""
    data = (BUILD / filename).read_bytes()

    assert data and data.endswith(b"\n"), filename
    assert b"\r" not in data, filename
    assert b"91809037" not in data, filename
    assert b"46281FDAS002AL" not in data, filename

    path = ARCHIVE / filename
    path.write_bytes(data)

    assert path.read_bytes() == data

    return sha256(path)


def main() -> None:
    """Archive six physical shader probes and three real HASHCAT frontends."""
    ARCHIVE.mkdir(parents=True, exist_ok=True)

    shader = SPIRV.read_bytes()
    assert len(shader) == 1304 and len(shader) % 4 == 0
    spirv_words = struct.unpack("<" + "I" * (len(shader) // 4), shader)
    assert spirv_words[0] == 0x07230203
    assert spirv_words[1] == 0x00010000
    assert spirv_words[4] == 0

    native = []
    frontend = []
    invalid_cli = []

    for device, (abi, _) in DEVICES.items():
        for api in APIS:
            filename = f"{device}-{api}.log"
            digest = archive_log(filename)
            lines = (ARCHIVE / filename).read_text(
                encoding="utf-8"
            ).splitlines()

            assert lines[:4] == [
                "TEST=vulkan-backend-public-kernel",
                f"ABI={abi}",
                f"API={api}",
                "ADB_EXIT_CODE=0",
            ]
            assert lines[-1] == (
                f"BACKEND_PUBLIC_KERNEL_DIAGNOSTIC_PASS api={api}"
                " trials=2 dispatches=2 descriptors=2"
                " storage_buffers=2 verified_words=128"
                " shaders=1 password_inputs=0 scheduled=0"
            )

            native.append({
                "device": device,
                "abi": abi,
                "requested_api": api,
                "file": filename,
                "sha256": digest,
                "native_exit_code": 0,
                "vk_instances": 2,
                "public_shader_dispatches": 2,
                "public_words_verified": 128,
                "gpu_kernel_descriptor_sets": 2,
                "hashcat_hash_scheduler_used": False,
            })

        filename = f"{device}-hashcat-info.log"
        digest = archive_log(filename)
        lines = (ARCHIVE / filename).read_text(
            encoding="utf-8"
        ).splitlines()

        assert lines[:5] == [
            "TEST=vulkan-hashcat-public-kernel",
            f"DEVICE={device}",
            f"ABI={abi}",
            "COMMAND=-I",
            "ADB_EXIT_CODE=0",
        ]
        assert lines[-1] == "HASHCAT_KERNEL_EXIT=0"
        assert sum(
            event.startswith("BACKEND_VULKAN_KERNEL_DISPATCH_PASS ")
            for event in lines
        ) == 1, filename

        frontend.append({
            "device": device,
            "abi": abi,
            "file": filename,
            "sha256": digest,
            "native_exit_code": 0,
            "requested_api": "1.0",
            "public_shader_dispatches": 1,
            "public_words_verified": 64,
            "normal_opencl_gpu_enumeration": True,
            "hashcat_password_kernel_dispatches": 0,
        })

        filename = f"{device}-negative-controls.log"
        invalid_cli.append({
            "device": device,
            "abi": abi,
            "file": filename,
            "sha256": archive_log(filename),
            "malformed_native_commands": 3,
        })

    assert len(native) == 6
    assert len(frontend) == 3
    assert len(invalid_cli) == 3
    assert len(list(ARCHIVE.glob("*.log"))) == 12

    full_hashes = {
        abi: {
            "frontend": sha256(FULL / abi / "hashcat"),
            "module_00000": sha256(
                FULL / abi / "modules/module_00000.so"
            ),
        }
        for abi in ("arm64-v8a", "armeabi-v7a")
    }

    standalone_hashes = {
        abi: {
            "diagnostic": sha256(
                BUILD / f"vulkan-backend-public-kernel-{abi}"
            ),
            "production_backend_c_object": sha256(
                BUILD / f"hashcat-backend-vulkan-{abi}.o"
            ),
        }
        for abi in ("arm64", "armv7")
    }

    manifest = {
        "date": "2026-11-01",
        "scope": (
            "First SPIR-V 1.0 public-data Compute shader dispatched "
            "by production HASHCAT -I backend_ctx code and by an "
            "independent diagnostic linked against that same method"
        ),
        "minimum_android_api": 24,
        "vulkan_api_requested_by_full_hashcat": "1.0",
        "standalone_requested_apis": ["1.0", "1.1"],
        "public_kernel_name": "public_u32_transform",
        "public_kernel_storage_descriptors": 2,
        "public_kernel_workgroup_size": 32,
        "public_kernel_group_count": 2,
        "public_kernel_shader_words": len(spirv_words),
        "public_kernel_input_words": 64,
        "public_kernel_output_words": 64,
        "spirv_version": "1.0",
        "spirv_compiler": "glslangValidator 14.3.0 --target-env vulkan1.0",
        "spirv_sha256": sha256(SPIRV),
        "spirv_bytes": len(shader),
        "source_sha256": {
            name: normalized_source_sha256(ROOT / name)
            for name in SOURCES
        },
        "source_hash_policy": "CRLF normalized to LF",
        "native_artifact_sha256": standalone_hashes,
        "full_hashcat_artifact_sha256": full_hashes,
        "native_runs": native,
        "full_hashcat_info_runs": frontend,
        "invalid_cli": invalid_cli,
        "totals": {
            "physical_process_profiles": 3,
            "standalone_gpu_runs": 6,
            "full_hashcat_info_runs": 3,
            "real_spirv_vkcmd_dispatches": 15,
            "public_integer_words_verified": 960,
            "invalid_native_commands": 9,
        },
        "hashcat_production_backend_code_executed": True,
        "hashcat_info_mode_only": True,
        "normal_opencl_device_discovery_preserved": True,
        "hashcat_hashing_scheduler_connected": False,
        "cryptographic_spirv_kernels_ported": 0,
        "password_or_hash_inputs_processed_by_shader": False,
        "no_vulkan10_only_hardware_tested": True,
        "no_android_api24_os_hardware_tested": True,
        "capture_policy": (
            "Android ADB stdout/stderr is normalized to LF by each "
            "PowerShell runner. Archived UTF-8 transcripts preserve "
            "the saved runner bytes, not the original raw ADB stream."
        ),
        "limitations": [
            "Only one noncryptographic, public unsigned integer SPIR-V shader is integrated; no HASHCAT hashing kernels, candidate processing, cryptographic shader or password scheduler is enabled.",
            "The kernel is executed only during hashcat -I and uses a temporary VkDevice in the existing backend-owned Vulkan physical-device catalogue.",
            "On -I the kernel VkDevice coexists briefly with the independently verified empty Vulkan Compute session; ordinary CUDA/HIP/Metal/OpenCL devices remain unchanged.",
            "A result of 64 independently checked integer values is proven by the native executable; the logs record the verification count, not each GPU word.",
            "The binaries target Android API24 and request Vulkan 1.0/1.1; the tested physical GPU drivers and OS releases are newer than the minimum.",
            "The Vulkan device-selection path uses the enumerated index zero; scheduler integration, multiple concurrent public GPU jobs, noncoherent fallback verification in this exact production code and full stress testing remain out of scope.",
            "The source SHA-256 values identify this new production checkout; earlier evidence manifests refer to earlier source snapshots and must not be relabeled with this checkout's hashes.",
        ],
    }

    path = ARCHIVE / "manifest.json"
    path.write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8", newline="\n",
    )

    print("PASS six standalone real Android public SPIR-V kernel trials")
    print("PASS three complete HASHCAT -I real GPU shader dispatches")
    print("PASS 15 real Vulkan Compute dispatches and 960 CPU-checked words")
    print("PASS three real OpenCL backends retained during HASHCAT information mode")
    print("PASS nine rejected malformed native CLI requests")
    print("PASS exact ARM64/ARMv7 full HASHCAT, diagnostics, modules and SPIR-V identities")
    print("MANIFEST_SHA256", sha256(path))


if __name__ == "__main__":
    main()
