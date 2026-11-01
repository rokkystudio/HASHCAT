#!/usr/bin/env python3
"""Independently verify real Android Vulkan 1.0 Compute queue sessions.

Checks every native Vulkan instance, physical/logical device, empty
queue submission, fence completion and resource teardown. Audits full
HASHCAT -I and standalone backend_ctx diagnostics on three real Android
device/ABI configurations. No ADB, Vulkan, GPU kernel or hash execution
occurs during verification.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = ROOT / "tests/fixtures/android-gpu/vulkan-backend-session-20261101"

OPEN = re.compile(
    r"DEVICE_CATALOG_OPEN requested_api=(1\.[01]) "
    r"instance_api=(1\.[01]) devices=(\d+) extensions=0 layers=0"
)
ACCEPT = re.compile(
    r"ACCEPT_COMPUTE index=0 queue_family=(\d+) local_max=(\d+) "
    r"invocations_max=(\d+) groups_max=(\d+) shared_max=(\d+) "
    r"storage_range_max=(\d+) descriptor_max=(\d+) non_coherent_atom=(\d+)"
)
REJECT = re.compile(
    r"REJECT_COMPUTE dimension=0 local=4294967295 "
    r"max_local=(\d+) groups=1 max_groups=(\d+)"
)
CANDIDATE = re.compile(
    r"VULKAN_CANDIDATE index=0 name=(.+) vendor=0x([0-9a-f]+) "
    r"device=0x([0-9a-f]+) api=1\.(\d+)\.(\d+) driver=0x([0-9a-f]+)"
)


def sha256(path: Path) -> str:
    """Calculate the SHA-256 digest over all exact file bytes."""
    assert path.is_file(), path

    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_sha256(path: Path) -> str:
    """Return the manifest's canonical LF-normalized source digest."""
    return hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")
    ).hexdigest()


def exact(lines: list[str], item: str) -> None:
    """Require exactly one unmodified native log event."""
    assert lines.count(item) == 1, item


def verify_diagnostic_body(lines: list[str], api: str) -> None:
    """Validate two real Vulkan instances and their Compute queue sessions.

    Requires a successful native fence after an empty primary command
    buffer submission, explicit workload capability checks, rejected
    invalid ownership operations and VkDevice cleanup before VkInstance.
    The function does not infer GPU feature support from its vendor name.
    """
    assert api in ("1.0", "1.1")
    assert len(lines) == 35, len(lines)

    exact(
        lines,
        f"BACKEND_SESSION_DIAGNOSTIC_PASS api={api} trials=2 "
        "invalid=8 owner_rejects=4 unsupported=2 "
        "empty_submissions=2 scheduled=0",
    )
    assert lines[-1].startswith("BACKEND_SESSION_DIAGNOSTIC_PASS ")

    for trial in (0, 1):
        segment = lines[trial * 17:(trial + 1) * 17]

        loader = re.fullmatch(
            r"VULKAN_LOADER_VERSION=1\.(\d+)\.(\d+) "
            r"source=(vkEnumerateInstanceVersion|core10_missing_entry_point)",
            segment[0],
        )
        assert loader is not None
        minor = int(loader.group(1))
        assert minor >= int(api.split(".")[1])

        assert segment[1] == (
            f"SELECT_INSTANCE_API={api} loader_minor={minor} cap=1.1"
        )

        opened = OPEN.fullmatch(segment[2])
        assert opened is not None
        assert opened.group(1) == opened.group(2) == api
        device_count = int(opened.group(3))
        assert device_count > 0

        assert segment[3] == (
            f"BACKEND_VULKAN_CATALOG_READY devices={device_count} "
            f"requested_api={api} scheduled=0"
        )

        selected_api = (
            rf"SELECT_DEVICE_API={re.escape(api)} "
            r"reported_minor=(\d+) instance_minor=(\d+) cap=1\.1"
        )

        selected_first = re.fullmatch(selected_api, segment[4])
        selected_second = re.fullmatch(selected_api, segment[8])

        assert selected_first is not None
        assert selected_second is not None
        assert segment[4] == segment[8]
        assert int(selected_first.group(1)) >= int(api.split(".")[1])
        assert int(selected_first.group(2)) == int(api.split(".")[1])

        candidate = CANDIDATE.fullmatch(segment[5])
        assert candidate is not None
        assert candidate.group(1)
        assert segment[5] == segment[9]
        assert int(candidate.group(4)) >= int(api.split(".")[1])

        reject = REJECT.fullmatch(segment[6])
        assert reject is not None
        assert int(reject.group(1)) > 0
        assert int(reject.group(1)) < 4294967295
        assert int(reject.group(2)) > 0

        assert segment[7] == (
            f"BACKEND_SESSION_INVALID_CONTROLS trial={trial} "
            "invalid=4 unsupported=1 active=0"
        )

        accepted = ACCEPT.fullmatch(segment[10])
        assert accepted is not None
        queue_family = int(accepted.group(1))
        assert queue_family >= 0
        assert int(accepted.group(2)) == int(reject.group(1))
        assert int(accepted.group(4)) == int(reject.group(2))
        assert int(accepted.group(3)) >= 1
        assert int(accepted.group(6)) >= 4
        assert int(accepted.group(7)) >= 1
        cache_atom = int(accepted.group(8))
        assert cache_atom > 0 and cache_atom & (cache_atom - 1) == 0

        assert segment[11] == (
            f"DEVICE_CATALOG_SELECT index=0 queue_family={queue_family} "
            f"effective_api={api} selections=1"
        )
        assert segment[12] == (
            f"BACKEND_VULKAN_SESSION_READY index=0 "
            f"queue_family={queue_family} effective_api={api} "
            "command_buffers=1 empty_submissions=1 "
            "features=0 extensions=0 scheduled=0"
        )
        assert segment[13] == (
            f"BACKEND_SESSION_OWNER_CONTROLS trial={trial} "
            "rejects=2 active=1"
        )
        assert segment[14] == (
            f"BACKEND_VULKAN_SESSION_CLOSE index=0 "
            f"queue_family={queue_family} "
            "empty_submissions=1 wait_result=0"
        )
        assert segment[15] == (
            f"DEVICE_CATALOG_CLOSE devices={device_count} selections=1"
        )
        assert segment[16] == (
            f"BACKEND_SESSION_TRIAL_PASS trial={trial} "
            "invalid=4 owner_rejects=2 empty_submissions=1 scheduled=0"
        )

    assert not any(
        line.startswith(("MULTI_SUBMIT", "KERN_RUN", "JOB_PORT_SUBMIT"))
        for line in lines
    )


