#!/usr/bin/env python3
"""Verify physical Android Vulkan four-stage cleanup/recovery evidence.

Checks all 37 explicitly numbered partial-resource checkpoints on
Adreno ARM64/ARMv7 and Mali ARM64 under Vulkan 1.0/1.1, reconstructs
the exact ownership inventory expected at each checkpoint, requires a
fresh CPU-verified four-stage GPU session after every cleanup, verifies
full hashcat -I runs and identifies source, SPIR-V and Android binaries.

This is an offline verifier: it does not contact devices or call Vulkan.
The injected initialization results are host-test controls, not
observations of genuine driver failure or GPU device loss.
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
ARCHIVE = ROOT / "tests/fixtures/android-gpu/vulkan-backend-public-fault-20261102"
MANIFEST = ARCHIVE / "manifest.json"

PROFILES = {
    "redmagic-arm64": ("arm64-v8a", "arm64", "Adreno (TM) 730"),
    "redmagic-armv7": ("armeabi-v7a", "armv7", "Adreno (TM) 730"),
    "pixel-arm64": ("arm64-v8a", "arm64", "Mali-G715"),
}
APIS = ("1.0", "1.1")

RESOURCE_POINTS = (
    "host_arrays",
    "device",
    *("buffer", "memory", "map") * 5,
    "descriptor_layout",
    "pipeline_layout",
    "descriptor_pool",
    *("descriptor_set", "shader", "pipeline") * 4,
    "command_pool",
    "command_buffer",
    "fence",
    "before_submit",
    "after_submit",
)
assert len(RESOURCE_POINTS) == 37

ACCEPT = (
    "BACKEND_VULKAN_PROGRAM_ACCEPT"
    " stages=4 buffers=5 bytes=256 descriptors_per_stage=2"
)
DISPATCH = (
    r"BACKEND_VULKAN_PROGRAM_DISPATCH_PASS"
    r" stages=4 buffers=5 words=64"
    r" dispatches=4 verified_words=256"
    r" dependency_barriers=3 descriptors=8"
    r" coherent_input=[01] noncoherent_outputs=[0-4]"
    r" password_inputs=0 scheduled=0"
)
READY = (
    r"BACKEND_VULKAN_PROGRAM_READY"
    r" index=0 queue_family=\d+ api=1\.[01]"
    r" stages=4 buffers=5 descriptor_sets=4 descriptors=8"
    r" shader_modules=4 pipelines=4 local=32 groups=2"
    r" program_validated=1 dynamic_resources=1"
    r" features=0 extensions=0 scheduled=0"
)

CLEANUP_KEYS = (
    "point",
    "reached",
    "hit",
    "submitted",
    "device",
    "buffers",
    "memories",
    "maps",
    "descriptor_sets",
    "shaders",
    "pipelines",
    "descriptor_pool",
    "pipeline_layout",
    "descriptor_layout",
    "fence",
    "command_buffer",
    "command_pool",
    "host_arrays",
    "wait_idle",
    "completed",
    "cleanup_result",
    "simulated",
)


def sha256(path: Path) -> str:
    """Compute SHA-256 over complete existing source, ELF or log bytes."""
    assert path.is_file(), path
    return hashlib.sha256(path.read_bytes()).hexdigest()


def normalized_sha256(path: Path) -> str:
    """Hash source after deterministic CRLF-to-LF normalization."""
    return hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")
    ).hexdigest()


def archived_source_sha256(source: str) -> str:
    """Hash current source precommit, then enforce exact archive Git provenance.

    Locates the last commit that wrote the saved manifest, validates the
    on-disk manifest against that blob, and reads the source from the
    same exact Git revision. Source changes committed later do not
    affect the historical GPU data, and a modified manifest is rejected.
    """
    rel_manifest = MANIFEST.relative_to(ROOT).as_posix()
    revision = subprocess.run(
        ["git", "log", "-1", "--format=%H", "--", rel_manifest],
        cwd=ROOT, check=True, capture_output=True, text=True,
    ).stdout.strip()

    if not revision:
        return normalized_sha256(ROOT / source)

    assert re.fullmatch(r"[0-9a-f]{40}", revision)

    saved = subprocess.run(
        ["git", "show", f"{revision}:{rel_manifest}"],
        cwd=ROOT, check=True, capture_output=True,
    ).stdout

    assert saved == MANIFEST.read_bytes(), (
        "Archived Vulkan fault manifest differs from its committed revision"
    )

    contents = subprocess.run(
        ["git", "show", f"{revision}:{source}"],
        cwd=ROOT, check=True, capture_output=True,
    ).stdout

    return hashlib.sha256(
        contents.replace(b"\r\n", b"\n")
    ).hexdigest()


def parse_cleanup(line: str) -> dict[str, int]:
    """Read one exact partial ownership inventory without missing fields."""
    tokens = line.split()
    assert tokens[0] == "BACKEND_PUBLIC_FAULT_RELEASE"
    values = {}

    for token in tokens[1:]:
        key, sep, value = token.partition("=")
        assert sep and key not in values, token
        assert re.fullmatch(r"-?\d+", value), token
        values[key] = int(value)

    assert tuple(values) == CLEANUP_KEYS, tuple(values)
    return values


def expected_cleanup(point: int) -> dict[str, int]:
    """Derive exact owned resources from the declared acquisition sequence.

    Five buffers acquire buffer/memory/map triplets beginning at point 3.
    Four stages acquire set/shader/pipeline triplets beginning at point
    21. Command pool/buffer/fence follow at 33/34/35, and only point 37
    has successfully submitted work. No hardware heuristics are used.
    """
    assert 1 <= point <= 37

    def triplet(start: int, count: int) -> int:
        if point < start:
            return 0
        return min(count, (point - start) // 3 + 1)

    return {
        "point": point,
        "reached": point,
        "hit": 1,
        "submitted": int(point == 37),
        "device": int(point >= 2),
        "buffers": triplet(3, 5),
        "memories": triplet(4, 5),
        "maps": triplet(5, 5),
        "descriptor_sets": triplet(21, 4),
        "shaders": triplet(22, 4),
        "pipelines": triplet(23, 4),
        "descriptor_pool": int(point >= 20),
        "pipeline_layout": int(point >= 19),
        "descriptor_layout": int(point >= 18),
        "fence": int(point >= 35),
        "command_buffer": int(point >= 34),
        "command_pool": int(point >= 33),
        "host_arrays": 4,
        "wait_idle": 0,
        "completed": 1,
        "cleanup_result": 0,
        "simulated": 1,
    }


def assert_normal_scheduling(lines: list[str]) -> None:
    """Require no password scheduling, invented driver loss or fake VkResults."""
    joined = "\n".join(lines)

    for forbidden in (
        "scheduled=1",
        "password_inputs=1",
        "real_driver_losses=1",
        "actual_device_loss=1",
        "VK_ERROR_DEVICE_LOST_REPRODUCED",
        "GPU_HANG_INDUCED",
        "CRYPTOGRAPHIC_SPIRV_PASS",
    ):
        assert forbidden not in joined, forbidden


def assert_native(lines: list[str], device: str, api: str) -> None:
    """Verify real GPU catalogue events and 37 ordered failure/recovery pairs."""
    abi, _, gpu = PROFILES[device]

    assert lines[:4] == [
        "TEST=vulkan-backend-public-fault",
        f"ABI={abi}",
        f"API={api}",
        "ADB_EXIT_CODE=0",
    ]

    events = lines[4:]
    assert events[-1] == (
        f"BACKEND_PUBLIC_FAULT_DIAGNOSTIC_PASS api={api}"
        " checkpoints=37 injected=37 recovered_devices=37"
        " recovered_dispatches=148 verified_words=9472"
        " abandoned_before_submit=1 abandoned_after_submit=1"
        " real_driver_losses=0 password_inputs=0 scheduled=0"
    )
    assert_normal_scheduling(events)

    assert events[0].startswith("VULKAN_LOADER_VERSION=1.")
    assert events[1].startswith(f"SELECT_INSTANCE_API={api} ")
    assert events[2] == (
        f"DEVICE_CATALOG_OPEN requested_api={api}"
        f" instance_api={api} devices=1 extensions=0 layers=0"
    )
    assert events[3] == (
        f"BACKEND_VULKAN_CATALOG_READY devices=1"
        f" requested_api={api} scheduled=0"
    )
    assert events[4] == (
        "BACKEND_PUBLIC_FAULT_CONTROLS_PASS"
        " invalid_points=2 invalid_context=3"
        " gpu_submissions=0 scheduled=0"
    )
    assert events[-2] == "DEVICE_CATALOG_CLOSE devices=1 selections=74"

    expected_prefix_counts = {
        "DEVICE_CATALOG_OPEN ": 1,
        "DEVICE_CATALOG_CLOSE ": 1,
        "DEVICE_CATALOG_SELECT ": 74,
        "BACKEND_VULKAN_PROGRAM_ACCEPT ": 74,
        "BACKEND_VULKAN_PROGRAM_READY ": 39,
        "BACKEND_VULKAN_PROGRAM_DISPATCH_PASS ": 37,
        "BACKEND_VULKAN_PROGRAM_CLOSE ": 74,
        "BACKEND_PUBLIC_FAULT_TRIGGER ": 37,
        "BACKEND_PUBLIC_FAULT_RELEASE ": 37,
        "BACKEND_PUBLIC_FAULT_RECOVERY_PASS ": 37,
        "BACKEND_PUBLIC_FAULT_CONTROLS_PASS ": 1,
    }

    for prefix, expected in expected_prefix_counts.items():
        actual = sum(line.startswith(prefix) for line in events)
        assert actual == expected, (device, api, prefix, actual, expected)

    candidates = [
        line for line in events
        if line.startswith("VULKAN_CANDIDATE index=0 name=")
    ]
    assert len(candidates) == 74
    assert all(line.startswith(
        f"VULKAN_CANDIDATE index=0 name={gpu} "
    ) for line in candidates)

    selections = [
        line for line in events if line.startswith("DEVICE_CATALOG_SELECT ")
    ]
    for index, line in enumerate(selections, start=1):
        assert re.fullmatch(
            rf"DEVICE_CATALOG_SELECT index=0 queue_family=\d+"
            rf" effective_api={re.escape(api)} selections={index}",
            line,
        ), (index, line)

    assert events.count(ACCEPT) == 74

    ready = [
        line for line in events
        if line.startswith("BACKEND_VULKAN_PROGRAM_READY ")
    ]
    assert all(
        re.fullmatch(READY, line) and f" api={api} " in line
        for line in ready
    )

    dispatch = [
        line for line in events
        if line.startswith("BACKEND_VULKAN_PROGRAM_DISPATCH_PASS ")
    ]
    assert len(dispatch) == 37
    assert all(re.fullmatch(DISPATCH, line) for line in dispatch)

    trigger_positions = []
    release_positions = []
    recover_positions = []

    for point, label in enumerate(RESOURCE_POINTS, start=1):
        prefix = f"BACKEND_PUBLIC_FAULT_TRIGGER point={point} "
        triggers = [
            (i, line) for i, line in enumerate(events)
            if line.startswith(prefix)
        ]
        assert len(triggers) == 1, (point, triggers)

        submitted = int(point == 37)
        assert triggers[0][1] == (
            f"BACKEND_PUBLIC_FAULT_TRIGGER point={point}"
            f" resource={label} simulated=1"
            f" submitted={submitted} result=-3"
        )
        trigger_positions.append(triggers[0][0])

        prefix = f"BACKEND_PUBLIC_FAULT_RELEASE point={point} "
        releases = [
            (i, line) for i, line in enumerate(events)
            if line.startswith(prefix)
        ]
        assert len(releases) == 1
        actual_counts = parse_cleanup(releases[0][1])
        expected_counts = expected_cleanup(point)
        assert actual_counts == expected_counts, (
            device, api, point, actual_counts, expected_counts
        )
        release_positions.append(releases[0][0])

        recovery = (
            f"BACKEND_PUBLIC_FAULT_RECOVERY_PASS point={point}"
            " injected=1 new_device=1"
            " dispatches=4 verified_words=256 scheduled=0"
        )
        matching_recovery = [
            i for i, line in enumerate(events) if line == recovery
        ]
        assert len(matching_recovery) == 1
        recover_positions.append(matching_recovery[0])

        failed_close = (
            "BACKEND_VULKAN_PROGRAM_CLOSE index=0 stages=4"
            " result=-3 released=1 scheduled=0"
        )
        working_close = (
            "BACKEND_VULKAN_PROGRAM_CLOSE index=0 stages=4"
            " result=0 released=1 scheduled=0"
        )

        first, second, third = (
            trigger_positions[-1],
            release_positions[-1],
            recover_positions[-1],
        )
        assert first < second < third
        assert events[second + 1] == failed_close, (point, events[second + 1])
        assert events[third - 1] == working_close, (point, events[third - 1])
        assert re.fullmatch(DISPATCH, events[third - 2])
        assert events[third - 3].startswith("BACKEND_VULKAN_PROGRAM_READY ")

        if point > 1:
            assert recover_positions[-2] < first

    assert len(trigger_positions) == 37
    assert len(release_positions) == 37
    assert len(recover_positions) == 37
    assert trigger_positions == sorted(trigger_positions)
    assert release_positions == sorted(release_positions)
    assert recover_positions == sorted(recover_positions)


def assert_full(lines: list[str], device: str) -> None:
    """Verify complete non-fault HASHCAT -I public Compute and OpenCL session."""
    abi, _, gpu = PROFILES[device]
    assert lines[:5] == [
        "TEST=vulkan-hashcat-public-fault",
        f"DEVICE={device}",
        f"ABI={abi}",
        "COMMAND=-I",
        "ADB_EXIT_CODE=0",
    ]
    assert lines[-1] == "HASHCAT_KERNEL_EXIT=0"
    assert_normal_scheduling(lines)
    assert not any(line.startswith(
        ("BACKEND_PUBLIC_FAULT_TRIGGER ", "BACKEND_PUBLIC_FAULT_RELEASE ")
    ) for line in lines)

    events = lines[5:]

    for stages, buffers in ((1, 2), (2, 3), (4, 5)):
        assert events.count(
            f"BACKEND_VULKAN_PROGRAM_ACCEPT stages={stages}"
            f" buffers={buffers} bytes=256 descriptors_per_stage=2"
        ) == 1

    expected = {
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

    positions = {}
    for prefix, count in expected.items():
        indices = [
            i for i, line in enumerate(events) if line.startswith(prefix)
        ]
        assert len(indices) == count, (device, prefix, indices)
        positions[prefix] = indices[0]

    ordered = tuple(expected)
    assert [positions[prefix] for prefix in ordered] == sorted(
        positions.values()
    )

    kernel = events[positions["BACKEND_VULKAN_KERNEL_DISPATCH_PASS "]]
    assert re.fullmatch(
        r"BACKEND_VULKAN_KERNEL_DISPATCH_PASS"
        r" kernel=public_u32_transform words=64 local=32 groups=2"
        r" verified=64 coherent_input=[01] coherent_output=[01]"
        r" shaders=1 password_inputs=0 scheduled=0",
        kernel,
    )
    chain = events[positions["BACKEND_VULKAN_CHAIN_DISPATCH_PASS "]]
    assert re.fullmatch(
        r"BACKEND_VULKAN_CHAIN_DISPATCH_PASS"
        r" stage1=public_u32_transform stage2=public_u32_rotate"
        r" words=64 local=32 groups=2 dispatches=2"
        r" verified_first=64 verified_second=64"
        r" coherent_input=[01] coherent_intermediate=[01]"
        r" coherent_output=[01] barrier=shader_write_to_shader_read"
        r" shaders=2 password_inputs=0 scheduled=0",
        chain,
    )
    assert re.fullmatch(
        DISPATCH,
        events[positions["BACKEND_VULKAN_PROGRAM_DISPATCH_PASS "]],
    )
    assert re.fullmatch(
        READY,
        events[positions["BACKEND_VULKAN_PROGRAM_READY "]],
    )
    assert events[positions["BACKEND_VULKAN_PROGRAM_CLOSE "]] == (
        "BACKEND_VULKAN_PROGRAM_CLOSE index=0 stages=4"
        " result=0 released=1 scheduled=0"
    )
    assert events[positions["DEVICE_CATALOG_CLOSE "]] == (
        "DEVICE_CATALOG_CLOSE devices=1 selections=4"
    )

    text = "\n".join(events)
    for required in (
        "hashcat (v7.1.2) starting in backend information mode",
        "OpenCL Info:",
        "Backend Device ID #01",
        gpu.split(" ")[0],
        "Vulkan 1.0 public SPIR-V Compute kernel dispatched and verified",
        "Vulkan 1.0 public two-stage SPIR-V Compute chain dispatched and verified",
        "Vulkan 1.0 public four-stage SPIR-V Compute chain dispatched and verified",
    ):
        assert required in text, (device, required)

    opencl = [i for i, line in enumerate(events) if "OpenCL Info:" in line]
    assert len(opencl) == 1
    assert positions["BACKEND_VULKAN_PROGRAM_CLOSE "] < opencl[0]
    assert opencl[0] < positions["BACKEND_VULKAN_SESSION_CLOSE "]


def assert_cli(lines: list[str], abi: str) -> None:
    """Require exact native exit code 2 for three malformed CLI requests."""
    assert lines == [
        "TEST=vulkan-backend-public-fault-invalid-inputs",
        f"ABI={abi}",
        "INVALID_CASES=3",
        "INVALID_CASE=missing-arguments exit=2",
        "INVALID_CASE=unsupported-api exit=2",
        "INVALID_CASE=wrong-flag exit=2",
    ]


def inspect_elf(
    readelf: Path,
    executable: Path,
    abi: str,
    vulkan_linked: bool,
) -> None:
    """Verify Android API24 ELF class, real core1.0 symbols and loader linkage."""
    def read(*flags: str) -> str:
        return subprocess.run(
            [str(readelf), "-W", *flags, str(executable)],
            check=True, capture_output=True, text=True,
            encoding="utf-8",
        ).stdout

    header = read("--file-header")
    notes = read("--notes")
    dynamic = read("--dynamic")

    expected_class = "ELF64" if "arm64" in abi else "ELF32"
    expected_machine = "AArch64" if "arm64" in abi else "ARM"
    assert re.search(rf"Class:\s+{expected_class}\b", header)
    assert re.search(rf"Machine:\s+{expected_machine}\b", header)
    assert "NT_ANDROID_TYPE_IDENT" in notes
    assert "description data: 18 00 00 00" in notes

    if vulkan_linked:
        symbols = read("--dyn-syms")
        assert "Shared library: [libvulkan.so]" in dynamic

        for symbol in (
            "vkCreateDevice",
            "vkDestroyDevice",
            "vkCreateBuffer",
            "vkDestroyBuffer",
            "vkAllocateMemory",
            "vkFreeMemory",
            "vkMapMemory",
            "vkUnmapMemory",
            "vkCreateShaderModule",
            "vkDestroyShaderModule",
            "vkCreateComputePipelines",
            "vkDestroyPipeline",
            "vkCmdDispatch",
            "vkCmdPipelineBarrier",
            "vkQueueSubmit",
            "vkWaitForFences",
            "vkDeviceWaitIdle",
        ):
            assert re.search(rf"UND\s+{symbol}\b", symbols), (
                abi, symbol
            )
        assert re.search(
            r"UND\s+vkEnumerateInstanceVersion\b", symbols
        ) is None
    else:
        assert "libvulkan.so" not in dynamic


def assert_mutations(
    native: list[str],
    full: list[str],
) -> None:
    """Reject falsified checkpoint inventory, submission and CPU result events.

    Each mutation alters a single saved event in memory. An independent
    verifier must reject the altered trace without touching the archive.
    """
    cases = (
        (native, "BACKEND_PUBLIC_FAULT_TRIGGER point=1 ", "resource=host_arrays", "resource=device", "native"),
        (native, "BACKEND_PUBLIC_FAULT_TRIGGER point=1 ", "simulated=1", "simulated=0", "native"),
        (native, "BACKEND_PUBLIC_FAULT_TRIGGER point=2 ", "result=-3", "result=0", "native"),
        (native, "BACKEND_PUBLIC_FAULT_TRIGGER point=36 ", "resource=before_submit", "resource=after_submit", "native"),
        (native, "BACKEND_PUBLIC_FAULT_TRIGGER point=37 ", "submitted=1", "submitted=0", "native"),
        (native, "BACKEND_PUBLIC_FAULT_RELEASE point=1 ", "host_arrays=4", "host_arrays=3", "native"),
        (native, "BACKEND_PUBLIC_FAULT_RELEASE point=2 ", "device=1", "device=0", "native"),
        (native, "BACKEND_PUBLIC_FAULT_RELEASE point=3 ", "buffers=1", "buffers=0", "native"),
        (native, "BACKEND_PUBLIC_FAULT_RELEASE point=4 ", "memories=1", "memories=0", "native"),
        (native, "BACKEND_PUBLIC_FAULT_RELEASE point=5 ", "maps=1", "maps=0", "native"),
        (native, "BACKEND_PUBLIC_FAULT_RELEASE point=18 ", "descriptor_layout=1", "descriptor_layout=0", "native"),
        (native, "BACKEND_PUBLIC_FAULT_RELEASE point=19 ", "pipeline_layout=1", "pipeline_layout=0", "native"),
        (native, "BACKEND_PUBLIC_FAULT_RELEASE point=20 ", "descriptor_pool=1", "descriptor_pool=0", "native"),
        (native, "BACKEND_PUBLIC_FAULT_RELEASE point=21 ", "descriptor_sets=1", "descriptor_sets=0", "native"),
        (native, "BACKEND_PUBLIC_FAULT_RELEASE point=22 ", "shaders=1", "shaders=0", "native"),
        (native, "BACKEND_PUBLIC_FAULT_RELEASE point=23 ", "pipelines=1", "pipelines=0", "native"),
        (native, "BACKEND_PUBLIC_FAULT_RELEASE point=33 ", "command_pool=1", "command_pool=0", "native"),
        (native, "BACKEND_PUBLIC_FAULT_RELEASE point=34 ", "command_buffer=1", "command_buffer=0", "native"),
        (native, "BACKEND_PUBLIC_FAULT_RELEASE point=35 ", "fence=1", "fence=0", "native"),
        (native, "BACKEND_PUBLIC_FAULT_RELEASE point=36 ", "submitted=0", "submitted=1", "native"),
        (native, "BACKEND_PUBLIC_FAULT_RELEASE point=37 ", "submitted=1", "submitted=0", "native"),
        (native, "BACKEND_PUBLIC_FAULT_RELEASE point=37 ", "wait_idle=0", "wait_idle=-4", "native"),
        (native, "BACKEND_PUBLIC_FAULT_RELEASE point=37 ", "completed=1", "completed=0", "native"),
        (native, "BACKEND_PUBLIC_FAULT_RELEASE point=37 ", "cleanup_result=0", "cleanup_result=-3", "native"),
        (native, "BACKEND_PUBLIC_FAULT_RECOVERY_PASS point=1 ", "new_device=1", "new_device=0", "native"),
        (native, "BACKEND_VULKAN_PROGRAM_DISPATCH_PASS ", "verified_words=256", "verified_words=255", "native"),
        (native, "BACKEND_VULKAN_PROGRAM_CLOSE ", "result=-3", "result=0", "native"),
        (native, "DEVICE_CATALOG_CLOSE ", "selections=74", "selections=73", "native"),
        (full, "TEST=", "vulkan-hashcat-public-fault", "vulkan-hashcat-fake", "full"),
        (full, "BACKEND_VULKAN_PROGRAM_ACCEPT stages=4 ", "buffers=5", "buffers=4", "full"),
        (full, "BACKEND_VULKAN_PROGRAM_DISPATCH_PASS ", "verified_words=256", "verified_words=255", "full"),
        (full, "BACKEND_VULKAN_CHAIN_DISPATCH_PASS ", "verified_second=64", "verified_second=63", "full"),
        (full, "BACKEND_VULKAN_KERNEL_DISPATCH_PASS ", "verified=64", "verified=63", "full"),
        (full, "DEVICE_CATALOG_CLOSE ", "selections=4", "selections=3", "full"),
        (full, "OpenCL Info:", "OpenCL Info:", "OpenGL Info:", "full"),
        (full, "HASHCAT_KERNEL_EXIT=0", "HASHCAT_KERNEL_EXIT=0", "HASHCAT_KERNEL_EXIT=1", "full"),
    )
    rejected = 0

    for original, prefix, old, new, kind in cases:
        changed = original.copy()
        found = [
            index for index, line in enumerate(changed)
            if line.startswith(prefix) and old in line
        ]
        assert found, (kind, prefix, old)
        index = found[0]
        changed[index] = changed[index].replace(old, new, 1)
        assert changed[index] != original[index]

        try:
            if kind == "native":
                assert_native(changed, "pixel-arm64", "1.0")
            else:
                assert_full(changed, "pixel-arm64")
        except AssertionError:
            rejected += 1
        else:
            raise AssertionError(
                f"Accepted falsified Vulkan fault evidence: {kind} {prefix} {old}"
            )

    assert rejected == len(cases)
    print(f"PASS {rejected} deliberately falsified Vulkan fault records rejected")


def main() -> None:
    """Verify archived Android logs and optionally exact ARM64/ARMv7 ELFs."""
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
    assert manifest["requested_vulkan_apis"] == list(APIS)
    assert manifest["physical_gpu_profiles"] == list(PROFILES)
    assert manifest["checkpoints"] == 37
    assert manifest["checkpoints_tested_on_real_gpu"] is True
    assert manifest["faults_are_test_control_not_driver_failures"] is True
    assert manifest["actual_driver_device_losses"] == 0
    assert manifest["actual_failed_queue_submissions_observed"] == 0
    assert manifest["actual_failed_fence_waits_observed"] == 0
    assert manifest["after_submit_real_queue_successes"] == 6
    assert manifest["after_submit_idle_succeeded"] is True
    assert manifest["hashcat_info_mode_only"] is True
    assert manifest["hashcat_hashing_scheduler_connected"] is False
    assert manifest["password_or_hash_inputs_processed"] is False
    assert manifest["cryptographic_spirv_kernels_ported"] == 0
    assert manifest["normal_opencl_discovery_preserved"] is True
    assert manifest["source_hash_policy"] == "CRLF normalized to LF"
    assert len(manifest["source_sha256"]) == 28

    for source, digest in manifest["source_sha256"].items():
        assert archived_source_sha256(source) == digest, source

    assert len(manifest["native_runs"]) == 6
    assert len(manifest["full_hashcat_info_runs"]) == 3
    assert len(manifest["invalid_cli"]) == 3
    assert len(list(ARCHIVE.glob("*.log"))) == 12

    expected_native = {
        (device, api) for device in PROFILES for api in APIS
    }
    observed_native = set()

    for row in manifest["native_runs"]:
        device, api = row["device"], row["requested_api"]
        assert (device, api) in expected_native
        assert (device, api) not in observed_native
        observed_native.add((device, api))
        assert row["abi"] == PROFILES[device][0]
        assert row["native_exit_code"] == 0
        assert row["real_instances"] == 1
        assert row["simulated_error_checkpoints"] == 37
        assert row["partial_resource_jobs_released"] == 37
        assert row["new_working_four_stage_devices"] == 37
        assert row["recovered_real_dispatches"] == 148
        assert row["cpu_verified_public_words"] == 9472
        assert row["after_submit_real_queue_successes"] == 1
        assert row["after_submit_unchecked_dispatches"] == 4

        path = ARCHIVE / row["file"]
        data = path.read_bytes()
        assert data.endswith(b"\n") and b"\r" not in data
        assert b"46281FDAS002AL" not in data
        assert b"91809037" not in data
        assert sha256(path) == row["sha256"]
        assert_native(path.read_text(encoding="utf-8").splitlines(), device, api)

    assert observed_native == expected_native

    observed_full = set()
    for row in manifest["full_hashcat_info_runs"]:
        device = row["device"]
        assert device in PROFILES and device not in observed_full
        observed_full.add(device)
        assert row["abi"] == PROFILES[device][0]
        assert row["native_exit_code"] == 0
        assert row["real_instances"] == 1
        assert row["one_stage_dispatches"] == 1
        assert row["two_stage_dispatches"] == 2
        assert row["four_stage_dispatches"] == 4
        assert row["cpu_verified_public_words"] == 448
        assert row["fault_injection_enabled"] is False
        assert row["ordinary_opencl_gpu_enumeration"] is True
        assert row["hashcat_hash_scheduler_enabled"] is False

        path = ARCHIVE / row["file"]
        assert sha256(path) == row["sha256"]
        assert_full(path.read_text(encoding="utf-8").splitlines(), device)

    assert observed_full == set(PROFILES)

    observed_invalid = set()
    for row in manifest["invalid_cli"]:
        device = row["device"]
        assert device in PROFILES and device not in observed_invalid
        observed_invalid.add(device)
        assert row["abi"] == PROFILES[device][0]
        assert row["malformed_cli_rejections"] == 3
        path = ARCHIVE / row["file"]
        assert sha256(path) == row["sha256"]
        assert_cli(path.read_text(encoding="utf-8").splitlines(), row["abi"])

    assert observed_invalid == set(PROFILES)

    assert manifest["totals"] == {
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
    }

    modules = manifest["spirv_modules"]
    assert set(modules) == {"public_u32_transform", "public_u32_rotate"}
    assert modules["public_u32_transform"]["words"] == 326
    assert modules["public_u32_transform"]["bytes"] == 1304
    assert modules["public_u32_rotate"]["words"] == 363
    assert modules["public_u32_rotate"]["bytes"] == 1452

    if args.native is not None:
        native_directory = args.native.resolve(strict=True)

        for abi, record in manifest["native_artifact_sha256"].items():
            executable = (
                native_directory / f"vulkan-backend-public-fault-{abi}"
            )
            object_path = (
                native_directory / f"hashcat-backend-vulkan-{abi}.o"
            )
            assert sha256(executable) == record["diagnostic"]
            assert sha256(object_path) == record["production_backend_object"]

            if args.readelf is not None:
                inspect_elf(
                    args.readelf.resolve(strict=True), executable, abi, True
                )

        for name, filename in (
            ("public_u32_transform", "vulkan-backend-public-stage1.spv"),
            ("public_u32_rotate", "vulkan-backend-public-stage2.spv"),
        ):
            path = native_directory / filename
            data = path.read_bytes()
            assert len(data) % 4 == 0
            words = struct.unpack("<" + "I" * (len(data) // 4), data)
            assert words[0] == 0x07230203
            assert words[1] == 0x00010000
            assert words[4] == 0
            assert sha256(path) == modules[name]["sha256"]

    if args.full is not None:
        full_directory = args.full.resolve(strict=True)

        for abi, record in manifest["full_hashcat_artifact_sha256"].items():
            frontend = full_directory / abi / "hashcat"
            module = full_directory / abi / "modules/module_00000.so"
            assert sha256(frontend) == record["frontend"]
            assert sha256(module) == record["module_00000"]

            if args.readelf is not None:
                readelf = args.readelf.resolve(strict=True)
                inspect_elf(readelf, frontend, abi, True)
                inspect_elf(readelf, module, abi, False)

        print("PASS exact Android ARM64/ARMv7 full HASHCAT and module SHA-256")

    assert_mutations(
        (ARCHIVE / "pixel-arm64-1.0.log").read_text(
            encoding="utf-8"
        ).splitlines(),
        (ARCHIVE / "pixel-arm64-hashcat-fault-info.log").read_text(
            encoding="utf-8"
        ).splitlines(),
    )

    print("PASS 6 real Adreno/Mali native Vulkan 1.0/1.1 processes")
    print("PASS 222 controlled post-acquisition errors and ownership inventories")
    print("PASS 222 fresh Vulkan devices with 56832 CPU-verified public words")
    print("PASS real after-submit idle cleanup, without claiming driver device loss")
    print("PASS 3 full normal HASHCAT -I public Vulkan and OpenCL processes")
    print("PASS 909 verified GPU dispatches and 58176 public uint32 outputs")


if __name__ == "__main__":
    main()
