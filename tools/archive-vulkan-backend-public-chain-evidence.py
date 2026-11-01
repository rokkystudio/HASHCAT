#!/usr/bin/env python3
"""Archive actual two-stage Vulkan Compute results from Android HASHCAT.

Keeps the UTF-8 LF-only Android runner logs byte-identical to their
saved originals. Records source, embedded SPIR-V, native ARM64/ARMv7
diagnostic and full Android HASHCAT executable identities. Each shader
runs only public unsigned-integer test arithmetic in information mode;
the archive never invokes GPU work or processes password candidates.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import struct


ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build/diagnostic-multimode/vulkan-backend-public-chain-20261101"
FULL = ROOT / "build/diagnostic-multimode/vulkan-hashcat-public-chain-20261101"
ARCHIVE = ROOT / "tests/fixtures/android-gpu/vulkan-backend-public-chain-20261101"
STAGE1 = BUILD / "vulkan-backend-public-stage1.spv"
STAGE2 = BUILD / "vulkan-backend-public-stage2.spv"

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
    "source/include/backend_vulkan_public_stage2_spirv.h",
    "tools/vulkan-backend-public-kernel.comp",
    "tools/vulkan-backend-public-stage2.comp",
    "tools/generate-vulkan-backend-public-kernel.py",
    "tools/vulkan-backend-public-chain-diagnostic.c",
    "tools/build-vulkan-backend-public-chain.ps1",
    "tools/run-vulkan-backend-public-chain.ps1",
    "tools/run-vulkan-hashcat-public-chain.ps1",
    "tools/archive-vulkan-backend-public-chain-evidence.py",
    "tools/test-vulkan-backend-public-chain-evidence.py",
    "tools/vulkan-core10-device-catalog.c",
    "tools/vulkan-core10-device-catalog.h",
    "tools/vulkan-auto-config.c",
    "tools/vulkan-auto-config.h",
    "tools/vulkan-core11-policy.c",
    "tools/vulkan-core11-policy.h",
    "scripts/BUILD.ps1",
)


def sha256(path: Path) -> str:
    """Hash the complete bytes of an existing native binary or saved log."""
    assert path.is_file(), path

    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_sha256(path: Path) -> str:
    """Hash source with the original Vulkan evidence CRLF-to-LF policy."""
    return hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")
    ).hexdigest()


def archive_log(name: str) -> str:
    """Retain the exact LF-normalized PowerShell/ADB transcript bytes."""
    source = BUILD / name
    data = source.read_bytes()

    assert data.endswith(b"\n") and b"\r" not in data, name
    assert b"91809037" not in data and b"46281FDAS002AL" not in data

    target = ARCHIVE / name
    target.write_bytes(data)
    assert target.read_bytes() == data

    return sha256(target)


def main() -> None:
    """Archive six real standalone runs and three genuine HASHCAT -I runs."""
    ARCHIVE.mkdir(parents=True, exist_ok=True)

    modules = {}
    for stage, path in ((1, STAGE1), (2, STAGE2)):
        raw = path.read_bytes()
        assert len(raw) >= 24 and len(raw) % 4 == 0
        words = struct.unpack("<" + "I" * (len(raw) // 4), raw)

        assert words[0] == 0x07230203
        assert words[1] == 0x00010000
        assert words[4] == 0

        modules[str(stage)] = {
            "bytes": len(raw),
            "words": len(words),
            "sha256": sha256(path),
        }

    assert modules["1"]["words"] == 326
    assert modules["2"]["words"] == 363

    native = []
    full_runs = []
    invalid = []

    for name, (abi, _) in DEVICES.items():
        for api in APIS:
            filename = f"{name}-{api}.log"
            digest = archive_log(filename)
            lines = (ARCHIVE / filename).read_text(
                encoding="utf-8"
            ).splitlines()

            assert lines[:4] == [
                "TEST=vulkan-backend-public-chain",
                f"ABI={abi}",
                f"API={api}",
                "ADB_EXIT_CODE=0",
            ]
            assert lines[-1] == (
                f"BACKEND_PUBLIC_CHAIN_DIAGNOSTIC_PASS"
                f" api={api} trials=2 dispatches=4"
                " descriptors=4 storage_buffers=3"
                " verified_words=256 shaders=2"
                " password_inputs=0 scheduled=0"
            )

            assert sum(
                line.startswith("BACKEND_VULKAN_CHAIN_DISPATCH_PASS ")
                for line in lines
            ) == 2

            native.append({
                "device": name,
                "abi": abi,
                "requested_api": api,
                "file": filename,
                "sha256": digest,
                "native_exit_code": 0,
                "vk_instances": 2,
                "compute_dispatches": 4,
                "intermediate_words_verified": 128,
                "final_words_verified": 128,
                "cryptographic_shaders": 0,
            })

        filename = f"{name}-hashcat-chain-info.log"
        digest = archive_log(filename)
        lines = (ARCHIVE / filename).read_text(
            encoding="utf-8"
        ).splitlines()

        assert lines[:5] == [
            "TEST=vulkan-hashcat-public-chain",
            f"DEVICE={name}",
            f"ABI={abi}",
            "COMMAND=-I",
            "ADB_EXIT_CODE=0",
        ]
        assert lines[-1] == "HASHCAT_KERNEL_EXIT=0"

        assert sum(
            line.startswith("BACKEND_VULKAN_KERNEL_DISPATCH_PASS ")
            for line in lines
        ) == 1
        assert sum(
            line.startswith("BACKEND_VULKAN_CHAIN_DISPATCH_PASS ")
            for line in lines
        ) == 1

        full_runs.append({
            "device": name,
            "abi": abi,
            "file": filename,
            "sha256": digest,
            "native_exit_code": 0,
            "requested_api": "1.0",
            "single_stage_dispatches": 1,
            "two_stage_dispatches": 2,
            "intermediate_words_verified": 64,
            "final_words_verified": 64,
            "first_kernel_words_verified": 64,
            "normal_opencl_gpu_enumeration": True,
            "hashcat_hashing_scheduler_connected": False,
        })

        filename = f"{name}-negative-controls.log"

        invalid.append({
            "device": name,
            "abi": abi,
            "file": filename,
            "sha256": archive_log(filename),
            "malformed_native_commands": 3,
        })

    assert len(native) == 6
    assert len(full_runs) == 3
    assert len(invalid) == 3
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
                BUILD / f"vulkan-backend-public-chain-{abi}"
            ),
            "production_backend_object": sha256(
                BUILD / f"hashcat-backend-vulkan-{abi}.o"
            ),
        }
        for abi in ("arm64", "armv7")
    }

    manifest = {
        "date": "2026-11-01",
        "scope": (
            "Two public noncryptographic Vulkan SPIR-V 1.0 Compute shaders "
            "with one explicit shader-write-to-read GPU buffer barrier "
            "in the production Android HASHCAT -I backend"
        ),
        "minimum_android_api": 24,
        "standalone_requested_apis": list(APIS),
        "full_hashcat_requested_api": "1.0",
        "stage_names": ["public_u32_transform", "public_u32_rotate"],
        "two_stage_storage_buffers": 3,
        "two_stage_descriptor_sets": 2,
        "two_stage_descriptor_count": 4,
        "two_stage_dispatches": 2,
        "words_per_stage": 64,
        "shader_compiler": "glslangValidator 14.3.0 --target-env vulkan1.0",
        "spirv": modules,
        "source_sha256": {
            name: source_sha256(ROOT / name)
            for name in SOURCES
        },
        "source_hash_policy": "CRLF normalized to LF",
        "native_artifact_sha256": standalone_hashes,
        "full_hashcat_artifact_sha256": full_hashes,
        "native_runs": native,
        "full_hashcat_info_runs": full_runs,
        "invalid_cli": invalid,
        "totals": {
            "physical_process_profiles": 3,
            "standalone_gpu_runs": 6,
            "full_hashcat_info_runs": 3,
            "real_spirv_vkcmd_dispatches": 33,
            "public_integer_words_verified": 2112,
            "invalid_native_commands": 9,
        },
        "hashcat_production_backend_code_executed": True,
        "hashcat_info_mode_only": True,
        "normal_opencl_device_discovery_preserved": True,
        "hashcat_hashing_scheduler_connected": False,
        "cryptographic_spirv_kernels_ported": 0,
        "password_or_hash_inputs_processed": False,
        "no_vulkan10_only_hardware_tested": True,
        "no_android_api24_os_hardware_tested": True,
        "capture_policy": (
            "Native Android stdout/stderr is captured over ADB and "
            "normalized to UTF-8 LF by the PowerShell runner. "
            "Archived logs preserve the saved runner bytes, not "
            "unprocessed raw ADB output."
        ),
        "limitations": [
            "The second public Compute kernel is not a HASHCAT cryptographic OpenCL-mode port; it only transforms synthetic uint32 data.",
            "Both new pipelines are dispatched only from hashcat -I information mode and a standalone diagnostic linked against production source.",
            "The two-stage job uses a separate temporary VkDevice owned by the same backend_ctx Vulkan catalogue. The original one-stage and empty-session checks remain separate.",
            "Only the first physical device (index zero) and two fixed workgroups of 32 threads per stage are tested.",
            "Newer Vulkan drivers and Android OS versions were used despite targeting Android API24 and Vulkan 1.0.",
            "No actual shader loss, GPU device reset, concurrent hash scheduling, or physical Vulkan-1.0-only GPU was tested.",
        ],
    }

    target = ARCHIVE / "manifest.json"
    target.write_text(
        json.dumps(manifest, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8", newline="\n",
    )

    print("PASS 6 native real Android two-stage GPU diagnostic runs")
    print("PASS 3 full Android hashcat -I two-stage GPU runs")
    print("PASS 33 vkCmdDispatch calls and 2112 CPU-verified public integers")
    print("PASS 9 malformed native CLI requests rejected")
    print("PASS both SPIR-V modules and exact source/ELF identities")
    print("MANIFEST_SHA256", sha256(target))


if __name__ == "__main__":
    main()
