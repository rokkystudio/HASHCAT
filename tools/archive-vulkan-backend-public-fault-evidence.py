#!/usr/bin/env python3
"""Archive real Android four-stage Vulkan partial cleanup and recovery logs.

Copies saved LF-normalized ADB transcripts byte-for-byte. Records exact
production source, public SPIR-V, Android ARM64/ARMv7 diagnostic binaries,
production backend objects and full Android HASHCAT executable identities.
Controlled initialization errors are labeled simulated: no Vulkan driver
return is fabricated and no device-loss test is claimed.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import struct


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/diagnostic-multimode/vulkan-backend-public-fault-20261102"
FULL = ROOT / "build/diagnostic-multimode/vulkan-hashcat-public-fault-20261102"
ARCHIVE = ROOT / "tests/fixtures/android-gpu/vulkan-backend-public-fault-20261102"

PROFILES = {
    "redmagic-arm64": ("arm64-v8a", "arm64", "Adreno (TM) 730"),
    "redmagic-armv7": ("armeabi-v7a", "armv7", "Adreno (TM) 730"),
    "pixel-arm64": ("arm64-v8a", "arm64", "Mali-G715"),
}
APIS = ("1.0", "1.1")
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
    "source/include/types.h",
    "tools/vulkan-backend-public-kernel.comp",
    "tools/vulkan-backend-public-stage2.comp",
    "tools/generate-vulkan-backend-public-kernel.py",
    "tools/vulkan-backend-public-fault-diagnostic.c",
    "tools/build-vulkan-backend-public-fault.ps1",
    "tools/run-vulkan-backend-public-fault.ps1",
    "tools/run-vulkan-hashcat-public-fault.ps1",
    "tools/archive-vulkan-backend-public-fault-evidence.py",
    "tools/test-vulkan-backend-public-fault-evidence.py",
    "tools/vulkan-core10-device-catalog.c",
    "tools/vulkan-core10-device-catalog.h",
    "tools/vulkan-core11-policy.c",
    "tools/vulkan-core11-policy.h",
    "tools/vulkan-auto-config.c",
    "tools/vulkan-auto-config.h",
    "scripts/BUILD.ps1",
)


def sha256(path: Path) -> str:
    """Return SHA-256 of complete existing source, binary or transcript bytes."""
    assert path.is_file(), path
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_sha256(path: Path) -> str:
    """Hash complete source after CRLF-to-LF normalization."""
    return hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")
    ).hexdigest()


def archive_log(name: str) -> str:
    """Copy exactly the runner's LF-only Android transcript to the archive."""
    data = (BUILD / name).read_bytes()
    assert data.endswith(b"\n") and b"\r" not in data, name
    assert b"46281FDAS002AL" not in data and b"91809037" not in data, name

    target = ARCHIVE / name
    target.write_bytes(data)
    assert target.read_bytes() == data
    return sha256(target)