def verify_full_frontend(
    lines: list[str],
    device: str,
    abi: str,
    opencl_vendor: str,
    gpu_name: str,
    loader_minor: int,
) -> None:
    """Validate one complete native hashcat -I GPU device session.

    Requires the opt-in VkInstance and single VkDevice to be opened,
    a completed empty fence submission, ordinary OpenCL GPU discovery,
    and dependent logical-device destruction before instance teardown.
    Verifies that the Vulkan hashing scheduler remains disabled.
    """
    assert lines[:5] == [
        "TEST=vulkan-hashcat-compute-session",
        f"DEVICE={device}",
        f"ABI={abi}",
        "COMMAND=-I",
        "ADB_EXIT_CODE=0",
    ]
    body = lines[5:]
    assert body[0] == "hashcat (v7.1.2) starting in backend information mode"
    assert body[-1] == "HASHCAT_SESSION_EXIT=0"

    loader = [
        line for line in body
        if line.startswith("VULKAN_LOADER_VERSION=")
    ]
    assert len(loader) == 1
    assert re.fullmatch(
        rf"VULKAN_LOADER_VERSION=1\.{loader_minor}\.\d+ "
        r"source=vkEnumerateInstanceVersion",
        loader[0],
    )

    expected = [
        f"SELECT_INSTANCE_API=1.0 loader_minor={loader_minor} cap=1.1",
        "DEVICE_CATALOG_OPEN requested_api=1.0 "
        "instance_api=1.0 devices=1 extensions=0 layers=0",
        "BACKEND_VULKAN_CATALOG_READY devices=1 requested_api=1.0 scheduled=0",
        "Vulkan 1.0 discovery found 1 physical device(s); "
        "Vulkan Compute scheduling is not enabled.",
        "DEVICE_CATALOG_SELECT index=0 queue_family=0 "
        "effective_api=1.0 selections=1",
        "BACKEND_VULKAN_SESSION_READY index=0 queue_family=0 "
        "effective_api=1.0 command_buffers=1 empty_submissions=1 "
        "features=0 extensions=0 scheduled=0",
        "Vulkan 1.0 Compute device session verified with an empty queue "
        "submission; Vulkan hashing remains disabled.",
        "OpenCL Info:",
        "OpenCL Platform ID #1",
        "  Backend Device ID #01",
        "    Type...........: GPU",
        f"  Vendor..: {opencl_vendor}",
        f"    Name...........: {gpu_name}",
        "BACKEND_VULKAN_SESSION_CLOSE index=0 queue_family=0 "
        "empty_submissions=1 wait_result=0",
        "DEVICE_CATALOG_CLOSE devices=1 selections=1",
    ]

    for line in expected:
        exact(body, line)

    ordered = [body.index(line) for line in (
        expected[0], expected[1], expected[2], expected[3],
        expected[4], expected[5], expected[6],
        expected[7], expected[13], expected[14],
    )]
    assert ordered == sorted(ordered), ordered

    assert sum(line.startswith("DEVICE_CATALOG_OPEN ") for line in body) == 1
    assert sum(line.startswith("DEVICE_CATALOG_CLOSE ") for line in body) == 1
    assert sum(line.startswith("BACKEND_VULKAN_SESSION_READY ") for line in body) == 1
    assert sum(line.startswith("BACKEND_VULKAN_SESSION_CLOSE ") for line in body) == 1
    assert not any(
        line.startswith(("MULTI_SUBMIT", "JOB_PORT_SUBMIT", "KERN_RUN"))
        for line in body
    )


