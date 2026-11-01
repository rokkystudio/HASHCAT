#!/usr/bin/env python3
"""Verify the first real SPIR-V public Compute kernel in HASHCAT -I.

This offline regression checks six real Android GPU shader diagnostic
runs and three full HASHCAT information-mode executions, exact Vulkan
1.0 SPIR-V words, production source identities, Android ARM64/ARMv7
executables, module_00000 and the actual Vulkan shader API symbols.
Deliberate log mutations must be rejected. No ADB, GPU or password
processing is invoked by this verifier.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = ROOT / "tests/fixtures/android-gpu/vulkan-backend-public-kernel-20261101"
SPIRV_HEADER = ROOT / "source/include/backend_vulkan_public_spirv.h"
SPIRV_BINARY = ROOT / "build/diagnostic-multimode/vulkan-backend-public-kernel.spv"

DEVICES = {
    "redmagic-arm64": ("arm64-v8a", "arm64", "Adreno"),
    "redmagic-armv7": ("armeabi-v7a", "armv7", "Adreno"),
    "pixel-arm64": ("arm64-v8a", "arm64", "Mali-G715"),
}


def sha256(path: Path) -> str:
    """Return SHA-256 of one complete existing file."""
    assert path.is_file(), path

    return hashlib.sha256(path.read_bytes()).hexdigest()


def normalized_source_sha256(path: Path) -> str:
    """Hash checked-out source after normalizing CRLF line endings."""
    return hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")
    ).hexdigest()


def require_order(lines: list[str], patterns: list[str]) -> None:
    """Require exact ordered event patterns with unique matches.

    Each event pattern must match one line after the previous one; no
    fallback or inferred successful result is allowed. Other ordinary
    OpenCL diagnostic lines remain permitted between Vulkan events.
    """
    cursor = 0

    for pattern in patterns:
        matching = [
            i for i in range(cursor, len(lines))
            if re.fullmatch(pattern, lines[i]) is not None
        ]
        assert matching, ("Missing Vulkan event", pattern)
        cursor = matching[0] + 1


def kernel_events(
    api: str,
    selected_count: int,
) -> list[str]:
    """Declare the production-owned shader's exact public Vulkan events."""
    return [
        rf"DEVICE_CATALOG_SELECT index=0 queue_family=\d+ "
        rf"effective_api={re.escape(api)} selections={selected_count}",
        rf"BACKEND_VULKAN_KERNEL_READY index=0 queue_family=\d+ "
        rf"api={re.escape(api)} shader_words=326 descriptors=2 "
        r"local=32 groups=2 features=0 extensions=0 scheduled=0",
        r"BACKEND_VULKAN_KERNEL_DISPATCH_PASS "
        r"kernel=public_u32_transform words=64 local=32 groups=2 "
        r"verified=64 coherent_input=[01] coherent_output=[01] "
        r"shaders=1 password_inputs=0 scheduled=0",
        r"BACKEND_VULKAN_KERNEL_CLOSE index=0 result=0 "
        r"released=1 scheduled=0",
    ]


