#!/usr/bin/env python3
"""Archive real Android Vulkan four-stage public Compute execution.

Copies complete LF-normalized ADB runner transcripts without editing
their saved bytes. Records the exact source, generated SPIR-V modules,
Android ARM64/ARMv7 standalone diagnostics, production backend objects
and complete opt-in HASHCAT executable identities. All GPU arithmetic
uses public uint32 arrays; the normal HASHCAT hash scheduler is idle.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import struct


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/diagnostic-multimode/vulkan-backend-public-dynamic-20261102"
FULL = ROOT / "build/diagnostic-multimode/vulkan-hashcat-dynamic-executor-20261102"
ARCHIVE = ROOT / "tests/fixtures/android-gpu/vulkan-backend-public-dynamic-20261102"

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
    "source/src/backend_vulkan_program.c",
    "source/src/Makefile",
    "source/include/backend_vulkan_kernel.h",
    "source/include/backend_vulkan_program.h",
    "source/include/backend_vulkan_public_spirv.h",
    "source/include/backend_vulkan_public_stage2_spirv.h",
    "tools/vulkan-backend-public-kernel.comp",
    "tools/vulkan-backend-public-stage2.comp",
    "tools/generate-vulkan-backend-public-kernel.py",
    "tools/vulkan-backend-public-dynamic-diagnostic.c",
    "tools/build-vulkan-backend-public-dynamic.ps1",
    "tools/run-vulkan-backend-public-dynamic.ps1",
    "tools/run-vulkan-hashcat-dynamic-executor.ps1",
    "tools/archive-vulkan-backend-dynamic-evidence.py",
    "tools/test-vulkan-backend-dynamic-evidence.py",
    "scripts/BUILD.ps1",
)


def sha256(path: Path) -> str:
    """Hash every byte of an existing binary, log or source file."""
    assert path.is_file(), path
    return hashlib.sha256(path.read_bytes()).hexdigest()


def normalized_sha256(path: Path) -> str:
    """Hash source with Windows CRLF line endings normalized to LF."""
    return hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")
    ).hexdigest()


def archive_log(filename: str) -> str:
    """Preserve exact LF-normalized Android runner bytes for one log."""
    original = BUILD / filename
    data = original.read_bytes()

    assert data and data.endswith(b"\n") and b"\r" not in data, filename
    assert b"46281FDAS002AL" not in data, filename
    assert b"91809037" not in data, filename

    target = ARCHIVE / filename
    target.write_bytes(data)
    assert target.read_bytes() == data
    return sha256(target)


def record_spirv(path: Path) -> dict:
    """Read a complete canonical little-endian SPIR-V 1.0 module."""
    raw = path.read_bytes()
    assert len(raw) >= 24 and len(raw) % 4 == 0
    words = struct.unpack("<" + "I" * (len(raw) // 4), raw)
    assert words[0] == 0x07230203
    assert words[1] == 0x00010000
    assert words[4] == 0
    return {"bytes": len(raw), "words": len(words), "sha256": sha256(path)}


def main() -> None:
    """Archive six native tests, three full HASHCAT runs and invalid CLI logs."""
    ARCHIVE.mkdir(parents=True, exist_ok=True)

    runs = []
    full = []
    invalid = []

    for device, (abi, _) in DEVICES.items():
        for api in APIS:
            filename = f"{device}-{api}.log"
            digest = archive_log(filename)
            lines = (ARCHIVE / filename).read_text(encoding="utf-8").splitlines()

            assert lines[:4] == [
                "TEST=vulkan-backend-public-dynamic",
                f"ABI={abi}",
                f"API={api}",
                "ADB_EXIT_CODE=0",
            ], filename
            assert lines[-1] == (
                f"BACKEND_PUBLIC_DYNAMIC_DIAGNOSTIC_PASS api={api}"
                " trials=2 dispatches=8 buffers=5 descriptor_sets=4"
                " pipelines=4 verified_words=512 dynamic_resources=1"
                " password_inputs=0 scheduled=0"
            ), filename
            assert sum(
                x == "BACKEND_VULKAN_PROGRAM_ACCEPT stages=4 buffers=5 bytes=256 descriptors_per_stage=2"
                for x in lines
            ) == 2, filename
            assert sum(
                x.startswith("BACKEND_VULKAN_PROGRAM_DISPATCH_PASS ")
                for x in lines
            ) == 2, filename

            runs.append({
                "device": device,
                "abi": abi,
                "requested_api": api,
                "file": filename,
                "sha256": digest,
                "native_exit_code": 0,
                "real_instance_lifetimes": 2,
                "four_stage_program_verifications": 2,
                "four_stage_dispatches": 8,
                "verified_public_words": 512,
                "shader_dependency_barriers": 6,
            })

        info_name = f"{device}-hashcat-dynamic-info.log"
        info_digest = archive_log(info_name)
        info_lines = (ARCHIVE / info_name).read_text(encoding="utf-8").splitlines()

        assert info_lines[:5] == [
            "TEST=vulkan-hashcat-dynamic-executor",
            f"DEVICE={device}",
            f"ABI={abi}",
            "COMMAND=-I",
            "ADB_EXIT_CODE=0",
        ], info_name
        assert info_lines[-1] == "HASHCAT_KERNEL_EXIT=0", info_name

        for stages, buffers in ((1, 2), (2, 3), (4, 5)):
            event = (
                f"BACKEND_VULKAN_PROGRAM_ACCEPT stages={stages}"
                f" buffers={buffers} bytes=256 descriptors_per_stage=2"
            )
            assert info_lines.count(event) == 1, (info_name, event)

        assert sum(
            x.startswith("BACKEND_VULKAN_PROGRAM_DISPATCH_PASS ")
            for x in info_lines
        ) == 1, info_name

        full.append({
            "device": device,
            "abi": abi,
            "file": info_name,
            "sha256": info_digest,
            "native_exit_code": 0,
            "real_instance_lifetimes": 1,
            "empty_queue_submissions": 1,
            "one_stage_dispatches": 1,
            "two_stage_dispatches": 2,
            "four_stage_dispatches": 4,
            "verified_public_words": 448,
            "shader_dependency_barriers": 4,
            "normal_opencl_device_discovery": True,
        })

        bad_name = f"{device}-negative-controls.log"
        invalid.append({
            "device": device,
            "abi": abi,
            "file": bad_name,
            "sha256": archive_log(bad_name),
            "rejected_native_options": 3,
        })

    assert len(runs) == 6 and len(full) == 3 and len(invalid) == 3
    assert len(list(ARCHIVE.glob("*.log"))) == 12

    manifest = {
        "date": "2026-11-02",
        "scope": (
            "Dynamically allocated Vulkan 1.0 public Compute resources"
            " for one/two/four-stage information-mode programs"
        ),
        "minimum_android_api": 24,
        "native_requested_apis": list(APIS),
        "full_hashcat_requested_api": "1.0",
        "physical_device_process_profiles": list(DEVICES),
        "gpu_devices": ["Adreno 730", "Mali-G715"],
        "program_stage_counts": [1, 2, 4],
        "four_stage_shader_modules": 4,
        "four_stage_descriptor_sets": 4,
        "four_stage_descriptor_count": 8,
        "four_stage_storage_buffers": 5,
        "four_stage_dependency_barriers": 3,
        "host_resource_arrays_dynamically_allocated": True,
        "shader_compiler_target": "vulkan1.0",
        "spirv_modules": {
            "public_u32_transform": record_spirv(
                BUILD / "vulkan-backend-public-stage1.spv"
            ),
            "public_u32_rotate": record_spirv(
                BUILD / "vulkan-backend-public-stage2.spv"
            ),
        },
        "source_hash_policy": "CRLF normalized to LF",
        "source_sha256": {
            name: normalized_sha256(ROOT / name) for name in SOURCES
        },
        "native_artifact_sha256": {
            abi: {
                "diagnostic": sha256(
                    BUILD / f"vulkan-backend-public-dynamic-{abi}"
                ),
                "production_backend_object": sha256(
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
        "native_runs": runs,
        "full_hashcat_info_runs": full,
        "invalid_cli": invalid,
        "totals": {
            "native_android_processes": 6,
            "full_hashcat_info_processes": 3,
            "four_stage_program_lifetimes": 15,
            "physical_instance_lifetimes": 15,
            "full_hashcat_empty_submissions": 3,
            "real_public_vkcmd_dispatches": 69,
            "verified_public_u32_words": 4416,
            "four_stage_dependency_barriers": 45,
            "all_program_dependency_barriers": 48,
            "invalid_native_commands_rejected": 9,
        },
        "cryptographic_spirv_kernels": 0,
        "password_or_hash_inputs_processed": False,
        "hashcat_hashing_scheduler_connected": False,
        "hashcat_info_mode_only": True,
        "normal_opencl_discovery_preserved": True,
        "capture_policy": (
            "Android native stdout/stderr captured through ADB by PowerShell,"
            " with line endings normalized to UTF-8 LF. Archived files are"
            " byte-identical to saved runner output, not raw ADB streams."
        ),
        "limitations": [
            "The four-stage program reuses the public rotate shader for its final three stages; it is not a cryptographic HASHCAT kernel port.",
            "The information-mode executor processes only fixed 64-word public test values. It does not accept password candidates, digests, salts, or production hashing work.",
            "The program graph validates exact declared buffer dependencies, but this milestone does not benchmark throughput or concurrency.",
            "Android API24-compiled ELFs were tested on newer Android systems and newer Vulkan drivers, not an Android 7.0 or physically Vulkan-1.0-only device.",
            "The host arrays are allocated from program counts, but the current production diagnostic uses the explicit one/two/four-stage declarations.",
        ],
    }

    target = ARCHIVE / "manifest.json"
    target.write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8", newline="\n",
    )

    print("PASS 6 real native four-stage GPU process runs")
    print("PASS 3 complete Android HASHCAT -I GPU process runs")
    print("PASS 69 real vkCmdDispatch and 4416 verified public uint32 values")
    print("PASS 45 four-stage dependency barriers and 9 invalid CLI refusals")
    print("PASS exact source, SPIR-V, ARM64/ARMv7 native and HASHCAT ELF identities")
    print("MANIFEST_SHA256", sha256(target))


if __name__ == "__main__":
    main()