def verify_cli(lines: list[str], abi: str) -> None:
    """Verify the three intentionally malformed native Android requests."""
    assert lines == [
        "TEST=vulkan-backend-session-invalid-inputs",
        f"ABI={abi}",
        "INVALID_CASES=3",
        "INVALID_CASE=missing-minimum exit=2",
        "INVALID_CASE=unsupported-api exit=2",
        "INVALID_CASE=extra-argument exit=2",
    ]


def change_event(
    lines: list[str],
    prefix: str,
    needle: str,
    substitution: str,
) -> list[str]:
    """Return a complete altered log with exactly one modified native event."""
    changed = lines.copy()
    matches = [
        i for i, line in enumerate(changed)
        if line.startswith(prefix) and needle in line
    ]
    assert matches, (prefix, needle)
    index = matches[0]
    changed[index] = changed[index].replace(needle, substitution, 1)
    assert changed != lines

    return changed


def verify_mutations(manifest: dict) -> None:
    """Demonstrate refusal of false GPU submissions and cleanup evidence."""
    record = next(
        row for row in manifest["device_runs"]
        if row["device"] == "redmagic-arm64" and row["api"] == "1.0"
    )
    original = (ARCHIVE / record["file"]).read_text(
        encoding="utf-8"
    ).splitlines()[4:]

    alterations = (
        ("BACKEND_VULKAN_SESSION_READY ", "empty_submissions=1", "empty_submissions=0"),
        ("BACKEND_VULKAN_SESSION_CLOSE ", "empty_submissions=1", "empty_submissions=0"),
        ("BACKEND_VULKAN_SESSION_CLOSE ", "wait_result=0", "wait_result=-4"),
        ("BACKEND_SESSION_OWNER_CONTROLS ", "rejects=2", "rejects=1"),
        ("BACKEND_SESSION_INVALID_CONTROLS ", "invalid=4", "invalid=3"),
        ("REJECT_COMPUTE ", "local=4294967295", "local=1"),
        ("DEVICE_CATALOG_CLOSE ", "selections=1", "selections=0"),
        ("BACKEND_SESSION_DIAGNOSTIC_PASS ", "scheduled=0", "scheduled=1"),
    )

    for prefix, source, new_value in alterations:
        mutated = change_event(original, prefix, source, new_value)

        try:
            verify_diagnostic_body(mutated, "1.0")
        except AssertionError:
            print("PASS rejected incorrect session evidence", prefix.strip())
        else:
            raise AssertionError(f"Accepted false Vulkan session evidence: {prefix}")

    premature = original.copy()
    open_index = next(
        i for i, line in enumerate(premature)
        if line.startswith("BACKEND_VULKAN_SESSION_READY ")
    )
    close_index = next(
        i for i, line in enumerate(premature)
        if line.startswith("DEVICE_CATALOG_CLOSE ")
    )
    premature[open_index], premature[close_index] = (
        premature[close_index], premature[open_index]
    )

    try:
        verify_diagnostic_body(premature, "1.0")
    except AssertionError:
        print("PASS rejected Vulkan instance destruction before session")
    else:
        raise AssertionError("Accepted premature Vulkan instance teardown")

    full = next(
        row for row in manifest["frontend_runs"]
        if row["device"] == "redmagic-arm64"
    )
    native = (ARCHIVE / full["file"]).read_text(
        encoding="utf-8"
    ).splitlines()

    full_alterations = (
        ("BACKEND_VULKAN_SESSION_READY ", "scheduled=0", "scheduled=1"),
        ("BACKEND_VULKAN_SESSION_CLOSE ", "wait_result=0", "wait_result=-4"),
        ("DEVICE_CATALOG_CLOSE ", "selections=1", "selections=0"),
        ("OpenCL Info:", "OpenCL Info:", "OpenCL disabled:"),
        ("HASHCAT_SESSION_EXIT=0", "HASHCAT_SESSION_EXIT=0", "HASHCAT_SESSION_EXIT=1"),
    )

    for prefix, old, new_value in full_alterations:
        mutated = change_event(native, prefix, old, new_value)

        try:
            verify_full_frontend(
                mutated, full["device"], full["abi"], full["opencl_vendor"],
                full["gpu_name"], full["loader_minor"],
            )
        except AssertionError:
            print("PASS rejected incorrect full HASHCAT session", prefix.strip())
        else:
            raise AssertionError(f"Accepted false full HASHCAT session: {prefix}")

    cli = next(
        row for row in manifest["invalid_cli_runs"]
        if row["device"] == "pixel-arm64"
    )
    cli_original = (ARCHIVE / cli["file"]).read_text(
        encoding="utf-8"
    ).splitlines()
    cli_altered = change_event(
        cli_original, "INVALID_CASE=unsupported-api",
        "exit=2", "exit=0",
    )

    try:
        verify_cli(cli_altered, cli["abi"])
    except AssertionError:
        print("PASS rejected corrupted Android CLI session control")
    else:
        raise AssertionError("Accepted an invalid native session command")