def verify_standalone(
    lines: list[str],
    abi: str,
    api: str,
    gpu: str,
) -> None:
    """Verify two real Vulkan shader, descriptor and resource lifecycles."""
    assert lines[:4] == [
        "TEST=vulkan-backend-public-kernel",
        f"ABI={abi}",
        f"API={api}",
        "ADB_EXIT_CODE=0",
    ]

    body = lines[4:]
    assert body[-1] == (
        f"BACKEND_PUBLIC_KERNEL_DIAGNOSTIC_PASS api={api}"
        " trials=2 dispatches=2 descriptors=2 storage_buffers=2"
        " verified_words=128 shaders=1 password_inputs=0 scheduled=0"
    )

    for prefix, count in (
        ("DEVICE_CATALOG_OPEN ", 2),
        ("DEVICE_CATALOG_CLOSE ", 2),
        ("BACKEND_VULKAN_CATALOG_READY ", 2),
        ("BACKEND_VULKAN_KERNEL_READY ", 2),
        ("BACKEND_VULKAN_KERNEL_DISPATCH_PASS ", 2),
        ("BACKEND_VULKAN_KERNEL_CLOSE ", 2),
        ("BACKEND_PUBLIC_KERNEL_TRIAL_PASS ", 2),
    ):
        assert sum(
            line.startswith(prefix) for line in body
        ) == count, prefix

    expected = []

    for trial in (0, 1):
        expected += [
            r"VULKAN_LOADER_VERSION=1\.\d+\.\d+ "
            r"source=vkEnumerateInstanceVersion",
            rf"SELECT_INSTANCE_API={re.escape(api)} "
            r"loader_minor=\d+ cap=1\.1",
            rf"DEVICE_CATALOG_OPEN requested_api={re.escape(api)} "
            rf"instance_api={re.escape(api)} devices=1 "
            r"extensions=0 layers=0",
            rf"BACKEND_VULKAN_CATALOG_READY devices=1 "
            rf"requested_api={re.escape(api)} scheduled=0",
            rf"SELECT_DEVICE_API={re.escape(api)} "
            r"reported_minor=\d+ instance_minor=\d+ cap=1\.1",
            rf"VULKAN_CANDIDATE index=0 name=.*{gpu}.* "
            r"vendor=0x[0-9a-f]+ device=0x[0-9a-f]+ "
            r"api=1\.\d+\.\d+ driver=0x[0-9a-f]+",
            r"ACCEPT_COMPUTE index=0 queue_family=\d+ "
            r"local_max=\d+ invocations_max=\d+ groups_max=\d+ "
            r"shared_max=\d+ storage_range_max=\d+ descriptor_max=\d+ "
            r"non_coherent_atom=\d+",
        ]
        expected += kernel_events(api, 1)
        expected += [
            "DEVICE_CATALOG_CLOSE devices=1 selections=1",
            rf"BACKEND_PUBLIC_KERNEL_TRIAL_PASS trial={trial} "
            r"descriptors=2 pipelines=1 dispatches=1 "
            r"verified_words=64 scheduled=0",
        ]

    expected.append(
        rf"BACKEND_PUBLIC_KERNEL_DIAGNOSTIC_PASS api={re.escape(api)}"
        r" trials=2 dispatches=2 descriptors=2 storage_buffers=2"
        r" verified_words=128 shaders=1 password_inputs=0 scheduled=0"
    )

    require_order(body, expected)

    assert not any(
        line.startswith(("KERN_RUN ", "JOB_PORT_SUBMIT ",
                         "BACKEND_VULKAN_KERNEL_MISMATCH "))
        for line in body
    )


