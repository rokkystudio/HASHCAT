#!/usr/bin/env python3
"""Independently audit original Vulkan 1.0 storage/uniform multi-set GPU logs.

Verifies all real one/two descriptor-set executions against a separate
integer CPU implementation, exact current-ABI Vulkan memory type masks
and coherent/noncoherent cache synchronization, plus 144 deterministic
post-acquisition resource cleanup trials and 27 invalid CLI results.

Source and original-log SHA-256 identities are mandatory; compiled binary
hashes can be verified with --artifacts. No Vulkan, ADB or password-processing
operation runs during this offline validation.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct


ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = ROOT / "tests/fixtures/android-gpu/vulkan-multiset-20261031"

DEVICES = {
    "redmagic-arm64": "arm64-v8a",
    "redmagic-armv7": "armeabi-v7a",
    "pixel-arm64": "arm64-v8a",
}
MODES = ("default10", "explicit10", "required11", "noncoherent10")
OPTIMIZATIONS = ("O0", "O")

SOURCES = {
    "tools/vulkan-core10-descriptor-policy.h",
    "tools/vulkan-core10-descriptor-policy.c",
    "tools/test-vulkan-core10-descriptor-policy-host.c",
    "tools/vulkan-core10-multiset.h",
    "tools/vulkan-core10-multiset.c",
    "tools/vulkan-core10-multiset-diagnostic.c",
    "tools/vulkan-multiset-one.comp",
    "tools/vulkan-multiset-two.comp",
    "tools/build-vulkan-core10-multiset.ps1",
    "tools/run-vulkan-core10-multiset.ps1",
    "tools/archive-vulkan-core10-multiset-evidence.py",
    "tools/test-vulkan-core10-multiset-evidence.py",
    "tools/vulkan-auto-config.c",
    "tools/vulkan-auto-config.h",
    "tools/vulkan-core11-policy.c",
    "tools/vulkan-core11-policy.h",
    "tools/vulkan-core10-graph-memory.c",
    "tools/vulkan-core10-graph-memory.h",
    "tools/vulkan-memory-heap-policy.h",
}

ARTIFACTS = {
    "vulkan-multiset-arm64",
    "vulkan-multiset-armv7",
    "multiset-descriptor-host-O0.exe",
    "multiset-descriptor-host-O2.exe",
    *(
        f"multiset-{variant}-{optimization}-vk1.0.spv"
        for variant in ("one", "two")
        for optimization in OPTIMIZATIONS
    ),
}

EXPECTED_DIGESTS = (
    "da420145579e3ab0",
    "3dc96f338cbbffac",
    "fc6cfc42aeaff265",
    "00ba58c7ff9a53e2",
)
HOST_PASSES = (
    "PASS_DESCRIPTOR_ONE_SET sparse_storage_and_uniform",
    "PASS_DESCRIPTOR_TWO_SETS independent_layouts_and_types",
    "PASS_DESCRIPTOR_LIMITS bound_sets_stage_resources_storage_uniform",
    "PASS_DESCRIPTOR_RANGES unsupported_type_wrong_usage_alignment_length",
    "PASS_DESCRIPTOR_STRUCTURE duplicate_empty_set_invalid_set_empty_input",
    "PASS_DESCRIPTOR_POLICY no_vulkan_driver no_model_profiles",
)
INVALID_NAMES = (
    "missing-sets",
    "zero-sets",
    "three-sets",
    "zero-submissions",
    "five-submissions",
    "invalid-api",
    "zero-fault",
    "fault-over-limit",
    "missing-shader",
)

MEMORY = re.compile(
    r"^MULTI_REQUIREMENTS index=(\d+) label=([a-z-]+) "
    r"size=(\d+) alignment=(\d+) bits=0x([0-9a-f]{8})$"
)
PLACEMENT = re.compile(
    r"^GRAPH_PLAN_BUFFER index=(\d+) label=([a-z-]+) "
    r"type=(\d+) heap=(\d+) bytes=(\d+) coherent=([01])$"
)
HEAP = re.compile(
    r"^GRAPH_PLAN_HEAP index=(\d+) reserved=(\d+) declared=(\d+)$"
)
MAPPED = re.compile(
    r"^MULTI_MAPPED index=(\d+) type=(\d+) coherent=([01])$"
)
DESCRIPTOR = re.compile(
    r"^MULTI_DESCRIPTOR index=(\d+) set=(\d+) binding=(\d+) "
    r"type=(storage|uniform) buffer=(\d+) range=(\d+)$"
)
SYNC = re.compile(
    r"^MULTI_SYNC index=(\d+) operation=(flush|invalidate) "
    r"coherent=([01]) driver_call=([01]) atom=(\d+) result=(-?\d+)$"
)
SUBMIT = re.compile(
    r"^MULTI_SUBMIT completed=(\d+) reused=([01]) result=0$"
)
PASS = re.compile(
    r"^MULTI_PASS sets=(\d+) submission=(\d+) "
    r"verified_words=2048 output=([0-9a-f]{16})$"
)
DESTROY = re.compile(
    r"^MULTI_DESTROY acquisitions=(\d+) buffers=(\d+) "
    r"memories=(\d+) maps=(\d+) layouts=(\d+) sets=(\d+) "
    r"shaders=(\d+) pipelines=(\d+) submissions=(\d+) "
    r"idle_result=(-?\d+)$"
)
FAULT = re.compile(
    r"^MULTI_INJECT_FAULT acquisition=(\d+) resource=([a-z-]+)$"
)


def sha256(path: Path) -> str:
    """Calculate the exact byte identity for one source or device record."""
    assert path.is_file(), path

    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_sha256(path: Path) -> str:
    """Calculate canonical UTF-8/LF source SHA-256 on a Windows checkout."""
    assert path.is_file(), path

    return hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")
    ).hexdigest()


def fnv64(words: list[int]) -> str:
    """Hash little-endian unsigned 32-bit GPU words with FNV-1a64."""
    digest = 14695981039346656037

    for word in words:
        for byte in struct.pack("<I", word):
            digest = ((digest ^ byte) * 1099511628211) & 0xFFFFFFFFFFFFFFFF

    return f"{digest:016x}"


def cpu_reference() -> list[str]:
    """Independently compute every public uniform-adjusted GPU output word."""
    mask = 0xFFFFFFFF
    digests = []

    for submission in range(4):
        factors = (
            (1664525 + 101 * submission) & mask,
            (1013904223 + 97 * submission) & mask,
            3 + 4 * submission,
            (0xA5A5A5A5 ^ ((submission * 0x9E3779B9) & mask)) & mask,
        )
        output = []

        for index in range(2048):
            base = (
                (index * 0x9E3779B9)
                ^ (index << 15)
                ^ (index >> 3)
                ^ 0xA5A5A5A5
            ) & mask
            value = base ^ ((submission * 0x9E3779B9) & mask)
            mixed = (value * factors[0] + factors[1]) & mask
            rotated = (
                (mixed << factors[2])
                | (mixed >> (32 - factors[2]))
            ) & mask

            output.append(
                ((mixed ^ rotated) + (index ^ factors[3])) & mask
            )

        digests.append(fnv64(output))

    assert tuple(digests) == EXPECTED_DIGESTS

    return digests


def gpu_memory_types(
    manifest: dict,
    device: str,
) -> tuple[dict[int, int], dict[int, tuple[int, int]]]:
    """Read actual Vulkan heap and memory type records for the same process ABI."""
    expected = {
        "redmagic-arm64": (
            "tests/fixtures/android-gpu/vulkan-heap-20261031/"
            "redmagic-allocation.log"
        ),
        "redmagic-armv7": (
            "tests/fixtures/android-gpu/vulkan-core-baseline-20261031/"
            "redmagic-armv7-memory-properties.log"
        ),
        "pixel-arm64": (
            "tests/fixtures/android-gpu/vulkan-heap-20261031/"
            "pixel-allocation.log"
        ),
    }[device]
    record = manifest["heap_reference_sha256"][device]

    assert record["file"] == expected
    path = ROOT / expected
    assert sha256(path) == record["sha256"]

    heaps = {}
    types = {}

    for row in path.read_text(encoding="utf-8").splitlines():
        heap = re.fullmatch(
            r"DEVICE_HEAP index=(\d+) size=(\d+) flags=0x([0-9a-f]{8})",
            row,
        )

        if heap:
            index, size, flags = heap.groups()
            heaps[int(index)] = int(size)

        memory_type = re.fullmatch(
            r"DEVICE_MEMORY_TYPE index=(\d+) heap=(\d+) "
            r"flags=0x([0-9a-f]{8})",
            row,
        )

        if memory_type:
            index, heap, flags = memory_type.groups()
            types[int(index)] = (int(heap), int(flags, 16))

    assert len(heaps) == 2 and len(types) >= 4

    if device == "redmagic-armv7":
        assert heaps[0] == 4294967296
    elif device == "redmagic-arm64":
        assert heaps[0] == 11584221184

    return heaps, types


def verify_api(rows: list[str], sets: int, mode: str) -> None:
    """Check explicit core Vulkan API and exact device capability selection."""
    expected_api = "1.1" if mode == "required11" else "1.0"
    instance = [
        row for row in rows if row.startswith("SELECT_INSTANCE_API=")
    ]
    device = [
        row for row in rows if row.startswith("SELECT_DEVICE_API=")
    ]
    selected = [
        row for row in rows if row.startswith("MULTI_SELECTED_DEVICE ")
    ]

    assert len(instance) == len(device) == len(selected) == 1
    assert instance[0].startswith(
        f"SELECT_INSTANCE_API={expected_api} "
    )
    assert device[0].startswith(f"SELECT_DEVICE_API={expected_api} ")
    assert (
        f"requested_api={expected_api} effective_api={expected_api} "
        f"sets={sets} storage=2 uniform=1 workgroup=32 groups=64 "
        "features=0 extensions=0"
    ) in selected[0]


def verify_descriptors(rows: list[str], sets: int) -> None:
    """Verify both real Vulkan storage and uniform descriptor binding arrays."""
    assert (
        f"DESCRIPTOR_ACCEPT sets={sets} bindings=3 storage=2 uniform=1"
    ) in rows

    layouts = [
        re.fullmatch(
            r"MULTI_SET_LAYOUT index=(\d+) bindings=(\d+)",
            row,
        )
        for row in rows
        if row.startswith("MULTI_SET_LAYOUT ")
    ]
    allocated = [
        re.fullmatch(
            r"MULTI_SET_ALLOCATED index=(\d+)",
            row,
        )
        for row in rows
        if row.startswith("MULTI_SET_ALLOCATED ")
    ]
    descriptors = [
        DESCRIPTOR.fullmatch(row)
        for row in rows
        if row.startswith("MULTI_DESCRIPTOR ")
    ]

    assert len(layouts) == len(allocated) == sets
    assert len(descriptors) == 3
    assert all(
        item is not None
        for item in layouts + allocated + descriptors
    )

    for index in range(sets):
        assert layouts[index].group(1) == str(index)
        assert allocated[index].group(1) == str(index)
        expected_count = 3 if sets == 1 else (1 if index == 0 else 2)
        assert layouts[index].group(2) == str(expected_count)

    expected = (
        ((0, 2, "storage", 0, 8192),
         (0, 7, "storage", 1, 8192),
         (0, 4, "uniform", 4, 16))
        if sets == 1 else
        ((0, 2, "storage", 0, 8192),
         (1, 1, "storage", 1, 8192),
         (1, 5, "uniform", 4, 16))
    )

    for index, item in enumerate(descriptors):
        row_index, set_number, binding, kind, buffer, size = item.groups()

        assert int(row_index) == index
        assert (
            int(set_number), int(binding), kind,
            int(buffer), int(size),
        ) == expected[index]


def verify_memory(
    rows: list[str],
    manifest: dict,
    device: str,
    require_noncoherent: bool,
) -> tuple[list[re.Match], int]:
    """Independently audit all five real Vulkan memory requirements and heaps."""
    requirements = [
        MEMORY.fullmatch(row)
        for row in rows if row.startswith("MULTI_REQUIREMENTS ")
    ]
    placements = [
        PLACEMENT.fullmatch(row)
        for row in rows if row.startswith("GRAPH_PLAN_BUFFER ")
    ]
    heap_lines = [
        HEAP.fullmatch(row)
        for row in rows if row.startswith("GRAPH_PLAN_HEAP ")
    ]
    mapped = [
        MAPPED.fullmatch(row)
        for row in rows if row.startswith("MULTI_MAPPED ")
    ]

    assert len(requirements) == len(placements) == 5
    assert len(heap_lines) == 2
    assert len(mapped) == 3
    assert all(
        item is not None
        for item in requirements + placements + heap_lines + mapped
    )

    expected_labels = (
        "gpu-input", "gpu-output",
        "staging-upload", "staging-download", "uniform-parameters",
    )
    sizes = (8192, 8192, 8192, 8192, 16)
    heaps, types = gpu_memory_types(manifest, device)
    reserved = {index: 0 for index in heaps}

    for index in range(5):
        i, label, size, alignment, bits = requirements[index].groups()
        j, plan_label, memory_type, heap, allocation, coherent = (
            placements[index].groups()
        )

        assert int(i) == int(j) == index
        assert label == plan_label == expected_labels[index]
        assert int(size) == int(allocation)
        assert int(allocation) >= sizes[index]
        assert int(alignment) > 0
        assert int(alignment) & (int(alignment) - 1) == 0

        selected_type = int(memory_type)
        selected_heap = int(heap)

        assert selected_type in types
        assert selected_heap == types[selected_type][0]
        assert int(bits, 16) & (1 << selected_type)
        assert int(coherent) == int(
            bool(types[selected_type][1] & 4)
        )
        assert not (types[selected_type][1] & (32 | 64))

        if index >= 2:
            assert types[selected_type][1] & 2

            if require_noncoherent:
                assert not (types[selected_type][1] & 4)

        reserved[selected_heap] += int(allocation)

    for index, buffer_index in enumerate((2, 3, 4)):
        mapped_index, memory_type, coherent = mapped[index].groups()
        plan = placements[buffer_index]

        assert int(mapped_index) == buffer_index
        assert int(memory_type) == int(plan.group(3))
        assert int(coherent) == int(plan.group(6))

    for item in heap_lines:
        heap, bytes_allocated, declared = item.groups()
        index = int(heap)

        assert index in heaps
        assert int(bytes_allocated) == reserved[index]
        assert int(declared) == heaps[index]
        assert int(bytes_allocated) <= int(declared)

    assert sum(reserved.values()) >= sum(sizes)

    atom_lines = [
        row for row in rows if row.startswith("ACCEPT_COMPUTE ")
    ]

    assert len(atom_lines) == 1
    atom = re.search(
        r"non_coherent_atom=(\d+)$", atom_lines[0]
    )

    assert atom is not None and int(atom.group(1)) >= 1

    return placements, int(atom.group(1))


def verify_success(
    rows: list[str],
    manifest: dict,
    device: str,
    sets: int,
    mode: str,
    expected: list[str],
) -> None:
    """Validate real descriptor layouts, memory, fence and every CPU result."""
    verify_api(rows, sets, mode)

    noncoherent = mode == "noncoherent10"
    max_fault = 21 + 2 * sets

    assert (
        f"MULTI_REQUEST sets={sets} buffers=5 bindings=3 "
        "storage=2 uniform=1 stages=1 mapped=3 "
        f"noncoherent={int(noncoherent)}"
    ) in rows
    assert (
        f"MULTI_READY acquisitions={max_fault} buffers=5 sets={sets} "
        "bindings=3 storage=2 uniform=1 stages=1 mapped=3 api=1.0"
    ) in rows

    verify_descriptors(rows, sets)
    placement, atom = verify_memory(
        rows, manifest, device, noncoherent
    )

    submit = [
        SUBMIT.fullmatch(row)
        for row in rows if row.startswith("MULTI_SUBMIT ")
    ]
    output = [
        PASS.fullmatch(row)
        for row in rows if row.startswith("MULTI_PASS ")
    ]
    sync = [
        SYNC.fullmatch(row)
        for row in rows if row.startswith("MULTI_SYNC ")
    ]
    destroyed = [
        DESTROY.fullmatch(row)
        for row in rows if row.startswith("MULTI_DESTROY ")
    ]

    assert len(submit) == len(output) == 4
    assert len(sync) == 12
    assert len(destroyed) == 1
    assert all(
        item is not None
        for item in submit + output + sync + destroyed
    )

    for submission in range(4):
        assert submit[submission].groups() == (
            str(submission + 1), str(int(submission > 0))
        )
        assert output[submission].groups() == (
            str(sets), str(submission), expected[submission]
        )

        for position, buffer, operation in (
            (0, 2, "flush"),
            (1, 4, "flush"),
            (2, 3, "invalidate"),
        ):
            index, current_op, coherent, invoked, current_atom, result = (
                sync[3 * submission + position].groups()
            )

            assert int(index) == buffer
            assert current_op == operation
            assert int(coherent) == int(placement[buffer].group(6))
            assert int(invoked) == 1 - int(coherent)
            assert int(current_atom) == atom
            assert int(result) == 0

    assert tuple(map(int, destroyed[0].groups())) == (
        max_fault, 5, 5, 3, sets, sets, 1, 1, 4, 0,
    )
    assert rows[-1].startswith("MULTI_DESTROY ")


def verify_incompatible_memory(
    rows: list[str],
    manifest: dict,
    device: str,
    sets: int,
) -> None:
    """Confirm absent exact HOST_VISIBLE noncoherent types without fallback."""
    verify_api(rows, sets, "noncoherent10")

    assert (
        f"MULTI_REQUEST sets={sets} buffers=5 bindings=3 "
        "storage=2 uniform=1 stages=1 mapped=3 noncoherent=1"
    ) in rows
    assert "MULTI_CREATE_FAILED result=-8 fail_after=0" in rows
    assert "MULTI_NO_COMPATIBLE_CONFIGURATION" in rows
    assert not any(
        row.startswith("MULTI_PASS ")
        or row.startswith("MULTI_SUBMIT ")
        for row in rows
    )

    requirements = [
        MEMORY.fullmatch(row)
        for row in rows if row.startswith("MULTI_REQUIREMENTS ")
    ]

    assert len(requirements) == 5
    assert all(item is not None for item in requirements)

    rejected = [
        re.fullmatch(
            r"GRAPH_REJECT_MEMORY_TYPE index=(\d+) role=1 "
            r"bits=0x([0-9a-f]{8}) forbidden=0x([0-9a-f]{8})",
            row,
        )
        for row in rows
        if row.startswith("GRAPH_REJECT_MEMORY_TYPE ")
    ]

    assert len(rejected) == 1 and rejected[0] is not None
    index, bitmask, forbidden = rejected[0].groups()

    assert int(index) in (2, 3, 4)
    assert int(forbidden, 16) & 4
    assert int(bitmask, 16) == int(
        requirements[int(index)].group(5), 16
    )

    _, types = gpu_memory_types(manifest, device)

    assert not any(
        int(bitmask, 16) & (1 << memory_type)
        and flags & 2
        and not (flags & int(forbidden, 16))
        for memory_type, (_, flags) in types.items()
    )

    destroyed = [
        DESTROY.fullmatch(row)
        for row in rows if row.startswith("MULTI_DESTROY ")
    ]

    assert len(destroyed) == 1 and destroyed[0] is not None
    assert tuple(map(int, destroyed[0].groups())) == (
        6, 5, 0, 0, 0, 0, 0, 0, 0, 0,
    )


def verify_device_runs(
    manifest: dict,
    reference: list[str],
) -> None:
    """Check every one of 48 original real-device run/unsupported records."""
    entries = manifest["device_runs"]

    assert len(entries) == 48
    assert {
        (entry["device"], entry["sets"], entry["mode"], entry["optimization"])
        for entry in entries
    } == {
        (device, sets, mode, optimization)
        for device in DEVICES
        for sets in (1, 2)
        for mode in MODES
        for optimization in OPTIMIZATIONS
    }

    successes = 0
    rejections = 0

    for entry in entries:
        assert set(entry) == {
            "device", "abi", "sets", "mode", "optimization",
            "file", "exit_code", "sha256",
        }

        device = entry["device"]
        sets = entry["sets"]
        mode = entry["mode"]
        optimization = entry["optimization"]
        filename = f"{device}-set{sets}-{mode}-{optimization}.log"

        assert entry["file"] == filename
        assert entry["abi"] == DEVICES[device]
        assert sha256(ARCHIVE / filename) == entry["sha256"]

        rows = (ARCHIVE / filename).read_text(
            encoding="utf-8"
        ).splitlines()

        incompatible = (
            mode == "noncoherent10"
            and device.startswith("redmagic")
        )
        expected_exit = int(incompatible)

        assert entry["exit_code"] == expected_exit
        assert rows[:6] == [
            "TEST=vulkan-core10-multiset",
            f"SETS={sets}",
            f"MODE={mode}",
            f"OPTIMIZATION={optimization}",
            f"ABI={DEVICES[device]}",
            f"ADB_EXIT_CODE={expected_exit}",
        ]

        if incompatible:
            verify_incompatible_memory(
                rows[6:], manifest, device, sets
            )
            rejections += 1
        else:
            verify_success(
                rows[6:], manifest, device, sets, mode, reference
            )
            successes += 1

        print(
            f"PASS {device} sets={sets} {mode} -{optimization}: "
            + (
                "precise noncoherent memory incompatibility"
                if incompatible
                else "4 real uniform-updated GPU submissions"
            )
        )

    assert successes == 40 and rejections == 8


def fault_resource_names(sets: int) -> tuple[str, ...]:
    """List each actually acquired Vulkan resource in exact creation order."""
    return (
        "device",
        *("buffer",) * 5,
        *("memory",) * 5,
        *("mapped-buffer",) * 3,
        *("set-layout",) * sets,
        "pipeline-layout",
        "shader-module",
        "compute-pipeline",
        "descriptor-pool",
        *("descriptor-set",) * sets,
        "command-pool",
        "command-buffer",
        "fence",
    )


def expected_destroy(
    sets: int,
    point: int,
) -> tuple[int, ...]:
    """Compute exact resources to release after one successful acquisition."""
    return (
        point,
        min(max(point - 1, 0), 5),
        min(max(point - 6, 0), 5),
        min(max(point - 11, 0), 3),
        min(max(point - 14, 0), sets),
        min(max(point - 18 - sets, 0), sets),
        int(point >= 16 + sets),
        int(point >= 17 + sets),
        0,
        0,
    )


def verify_faults(manifest: dict) -> None:
    """Audit all 144 real post-acquisition fault/partial cleanup states."""
    records = manifest["fault_runs"]

    assert len(records) == 3
    assert {entry["device"] for entry in records} == set(DEVICES)

    for entry in records:
        device = entry["device"]
        filename = f"{device}-faults.log"

        assert entry["file"] == filename
        assert entry["abi"] == DEVICES[device]
        assert entry["fault_points"] == 48
        assert sha256(ARCHIVE / filename) == entry["sha256"]

        rows = (ARCHIVE / filename).read_text(
            encoding="utf-8"
        ).splitlines()

        assert rows[:3] == [
            "TEST=vulkan-core10-multiset-faults",
            f"ABI={entry['abi']}",
            "SET_COUNTS=1,2",
        ]

        blocks = []
        current = []

        for row in rows[3:]:
            if row.startswith("FAULT_CASE "):
                if current:
                    blocks.append(current)

                current = [row]
            else:
                assert current
                current.append(row)

        assert current
        blocks.append(current)
        assert len(blocks) == 48

        cases = [
            (sets, point)
            for sets in (1, 2)
            for point in range(1, 22 + 2 * sets)
        ]

        for (sets, point), block in zip(
            cases, blocks, strict=True
        ):
            assert block[0] == (
                f"FAULT_CASE sets={sets} point={point} exit=42"
            )

            names = fault_resource_names(sets)
            assert len(names) == 21 + 2 * sets
            injection = [
                FAULT.fullmatch(row)
                for row in block
                if row.startswith("MULTI_INJECT_FAULT ")
            ]
            assert len(injection) == 1
            assert injection[0] is not None
            assert int(injection[0].group(1)) == point
            assert injection[0].group(2) == names[point - 1]

            teardown = [
                DESTROY.fullmatch(row)
                for row in block
                if row.startswith("MULTI_DESTROY ")
            ]
            assert len(teardown) == 1
            assert teardown[0] is not None
            assert tuple(map(int, teardown[0].groups())) == (
                expected_destroy(sets, point)
            )

            assert (
                f"MULTI_CREATE_FAILED result=-3 fail_after={point}"
            ) in block
            assert "MULTI_NO_COMPATIBLE_CONFIGURATION" in block
            assert not any(
                row.startswith("MULTI_PASS ")
                or row.startswith("MULTI_SUBMIT ")
                for row in block
            )

        print(
            f"PASS {device}: 48 real Vulkan object acquisitions with "
            "exact one/two-set partial cleanup"
        )


def verify_invalid(manifest: dict) -> None:
    """Audit all nine malformed CLI rejections on three actual Android ABIs."""
    entries = manifest["invalid_runs"]

    assert len(entries) == 3
    assert {entry["device"] for entry in entries} == set(DEVICES)

    for entry in entries:
        device = entry["device"]
        filename = f"{device}-negative-controls.log"

        assert entry["file"] == filename
        assert entry["abi"] == DEVICES[device]
        assert entry["invalid_cases"] == 9
        assert sha256(ARCHIVE / filename) == entry["sha256"]

        rows = (ARCHIVE / filename).read_text(
            encoding="utf-8"
        ).splitlines()

        assert rows == [
            "TEST=vulkan-core10-multiset-invalid-inputs",
            f"ABI={DEVICES[device]}",
            "INVALID_CASES=9",
            *(f"INVALID_CASE={name} exit=2" for name in INVALID_NAMES),
        ]

    print("PASS 27 strict invalid CLI rejections, real ARM64 and ARMv7")


def verify_native(manifest: dict) -> None:
    """Compare original driver-free C descriptor policy tests for O0/O2."""
    entries = manifest["native_runs"]

    assert len(entries) == 2
    assert {entry["optimization"] for entry in entries} == {"O0", "O2"}
    reference = None

    for entry in entries:
        optimization = entry["optimization"]
        filename = f"multiset-descriptor-host-{optimization}.log"

        assert entry["file"] == filename
        assert entry["exit_code"] == 0
        assert sha256(ARCHIVE / filename) == entry["sha256"]

        rows = (ARCHIVE / filename).read_text(
            encoding="utf-8"
        ).splitlines()

        assert rows[:4] == [
            "TEST=vulkan-core10-multiset-host-policy",
            f"OPTIMIZATION={optimization}",
            "GPU_DRIVER_LINKED=0",
            "EXIT_CODE=0",
        ]

        passes = tuple(
            row for row in rows if row.startswith("PASS_DESCRIPTOR_")
        )

        assert passes == HOST_PASSES

        if reference is None:
            reference = rows[4:]
        else:
            assert reference == rows[4:]

    print(
        "PASS strict native driver-free C descriptor policy, "
        "one/two sets and unsupported limits"
    )


def verify_manifest(manifest: dict) -> list[str]:
    """Verify 19 source identities, manifest dimensions and independent CPU."""
    assert manifest["date"] == "2026-10-31"
    assert manifest["scope"] == (
        "Vulkan 1.0 multi-set Compute with explicit STORAGE_BUFFER "
        "and UNIFORM_BUFFER descriptor layouts and noncoherent staging"
    )
    assert manifest["source_hash_policy"] == (
        "UTF-8 with CRLF normalized to LF"
    )
    assert manifest["minimum_android_api"] == 24
    assert manifest["requested_default_api"] == "1.0"
    assert manifest["physical_vulkan10_only_device_tested"] is False
    assert manifest["device_abi"] == DEVICES
    assert manifest["set_counts"] == [1, 2]
    assert manifest["descriptor_counts"] == {
        "storage": 2, "uniform": 1,
    }
    assert manifest["buffer_count"] == 5
    assert manifest["mapped_buffer_count"] == 3
    assert manifest["submissions_per_run"] == 4
    assert manifest["verified_words_per_submission"] == 2048
    assert manifest["resource_fault_counts"] == {
        "1": 23, "2": 25,
    }
    assert manifest["modes"] == list(MODES)
    assert manifest["optimizations"] == list(OPTIMIZATIONS)
    assert set(manifest["source_sha256"]) == SOURCES
    assert set(manifest["artifact_sha256"]) == ARTIFACTS
    assert set(manifest["heap_reference_sha256"]) == set(DEVICES)

    for name, digest in manifest["source_sha256"].items():
        assert re.fullmatch(r"[0-9a-f]{64}", digest)
        assert source_sha256(ROOT / name) == digest, name

    assert all(
        re.fullmatch(r"[0-9a-f]{64}", digest)
        for digest in manifest["artifact_sha256"].values()
    )

    reference = cpu_reference()
    assert tuple(reference) == EXPECTED_DIGESTS

    print(
        "PASS 19 canonical source identities and four independently "
        "computed CPU uniform generations"
    )

    return reference


def verify_artifacts(manifest: dict, directory: Path) -> None:
    """Verify exact two Android executables, four SPIR-V and two native tests."""
    assert directory.is_dir()

    for name, digest in manifest["artifact_sha256"].items():
        assert sha256(directory / name) == digest, name

    print("PASS eight real build artifacts: Android API24 both ABIs, SPIR-V, host C")


def verify_mutations(
    manifest: dict,
    reference: list[str],
) -> None:
    """Reject corrupt GPU uniform, descriptor, Vulkan API and fence evidence."""
    source = next(
        item for item in manifest["device_runs"]
        if item["device"] == "pixel-arm64"
        and item["sets"] == 2
        and item["mode"] == "noncoherent10"
        and item["optimization"] == "O"
    )
    original = (ARCHIVE / source["file"]).read_text(
        encoding="utf-8"
    ).splitlines()[6:]

    corruptions = (
        (
            "SELECT_INSTANCE_API=1.0 ",
            "SELECT_INSTANCE_API=1.1 ",
        ),
        (
            "MULTI_DESCRIPTOR index=2 set=1 binding=5 type=uniform",
            "MULTI_DESCRIPTOR index=2 set=1 binding=5 type=storage",
        ),
        (
            "MULTI_SYNC index=4 operation=flush coherent=0 driver_call=1",
            "MULTI_SYNC index=4 operation=flush coherent=0 driver_call=0",
        ),
        (
            "GRAPH_PLAN_BUFFER index=4 label=uniform-parameters type=1",
            "GRAPH_PLAN_BUFFER index=4 label=uniform-parameters type=5",
        ),
        (
            "MULTI_SUBMIT completed=4 reused=1",
            "MULTI_SUBMIT completed=4 reused=0",
        ),
        (
            "output=00ba58c7ff9a53e2",
            "output=0000000000000000",
        ),
    )

    for original_fragment, substitute in corruptions:
        altered = [
            row.replace(original_fragment, substitute)
            for row in original
        ]

        assert altered != original, original_fragment

        try:
            verify_success(
                altered, manifest, "pixel-arm64", 2,
                "noncoherent10", reference
            )
        except AssertionError:
            print(
                "PASS tampered GPU evidence rejected",
                original_fragment.split()[0],
            )
        else:
            raise AssertionError(
                f"Corrupted multi-set GPU evidence was accepted: {original_fragment}"
            )

    assert expected_destroy(1, 23) == (
        23, 5, 5, 3, 1, 1, 1, 1, 0, 0,
    )
    assert expected_destroy(2, 25) == (
        25, 5, 5, 3, 2, 2, 1, 1, 0, 0,
    )

    print("PASS independent exact one/two-set resource cleanup ledger")


def main() -> None:
    """Audit all original Vulkan GPU, fault, invalid input and native C data."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--artifacts", type=Path,
        help="Directory with eight exact Android/host/SPIR-V compiled artifacts",
    )
    args = parser.parse_args()

    manifest = json.loads(
        (ARCHIVE / "manifest.json").read_text(encoding="utf-8")
    )

    reference = verify_manifest(manifest)
    verify_native(manifest)
    verify_device_runs(manifest, reference)
    verify_faults(manifest)
    verify_invalid(manifest)

    if args.artifacts is not None:
        verify_artifacts(
            manifest, args.artifacts.resolve(strict=True)
        )

    verify_mutations(manifest, reference)

    expected = {
        item["file"]
        for item in (
            manifest["device_runs"]
            + manifest["fault_runs"]
            + manifest["invalid_runs"]
            + manifest["native_runs"]
        )
    }

    assert len(expected) == 56
    assert {path.name for path in ARCHIVE.glob("*.log")} == expected

    print(
        "PASS Vulkan 1.0 mixed storage/uniform descriptor sets: "
        "40 real GPU runs, 8 exact noncoherent incompatibilities, "
        "144 genuine resource-acquisition cleanup trials, "
        "27 malformed controls; tested real ARM64 and ARMv7"
    )


if __name__ == "__main__":
    main()