def inspect_elf(
    readelf: Path,
    executable: Path,
    module: Path,
    abi: str,
) -> None:
    """Check actual Android ABI, minimum API and core Vulkan loader imports.

    Uses only static llvm-readelf inspection of existing ELF files.
    Checks the linked full frontend session and its uninvolved standard
    hashcat module, without executing either binary.
    """
    def read(path: Path, *options: str) -> str:
        """Return successfully inspected ELF file metadata."""
        result = subprocess.run(
            [str(readelf), "-W", *options, str(path)],
            check=True, capture_output=True, text=True, encoding="utf-8",
        )

        return result.stdout

    header = read(executable, "--file-header")
    notes = read(executable, "--notes")
    dynamic = read(executable, "--dynamic")
    symbols = read(executable, "--dyn-syms")
    module_dynamic = read(module, "--dynamic")

    elf_class = "ELF64" if abi == "arm64-v8a" else "ELF32"
    machine = "AArch64" if abi == "arm64-v8a" else "ARM"

    assert re.search(rf"Class:\s+{elf_class}\b", header)
    assert re.search(rf"Machine:\s+{machine}\b", header)
    assert "NT_ANDROID_TYPE_IDENT" in notes
    assert "description data: 18 00 00 00" in notes

    assert "Shared library: [libvulkan.so]" in dynamic
    assert "Shared library: [libvulkan.so]" not in module_dynamic
    for function in (
        "vkGetInstanceProcAddr",
        "vkCreateInstance",
        "vkCreateDevice",
        "vkQueueSubmit",
        "vkWaitForFences",
        "vkDestroyDevice",
    ):
        assert re.search(rf"UND\s+{function}\b", symbols), function

    assert not re.search(r"UND\s+vkEnumerateInstanceVersion\b", symbols)


