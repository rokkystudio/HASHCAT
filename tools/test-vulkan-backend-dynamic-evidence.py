#!/usr/bin/env python3
"""Independently verify real Android dynamic four-stage Vulkan execution.

Validates preserved Adreno ARM64/ARMv7 and Mali ARM64 process logs,
source and SPIR-V SHA-256 identities, exact standalone and complete
HASHCAT ELFs, ordered Vulkan ownership and CPU-verified public-integer
Compute output. No ADB, GPU operation or HASHCAT hashing action occurs.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import struct


ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = ROOT / "tests/fixtures/android-gpu/vulkan-backend-public-dynamic-20261102"
MANIFEST = ARCHIVE / "manifest.json"
PROFILES = {
    "redmagic-arm64": ("arm64-v8a", "arm64", "Adreno (TM) 730"),
    "redmagic-armv7": ("armeabi-v7a", "armv7", "Adreno (TM) 730"),
    "pixel-arm64": ("arm64-v8a", "arm64", "Mali-G715"),
}
APIS = ("1.0", "1.1")
ACCEPT_4 = (
    "BACKEND_VULKAN_PROGRAM_ACCEPT stages=4 buffers=5"
    " bytes=256 descriptors_per_stage=2"
)
ACCEPT_2 = (
    "BACKEND_VULKAN_PROGRAM_ACCEPT stages=2 buffers=3"
    " bytes=256 descriptors_per_stage=2"
)
ACCEPT_1 = (
    "BACKEND_VULKAN_PROGRAM_ACCEPT stages=1 buffers=2"
    " bytes=256 descriptors_per_stage=2"
)

READY_4 = (
    r"BACKEND_VULKAN_PROGRAM_READY index=0 queue_family=(\d+)"
    r" api=1\.[01] stages=4 buffers=5 descriptor_sets=4"
    r" descriptors=8 shader_modules=4 pipelines=4 local=32 groups=2"
    r" program_validated=1 dynamic_resources=1"
    r" features=0 extensions=0 scheduled=0"
)
DISPATCH_4 = (
    r"BACKEND_VULKAN_PROGRAM_DISPATCH_PASS stages=4 buffers=5"
    r" words=64 dispatches=4 verified_words=256"
    r" dependency_barriers=3 descriptors=8 coherent_input=[01]"
    r" noncoherent_outputs=[0-4] password_inputs=0 scheduled=0"
)


def sha256(path: Path) -> str:
    """Hash all bytes in one existing source, ELF or archived transcript."""
    assert path.is_file(), path
    return hashlib.sha256(path.read_bytes()).hexdigest()


def normalized_sha256(path: Path) -> str:
    """Hash source bytes with CRLF replaced by LF."""
    return hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")
    ).hexdigest()


def archived_source_sha256(source: str) -> str:
    """Resolve the source from the exact saved archive revision.

    For an uncommitted new archive, reads current checkout source.
    After first Git commit, requires the manifest bytes to match the
    most recent committed manifest blob, and hashes that same commit's
    source blob. A modified committed archive is rejected explicitly;
    later source changes cannot relabel historical native results.
    """
    archive_path = MANIFEST.relative_to(ROOT).as_posix()
    result = subprocess.run(
        ["git", "log", "-1", "--format=%H", "--", archive_path],
        cwd=ROOT, capture_output=True, check=True, text=True,
    )
    revision = result.stdout.strip()

    if not revision:
        return normalized_sha256(ROOT / source)

    assert re.fullmatch(r"[0-9a-f]{40}", revision)

    saved = subprocess.run(
        ["git", "show", f"{revision}:{archive_path}"],
        cwd=ROOT, capture_output=True, check=True,
    ).stdout
    assert saved == MANIFEST.read_bytes(), "Committed hardware evidence was modified"

    original = subprocess.run(
        ["git", "show", f"{revision}:{source}"],
        cwd=ROOT, capture_output=True, check=True,
    ).stdout
    return hashlib.sha256(original.replace(b"\r\n", b"\n")).hexdigest()


def verify_common(lines: list[str]) -> None:
    """Reject any claim that public Compute execution schedules password work."""
    joined = "\n".join(lines)
    for banned in (
        "scheduled=1", "password_inputs=1",
        "KERN_RUN ", "JOB_PORT_SUBMIT ",
        "actual_driver_loss=1", "CRYPTOGRAPHIC_SHADER_PASS",
    ):
        assert banned not in joined, banned


def verify_native(lines: list[str], device: str, api: str) -> None:
    """Check exact ordering of two independent four-stage GPU lifetimes."""
    abi, _, gpu = PROFILES[device]
    assert lines[:4] == [
        "TEST=vulkan-backend-public-dynamic",
        f"ABI={abi}",
        f"API={api}",
        "ADB_EXIT_CODE=0",
    ]
    events = lines[4:]
    assert len(events) == 29, (device, api, len(events))
    assert events[-1] == (
        f"BACKEND_PUBLIC_DYNAMIC_DIAGNOSTIC_PASS api={api}"
        " trials=2 dispatches=8 buffers=5 descriptor_sets=4"
        " pipelines=4 verified_words=512 dynamic_resources=1"
        " password_inputs=0 scheduled=0"
    )
    verify_common(events)

    for trial in (0, 1):
        block = events[trial * 14:(trial + 1) * 14]
        assert len(block) == 14
        loader = re.fullmatch(
            r"VULKAN_LOADER_VERSION=1\.(\d+)\.(\d+)"
            r" source=vkEnumerateInstanceVersion", block[0]
        )
        assert loader is not None
        minor = int(loader.group(1))
        assert minor >= int(api.split(".")[1])
        assert block[1] == (
            f"SELECT_INSTANCE_API={api} loader_minor={minor} cap=1.1"
        )
        assert block[2] == (
            f"DEVICE_CATALOG_OPEN requested_api={api}"
            f" instance_api={api} devices=1 extensions=0 layers=0"
        )
        assert block[3] == (
            f"BACKEND_VULKAN_CATALOG_READY devices=1"
            f" requested_api={api} scheduled=0"
        )
        select_device = re.fullmatch(
            rf"SELECT_DEVICE_API={re.escape(api)}"
            r" reported_minor=(\d+) instance_minor=(\d+) cap=1\.1",
            block[4],
        )
        assert select_device is not None
        assert int(select_device.group(1)) >= int(api.split(".")[1])
        assert int(select_device.group(2)) == int(api.split(".")[1])
        candidate = re.fullmatch(
            r"VULKAN_CANDIDATE index=0 name=(.+)"
            r" vendor=0x([0-9a-f]+) device=0x([0-9a-f]+)"
            r" api=1\.(\d+)\.(\d+) driver=0x([0-9a-f]+)",
            block[5],
        )
        assert candidate is not None
        assert candidate.group(1) == gpu
        limits = re.fullmatch(
            r"ACCEPT_COMPUTE index=0 queue_family=(\d+)"
            r" local_max=(\d+) invocations_max=(\d+)"
            r" groups_max=(\d+) shared_max=(\d+)"
            r" storage_range_max=(\d+) descriptor_max=(\d+)"
            r" non_coherent_atom=(\d+)",
            block[6],
        )
        assert limits is not None
        family = int(limits.group(1))
        assert int(limits.group(2)) >= 32
        assert int(limits.group(3)) >= 32
        assert int(limits.group(4)) >= 2
        assert int(limits.group(6)) >= 256
        assert int(limits.group(7)) >= 2
        assert int(limits.group(8)) > 0
        assert block[7] == (
            f"DEVICE_CATALOG_SELECT index=0 queue_family={family}"
            f" effective_api={api} selections=1"
        )
        assert block[8] == ACCEPT_4
        ready = re.fullmatch(READY_4, block[9])
        assert ready is not None
        assert int(ready.group(1)) == family
        assert f" api={api} " in block[9]
        assert re.fullmatch(DISPATCH_4, block[10])
        assert block[11] == (
            "BACKEND_VULKAN_PROGRAM_CLOSE index=0 stages=4"
            " result=0 released=1 scheduled=0"
        )
        assert block[12] == "DEVICE_CATALOG_CLOSE devices=1 selections=1"
        assert block[13] == (
            f"BACKEND_PUBLIC_DYNAMIC_TRIAL_PASS trial={trial}"
            " buffers=5 descriptor_sets=4 pipelines=4"
            " dispatches=4 verified_words=256 dynamic_resources=1"
            " scheduled=0"
        )


def verify_full(lines: list[str], device: str) -> None:
    """Verify one real hashcat -I session with seven public dispatches."""
    abi, _, gpu = PROFILES[device]
    assert lines[:5] == [
        "TEST=vulkan-hashcat-dynamic-executor",
        f"DEVICE={device}",
        f"ABI={abi}",
        "COMMAND=-I",
        "ADB_EXIT_CODE=0",
    ]
    events = lines[5:]
    assert events[-1] == "HASHCAT_KERNEL_EXIT=0"
    verify_common(events)
    assert "hashcat (v7.1.2) starting in backend information mode" in events
    assert events.count(ACCEPT_1) == 1
    assert events.count(ACCEPT_2) == 1
    assert events.count(ACCEPT_4) == 1

    expected_prefixes = {
        "BACKEND_VULKAN_SESSION_READY ": 1,
        "BACKEND_VULKAN_KERNEL_READY ": 1,
        "BACKEND_VULKAN_KERNEL_DISPATCH_PASS ": 1,
        "BACKEND_VULKAN_KERNEL_CLOSE ": 1,
        "BACKEND_VULKAN_CHAIN_READY ": 1,
        "BACKEND_VULKAN_CHAIN_DISPATCH_PASS ": 1,
        "BACKEND_VULKAN_CHAIN_CLOSE ": 1,
        "BACKEND_VULKAN_PROGRAM_READY ": 1,
        "BACKEND_VULKAN_PROGRAM_DISPATCH_PASS ": 1,
        "BACKEND_VULKAN_PROGRAM_CLOSE ": 1,
        "BACKEND_VULKAN_SESSION_CLOSE ": 1,
        "DEVICE_CATALOG_CLOSE ": 1,
    }
    indices = {}
    for prefix, count in expected_prefixes.items():
        positions = [
            i for i, line in enumerate(events) if line.startswith(prefix)
        ]
        assert len(positions) == count, (device, prefix, positions)
        indices[prefix] = positions[0]

    ordered = (
        "BACKEND_VULKAN_SESSION_READY ",
        "BACKEND_VULKAN_KERNEL_READY ",
        "BACKEND_VULKAN_KERNEL_DISPATCH_PASS ",
        "BACKEND_VULKAN_KERNEL_CLOSE ",
        "BACKEND_VULKAN_CHAIN_READY ",
        "BACKEND_VULKAN_CHAIN_DISPATCH_PASS ",
        "BACKEND_VULKAN_CHAIN_CLOSE ",
        "BACKEND_VULKAN_PROGRAM_READY ",
        "BACKEND_VULKAN_PROGRAM_DISPATCH_PASS ",
        "BACKEND_VULKAN_PROGRAM_CLOSE ",
        "BACKEND_VULKAN_SESSION_CLOSE ",
        "DEVICE_CATALOG_CLOSE ",
    )
    assert [indices[x] for x in ordered] == sorted(indices.values())
    assert events[indices["BACKEND_VULKAN_SESSION_READY "]] == (
        "BACKEND_VULKAN_SESSION_READY index=0 queue_family=0"
        " effective_api=1.0 command_buffers=1 empty_submissions=1"
        " features=0 extensions=0 scheduled=0"
    )
    assert re.fullmatch(
        r"BACKEND_VULKAN_KERNEL_READY index=0 queue_family=\d+"
        r" api=1\.0 shader_words=326 descriptors=2 local=32 groups=2"
        r" program_validated=1 dynamic_resources=1"
        r" features=0 extensions=0 scheduled=0",
        events[indices["BACKEND_VULKAN_KERNEL_READY "]],
    )
    assert re.fullmatch(
        r"BACKEND_VULKAN_KERNEL_DISPATCH_PASS"
        r" kernel=public_u32_transform words=64 local=32 groups=2"
        r" verified=64 coherent_input=[01] coherent_output=[01]"
        r" shaders=1 password_inputs=0 scheduled=0",
        events[indices["BACKEND_VULKAN_KERNEL_DISPATCH_PASS "]],
    )
    assert re.fullmatch(
        r"BACKEND_VULKAN_CHAIN_READY index=0 queue_family=\d+"
        r" api=1\.0 shader_words_first=326 shader_words_second=363"
        r" descriptors=4 sets=2 buffers=3 local=32 groups=2"
        r" program_validated=1 dynamic_resources=1"
        r" features=0 extensions=0 scheduled=0",
        events[indices["BACKEND_VULKAN_CHAIN_READY "]],
    )
    assert re.fullmatch(
        r"BACKEND_VULKAN_CHAIN_DISPATCH_PASS"
        r" stage1=public_u32_transform stage2=public_u32_rotate"
        r" words=64 local=32 groups=2 dispatches=2"
        r" verified_first=64 verified_second=64"
        r" coherent_input=[01] coherent_intermediate=[01]"
        r" coherent_output=[01] barrier=shader_write_to_shader_read"
        r" shaders=2 password_inputs=0 scheduled=0",
        events[indices["BACKEND_VULKAN_CHAIN_DISPATCH_PASS "]],
    )
    assert re.fullmatch(
        READY_4,
        events[indices["BACKEND_VULKAN_PROGRAM_READY "]],
    )
    assert re.fullmatch(
        DISPATCH_4,
        events[indices["BACKEND_VULKAN_PROGRAM_DISPATCH_PASS "]],
    )
    assert events[indices["BACKEND_VULKAN_PROGRAM_CLOSE "]] == (
        "BACKEND_VULKAN_PROGRAM_CLOSE index=0 stages=4"
        " result=0 released=1 scheduled=0"
    )
    assert events[indices["BACKEND_VULKAN_SESSION_CLOSE "]] == (
        "BACKEND_VULKAN_SESSION_CLOSE index=0 queue_family=0"
        " empty_submissions=1 wait_result=0"
    )
    assert events[indices["DEVICE_CATALOG_CLOSE "]] == (
        "DEVICE_CATALOG_CLOSE devices=1 selections=4"
    )

    text = "\n".join(events)
    assert "OpenCL Info:" in text
    assert "Backend Device ID #01" in text
    assert gpu.split(" ")[0] in text
    for summary in (
        "Vulkan 1.0 public SPIR-V Compute kernel dispatched and verified",
        "Vulkan 1.0 public two-stage SPIR-V Compute chain dispatched and verified",
        "Vulkan 1.0 public four-stage SPIR-V Compute chain dispatched and verified",
    ):
        assert summary in text, summary
    assert indices["BACKEND_VULKAN_PROGRAM_CLOSE "] < text_to_index(
        events, "OpenCL Info:"
    ) < indices["BACKEND_VULKAN_SESSION_CLOSE "]


def text_to_index(events: list[str], fragment: str) -> int:
    """Return the unique event index containing one required fragment."""
    indices = [i for i, line in enumerate(events) if fragment in line]
    assert len(indices) == 1, fragment
    return indices[0]


def verify_cli(lines: list[str], abi: str) -> None:
    """Require three malformed native command requests to return code 2."""
    assert lines == [
        "TEST=vulkan-backend-public-dynamic-invalid-inputs",
        f"ABI={abi}",
        "INVALID_CASES=3",
        "INVALID_CASE=missing-arguments exit=2",
        "INVALID_CASE=unsupported-api exit=2",
        "INVALID_CASE=wrong-flag exit=2",
    ]


def verify_corruption(native: list[str], full: list[str]) -> None:
    """Reject falsified GPU dispatch, dependency and resource events."""
    trials = (
        (native, "stages=4 buffers=5", "stages=3 buffers=5", "native"),
        (native, "dynamic_resources=1", "dynamic_resources=0", "native"),
        (native, "descriptor_sets=4", "descriptor_sets=3", "native"),
        (native, "pipelines=4", "pipelines=3", "native"),
        (native, "verified_words=256", "verified_words=255", "native"),
        (native, "dependency_barriers=3", "dependency_barriers=2", "native"),
        (native, "descriptors=8", "descriptors=6", "native"),
        (native, "password_inputs=0", "password_inputs=1", "native"),
        (native, "scheduled=0", "scheduled=1", "native"),
        (native, "released=1", "released=0", "native"),
        (native, "requested_api=1.0", "requested_api=1.1", "native"),
        (full, "stages=1 buffers=2", "stages=1 buffers=3", "full"),
        (full, "stages=2 buffers=3", "stages=2 buffers=2", "full"),
        (full, "stages=4 buffers=5", "stages=4 buffers=4", "full"),
        (full, "empty_submissions=1", "empty_submissions=0", "full"),
        (full, "program_validated=1", "program_validated=0", "full"),
        (full, "verified=64", "verified=63", "full"),
        (full, "verified_second=64", "verified_second=63", "full"),
        (full, "dependency_barriers=3", "dependency_barriers=2", "full"),
        (full, "verified_words=256", "verified_words=255", "full"),
        (full, "dynamic_resources=1", "dynamic_resources=0", "full"),
        (full, "password_inputs=0", "password_inputs=1", "full"),
        (full, "scheduled=0", "scheduled=1", "full"),
        (full, "DEVICE_CATALOG_CLOSE devices=1 selections=4", "DEVICE_CATALOG_CLOSE devices=1 selections=3", "full"),
        (full, "OpenCL Info:", "OpenGL Info:", "full"),
        (full, "HASHCAT_KERNEL_EXIT=0", "HASHCAT_KERNEL_EXIT=1", "full"),
    )
    rejected = 0

    for original, old, new, kind in trials:
        changed = original.copy()
        indices = [i for i, line in enumerate(changed) if old in line]
        assert indices, old
        index = indices[0]
        changed[index] = changed[index].replace(old, new, 1)
        assert changed[index] != original[index]
        try:
            if kind == "native":
                verify_native(changed, "pixel-arm64", "1.0")
            else:
                verify_full(changed, "pixel-arm64")
        except AssertionError:
            rejected += 1
        else:
            raise AssertionError(f"Accepted forged GPU evidence: {kind} {old}")

    assert rejected == len(trials)
    print(f"PASS {rejected} deliberately falsified Vulkan GPU records rejected")


def inspect_elf(readelf: Path, executable: Path, abi: str, linked: bool) -> None:
    """Verify Android API24 ELF architecture and genuine Vulkan 1.0 linkage."""
    def read(*flags: str) -> str:
        return subprocess.run(
            [str(readelf), "-W", *flags, str(executable)],
            check=True, capture_output=True, text=True,
            encoding="utf-8",
        ).stdout

    header = read("--file-header")
    notes = read("--notes")
    dynamic = read("--dynamic")

    cls = "ELF64" if "arm64" in abi else "ELF32"
    machine = "AArch64" if "arm64" in abi else "ARM"
    assert re.search(rf"Class:\s+{cls}\b", header)
    assert re.search(rf"Machine:\s+{machine}\b", header)
    assert "NT_ANDROID_TYPE_IDENT" in notes
    assert "description data: 18 00 00 00" in notes

    if linked:
        symbols = read("--dyn-syms")
        assert "Shared library: [libvulkan.so]" in dynamic
        for fn in (
            "vkCreateShaderModule", "vkCreateComputePipelines",
            "vkCmdDispatch", "vkCmdPipelineBarrier", "vkCreateBuffer",
            "vkAllocateMemory", "vkQueueSubmit", "vkWaitForFences",
            "vkFlushMappedMemoryRanges", "vkInvalidateMappedMemoryRanges",
        ):
            assert re.search(rf"UND\s+{fn}\b", symbols), fn
        assert re.search(r"UND\s+vkEnumerateInstanceVersion\b", symbols) is None
    else:
        assert "libvulkan.so" not in dynamic


def main() -> None:
    """Verify archived real Android traces and optional exact ELF artifacts."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path)
    parser.add_argument("--full", type=Path)
    parser.add_argument("--readelf", type=Path)
    args = parser.parse_args()
    if args.readelf is not None and (
        args.native is None or args.full is None
    ):
        parser.error("--readelf requires both --native and --full")

    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    assert manifest["date"] == "2026-11-02"
    assert manifest["minimum_android_api"] == 24
    assert manifest["native_requested_apis"] == ["1.0", "1.1"]
    assert manifest["physical_device_process_profiles"] == list(PROFILES)
    assert manifest["program_stage_counts"] == [1, 2, 4]
    assert manifest["four_stage_shader_modules"] == 4
    assert manifest["four_stage_descriptor_sets"] == 4
    assert manifest["four_stage_descriptor_count"] == 8
    assert manifest["four_stage_storage_buffers"] == 5
    assert manifest["four_stage_dependency_barriers"] == 3
    assert manifest["host_resource_arrays_dynamically_allocated"] is True
    assert manifest["hashcat_hashing_scheduler_connected"] is False
    assert manifest["password_or_hash_inputs_processed"] is False
    assert manifest["cryptographic_spirv_kernels"] == 0
    assert manifest["hashcat_info_mode_only"] is True
    assert manifest["normal_opencl_discovery_preserved"] is True

    assert len(manifest["source_sha256"]) == 20
    for source, digest in manifest["source_sha256"].items():
        assert archived_source_sha256(source) == digest, source

    modules = manifest["spirv_modules"]
    assert modules["public_u32_transform"]["words"] == 326
    assert modules["public_u32_rotate"]["words"] == 363
    assert modules["public_u32_transform"]["bytes"] == 1304
    assert modules["public_u32_rotate"]["bytes"] == 1452

    assert len(manifest["native_runs"]) == 6
    assert len(manifest["full_hashcat_info_runs"]) == 3
    assert len(manifest["invalid_cli"]) == 3
    assert len(list(ARCHIVE.glob("*.log"))) == 12

    expected_native = {
        (device, api) for device in PROFILES for api in APIS
    }
    observed_native = set()
    for row in manifest["native_runs"]:
        case = row["device"], row["requested_api"]
        assert case in expected_native and case not in observed_native
        observed_native.add(case)
        assert row["abi"] == PROFILES[row["device"]][0]
        assert row["native_exit_code"] == 0
        assert row["real_instance_lifetimes"] == 2
        assert row["four_stage_program_verifications"] == 2
        assert row["four_stage_dispatches"] == 8
        assert row["verified_public_words"] == 512
        assert row["shader_dependency_barriers"] == 6

        path = ARCHIVE / row["file"]
        data = path.read_bytes()
        assert data.endswith(b"\n") and b"\r" not in data
        assert b"46281FDAS002AL" not in data and b"91809037" not in data
        assert sha256(path) == row["sha256"]
        verify_native(path.read_text(encoding="utf-8").splitlines(), *case)
    assert observed_native == expected_native

    seen_full = set()
    for row in manifest["full_hashcat_info_runs"]:
        device = row["device"]
        assert device in PROFILES and device not in seen_full
        seen_full.add(device)
        assert row["abi"] == PROFILES[device][0]
        assert row["native_exit_code"] == 0
        assert row["empty_queue_submissions"] == 1
        assert row["one_stage_dispatches"] == 1
        assert row["two_stage_dispatches"] == 2
        assert row["four_stage_dispatches"] == 4
        assert row["verified_public_words"] == 448
        assert row["shader_dependency_barriers"] == 4

        path = ARCHIVE / row["file"]
        assert sha256(path) == row["sha256"]
        verify_full(path.read_text(encoding="utf-8").splitlines(), device)
    assert seen_full == set(PROFILES)

    seen_cli = set()
    for row in manifest["invalid_cli"]:
        device = row["device"]
        assert device in PROFILES and device not in seen_cli
        seen_cli.add(device)
        assert row["abi"] == PROFILES[device][0]
        assert row["rejected_native_options"] == 3
        path = ARCHIVE / row["file"]
        assert sha256(path) == row["sha256"]
        verify_cli(path.read_text(encoding="utf-8").splitlines(), row["abi"])
    assert seen_cli == set(PROFILES)

    assert manifest["totals"] == {
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
    }

    if args.native is not None:
        directory = args.native.resolve(strict=True)
        for abi, hashes in manifest["native_artifact_sha256"].items():
            executable = directory / f"vulkan-backend-public-dynamic-{abi}"
            backend = directory / f"hashcat-backend-vulkan-{abi}.o"
            assert sha256(executable) == hashes["diagnostic"]
            assert sha256(backend) == hashes["production_backend_object"]
            if args.readelf is not None:
                inspect_elf(args.readelf.resolve(strict=True), executable, abi, True)

        for name, source in (
            ("public_u32_transform", "vulkan-backend-public-stage1.spv"),
            ("public_u32_rotate", "vulkan-backend-public-stage2.spv"),
        ):
            data = (directory / source).read_bytes()
            words = struct.unpack("<" + "I" * (len(data) // 4), data)
            assert words[0] == 0x07230203
            assert words[1] == 0x00010000
            assert words[4] == 0
            assert len(words) == modules[name]["words"]
            assert sha256(directory / source) == modules[name]["sha256"]

    if args.full is not None:
        directory = args.full.resolve(strict=True)
        for abi, hashes in manifest["full_hashcat_artifact_sha256"].items():
            exe = directory / abi / "hashcat"
            mod = directory / abi / "modules/module_00000.so"
            assert sha256(exe) == hashes["frontend"]
            assert sha256(mod) == hashes["module_00000"]
            if args.readelf is not None:
                inspect_elf(args.readelf.resolve(strict=True), exe, abi, True)
                inspect_elf(args.readelf.resolve(strict=True), mod, abi, False)
        print("PASS exact Android ARM64/ARMv7 full HASHCAT SHA-256 and module isolation")

    verify_corruption(
        (ARCHIVE / "pixel-arm64-1.0.log").read_text(encoding="utf-8").splitlines(),
        (ARCHIVE / "pixel-arm64-hashcat-dynamic-info.log").read_text(
            encoding="utf-8"
        ).splitlines(),
    )
    print("PASS 6 real native GPU processes with 12 four-stage lifetimes")
    print("PASS 3 full HASHCAT -I public GPU processes with normal OpenCL")
    print("PASS 69 real vkCmdDispatch calls and 4416 CPU-verified public words")
    print("PASS 45 four-stage dependency barriers and 9 invalid native CLIs")


if __name__ == "__main__":
    main()
