#!/usr/bin/env python3
"""Verify the host-only Vulkan program graph contract and Android ELFs.

The validator is a production-adjacent declaration layer that performs
no Vulkan driver call. It checks two embedded public SPIR-V 1.0 modules,
sequential storage-buffer dependencies and explicit physical-device
limits supplied as plain structs. Verification reads saved Android
output, exact source identities and ARM64/ARMv7 ELF metadata only.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = ROOT / "tests/fixtures/android-gpu/vulkan-backend-program-20261101"
MANIFEST = ARCHIVE / "manifest.json"

EXPECTED_CASES = {
    "two_stage_valid": 0,
    "one_stage_valid": 0,
    "null_program": -3,
    "null_limits": -3,
    "broken_dependency": -3,
    "source_destination_alias": -3,
    "reused_destination": -3,
    "zero_group": -3,
    "zero_local": -3,
    "group_limit": -8,
    "invocation_limit": -8,
    "storage_range": -8,
    "descriptor_limit": -8,
    "spirv_magic": -3,
    "spirv_version": -3,
    "empty_name": -3,
}


def sha256(path: Path) -> str:
    """Return SHA-256 over the complete bytes of one existing file."""
    assert path.is_file(), path
    return hashlib.sha256(path.read_bytes()).hexdigest()


def normalized_sha256(path: Path) -> str:
    """Hash source bytes after the repository evidence CRLF-to-LF rule."""
    return hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")
    ).hexdigest()


def archived_source_sha256(source: str) -> str:
    """Read source from the commit owning the checked-out manifest when possible.

    Before the new manifest is first committed, validates the current
    worktree source directly. Once committed, historical verification
    uses the source blob from the last commit containing the exact same
    manifest bytes, so later unrelated source evolution does not rewrite
    or invalidate preserved evidence.
    """
    revision = subprocess.run(
        ["git", "log", "-1", "--format=%H", "--", MANIFEST.relative_to(ROOT).as_posix()],
        cwd=ROOT, check=True, capture_output=True, text=True,
    ).stdout.strip()

    if len(revision) != 40:
        return normalized_sha256(ROOT / source)

    committed_manifest = subprocess.run(
        ["git", "show", f"{revision}:{MANIFEST.relative_to(ROOT).as_posix()}"],
        cwd=ROOT, check=True, capture_output=True,
    ).stdout

    if committed_manifest != MANIFEST.read_bytes():
        return normalized_sha256(ROOT / source)

    content = subprocess.run(
        ["git", "show", f"{revision}:{source}"],
        cwd=ROOT, check=True, capture_output=True,
    ).stdout

    return hashlib.sha256(content.replace(b"\r\n", b"\n")).hexdigest()


def verify_log(lines: list[str]) -> None:
    """Require the exact 16 accepted/rejected declaration outcomes."""
    assert lines[:5] == [
        "TEST=vulkan-backend-program-graph",
        "ABI=arm64-v8a",
        "DRIVER_CALLS=0",
        "GPU_DISPATCHES=0",
        "ADB_EXIT_CODE=0",
    ]

    events = lines[5:]
    assert events[-1] == (
        "BACKEND_VULKAN_PROGRAM_DIAGNOSTIC_PASS"
        " accepted=2 structural_rejects=10 limit_rejects=4"
        " driver_calls=0 gpu_dispatches=0"
        " password_inputs=0 scheduled=0"
    )

    case_lines = [
        line for line in events
        if line.startswith("BACKEND_VULKAN_PROGRAM_CASE_PASS ")
    ]
    assert len(case_lines) == len(EXPECTED_CASES)

    seen = {}

    for line in case_lines:
        match = re.fullmatch(
            r"BACKEND_VULKAN_PROGRAM_CASE_PASS"
            r" case=([a-z0-9_]+) result=(-?\d+)",
            line,
        )
        assert match is not None, line
        name = match.group(1)
        assert name in EXPECTED_CASES and name not in seen
        seen[name] = int(match.group(2))

    assert seen == EXPECTED_CASES
    assert sum(
        line.startswith("BACKEND_VULKAN_PROGRAM_ACCEPT ")
        for line in events
    ) == 2
    assert sum(
        line.startswith("BACKEND_VULKAN_PROGRAM_REJECT_")
        for line in events
    ) == 12

    joined = "\n".join(events)
    for forbidden in (
        "vkCreate", "vkQueue", "vkCmd", "DEVICE_CATALOG",
        "BACKEND_VULKAN_KERNEL_", "OpenCL Info:", "password=",
    ):
        assert forbidden not in joined, forbidden


def inspect_elf(readelf: Path, executable: Path, abi: str) -> None:
    """Require Android API24 architecture and no Vulkan loader dependency."""
    def read(*options: str) -> str:
        return subprocess.run(
            [str(readelf), "-W", *options, str(executable)],
            check=True, capture_output=True, text=True, encoding="utf-8",
        ).stdout

    header = read("--file-header")
    notes = read("--notes")
    dynamic = read("--dynamic")
    symbols = read("--dyn-syms")

    expected_class = "ELF64" if abi == "arm64" else "ELF32"
    expected_machine = "AArch64" if abi == "arm64" else "ARM"

    assert re.search(rf"Class:\s+{expected_class}\b", header)
    assert re.search(rf"Machine:\s+{expected_machine}\b", header)
    assert "NT_ANDROID_TYPE_IDENT" in notes
    assert "description data: 18 00 00 00" in notes
    assert "libvulkan.so" not in dynamic
    assert re.search(r"UND\s+vk[A-Z]", symbols) is None


def verify_mutations(lines: list[str]) -> None:
    """Reject fabricated GPU activity, statuses and graph acceptance."""
    mutations = (
        ("DRIVER_CALLS=0", "DRIVER_CALLS=1"),
        ("GPU_DISPATCHES=0", "GPU_DISPATCHES=1"),
        ("ADB_EXIT_CODE=0", "ADB_EXIT_CODE=1"),
        ("case=broken_dependency result=-3", "case=broken_dependency result=0"),
        ("case=group_limit result=-8", "case=group_limit result=0"),
        ("case=spirv_version result=-3", "case=spirv_version result=0"),
        ("accepted=2", "accepted=3"),
        ("structural_rejects=10", "structural_rejects=9"),
        ("limit_rejects=4", "limit_rejects=3"),
        ("driver_calls=0", "driver_calls=1"),
        ("gpu_dispatches=0", "gpu_dispatches=1"),
        ("password_inputs=0", "password_inputs=1"),
    )

    rejected = 0

    for old, new in mutations:
        changed = lines.copy()
        index = next(i for i, line in enumerate(changed) if old in line)
        changed[index] = changed[index].replace(old, new, 1)

        try:
            verify_log(changed)
        except AssertionError:
            rejected += 1
        else:
            raise AssertionError(f"Accepted falsified program graph record: {old}")

    assert rejected == len(mutations)
    print(f"PASS {rejected} deliberately falsified program-graph records rejected")


def main() -> None:
    """Verify source identities, Android process log and both ABI binaries."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--artifacts", type=Path,
        help="Directory with exact Android ARM64/ARMv7 validator executables",
    )
    parser.add_argument(
        "--readelf", type=Path,
        help="Android NDK llvm-readelf for read-only ELF inspection",
    )
    args = parser.parse_args()

    if args.readelf is not None and args.artifacts is None:
        parser.error("--readelf requires --artifacts")

    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))

    assert manifest["date"] == "2026-11-01"
    assert manifest["minimum_android_api"] == 24
    assert manifest["vulkan_loader_linked"] is False
    assert manifest["vulkan_driver_calls"] == 0
    assert manifest["gpu_dispatches"] == 0
    assert manifest["hashcat_hashing_scheduler_connected"] is False
    assert manifest["password_or_hash_inputs_processed"] is False
    assert manifest["accepted_cases"] == 2
    assert manifest["rejected_cases"] == 14
    assert len(manifest["source_sha256"]) == 9

    for source, digest in manifest["source_sha256"].items():
        assert archived_source_sha256(source) == digest, source

    log_path = ARCHIVE / manifest["android_run"]["file"]
    data = log_path.read_bytes()
    assert data.endswith(b"\n") and b"\r" not in data
    assert sha256(log_path) == manifest["android_run"]["sha256"]
    verify_log(log_path.read_text(encoding="utf-8").splitlines())

    if args.artifacts is not None:
        directory = args.artifacts.resolve(strict=True)
        assert set(manifest["native_elf_sha256"]) == {"arm64", "armv7"}

        for abi, digest in manifest["native_elf_sha256"].items():
            executable = directory / f"vulkan-backend-program-{abi}"
            assert sha256(executable) == digest, abi

            if args.readelf is not None:
                inspect_elf(args.readelf.resolve(strict=True), executable, abi)

        print("PASS exact Android ARM64/ARMv7 program-validator ELF SHA-256")

        if args.readelf is not None:
            print("PASS Android API24 ELF metadata and no libvulkan dependency")

    verify_mutations(log_path.read_text(encoding="utf-8").splitlines())

    print("PASS 2 accepted + 14 rejected explicit Vulkan program declarations")
    print("PASS zero Vulkan driver calls, GPU dispatches and HASHCAT hash scheduling")


if __name__ == "__main__":
    main()