def verify_full_hashcat(
    lines: list[str],
    device: str,
    abi: str,
    gpu: str,
) -> None:
    """Verify one actual hashcat -I GPU dispatch and OpenCL coexistence.

    The exact production process must open the existing empty Vulkan
    session, then independently dispatch/verify a real shader, preserve
    ordinary OpenCL GPU enumeration, close the empty session and finally
    destroy the shared Vulkan instance without registering a hashing
    device.
    """
    assert lines[:5] == [
        "TEST=vulkan-hashcat-public-kernel",
        f"DEVICE={device}",
        f"ABI={abi}",
        "COMMAND=-I",
        "ADB_EXIT_CODE=0",
    ]

    body = lines[5:]
    assert body[0] == "hashcat (v7.1.2) starting in backend information mode"
    assert body[-1] == "HASHCAT_KERNEL_EXIT=0"

    patterns = [
        r"hashcat \(v7\.1\.2\) starting in backend information mode",
        r"VULKAN_LOADER_VERSION=1\.\d+\.\d+ "
        r"source=vkEnumerateInstanceVersion",
        r"SELECT_INSTANCE_API=1\.0 loader_minor=\d+ cap=1\.1",
        r"DEVICE_CATALOG_OPEN requested_api=1\.0 "
        r"instance_api=1\.0 devices=1 extensions=0 layers=0",
        r"BACKEND_VULKAN_CATALOG_READY devices=1 "
        r"requested_api=1\.0 scheduled=0",
        r"Vulkan 1\.0 discovery found 1 physical device\(s\); "
        r"Vulkan Compute scheduling is not enabled\.",
        r"DEVICE_CATALOG_SELECT index=0 queue_family=\d+ "
        r"effective_api=1\.0 selections=1",
        r"BACKEND_VULKAN_SESSION_READY index=0 queue_family=\d+ "
        r"effective_api=1\.0 command_buffers=1 empty_submissions=1 "
        r"features=0 extensions=0 scheduled=0",
        r"Vulkan 1\.0 Compute device session verified with an empty "
        r"queue submission; Vulkan hashing remains disabled\.",
    ]

    patterns += kernel_events("1.0", 2)
    patterns += [
        r"Vulkan 1\.0 public SPIR-V Compute kernel dispatched "
        r"and verified; hashing remains disabled\.",
        r"OpenCL Info:",
        r"OpenCL Platform ID #1",
        r"  Backend Device ID #01",
        rf"    Name\.+: .*{gpu}.*",
        r"BACKEND_VULKAN_SESSION_CLOSE index=0 queue_family=\d+ "
        r"empty_submissions=1 wait_result=0",
        r"DEVICE_CATALOG_CLOSE devices=1 selections=2",
        r"HASHCAT_KERNEL_EXIT=0",
    ]

    require_order(body, patterns)

    for prefix, count in (
        ("DEVICE_CATALOG_OPEN ", 1),
        ("DEVICE_CATALOG_CLOSE ", 1),
        ("BACKEND_VULKAN_SESSION_READY ", 1),
        ("BACKEND_VULKAN_SESSION_CLOSE ", 1),
        ("BACKEND_VULKAN_KERNEL_READY ", 1),
        ("BACKEND_VULKAN_KERNEL_DISPATCH_PASS ", 1),
        ("BACKEND_VULKAN_KERNEL_CLOSE ", 1),
        ("OpenCL Info:", 1),
    ):
        assert sum(
            event.startswith(prefix) for event in body
        ) == count, (device, prefix)

    assert not any(
        event.startswith((
            "BACKEND_VULKAN_KERNEL_MISMATCH ",
            "KERN_RUN ", "JOB_PORT_SUBMIT ",
        ))
        for event in body
    )


def verify_cli(lines: list[str], abi: str) -> None:
    """Require three malformed native commands to exit with status two."""
    assert lines == [
        "TEST=vulkan-backend-public-kernel-invalid-inputs",
        f"ABI={abi}",
        "INVALID_CASES=3",
        "INVALID_CASE=missing-arguments exit=2",
        "INVALID_CASE=unsupported-api exit=2",
        "INVALID_CASE=wrong-flag exit=2",
    ]


def verify_spirv(
    manifest: dict,
    artifacts: Path | None,
) -> None:
    """Confirm exact Vulkan SPIR-V 1.0 module and embedded C word array."""
    assert manifest["public_kernel_name"] == "public_u32_transform"
    assert manifest["public_kernel_storage_descriptors"] == 2
    assert manifest["public_kernel_workgroup_size"] == 32
    assert manifest["public_kernel_group_count"] == 2
    assert manifest["public_kernel_shader_words"] == 326
    assert manifest["public_kernel_input_words"] == 64
    assert manifest["public_kernel_output_words"] == 64
    assert manifest["spirv_version"] == "1.0"
    assert manifest["spirv_bytes"] == 1304

    code = SPIRV_HEADER.read_text(encoding="utf-8")
    matches = re.findall(r"UINT32_C \(0x([0-9a-f]{8})\)", code)
    assert len(matches) == 326

    embedded = struct.pack(
        "<" + "I" * len(matches),
        *(int(word, 16) for word in matches),
    )
    assert len(embedded) == 1304
    assert sha256_bytes(embedded) == manifest["spirv_sha256"]
    words = struct.unpack_from("<5I", embedded)
    assert words[0] == 0x07230203
    assert words[1] == 0x00010000
    assert words[4] == 0

    shader = (ROOT / "tools/vulkan-backend-public-kernel.comp").read_text(
        encoding="utf-8"
    )
    assert "layout (local_size_x = 32" in shader
    assert "layout (set = 0, binding = 0, std430)" in shader
    assert "layout (set = 0, binding = 1, std430)" in shader
    assert "(value * 1664525u + 1013904223u) ^ (index * 2654435769u)" in shader

    outputs = [
        (
            ((0x10203040 ^ ((i * 0x01010101) & 0xFFFFFFFF))
             * 1664525 + 1013904223) & 0xFFFFFFFF
        ) ^ ((i * 2654435769) & 0xFFFFFFFF)
        for i in range(64)
    ]
    assert len(set(outputs)) == 64

    if artifacts is not None:
        module = SPIRV_BINARY
        assert sha256(module) == manifest["spirv_sha256"]
        assert module.read_bytes() == embedded

    with tempfile.TemporaryDirectory() as directory:
        regenerated = Path(directory) / "embedded.h"
        result = subprocess.run(
            [
                sys.executable, "-B",
                str(ROOT / "tools/generate-vulkan-backend-public-kernel.py"),
                "--spirv", str(SPIRV_BINARY)
                if artifacts is not None
                else str(write_temp_spirv(Path(directory), embedded)),
                "--header", str(regenerated),
            ],
            check=True, capture_output=True, text=True,
        )
        assert "SPIRV_EMBEDDED bytes=1304 words=326" in result.stdout
        assert regenerated.read_bytes() == SPIRV_HEADER.read_bytes()


