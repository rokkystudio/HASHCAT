#!/usr/bin/env python3
"""Verify two real public SPIR-V Compute stages in Android HASHCAT -I.

Checks six native real-GPU chain diagnostic processes and three complete
Android HASHCAT information-mode processes against exact event order,
source/ELF/SPIR-V SHA-256, shader entry points, CPU reference arithmetic,
and the ordinary OpenCL GPU discovery records. Rejects intentionally
corrupted shader, barrier, ownership and scheduler claims. The offline
verifier never runs ADB, shaders, hashing or password candidates.
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
ARCHIVE = ROOT / "tests/fixtures/android-gpu/vulkan-backend-public-chain-20261101"

DEVICES = {
    "redmagic-arm64": ("arm64-v8a", "arm64", "Adreno"),
    "redmagic-armv7": ("armeabi-v7a", "armv7", "Adreno"),
    "pixel-arm64": ("arm64-v8a", "arm64", "Mali-G715"),
}
APIS = ("1.0", "1.1")

MODULES = {
    "1": (
        ROOT / "build/diagnostic-multimode/vulkan-backend-public-chain-20261101/vulkan-backend-public-stage1.spv",
        ROOT / "source/include/backend_vulkan_public_spirv.h",
        "backend_vulkan_public_spirv",
        "HC_BACKEND_VULKAN_PUBLIC_SPIRV_H",
        "tools/vulkan-backend-public-kernel.comp",
    ),
    "2": (
        ROOT / "build/diagnostic-multimode/vulkan-backend-public-chain-20261101/vulkan-backend-public-stage2.spv",
        ROOT / "source/include/backend_vulkan_public_stage2_spirv.h",
        "backend_vulkan_public_stage2_spirv",
        "HC_BACKEND_VULKAN_PUBLIC_STAGE2_SPIRV_H",
        "tools/vulkan-backend-public-stage2.comp",
    ),
}


def sha256(path: Path) -> str:
    """Hash the exact bytes of one available GPU log, ELF or SPIR-V."""
    assert path.is_file(), path

    return hashlib.sha256(path.read_bytes()).hexdigest()


def normalized_source_sha256(path: Path) -> str:
    """Hash checkout source after documented CRLF-to-LF conversion."""
    return hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")
    ).hexdigest()


def match(line: str, pattern: str) -> re.Match[str]:
    """Require one whole native diagnostic event matching a declared pattern."""
    value = re.fullmatch(pattern, line)
    assert value is not None, (pattern, line)

    return value


def require_order(lines: list[str], patterns: list[str]) -> None:
    """Require every declared GPU event once in source execution order."""
    cursor = 0

    for pattern in patterns:
        candidates = [
            i for i in range(cursor, len(lines))
            if re.fullmatch(pattern, lines[i]) is not None
        ]

        assert len(candidates) == 1, ("Invalid Vulkan event", pattern)
        cursor = candidates[0] + 1


def verify_standalone(
    lines: list[str],
    abi: str,
    api: str,
    gpu: str,
) -> None:
    """Verify two complete real Vulkan device/instance chain lifetimes."""
    assert lines[:4] == [
        "TEST=vulkan-backend-public-chain",
        f"ABI={abi}",
        f"API={api}",
        "ADB_EXIT_CODE=0",
    ]

    body = lines[4:]
    assert len(body) == 27, len(body)

    assert body[-1] == (
        f"BACKEND_PUBLIC_CHAIN_DIAGNOSTIC_PASS"
        f" api={api} trials=2 dispatches=4 descriptors=4"
        " storage_buffers=3 verified_words=256"
        " shaders=2 password_inputs=0 scheduled=0"
    )

    for trial in (0, 1):
        start = trial * 13
        segment = body[start:start + 13]
        assert len(segment) == 13

        loader = match(
            segment[0],
            r"VULKAN_LOADER_VERSION=1\.(\d+)\.(\d+)"
            r" source=vkEnumerateInstanceVersion",
        )
        minor = int(loader.group(1))
        assert minor >= int(api.split(".")[1])

        assert segment[1] == (
            f"SELECT_INSTANCE_API={api} loader_minor={minor} cap=1.1"
        )
        assert segment[2] == (
            f"DEVICE_CATALOG_OPEN requested_api={api}"
            f" instance_api={api} devices=1 extensions=0 layers=0"
        )
        assert segment[3] == (
            f"BACKEND_VULKAN_CATALOG_READY devices=1"
            f" requested_api={api} scheduled=0"
        )

        select = match(
            segment[4],
            rf"SELECT_DEVICE_API={re.escape(api)}"
            r" reported_minor=(\d+) instance_minor=(\d+) cap=1\.1",
        )
        assert int(select.group(1)) >= int(api.split(".")[1])
        assert int(select.group(2)) == int(api.split(".")[1])

        candidate = match(
            segment[5],
            r"VULKAN_CANDIDATE index=0 name=(.+)"
            r" vendor=0x[0-9a-f]+ device=0x[0-9a-f]+"
            r" api=1\.(\d+)\.(\d+) driver=0x[0-9a-f]+",
        )
        assert gpu in candidate.group(1)
        assert int(candidate.group(2)) >= int(api.split(".")[1])

        limits = match(
            segment[6],
            r"ACCEPT_COMPUTE index=0 queue_family=(\d+)"
            r" local_max=(\d+) invocations_max=(\d+)"
            r" groups_max=(\d+) shared_max=(\d+)"
            r" storage_range_max=(\d+) descriptor_max=(\d+)"
            r" non_coherent_atom=(\d+)",
        )
        family = int(limits.group(1))
        assert int(limits.group(2)) >= 32
        assert int(limits.group(3)) >= 32
        assert int(limits.group(4)) >= 2
        assert int(limits.group(6)) >= 256
        assert int(limits.group(7)) >= 4
        atom = int(limits.group(8))
        assert atom > 0 and (atom & (atom - 1)) == 0

        assert segment[7] == (
            f"DEVICE_CATALOG_SELECT index=0 queue_family={family}"
            f" effective_api={api} selections=1"
        )
        assert segment[8] == (
            f"BACKEND_VULKAN_CHAIN_READY index=0"
            f" queue_family={family} api={api}"
            " shader_words_first=326 shader_words_second=363"
            " descriptors=4 sets=2 buffers=3 local=32 groups=2"
            " features=0 extensions=0 scheduled=0"
        )
        match(
            segment[9],
            r"BACKEND_VULKAN_CHAIN_DISPATCH_PASS"
            r" stage1=public_u32_transform stage2=public_u32_rotate"
            r" words=64 local=32 groups=2 dispatches=2"
            r" verified_first=64 verified_second=64"
            r" coherent_input=[01] coherent_intermediate=[01]"
            r" coherent_output=[01]"
            r" barrier=shader_write_to_shader_read"
            r" shaders=2 password_inputs=0 scheduled=0",
        )
        assert segment[10] == (
            "BACKEND_VULKAN_CHAIN_CLOSE index=0"
            " result=0 released=1 scheduled=0"
        )
        assert segment[11] == (
            "DEVICE_CATALOG_CLOSE devices=1 selections=1"
        )
        assert segment[12] == (
            f"BACKEND_PUBLIC_CHAIN_TRIAL_PASS trial={trial}"
            " descriptors=4 pipelines=2 dispatches=2"
            " verified_words=128 scheduled=0"
        )

    assert not any(
        x.startswith((
            "BACKEND_VULKAN_CHAIN_MISMATCH ",
            "BACKEND_VULKAN_KERNEL_MISMATCH ",
            "KERN_RUN ", "JOB_PORT_SUBMIT ",
        ))
        for x in body
    )


def verify_full_hashcat(
    lines: list[str],
    device: str,
    abi: str,
    gpu: str,
) -> None:
    """Verify three GPU dispatches in the real HASHCAT -I executable.

    A complete information-mode process opens a Vulkan catalogue and
    verifies an empty Compute session, one original public shader and
    a two-shader dependent chain. All three operations must use one
    VkInstance and preserve the ordinary OpenCL GPU device listing.
    """
    assert lines[:5] == [
        "TEST=vulkan-hashcat-public-chain",
        f"DEVICE={device}",
        f"ABI={abi}",
        "COMMAND=-I",
        "ADB_EXIT_CODE=0",
    ]

    body = lines[5:]
    assert body[0] == "hashcat (v7.1.2) starting in backend information mode"
    assert body[-1] == "HASHCAT_KERNEL_EXIT=0"

    required = [
        r"hashcat \(v7\.1\.2\) starting in backend information mode",
        r"VULKAN_LOADER_VERSION=1\.\d+\.\d+"
        r" source=vkEnumerateInstanceVersion",
        r"SELECT_INSTANCE_API=1\.0 loader_minor=\d+ cap=1\.1",
        r"DEVICE_CATALOG_OPEN requested_api=1\.0"
        r" instance_api=1\.0 devices=1 extensions=0 layers=0",
        r"BACKEND_VULKAN_CATALOG_READY devices=1"
        r" requested_api=1\.0 scheduled=0",
        r"Vulkan 1\.0 discovery found 1 physical device\(s\);"
        r" Vulkan Compute scheduling is not enabled\.",
        r"DEVICE_CATALOG_SELECT index=0 queue_family=\d+"
        r" effective_api=1\.0 selections=1",
        r"BACKEND_VULKAN_SESSION_READY index=0 queue_family=\d+"
        r" effective_api=1\.0 command_buffers=1"
        r" empty_submissions=1 features=0 extensions=0 scheduled=0",
        r"Vulkan 1\.0 Compute device session verified with an empty"
        r" queue submission; Vulkan hashing remains disabled\.",
        r"DEVICE_CATALOG_SELECT index=0 queue_family=\d+"
        r" effective_api=1\.0 selections=2",
        r"BACKEND_VULKAN_KERNEL_READY index=0 queue_family=\d+"
        r" api=1\.0 shader_words=326 descriptors=2"
        r" local=32 groups=2 features=0 extensions=0 scheduled=0",
        r"BACKEND_VULKAN_KERNEL_DISPATCH_PASS"
        r" kernel=public_u32_transform words=64 local=32 groups=2"
        r" verified=64 coherent_input=[01] coherent_output=[01]"
        r" shaders=1 password_inputs=0 scheduled=0",
        r"BACKEND_VULKAN_KERNEL_CLOSE index=0 result=0"
        r" released=1 scheduled=0",
        r"Vulkan 1\.0 public SPIR-V Compute kernel dispatched"
        r" and verified; hashing remains disabled\.",
        r"DEVICE_CATALOG_SELECT index=0 queue_family=\d+"
        r" effective_api=1\.0 selections=3",
        r"BACKEND_VULKAN_CHAIN_READY index=0 queue_family=\d+"
        r" api=1\.0 shader_words_first=326 shader_words_second=363"
        r" descriptors=4 sets=2 buffers=3 local=32 groups=2"
        r" features=0 extensions=0 scheduled=0",
        r"BACKEND_VULKAN_CHAIN_DISPATCH_PASS"
        r" stage1=public_u32_transform stage2=public_u32_rotate"
        r" words=64 local=32 groups=2 dispatches=2"
        r" verified_first=64 verified_second=64"
        r" coherent_input=[01] coherent_intermediate=[01]"
        r" coherent_output=[01] barrier=shader_write_to_shader_read"
        r" shaders=2 password_inputs=0 scheduled=0",
        r"BACKEND_VULKAN_CHAIN_CLOSE index=0 result=0"
        r" released=1 scheduled=0",
        r"Vulkan 1\.0 public two-stage SPIR-V Compute chain"
        r" dispatched and verified; hashing remains disabled\.",
        r"OpenCL Info:",
        r"OpenCL Platform ID #1",
        r"  Backend Device ID #01",
        rf"    Name\.+: .*{gpu}.*",
        r"BACKEND_VULKAN_SESSION_CLOSE index=0 queue_family=\d+"
        r" empty_submissions=1 wait_result=0",
        r"DEVICE_CATALOG_CLOSE devices=1 selections=3",
        r"HASHCAT_KERNEL_EXIT=0",
    ]

    require_order(body, required)

    for prefix, count in (
        ("DEVICE_CATALOG_OPEN ", 1),
        ("DEVICE_CATALOG_CLOSE ", 1),
        ("DEVICE_CATALOG_SELECT ", 3),
        ("BACKEND_VULKAN_SESSION_READY ", 1),
        ("BACKEND_VULKAN_SESSION_CLOSE ", 1),
        ("BACKEND_VULKAN_KERNEL_READY ", 1),
        ("BACKEND_VULKAN_KERNEL_DISPATCH_PASS ", 1),
        ("BACKEND_VULKAN_KERNEL_CLOSE ", 1),
        ("BACKEND_VULKAN_CHAIN_READY ", 1),
        ("BACKEND_VULKAN_CHAIN_DISPATCH_PASS ", 1),
        ("BACKEND_VULKAN_CHAIN_CLOSE ", 1),
        ("OpenCL Info:", 1),
    ):
        assert sum(
            event.startswith(prefix) for event in body
        ) == count, (device, prefix)

    assert not any(
        event.startswith((
            "BACKEND_VULKAN_KERNEL_MISMATCH ",
            "BACKEND_VULKAN_CHAIN_MISMATCH ",
            "KERN_RUN ", "JOB_PORT_SUBMIT ",
        ))
        for event in body
    )


def verify_cli(lines: list[str], abi: str) -> None:
    """Require three invalid native Vulkan-API CLI requests to return 2."""
    assert lines == [
        "TEST=vulkan-backend-public-chain-invalid-inputs",
        f"ABI={abi}",
        "INVALID_CASES=3",
        "INVALID_CASE=missing-arguments exit=2",
        "INVALID_CASE=unsupported-api exit=2",
        "INVALID_CASE=wrong-flag exit=2",
    ]


def verify_spirv(manifest: dict, artifacts: Path | None) -> None:
    """Verify both exact immutable SPIR-V 1.0 modules and C headers.

    Compiled module bytes must match the embedded uint32 C arrays and
    be regenerated byte-for-byte by the normal shader-header generator.
    The second source reads the preceding stage's intermediate buffer.
    Independent CPU expressions produce distinct 64-word vectors.
    """
    assert manifest["stage_names"] == [
        "public_u32_transform", "public_u32_rotate"
    ]
    assert manifest["two_stage_storage_buffers"] == 3
    assert manifest["two_stage_descriptor_sets"] == 2
    assert manifest["two_stage_descriptor_count"] == 4
    assert manifest["two_stage_dispatches"] == 2
    assert manifest["words_per_stage"] == 64
    assert manifest["shader_compiler"] == (
        "glslangValidator 14.3.0 --target-env vulkan1.0"
    )
    assert set(manifest["spirv"]) == {"1", "2"}

    for stage, (path, header, symbol, guard, source) in MODULES.items():
        info = manifest["spirv"][stage]
        assert info["words"] == (326 if stage == "1" else 363)
        assert info["bytes"] == info["words"] * 4

        content = header.read_text(encoding="utf-8")
        assert f"#ifndef {guard}" in content
        assert f"static const uint32_t {symbol}[]" in content
        assert f"Generated from {source} via" in content

        words = re.findall(r"UINT32_C \(0x([0-9a-f]{8})\)", content)
        assert len(words) == info["words"]

        binary = struct.pack(
            "<" + "I" * len(words),
            *(int(word, 16) for word in words),
        )

        assert hashlib.sha256(binary).hexdigest() == info["sha256"]
        assert struct.unpack_from("<I", binary, 0)[0] == 0x07230203
        assert struct.unpack_from("<I", binary, 4)[0] == 0x00010000
        assert struct.unpack_from("<I", binary, 16)[0] == 0

        if artifacts is not None:
            assert path.read_bytes() == binary
            assert sha256(path) == info["sha256"]

        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            shader = folder / "module.spv"
            generated = folder / "module.h"
            shader.write_bytes(binary)

            result = subprocess.run(
                [
                    sys.executable, "-B",
                    str(ROOT / "tools/generate-vulkan-backend-public-kernel.py"),
                    "--spirv", str(shader),
                    "--header", str(generated),
                    "--symbol", symbol,
                    "--guard", guard,
                    "--source", source,
                ],
                check=True, capture_output=True, text=True,
            )

            assert f"SPIRV_EMBEDDED bytes={len(binary)}" in result.stdout
            assert generated.read_bytes() == header.read_bytes()

    first = (ROOT / MODULES["1"][4]).read_text(encoding="utf-8")
    second = (ROOT / MODULES["2"][4]).read_text(encoding="utf-8")

    assert "(value * 1664525u + 1013904223u) ^ (index * 2654435769u)" in first
    assert "const uint previous = intermediate_buffer.words[index];" in second
    assert "(previous << 9u) | (previous >> 23u)" in second
    assert "(rotated + index * 2246822519u) ^ 0x6c8e9cf5u" in second

    initial = []
    intermediate = []
    output = []
    mask = 0xFFFFFFFF

    for i in range(64):
        x = 0x10203040 ^ ((i * 0x01010101) & mask)
        first_result = (
            (x * 1664525 + 1013904223) & mask
        ) ^ ((i * 2654435769) & mask)
        rotated = ((first_result << 9) & mask) | (first_result >> 23)
        final_result = (
            (rotated + i * 2246822519) & mask
        ) ^ 0x6c8e9cf5

        initial.append(x)
        intermediate.append(first_result)
        output.append(final_result)

    assert len(set(initial)) == 64
    assert len(set(intermediate)) == 64
    assert len(set(output)) == 64
    assert intermediate != output


def corrupt(
    lines: list[str],
    prefix: str,
    original: str,
    replacement: str,
) -> list[str]:
    """Falsify one real native Vulkan stage event without changing others."""
    candidates = [
        i for i, value in enumerate(lines)
        if value.startswith(prefix) and original in value
    ]
    assert candidates, (prefix, original)
    data = lines.copy()
    i = candidates[0]
    data[i] = data[i].replace(original, replacement, 1)
    assert data != lines

    return data


def verify_mutations(manifest: dict) -> None:
    """Reject false GPU completion, missing stage barrier and bad cleanup."""
    standalone = next(
        item for item in manifest["native_runs"]
        if item["device"] == "pixel-arm64"
        and item["requested_api"] == "1.0"
    )

    original = (ARCHIVE / standalone["file"]).read_text(
        encoding="utf-8"
    ).splitlines()

    mutations = (
        ("BACKEND_VULKAN_CHAIN_READY ", "shader_words_first=326", "shader_words_first=325"),
        ("BACKEND_VULKAN_CHAIN_READY ", "shader_words_second=363", "shader_words_second=362"),
        ("BACKEND_VULKAN_CHAIN_READY ", "descriptors=4", "descriptors=2"),
        ("BACKEND_VULKAN_CHAIN_READY ", "sets=2", "sets=1"),
        ("BACKEND_VULKAN_CHAIN_READY ", "buffers=3", "buffers=2"),
        ("BACKEND_VULKAN_CHAIN_READY ", "local=32", "local=64"),
        ("BACKEND_VULKAN_CHAIN_READY ", "groups=2", "groups=1"),
        ("BACKEND_VULKAN_CHAIN_READY ", "features=0", "features=1"),
        ("BACKEND_VULKAN_CHAIN_READY ", "extensions=0", "extensions=1"),
        ("BACKEND_VULKAN_CHAIN_DISPATCH_PASS ", "stage2=public_u32_rotate", "stage2=unknown"),
        ("BACKEND_VULKAN_CHAIN_DISPATCH_PASS ", "dispatches=2", "dispatches=1"),
        ("BACKEND_VULKAN_CHAIN_DISPATCH_PASS ", "verified_first=64", "verified_first=63"),
        ("BACKEND_VULKAN_CHAIN_DISPATCH_PASS ", "verified_second=64", "verified_second=63"),
        ("BACKEND_VULKAN_CHAIN_DISPATCH_PASS ", "barrier=shader_write_to_shader_read", "barrier=none"),
        ("BACKEND_VULKAN_CHAIN_DISPATCH_PASS ", "shaders=2", "shaders=1"),
        ("BACKEND_VULKAN_CHAIN_DISPATCH_PASS ", "password_inputs=0", "password_inputs=1"),
        ("BACKEND_VULKAN_CHAIN_DISPATCH_PASS ", "scheduled=0", "scheduled=1"),
        ("BACKEND_VULKAN_CHAIN_CLOSE ", "result=0", "result=-4"),
        ("BACKEND_VULKAN_CHAIN_CLOSE ", "released=1", "released=0"),
        ("DEVICE_CATALOG_SELECT ", "selections=1", "selections=2"),
        ("DEVICE_CATALOG_CLOSE ", "selections=1", "selections=0"),
        ("BACKEND_PUBLIC_CHAIN_TRIAL_PASS ", "pipelines=2", "pipelines=1"),
        ("BACKEND_PUBLIC_CHAIN_TRIAL_PASS ", "verified_words=128", "verified_words=64"),
        ("BACKEND_PUBLIC_CHAIN_DIAGNOSTIC_PASS ", "dispatches=4", "dispatches=3"),
        ("BACKEND_PUBLIC_CHAIN_DIAGNOSTIC_PASS ", "verified_words=256", "verified_words=128"),
    )

    rejected = 0

    for prefix, old, new in mutations:
        forged = corrupt(original, prefix, old, new)

        try:
            verify_standalone(forged, "arm64-v8a", "1.0", "Mali-G715")
        except AssertionError:
            rejected += 1
        else:
            raise AssertionError(
                f"Accepted forged public GPU intermediate result: {prefix}"
            )

    full_row = next(
        item for item in manifest["full_hashcat_info_runs"]
        if item["device"] == "pixel-arm64"
    )

    full = (ARCHIVE / full_row["file"]).read_text(
        encoding="utf-8"
    ).splitlines()

    forged_full_events = (
        ("BACKEND_VULKAN_KERNEL_DISPATCH_PASS ", "verified=64", "verified=63"),
        ("BACKEND_VULKAN_CHAIN_READY ", "shader_words_second=363", "shader_words_second=0"),
        ("BACKEND_VULKAN_CHAIN_DISPATCH_PASS ", "verified_second=64", "verified_second=0"),
        ("BACKEND_VULKAN_CHAIN_DISPATCH_PASS ", "barrier=shader_write_to_shader_read", "barrier=none"),
        ("BACKEND_VULKAN_CHAIN_CLOSE ", "result=0", "result=-4"),
        ("BACKEND_VULKAN_SESSION_CLOSE ", "wait_result=0", "wait_result=-4"),
        ("DEVICE_CATALOG_CLOSE ", "selections=3", "selections=2"),
        ("OpenCL Info:", "OpenCL Info:", "Missing OpenCL Info:"),
        ("HASHCAT_KERNEL_EXIT=0", "HASHCAT_KERNEL_EXIT=0", "HASHCAT_KERNEL_EXIT=1"),
    )

    for prefix, old, new in forged_full_events:
        falsified = corrupt(full, prefix, old, new)

        try:
            verify_full_hashcat(
                falsified, "pixel-arm64", "arm64-v8a", "Mali-G715"
            )
        except AssertionError:
            rejected += 1
        else:
            raise AssertionError(
                f"Accepted false full HASHCAT Vulkan dispatch: {prefix}"
            )

    out_of_order = original.copy()
    end_index = next(
        i for i, value in enumerate(out_of_order)
        if value.startswith("DEVICE_CATALOG_CLOSE ")
    )
    gpu_index = next(
        i for i, value in enumerate(out_of_order)
        if value.startswith("BACKEND_VULKAN_CHAIN_DISPATCH_PASS ")
    )

    out_of_order[end_index], out_of_order[gpu_index] = (
        out_of_order[gpu_index], out_of_order[end_index]
    )

    try:
        verify_standalone(out_of_order, "arm64-v8a", "1.0", "Mali-G715")
    except AssertionError:
        rejected += 1
    else:
        raise AssertionError("Accepted Vulkan instance destruction before GPU")

    assert rejected == len(mutations) + len(forged_full_events) + 1

    print(f"PASS {rejected} deliberately corrupted Vulkan chain records rejected")


def inspect_elf(
    readelf: Path,
    executable: Path,
    abi: str,
    require_vulkan: bool,
) -> None:
    """Verify Android API24 ELF and Vulkan core-1.0 Compute imports."""
    def read(*args: str) -> str:
        """Return read-only llvm-readelf output for one native executable."""
        return subprocess.run(
            [str(readelf), "-W", *args, str(executable)],
            check=True, capture_output=True, text=True,
        ).stdout

    header = read("--file-header")
    notes = read("--notes")
    dynamic = read("--dynamic")
    symbols = read("--dyn-syms")

    assert re.search(
        rf"Class:\s+{'ELF64' if abi == 'arm64' else 'ELF32'}\b",
        header,
    )
    assert re.search(
        rf"Machine:\s+{'AArch64' if abi == 'arm64' else 'ARM'}\b",
        header,
    )
    assert "NT_ANDROID_TYPE_IDENT" in notes
    assert "description data: 18 00 00 00" in notes

    if require_vulkan:
        assert "Shared library: [libvulkan.so]" in dynamic

        for symbol in (
            "vkGetInstanceProcAddr", "vkCreateDevice",
            "vkCreateBuffer", "vkAllocateMemory",
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
    """Audit 33 real GPU dispatches and complete source/ELF provenance."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--artifacts", type=Path,
        help="ARM64/ARMv7 native shader diagnostic binaries and SPIR-V",
    )
    parser.add_argument(
        "--full", type=Path,
        help="Complete Android HASHCAT binaries and module_00000.so",
    )
    parser.add_argument(
        "--readelf", type=Path,
        help="Android NDK llvm-readelf for ABI and Vulkan core inspection",
    )
    args = parser.parse_args()

    if args.readelf is not None and (
        args.artifacts is None or args.full is None
    ):
        parser.error("--readelf requires --artifacts and --full")

    manifest = json.loads(
        (ARCHIVE / "manifest.json").read_text(encoding="utf-8")
    )

    assert manifest["date"] == "2026-11-01"
    assert manifest["minimum_android_api"] == 24
    assert manifest["standalone_requested_apis"] == ["1.0", "1.1"]
    assert manifest["full_hashcat_requested_api"] == "1.0"
    assert manifest["hashcat_production_backend_code_executed"] is True
    assert manifest["hashcat_info_mode_only"] is True
    assert manifest["normal_opencl_device_discovery_preserved"] is True
    assert manifest["hashcat_hashing_scheduler_connected"] is False
    assert manifest["cryptographic_spirv_kernels_ported"] == 0
    assert manifest["password_or_hash_inputs_processed"] is False
    assert manifest["no_vulkan10_only_hardware_tested"] is True
    assert manifest["no_android_api24_os_hardware_tested"] is True

    assert len(manifest["source_sha256"]) == 26

    for source, digest in manifest["source_sha256"].items():
        assert normalized_source_sha256(ROOT / source) == digest, source

    assert len(manifest["native_runs"]) == 6
    assert len(manifest["full_hashcat_info_runs"]) == 3
    assert len(manifest["invalid_cli"]) == 3
    assert len(list(ARCHIVE.glob("*.log"))) == 12

    seen_native = set()
    native_dispatches = native_words = 0

    for row in manifest["native_runs"]:
        device = row["device"]
        assert device in DEVICES
        abi, _, gpu = DEVICES[device]
        key = (device, row["requested_api"])
        assert key not in seen_native
        seen_native.add(key)
        assert row["abi"] == abi
        assert row["requested_api"] in APIS
        assert row["native_exit_code"] == 0
        assert row["vk_instances"] == 2
        assert row["compute_dispatches"] == 4
        assert row["intermediate_words_verified"] == 128
        assert row["final_words_verified"] == 128
        assert row["cryptographic_shaders"] == 0

        path = ARCHIVE / row["file"]
        data = path.read_bytes()
        assert data.endswith(b"\n") and b"\r" not in data
        assert sha256(path) == row["sha256"]
        verify_standalone(
            path.read_text(encoding="utf-8").splitlines(),
            abi, row["requested_api"], gpu,
        )
        native_dispatches += row["compute_dispatches"]
        native_words += (
            row["intermediate_words_verified"]
            + row["final_words_verified"]
        )

    assert seen_native == {
        (device, api) for device in DEVICES for api in APIS
    }
    assert native_dispatches == 24
    assert native_words == 1536

    seen_full = set()
    full_dispatches = full_words = 0

    for row in manifest["full_hashcat_info_runs"]:
        device = row["device"]
        assert device in DEVICES and device not in seen_full
        seen_full.add(device)
        abi, _, gpu = DEVICES[device]
        assert row["abi"] == abi
        assert row["native_exit_code"] == 0
        assert row["requested_api"] == "1.0"
        assert row["single_stage_dispatches"] == 1
        assert row["two_stage_dispatches"] == 2
        assert row["intermediate_words_verified"] == 64
        assert row["final_words_verified"] == 64
        assert row["first_kernel_words_verified"] == 64
        assert row["normal_opencl_gpu_enumeration"] is True
        assert row["hashcat_hashing_scheduler_connected"] is False

        path = ARCHIVE / row["file"]
        data = path.read_bytes()
        assert data.endswith(b"\n") and b"\r" not in data
        assert sha256(path) == row["sha256"]

        verify_full_hashcat(
            path.read_text(encoding="utf-8").splitlines(),
            device, abi, gpu,
        )

        full_dispatches += (
            row["single_stage_dispatches"]
            + row["two_stage_dispatches"]
        )
        full_words += (
            row["first_kernel_words_verified"]
            + row["intermediate_words_verified"]
            + row["final_words_verified"]
        )

    assert seen_full == set(DEVICES)
    assert full_dispatches == 9
    assert full_words == 576

    seen_controls = set()

    for row in manifest["invalid_cli"]:
        device = row["device"]
        assert device in DEVICES and device not in seen_controls
        seen_controls.add(device)
        abi = DEVICES[device][0]
        assert row["abi"] == abi
        assert row["malformed_native_commands"] == 3
        path = ARCHIVE / row["file"]
        assert sha256(path) == row["sha256"]
        verify_cli(
            path.read_text(encoding="utf-8").splitlines(), abi
        )

    assert seen_controls == set(DEVICES)
    assert manifest["totals"] == {
        "physical_process_profiles": 3,
        "standalone_gpu_runs": 6,
        "full_hashcat_info_runs": 3,
        "real_spirv_vkcmd_dispatches": 33,
        "public_integer_words_verified": 2112,
        "invalid_native_commands": 9,
    }

    artifacts = (
        args.artifacts.resolve(strict=True)
        if args.artifacts is not None else None
    )
    full = (
        args.full.resolve(strict=True)
        if args.full is not None else None
    )

    verify_spirv(manifest, artifacts)

    if artifacts is not None:
        assert set(manifest["native_artifact_sha256"]) == {"arm64", "armv7"}

        for abi, hashes in manifest["native_artifact_sha256"].items():
            executable = artifacts / f"vulkan-backend-public-chain-{abi}"
            object_file = artifacts / f"hashcat-backend-vulkan-{abi}.o"
            assert sha256(executable) == hashes["diagnostic"]
            assert sha256(object_file) == hashes["production_backend_object"]

            if args.readelf is not None:
                inspect_elf(args.readelf, executable, abi, True)

        print("PASS exact native Android ARM64/ARMv7 GPU shader ELFs")

    if full is not None:
        assert set(manifest["full_hashcat_artifact_sha256"]) == {
            "arm64-v8a", "armeabi-v7a"
        }

        for abi, hashes in manifest["full_hashcat_artifact_sha256"].items():
            executable = full / abi / "hashcat"
            module = full / abi / "modules/module_00000.so"
            assert sha256(executable) == hashes["frontend"]
            assert sha256(module) == hashes["module_00000"]

            if args.readelf is not None:
                inspect_elf(
                    args.readelf, executable,
                    "arm64" if abi == "arm64-v8a" else "armv7", True,
                )
                inspect_elf(
                    args.readelf, module,
                    "arm64" if abi == "arm64-v8a" else "armv7", False,
                )

        print("PASS exact full Android HASHCAT ARM64/ARMv7 binaries and modules")

    if args.readelf is not None:
        print("PASS Android API24 ELF and real Vulkan Compute shader imports")

    verify_mutations(manifest)

    print("PASS 6 physical Android two-stage Vulkan GPU diagnostic processes")
    print("PASS 3 complete native HASHCAT -I GPU shader chain processes")
    print("PASS 33 real vkCmdDispatch and 2112 independently CPU-checked words")
    print("PASS two exact Vulkan SPIR-V 1.0 modules and generated C arrays")
    print("PASS ordinary OpenCL GPU discovery and no Vulkan hash scheduling")
    print("PASS 9 malformed native shader options rejected")


if __name__ == "__main__":
    main()