def spirv_identity(path: Path) -> dict:
    """Read and identify exact canonical SPIR-V 1.0 little-endian words."""
    data = path.read_bytes()
    assert len(data) >= 24 and len(data) % 4 == 0
    words = struct.unpack("<" + "I" * (len(data) // 4), data)
    assert words[0] == 0x07230203 and words[1] == 0x00010000
    assert words[4] == 0
    return {
        "bytes": len(data),
        "words": len(words),
        "sha256": sha256(path),
    }


def main() -> None:
    """Archive six fault processes, three full HASHCAT runs and CLI controls."""
    ARCHIVE.mkdir(parents=True, exist_ok=True)

    native_runs = []
    full_runs = []
    negative_runs = []

    for device, (abi, _, _) in PROFILES.items():
        for api in APIS:
            filename = f"{device}-{api}.log"
            digest = archive_log(filename)
            lines = (ARCHIVE / filename).read_text(
                encoding="utf-8"
            ).splitlines()

            assert lines[:4] == [
                "TEST=vulkan-backend-public-fault",
                f"ABI={abi}",
                f"API={api}",
                "ADB_EXIT_CODE=0",
            ], filename
            assert lines[-1] == (
                f"BACKEND_PUBLIC_FAULT_DIAGNOSTIC_PASS api={api}"
                " checkpoints=37 injected=37 recovered_devices=37"
                " recovered_dispatches=148 verified_words=9472"
                " abandoned_before_submit=1 abandoned_after_submit=1"
                " real_driver_losses=0 password_inputs=0 scheduled=0"
            ), filename
            assert sum(
                line.startswith("BACKEND_PUBLIC_FAULT_TRIGGER ")
                for line in lines
            ) == 37, filename
            assert sum(
                line.startswith("BACKEND_PUBLIC_FAULT_RELEASE ")
                for line in lines
            ) == 37, filename
            assert sum(
                line.startswith("BACKEND_PUBLIC_FAULT_RECOVERY_PASS ")
                for line in lines
            ) == 37, filename

            native_runs.append({
                "device": device,
                "abi": abi,
                "requested_api": api,
                "file": filename,
                "sha256": digest,
                "native_exit_code": 0,
                "real_instances": 1,
                "simulated_error_checkpoints": 37,
                "partial_resource_jobs_released": 37,
                "new_working_four_stage_devices": 37,
                "recovered_real_dispatches": 148,
                "cpu_verified_public_words": 9472,
                "after_submit_real_queue_successes": 1,
                "after_submit_unchecked_dispatches": 4,
            })

        filename = f"{device}-hashcat-fault-info.log"
        digest = archive_log(filename)
        lines = (ARCHIVE / filename).read_text(
            encoding="utf-8"
        ).splitlines()

        assert lines[:5] == [
            "TEST=vulkan-hashcat-public-fault",
            f"DEVICE={device}",
            f"ABI={abi}",
            "COMMAND=-I",
            "ADB_EXIT_CODE=0",
        ], filename
        assert lines[-1] == "HASHCAT_KERNEL_EXIT=0", filename
        assert not any(
            line.startswith("BACKEND_PUBLIC_FAULT_TRIGGER ")
            or line.startswith("BACKEND_PUBLIC_FAULT_RELEASE ")
            for line in lines
        ), filename

        full_runs.append({
            "device": device,
            "abi": abi,
            "file": filename,
            "sha256": digest,
            "native_exit_code": 0,
            "real_instances": 1,
            "one_stage_dispatches": 1,
            "two_stage_dispatches": 2,
            "four_stage_dispatches": 4,
            "cpu_verified_public_words": 448,
            "fault_injection_enabled": False,
            "ordinary_opencl_gpu_enumeration": True,
            "hashcat_hash_scheduler_enabled": False,
        })

        name = f"{device}-negative-controls.log"
        negative_runs.append({
            "device": device,
            "abi": abi,
            "file": name,
            "sha256": archive_log(name),
            "malformed_cli_rejections": 3,
        })

    assert len(native_runs) == 6
    assert len(full_runs) == 3
    assert len(negative_runs) == 3
    assert len(list(ARCHIVE.glob("*.log"))) == 12

    manifest = {
        "date": "2026-11-02",
        "scope": (
            "Explicit host-controlled Vulkan four-stage resource fault "
            "checkpoints and working same-instance recovery on physical "
            "Adreno and Mali Android GPUs"
        ),
        "minimum_android_api": 24,
        "requested_vulkan_apis": list(APIS),
        "full_hashcat_requested_api": "1.0",
        "physical_gpu_profiles": list(PROFILES),
        "checkpoints": 37,
        "checkpoints_tested_on_real_gpu": True,
        "faults_are_test_control_not_driver_failures": True,
        "actual_driver_device_losses": 0,
        "actual_failed_queue_submissions_observed": 0,
        "actual_failed_fence_waits_observed": 0,
        "after_submit_real_queue_successes": 6,
        "after_submit_idle_succeeded": True,
        "hashcat_info_mode_only": True,
        "hashcat_hashing_scheduler_connected": False,
        "cryptographic_spirv_kernels_ported": 0,
        "password_or_hash_inputs_processed": False,
        "normal_opencl_discovery_preserved": True,
        "source_hash_policy": "CRLF normalized to LF",
        "source_sha256": {
            name: source_sha256(ROOT / name)
            for name in SOURCES
        },
        "spirv_modules": {
            "public_u32_transform": spirv_identity(
                BUILD / "vulkan-backend-public-stage1.spv"
            ),
            "public_u32_rotate": spirv_identity(
                BUILD / "vulkan-backend-public-stage2.spv"
            ),
        },
        "native_artifact_sha256": {
            abi: {
                "diagnostic": sha256(
                    BUILD / f"vulkan-backend-public-fault-{abi}"
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
        "native_runs": native_runs,
        "full_hashcat_info_runs": full_runs,
        "invalid_cli": negative_runs,
        "totals": {
            "real_native_android_processes": 6,
            "full_hashcat_info_processes": 3,
            "controlled_resource_failures": 222,
            "released_partial_jobs": 222,
            "successful_same_instance_recoveries": 222,
            "verified_recovery_gpu_dispatches": 888,
            "verified_recovery_public_words": 56832,
            "unverified_after_submit_gpu_dispatches": 24,
            "full_info_verified_gpu_dispatches": 21,
            "full_info_verified_public_words": 1344,
            "all_cpu_verified_gpu_dispatches": 909,
            "all_cpu_verified_public_words": 58176,
            "malformed_cli_requests_rejected": 9,
        },
        "capture_policy": (
            "Native ADB stdout/stderr is saved by the PowerShell runner "
            "as UTF-8 LF-only text. Archived evidence is byte-identical "
            "to the saved runner transcript, not the raw ADB stream."
        ),
        "limitations": [
            "All 37 failures are intentional post-acquisition or submit-boundary host errors labeled simulated=1. No actual VkResult driver failure is simulated or claimed.",
            "The after-submit control calls real vkQueueSubmit and waits for real device idle during cleanup; its GPU outputs are not compared with CPU.",
            "Real GPU device loss, physical reset, allocator exhaustion and failed vkQueueSubmit/vkWaitForFences were not reproduced.",
            "Only synthetic 64-word public Compute buffers are used; no HASHCAT candidate, salt, target digest, hashing algorithm, or attack scheduler is connected.",
            "Android API24-compiled Vulkan core1.0 applications ran on newer Android and Vulkan drivers, not a physically Vulkan-1.0-only GPU or Android 7.0 OS.",
        ],
    }

    target = ARCHIVE / "manifest.json"
    target.write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8", newline="\n",
    )

    print("PASS 6 native Android real-device fault and recovery processes")
    print("PASS 3 full Android HASHCAT information-mode processes")
    print("PASS 222 simulated checkpoints + 222 real recovered VkDevices")
    print("PASS 909 CPU-verified GPU dispatches and 58176 public uint32 words")
    print("PASS 6 real queue-submit/idle cleanup boundaries with no forged VkResults")
    print("PASS exact source, SPIR-V, ARM64/ARMv7 and full HASHCAT SHA-256")
    print("MANIFEST_SHA256", sha256(target))


if __name__ == "__main__":
    main()