def sha256_bytes(data: bytes) -> str:
    """Hash a complete in-memory canonical SPIR-V 1.0 module."""
    return hashlib.sha256(data).hexdigest()


def write_temp_spirv(directory: Path, data: bytes) -> Path:
    """Materialize exact verified shader bytes for offline header regeneration."""
    target = directory / "shader.spv"
    target.write_bytes(data)

    return target


def corrupt(
    lines: list[str],
    prefix: str,
    old: str,
    replacement: str,
) -> list[str]:
    """Create one deliberately false native shader event for validation."""
    indexes = [
        index for index, line in enumerate(lines)
        if line.startswith(prefix) and old in line
    ]
    assert indexes, (prefix, old)

    result = lines.copy()
    index = indexes[0]
    result[index] = result[index].replace(old, replacement, 1)

    return result


def verify_mutations(manifest: dict) -> None:
    """Reject fabricated GPU dispatches, shader outputs and invalid cleanup."""
    record = next(
        row for row in manifest["native_runs"]
        if row["device"] == "pixel-arm64"
        and row["requested_api"] == "1.0"
    )
    original = (ARCHIVE / record["file"]).read_text(
        encoding="utf-8"
    ).splitlines()

    mutations = (
        ("BACKEND_VULKAN_KERNEL_READY ", "shader_words=326", "shader_words=325"),
        ("BACKEND_VULKAN_KERNEL_READY ", "descriptors=2", "descriptors=1"),
        ("BACKEND_VULKAN_KERNEL_READY ", "local=32", "local=64"),
        ("BACKEND_VULKAN_KERNEL_READY ", "groups=2", "groups=1"),
        ("BACKEND_VULKAN_KERNEL_READY ", "features=0", "features=1"),
        ("BACKEND_VULKAN_KERNEL_DISPATCH_PASS ", "kernel=public_u32_transform", "kernel=password_hash"),
        ("BACKEND_VULKAN_KERNEL_DISPATCH_PASS ", "words=64", "words=63"),
        ("BACKEND_VULKAN_KERNEL_DISPATCH_PASS ", "verified=64", "verified=63"),
        ("BACKEND_VULKAN_KERNEL_DISPATCH_PASS ", "shaders=1", "shaders=0"),
        ("BACKEND_VULKAN_KERNEL_DISPATCH_PASS ", "password_inputs=0", "password_inputs=1"),
        ("BACKEND_VULKAN_KERNEL_DISPATCH_PASS ", "scheduled=0", "scheduled=1"),
        ("BACKEND_VULKAN_KERNEL_CLOSE ", "result=0", "result=-4"),
        ("BACKEND_VULKAN_KERNEL_CLOSE ", "released=1", "released=0"),
        ("BACKEND_VULKAN_CATALOG_READY ", "scheduled=0", "scheduled=1"),
        ("DEVICE_CATALOG_OPEN ", "devices=1", "devices=0"),
        ("DEVICE_CATALOG_CLOSE ", "selections=1", "selections=0"),
        ("BACKEND_PUBLIC_KERNEL_TRIAL_PASS ", "verified_words=64", "verified_words=63"),
        ("BACKEND_PUBLIC_KERNEL_DIAGNOSTIC_PASS ", "dispatches=2", "dispatches=1"),
    )

    rejections = 0

    for prefix, old, new in mutations:
        damaged = corrupt(original, prefix, old, new)

        try:
            verify_standalone(damaged, record["abi"], "1.0", "Mali-G715")
        except AssertionError:
            rejections += 1
        else:
            raise AssertionError(f"Accepted incorrect GPU compute evidence {prefix}")

    full_record = next(
        row for row in manifest["full_hashcat_info_runs"]
        if row["device"] == "pixel-arm64"
    )
    full = (ARCHIVE / full_record["file"]).read_text(
        encoding="utf-8"
    ).splitlines()

    full_mutations = (
        ("BACKEND_VULKAN_KERNEL_DISPATCH_PASS ", "verified=64", "verified=0"),
        ("BACKEND_VULKAN_KERNEL_CLOSE ", "result=0", "result=-4"),
        ("BACKEND_VULKAN_SESSION_READY ", "empty_submissions=1", "empty_submissions=0"),
        ("BACKEND_VULKAN_SESSION_CLOSE ", "wait_result=0", "wait_result=-4"),
        ("BACKEND_VULKAN_CATALOG_READY ", "scheduled=0", "scheduled=1"),
        ("OpenCL Info:", "OpenCL Info:", "OpenCL Missing:"),
        ("DEVICE_CATALOG_CLOSE ", "selections=2", "selections=1"),
        ("HASHCAT_KERNEL_EXIT=0", "HASHCAT_KERNEL_EXIT=0", "HASHCAT_KERNEL_EXIT=1"),
    )

    for prefix, old, new in full_mutations:
        damaged = corrupt(full, prefix, old, new)

        try:
            verify_full_hashcat(
                damaged, "pixel-arm64", "arm64-v8a", "Mali-G715"
            )
        except AssertionError:
            rejections += 1
        else:
            raise AssertionError(
                f"Accepted altered full HASHCAT -I GPU event: {prefix}"
            )

    # The same native work must not close VkInstance before the kernel.
    altered = original.copy()
    close_index = next(
        i for i, value in enumerate(altered)
        if value.startswith("DEVICE_CATALOG_CLOSE ")
    )
    kernel_index = next(
        i for i, value in enumerate(altered)
        if value.startswith("BACKEND_VULKAN_KERNEL_DISPATCH_PASS ")
    )
    altered[close_index], altered[kernel_index] = (
        altered[kernel_index], altered[close_index]
    )

    try:
        verify_standalone(altered, "arm64-v8a", "1.0", "Mali-G715")
    except AssertionError:
        rejections += 1
    else:
        raise AssertionError("Accepted Vulkan VkInstance destruction before kernel")

    assert rejections == len(mutations) + len(full_mutations) + 1

    print(f"PASS {rejections} deliberate Vulkan public-kernel evidence corruptions rejected")