def main() -> None:
    """Validate offline real-device logs, source and optional binary proofs."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--artifacts", type=Path,
        help="Directory of native session diagnostic executables/objects",
    )
    parser.add_argument(
        "--full", type=Path,
        help="Directory of complete ARM64 and ARMv7 native HASHCAT builds",
    )
    parser.add_argument(
        "--readelf", type=Path,
        help="Android NDK llvm-readelf for independent ELF inspection",
    )
    args = parser.parse_args()

    if args.readelf is not None and args.full is None:
        parser.error("--readelf requires --full")

    manifest = json.loads(
        (ARCHIVE / "manifest.json").read_text(encoding="utf-8")
    )

    assert manifest["minimum_android_api"] == 24
    assert manifest["default_vulkan_api"] == "1.0"
    assert manifest["optional_api"] == "1.1"
    assert manifest["build_flag"] == "ENABLE_VULKAN_DISCOVERY=1"
    assert manifest["full_android_build_comptime"] == 1793491200
    assert manifest["full_android_frontend_built"] is True
    assert manifest["vulkan_hashing_scheduler_enabled"] is False
    assert manifest["release_packages_modified"] is False
    assert len(manifest["source_sha256"]) == 18
    assert len(manifest["device_runs"]) == 6
    assert len(manifest["frontend_runs"]) == 3
    assert len(manifest["invalid_cli_runs"]) == 3

    for source, digest in manifest["source_sha256"].items():
        assert source_sha256(ROOT / source) == digest, source

    for row in manifest["device_runs"]:
        path = ARCHIVE / row["file"]
        data = path.read_bytes()

        assert data and data.endswith(b"\n") and b"\r" not in data
        assert sha256(path) == row["sha256"]
        lines = path.read_text(encoding="utf-8").splitlines()
        assert lines[:4] == [
            "TEST=vulkan-backend-session",
            f"ABI={row['abi']}",
            f"API={row['api']}",
            "ADB_EXIT_CODE=0",
        ]
        assert row["device_sessions"] == row["empty_fence_submissions"] == 2
        assert row["invalid_host_calls"] == 8
        assert row["invalid_owner_order_calls"] == 4
        assert row["driver_workload_refusals"] == 2

        verify_diagnostic_body(lines[4:], row["api"])

    for row in manifest["frontend_runs"]:
        path = ARCHIVE / row["file"]
        data = path.read_bytes()

        assert data and data.endswith(b"\n") and b"\r" not in data
        assert sha256(path) == row["sha256"]
        lines = path.read_text(encoding="utf-8").splitlines()
        assert row["device_sessions"] == row["empty_fence_submissions"] == 1
        assert row["vulkan_kernel_scheduler_enabled"] is False

        verify_full_frontend(
            lines, row["device"], row["abi"], row["opencl_vendor"],
            row["gpu_name"], row["loader_minor"],
        )

    for row in manifest["invalid_cli_runs"]:
        path = ARCHIVE / row["file"]
        assert sha256(path) == row["sha256"]
        assert row["invalid_commands"] == 3

        verify_cli(path.read_text(encoding="utf-8").splitlines(), row["abi"])

    assert len(list(ARCHIVE.glob("*.log"))) == 12

    if args.artifacts is not None:
        directory = args.artifacts.resolve(strict=True)
        assert len(manifest["diagnostic_sha256"]) == 4

        for name, digest in manifest["diagnostic_sha256"].items():
            assert sha256(directory / name) == digest, name

        print("PASS four exact ARM64/ARMv7 native Vulkan session build products")

    if args.full is not None:
        directory = args.full.resolve(strict=True)

        for abi in ("arm64-v8a", "armeabi-v7a"):
            binary = directory / abi / "hashcat"
            metadata = manifest["full_binaries"][abi]

            assert sha256(binary) == metadata["sha256"], abi
            assert binary.stat().st_size == metadata["bytes"]
            assert len(list((directory / abi / "modules").glob("*.so"))) == metadata["modules"] == 581
            assert len(list((directory / abi / "bridges").glob("*.so"))) == metadata["bridges"] == 4
            assert sum(
                item.is_file() for item in (directory / abi / "OpenCL").rglob("*")
            ) == metadata["opencl_files"] == 1595

            if args.readelf is not None:
                inspect_elf(
                    args.readelf.resolve(strict=True), binary,
                    directory / abi / "modules/module_00000.so", abi,
                )

        print("PASS two actual full HASHCAT Android session binary identities")

        if args.readelf is not None:
            print("PASS both Android API 24 ELF headers and core 1.0 queue imports")

    verify_mutations(manifest)

    print("PASS 12 real native Vulkan device/queue/fence ownership sessions")
    print("PASS 3 complete HASHCAT -I sessions with Vulkan/OpenCL coexistence")
    print("PASS 48 invalid host controls, 24 premature ownership controls")
    print("PASS 12 driver limit refusals and 9 malformed Android CLI requests")
    print("PASS 15 deliberate corrupted GPU/session evidence rejections")


if __name__ == "__main__":
    main()
