#!/usr/bin/env python3
"""Verify program-declared Vulkan execution inside Android HASHCAT.

Reads the saved Pixel native/full-HASHCAT transcripts, source identities
and exact Android binaries. It confirms that each public program was
validated before VkDevice execution, the two-stage chain dispatched on
a real Mali-G715 GPU, normal OpenCL discovery remained present, and no
HASHCAT hashing scheduler or password input was connected.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = ROOT / "tests/fixtures/android-gpu/vulkan-backend-program-executor-20261101"
MANIFEST = ARCHIVE / "manifest.json"


def sha256(path: Path) -> str:
    """Return exact file SHA-256."""
    assert path.is_file(), path
    return hashlib.sha256(path.read_bytes()).hexdigest()


def normalized_sha256(path: Path) -> str:
    """Hash source with CRLF normalized to LF."""
    return hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")
    ).hexdigest()


def archived_source_sha256(source: str) -> str:
    """Use current source precommit and committed manifest provenance later."""
    revision = subprocess.run(
        ["git", "log", "-1", "--format=%H", "--", MANIFEST.relative_to(ROOT).as_posix()],
        cwd=ROOT, check=True, capture_output=True, text=True,
    ).stdout.strip()

    if len(revision) != 40:
        return normalized_sha256(ROOT / source)

    committed = subprocess.run(
        ["git", "show", f"{revision}:{MANIFEST.relative_to(ROOT).as_posix()}"],
        cwd=ROOT, check=True, capture_output=True,
    ).stdout

    if committed != MANIFEST.read_bytes():
        return normalized_sha256(ROOT / source)

    source_bytes = subprocess.run(
        ["git", "show", f"{revision}:{source}"],
        cwd=ROOT, check=True, capture_output=True,
    ).stdout

    return hashlib.sha256(
        source_bytes.replace(b"\r\n", b"\n")
    ).hexdigest()


def verify_native(lines: list[str], api: str) -> None:
    """Verify two exact graph validations and two real two-stage trials."""
    assert lines[:4] == [
        "TEST=vulkan-backend-program-executor",
        "ABI=arm64-v8a",
        f"API={api}",
        "ADB_EXIT_CODE=0",
    ]
    events = lines[4:]
    assert events[-1] == (
        f"BACKEND_PUBLIC_CHAIN_DIAGNOSTIC_PASS api={api}"
        " trials=2 dispatches=4 descriptors=4 storage_buffers=3"
        " verified_words=256 shaders=2 password_inputs=0 scheduled=0"
    )

    expected_accept = (
        "BACKEND_VULKAN_PROGRAM_ACCEPT stages=2 buffers=3"
        " bytes=256 descriptors_per_stage=2"
    )
    assert events.count(expected_accept) == 2

    ready = [
        x for x in events if x.startswith("BACKEND_VULKAN_CHAIN_READY ")
    ]
    dispatch = [
        x for x in events
        if x.startswith("BACKEND_VULKAN_CHAIN_DISPATCH_PASS ")
    ]
    assert len(ready) == len(dispatch) == 2

    for line in ready:
        assert re.fullmatch(
            rf"BACKEND_VULKAN_CHAIN_READY index=0 queue_family=\d+"
            rf" api={re.escape(api)} shader_words_first=326"
            r" shader_words_second=363 descriptors=4 sets=2 buffers=3"
            r" local=32 groups=2 program_validated=1"
            r" features=0 extensions=0 scheduled=0",
            line,
        )

    for line in dispatch:
        assert re.fullmatch(
            r"BACKEND_VULKAN_CHAIN_DISPATCH_PASS"
            r" stage1=public_u32_transform stage2=public_u32_rotate"
            r" words=64 local=32 groups=2 dispatches=2"
            r" verified_first=64 verified_second=64"
            r" coherent_input=[01] coherent_intermediate=[01]"
            r" coherent_output=[01] barrier=shader_write_to_shader_read"
            r" shaders=2 password_inputs=0 scheduled=0",
            line,
        )

    for trial in (0, 1):
        assert (
            f"BACKEND_PUBLIC_CHAIN_TRIAL_PASS trial={trial}"
            " descriptors=4 pipelines=2 dispatches=2"
            " verified_words=128 scheduled=0"
        ) in events

    joined = "\n".join(events)
    assert "scheduled=1" not in joined
    assert "password_inputs=1" not in joined


def verify_full(lines: list[str]) -> None:
    """Verify declaration validation, GPU work and normal OpenCL in HASHCAT -I."""
    assert lines[:5] == [
        "TEST=vulkan-hashcat-program-executor",
        "DEVICE=pixel-arm64",
        "ABI=arm64-v8a",
        "COMMAND=-I",
        "ADB_EXIT_CODE=0",
    ]
    events = lines[5:]
    assert events[-1] == "HASHCAT_KERNEL_EXIT=0"

    assert events.count(
        "BACKEND_VULKAN_PROGRAM_ACCEPT stages=1 buffers=2"
        " bytes=256 descriptors_per_stage=2"
    ) == 1
    assert events.count(
        "BACKEND_VULKAN_PROGRAM_ACCEPT stages=2 buffers=3"
        " bytes=256 descriptors_per_stage=2"
    ) == 1

    kernel_ready = [
        x for x in events if x.startswith("BACKEND_VULKAN_KERNEL_READY ")
    ]
    kernel_dispatch = [
        x for x in events
        if x.startswith("BACKEND_VULKAN_KERNEL_DISPATCH_PASS ")
    ]
    chain_ready = [
        x for x in events if x.startswith("BACKEND_VULKAN_CHAIN_READY ")
    ]
    chain_dispatch = [
        x for x in events
        if x.startswith("BACKEND_VULKAN_CHAIN_DISPATCH_PASS ")
    ]

    assert len(kernel_ready) == len(kernel_dispatch) == 1
    assert len(chain_ready) == len(chain_dispatch) == 1

    assert re.fullmatch(
        r"BACKEND_VULKAN_KERNEL_READY index=0 queue_family=\d+"
        r" api=1\.0 shader_words=326 descriptors=2"
        r" local=32 groups=2 program_validated=1"
        r" features=0 extensions=0 scheduled=0",
        kernel_ready[0],
    )
    assert re.fullmatch(
        r"BACKEND_VULKAN_KERNEL_DISPATCH_PASS"
        r" kernel=public_u32_transform words=64 local=32 groups=2"
        r" verified=64 coherent_input=[01] coherent_output=[01]"
        r" shaders=1 password_inputs=0 scheduled=0",
        kernel_dispatch[0],
    )
    assert re.fullmatch(
        r"BACKEND_VULKAN_CHAIN_READY index=0 queue_family=\d+"
        r" api=1\.0 shader_words_first=326 shader_words_second=363"
        r" descriptors=4 sets=2 buffers=3 local=32 groups=2"
        r" program_validated=1 features=0 extensions=0 scheduled=0",
        chain_ready[0],
    )
    assert re.fullmatch(
        r"BACKEND_VULKAN_CHAIN_DISPATCH_PASS"
        r" stage1=public_u32_transform stage2=public_u32_rotate"
        r" words=64 local=32 groups=2 dispatches=2"
        r" verified_first=64 verified_second=64"
        r" coherent_input=[01] coherent_intermediate=[01]"
        r" coherent_output=[01] barrier=shader_write_to_shader_read"
        r" shaders=2 password_inputs=0 scheduled=0",
        chain_dispatch[0],
    )

    joined = "\n".join(events)
    for required in (
        "BACKEND_VULKAN_SESSION_READY index=0",
        "BACKEND_VULKAN_KERNEL_READY index=0",
        "program_validated=1",
        "BACKEND_VULKAN_KERNEL_DISPATCH_PASS kernel=public_u32_transform",
        "BACKEND_VULKAN_CHAIN_READY index=0",
        "BACKEND_VULKAN_CHAIN_DISPATCH_PASS stage1=public_u32_transform stage2=public_u32_rotate",
        "barrier=shader_write_to_shader_read",
        "OpenCL Info:",
        "Backend Device ID #01",
        "Name...........: Mali-G715 r0p0",
        "DEVICE_CATALOG_CLOSE devices=1 selections=3",
    ):
        assert required in joined, required

    assert "password_inputs=1" not in joined
    assert "scheduled=1" not in joined


def inspect_elf(readelf: Path, executable: Path, abi: str, full: bool) -> None:
    """Require Android architecture and Vulkan linkage only for GPU executors."""
    def read(*args: str) -> str:
        return subprocess.run(
            [str(readelf), "-W", *args, str(executable)],
            check=True, capture_output=True, text=True, encoding="utf-8",
        ).stdout

    header = read("--file-header")
    notes = read("--notes")
    dynamic = read("--dynamic")
    symbols = read("--dyn-syms")

    expected_class = "ELF64" if "arm64" in abi else "ELF32"
    expected_machine = "AArch64" if "arm64" in abi else "ARM"

    assert re.search(rf"Class:\s+{expected_class}\b", header)
    assert re.search(rf"Machine:\s+{expected_machine}\b", header)
    assert "NT_ANDROID_TYPE_IDENT" in notes
    assert "description data: 18 00 00 00" in notes
    assert "Shared library: [libvulkan.so]" in dynamic

    for symbol in (
        "vkCreateShaderModule", "vkCreateComputePipelines",
        "vkCmdDispatch", "vkCmdPipelineBarrier",
        "vkQueueSubmit", "vkWaitForFences",
    ):
        assert re.search(rf"UND\s+{symbol}\b", symbols), (full, symbol)


def verify_mutations(native: list[str], full: list[str]) -> None:
    """Reject fabricated validation, dispatch, scheduling and OpenCL claims."""
    cases = [
        (native, "program_validated=1", "program_validated=0", "1.0"),
        (native, "verified_first=64", "verified_first=63", "1.0"),
        (native, "barrier=shader_write_to_shader_read", "barrier=none", "1.0"),
        (native, "password_inputs=0", "password_inputs=1", "1.0"),
        (native, "scheduled=0", "scheduled=1", "1.0"),
        (full, "stages=1 buffers=2", "stages=1 buffers=3", None),
        (full, "stages=2 buffers=3", "stages=2 buffers=2", None),
        (full, "program_validated=1", "program_validated=0", None),
        (full, "verified=64", "verified=63", None),
        (full, "verified_second=64", "verified_second=63", None),
        (full, "barrier=shader_write_to_shader_read", "barrier=none", None),
        (full, "Backend Device ID #01", "Backend Device ID #02", None),
        (full, "Mali-G715 r0p0", "Not-Mali", None),
        (full, "HASHCAT_KERNEL_EXIT=0", "HASHCAT_KERNEL_EXIT=1", None),
    ]

    rejected = 0

    for base, old, new, api in cases:
        changed = base.copy()
        index = next(i for i, x in enumerate(changed) if old in x)
        changed[index] = changed[index].replace(old, new, 1)

        try:
            if api is None:
                verify_full(changed)
            else:
                verify_native(changed, api)
        except AssertionError:
            rejected += 1
        else:
            raise AssertionError(f"Accepted falsified executor evidence: {old}")

    assert rejected == len(cases)
    print(f"PASS {rejected} falsified program-executor records rejected")


def main() -> None:
    """Verify current-revision Pixel execution and exact Android artifacts."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path)
    parser.add_argument("--full", type=Path)
    parser.add_argument("--readelf", type=Path)
    args = parser.parse_args()

    if args.readelf is not None and (args.native is None or args.full is None):
        parser.error("--readelf requires --native and --full")

    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))

    assert manifest["minimum_android_api"] == 24
    assert manifest["physical_gpu_profiles"] == ["pixel-arm64"]
    assert manifest["armv7_compile_only"] is True
    assert manifest["program_graph_connected_to_executor"] is True
    assert manifest["hashcat_info_mode_only"] is True
    assert manifest["hashcat_hashing_scheduler_connected"] is False
    assert manifest["cryptographic_spirv_kernels_ported"] == 0
    assert manifest["password_or_hash_inputs_processed"] is False
    assert len(manifest["source_sha256"]) == 23

    for source, digest in manifest["source_sha256"].items():
        assert archived_source_sha256(source) == digest, source

    assert len(manifest["native_runs"]) == 2
    for row in manifest["native_runs"]:
        path = ARCHIVE / row["file"]
        assert sha256(path) == row["sha256"]
        verify_native(path.read_text(encoding="utf-8").splitlines(), row["requested_api"])

    full_row = manifest["full_hashcat_info_run"]
    full_path = ARCHIVE / full_row["file"]
    assert sha256(full_path) == full_row["sha256"]
    verify_full(full_path.read_text(encoding="utf-8").splitlines())

    invalid = manifest["invalid_cli"]
    invalid_path = ARCHIVE / invalid["file"]
    assert sha256(invalid_path) == invalid["sha256"]
    assert invalid_path.read_text(encoding="utf-8").splitlines() == [
        "TEST=vulkan-backend-program-executor-invalid-inputs",
        "ABI=arm64-v8a",
        "INVALID_CASES=3",
        "INVALID_CASE=missing-arguments exit=2",
        "INVALID_CASE=unsupported-api exit=2",
        "INVALID_CASE=wrong-flag exit=2",
    ]

    assert manifest["totals"] == {
        "real_android_processes": 3,
        "program_validations_against_real_gpu_limits": 6,
        "real_spirv_vkcmd_dispatches": 11,
        "public_integer_words_verified": 704,
        "invalid_native_commands": 3,
    }

    if args.native is not None:
        native_dir = args.native.resolve(strict=True)
        for abi, items in manifest["native_artifact_sha256"].items():
            diagnostic = native_dir / f"vulkan-backend-public-chain-{abi}"
            assert sha256(diagnostic) == items["diagnostic"]
            assert sha256(native_dir / f"hashcat-backend-vulkan-{abi}.o") == items["backend_object"]
            if args.readelf is not None:
                inspect_elf(args.readelf.resolve(strict=True), diagnostic, abi, False)

    if args.full is not None:
        full_dir = args.full.resolve(strict=True)
        for abi, items in manifest["full_hashcat_artifact_sha256"].items():
            frontend = full_dir / abi / "hashcat"
            assert sha256(frontend) == items["frontend"]
            assert sha256(full_dir / abi / "modules/module_00000.so") == items["module_00000"]
            if args.readelf is not None:
                inspect_elf(args.readelf.resolve(strict=True), frontend, abi, True)

    verify_mutations(
        (ARCHIVE / manifest["native_runs"][0]["file"]).read_text(
            encoding="utf-8"
        ).splitlines(),
        full_path.read_text(encoding="utf-8").splitlines(),
    )

    print("PASS 6 graph validations against real Mali-G715 limits")
    print("PASS 11 real Vulkan dispatches and 704 public GPU/CPU words")
    print("PASS full hashcat -I program executor plus normal OpenCL discovery")
    print("PASS ARM64/ARMv7 native and full HASHCAT artifact identities")


if __name__ == "__main__":
    main()