def inspect_elf(
    readelf: Path,
    executable: Path,
    abi: str,
    vulkan: bool,
) -> None:
    """Verify Android API24 ELF and required real Vulkan Compute entry points."""
    def read(*arguments: str) -> str:
        """Return read-only llvm-readelf output for one native executable."""
        return subprocess.run(
            [str(readelf), "-W", *arguments, str(executable)],
            check=True, capture_output=True, text=True,
        ).stdout

    header = read("--file-header")
    notes = read("--notes")
    dynamic = read("--dynamic")
    symbols = read("--dyn-syms")
    elf_class = "ELF64" if abi == "arm64" else "ELF32"
    machine = "AArch64" if abi == "arm64" else "ARM"

    assert re.search(rf"Class:\s+{elf_class}\b", header)
    assert re.search(rf"Machine:\s+{machine}\b", header)
    assert "NT_ANDROID_TYPE_IDENT" in notes
    assert "description data: 18 00 00 00" in notes

    if vulkan:
        assert "Shared library: [libvulkan.so]" in dynamic

        for symbol in (
            "vkGetInstanceProcAddr", "vkCreateDevice",
            "vkCreateShaderModule", "vkCreateComputePipelines",
            "vkCreateDescriptorPool", "vkCreateDescriptorSetLayout",
            "vkUpdateDescriptorSets", "vkCmdBindPipeline",
            "vkCmdBindDescriptorSets", "vkCmdDispatch",
            "vkCmdPipelineBarrier", "vkQueueSubmit",
            "vkWaitForFences", "vkFlushMappedMemoryRanges",
            "vkInvalidateMappedMemoryRanges", "vkDestroyDevice",
        ):
            assert re.search(rf"UND\s+{symbol}\b", symbols), symbol

        assert not re.search(
            r"UND\s+vkEnumerateInstanceVersion\b", symbols
        )


def main() -> None:
    """Audit real Android GPU shader dispatches and exact source/ELF inputs."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--artifacts", type=Path,
        help="Isolated directory with ARM64/ARMv7 kernel ELF and compiled shader",
    )
    parser.add_argument(
        "--full", type=Path,
        help="Two fully built HASHCAT Android process ABI directories",
    )
    parser.add_argument(
        "--readelf", type=Path,
        help="Android NDK llvm-readelf for static ABI and Vulkan 1.0 proof",
    )
    args = parser.parse_args()

    if args.readelf is not None and (
        args.artifacts is None or args.full is None
    ):
        parser.error("--readelf requires both --artifacts and --full")

    manifest = json.loads(
        (ARCHIVE / "manifest.json").read_text(encoding="utf-8")
    )

    assert manifest["date"] == "2026-11-01"
    assert manifest["minimum_android_api"] == 24
    assert manifest["vulkan_api_requested_by_full_hashcat"] == "1.0"
    assert manifest["standalone_requested_apis"] == ["1.0", "1.1"]
    assert manifest["hashcat_production_backend_code_executed"] is True
    assert manifest["hashcat_info_mode_only"] is True
    assert manifest["normal_opencl_device_discovery_preserved"] is True
    assert manifest["hashcat_hashing_scheduler_connected"] is False
    assert manifest["cryptographic_spirv_kernels_ported"] == 0
    assert manifest["password_or_hash_inputs_processed_by_shader"] is False
    assert manifest["no_vulkan10_only_hardware_tested"] is True
    assert manifest["no_android_api24_os_hardware_tested"] is True

    assert len(manifest["source_sha256"]) == 24

    for source, digest in manifest["source_sha256"].items():
        assert normalized_source_sha256(ROOT / source) == digest, source

    assert len(manifest["native_runs"]) == 6
    assert len(manifest["full_hashcat_info_runs"]) == 3
    assert len(manifest["invalid_cli"]) == 3
    assert len(list(ARCHIVE.glob("*.log"))) == 12

    seen_native = set()
    native_dispatches = native_words = 0

    for row in manifest["native_runs"]:
        name = row["device"]
        assert name in DEVICES
        abi, _, gpu = DEVICES[name]
        key = (name, row["requested_api"])

        assert key not in seen_native
        seen_native.add(key)
        assert row["abi"] == abi
        assert row["requested_api"] in ("1.0", "1.1")
        assert row["native_exit_code"] == 0
        assert row["vk_instances"] == 2
        assert row["public_shader_dispatches"] == 2
        assert row["public_words_verified"] == 128
        assert row["gpu_kernel_descriptor_sets"] == 2
        assert row["hashcat_hash_scheduler_used"] is False

        path = ARCHIVE / row["file"]
        data = path.read_bytes()
        assert data and data.endswith(b"\n") and b"\r" not in data
        assert b"91809037" not in data and b"46281FDAS002AL" not in data
        assert sha256(path) == row["sha256"]

        verify_standalone(
            path.read_text(encoding="utf-8").splitlines(),
            abi, row["requested_api"], gpu,
        )

        native_dispatches += row["public_shader_dispatches"]
        native_words += row["public_words_verified"]

    assert len(seen_native) == 6
    assert native_dispatches == 12
    assert native_words == 768

    seen_full = set()
    frontend_dispatches = frontend_words = 0

    for row in manifest["full_hashcat_info_runs"]:
        name = row["device"]
        assert name in DEVICES and name not in seen_full
        seen_full.add(name)
        abi, _, gpu = DEVICES[name]

        assert row["abi"] == abi
        assert row["native_exit_code"] == 0
        assert row["requested_api"] == "1.0"
        assert row["public_shader_dispatches"] == 1
        assert row["public_words_verified"] == 64
        assert row["normal_opencl_gpu_enumeration"] is True
        assert row["hashcat_password_kernel_dispatches"] == 0

        path = ARCHIVE / row["file"]
        data = path.read_bytes()
        assert data and data.endswith(b"\n") and b"\r" not in data
        assert b"91809037" not in data and b"46281FDAS002AL" not in data
        assert sha256(path) == row["sha256"]

        verify_full_hashcat(
            path.read_text(encoding="utf-8").splitlines(),
            name, abi, gpu,
        )

        frontend_dispatches += row["public_shader_dispatches"]
        frontend_words += row["public_words_verified"]

    assert seen_full == set(DEVICES)
    assert frontend_dispatches == 3
    assert frontend_words == 192

    controls = set()

    for row in manifest["invalid_cli"]:
        name = row["device"]
        assert name in DEVICES and name not in controls
        controls.add(name)

        abi = DEVICES[name][0]
        assert row["abi"] == abi
        assert row["malformed_native_commands"] == 3

        path = ARCHIVE / row["file"]
        assert sha256(path) == row["sha256"]
        verify_cli(
            path.read_text(encoding="utf-8").splitlines(),
            abi,
        )

    assert controls == set(DEVICES)
    assert manifest["totals"] == {
        "physical_process_profiles": 3,
        "standalone_gpu_runs": 6,
        "full_hashcat_info_runs": 3,
        "real_spirv_vkcmd_dispatches": 15,
        "public_integer_words_verified": 960,
        "invalid_native_commands": 9,
    }

    directory = args.artifacts.resolve(strict=True) if args.artifacts else None
    full = args.full.resolve(strict=True) if args.full else None

    verify_spirv(manifest, directory)

    if directory is not None:
        for abi, hashes in manifest["native_artifact_sha256"].items():
            executable = directory / f"vulkan-backend-public-kernel-{abi}"
            backend_object = directory / f"hashcat-backend-vulkan-{abi}.o"

            assert sha256(executable) == hashes["diagnostic"]
            assert sha256(backend_object) == hashes["production_backend_c_object"]

            if args.readelf is not None:
                inspect_elf(args.readelf, executable, abi, True)

        print("PASS two exact native Vulkan kernel binaries and backend objects")

    if full is not None:
        for abi, hashes in manifest["full_hashcat_artifact_sha256"].items():
            executable = full / abi / "hashcat"
            module = full / abi / "modules/module_00000.so"

            assert sha256(executable) == hashes["frontend"]
            assert sha256(module) == hashes["module_00000"]

            if args.readelf is not None:
                inspect_elf(
                    args.readelf, executable,
                    "arm64" if abi == "arm64-v8a" else "armv7",
                    True,
                )
                inspect_elf(
                    args.readelf, module,
                    "arm64" if abi == "arm64-v8a" else "armv7",
                    False,
                )

        print("PASS two exact full Android HASHCAT binaries and modules")

    if args.readelf is not None:
        print("PASS native VkComputePipeline/vkCmdDispatch and Android API24 symbols")

    verify_mutations(manifest)

    print("PASS six public SPIR-V GPU diagnostic processes and 12 dispatches")
    print("PASS three real HASHCAT -I processes, Vulkan dispatch and OpenCL GPU")
    print("PASS 960 CPU-verified public uint32 words across 15 Vulkan dispatches")
    print("PASS complete SPIR-V 1.0 byte-for-byte C embedding")
    print("PASS nine rejected malformed native shader CLI commands")


if __name__ == "__main__":
    main()
