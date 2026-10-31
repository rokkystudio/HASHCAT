#!/usr/bin/env python3
"""Audit real Vulkan 1.0 variable-resource-graph execution evidence.

Independently computes exact public integer stage A/B/C CPU references for
four GPU input generations. Verifies every submitted GPU output, sequential
fence command reuse, actual Vulkan memory compatibility by process ABI, and
real noncoherent cache flush/invalidate behavior.

Checks all partial-resource cleanups after 20, 24 and 28 successful Vulkan
acquisitions, expected memory incompatibilities, malformed CLI inputs and
two native C memory-planner test variants. All evidence is verified offline
using original LF logs, source SHA-256 and optional binary artifact hashes.
No GPU execution, credential processing or network access is performed.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct


ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = (
    ROOT / "tests/fixtures/android-gpu/vulkan-dynamic-graph-20261031"
)
DEVICES = {
    "redmagic-arm64": "arm64-v8a",
    "redmagic-armv7": "armeabi-v7a",
    "pixel-arm64": "arm64-v8a",
}
MODES = ("default10", "explicit10", "required11", "noncoherent10")
OPTIMIZATIONS = ("O0", "O")
EMPTY_HASH = "cbf29ce484222325"
FNV_PRIME = 1099511628211
MASK32 = 0xFFFFFFFF
MASK64 = 0xFFFFFFFFFFFFFFFF

SOURCES = {
    "tools/vulkan-core10-graph-memory.h",
    "tools/vulkan-core10-graph-memory.c",
    "tools/vulkan-core10-graph.h",
    "tools/vulkan-core10-graph.c",
    "tools/vulkan-core10-graph-diagnostic.c",
    "tools/test-vulkan-core10-graph-memory-host.c",
    "tools/build-vulkan-core10-graph.ps1",
    "tools/run-vulkan-core10-graph.ps1",
    "tools/archive-vulkan-core10-graph-evidence.py",
    "tools/test-vulkan-core10-graph-evidence.py",
    "tools/vulkan-auto-config.c",
    "tools/vulkan-auto-config.h",
    "tools/vulkan-core11-policy.c",
    "tools/vulkan-core11-policy.h",
    "tools/vulkan-memory-heap-policy.h",
    "tools/vulkan-pipeline-stage-a.comp",
    "tools/vulkan-pipeline-stage-b.comp",
    "tools/vulkan-pipeline-stage-c.comp",
}

ARTIFACTS = {
    "vulkan-dynamic-graph-arm64",
    "vulkan-dynamic-graph-armv7",
    "vulkan-graph-host-O0.exe",
    "vulkan-graph-host-O2.exe",
    *(
        f"stage-{stage}-{opt}-vk1.0.spv"
        for opt in OPTIMIZATIONS for stage in "abc"
    ),
}

HOST_PASSES = (
    "PASS_GRAPH_VARIABLE count=4 gpu=2 mapped=2",
    "PASS_GRAPH_VARIABLE count=5 gpu=3 mapped=2",
    "PASS_GRAPH_VARIABLE count=6 gpu=4 mapped=2",
    "PASS_GRAPH_VARIABLE count=7 gpu=5 mapped=2",
    "PASS_GRAPH_BACKTRACK six_buffers_exact_heap_capacity",
    "PASS_GRAPH_NONCOHERENT valid_and_explicit_type_rejection",
    "PASS_GRAPH_NEGATIVE allocation_limit_alignment_capacity_heap_zero_count",
    "PASS_GRAPH_MEMORY_POLICY dynamic_4_5_6_7 no_vulkan_driver",
)

INVALID_CASES = (
    "missing-stage-count",
    "zero-stages",
    "four-stages",
    "zero-submissions",
    "seventeen-submissions",
    "invalid-version",
    "zero-fault",
    "fault-over-limit",
    "missing-shader",
)

REQUIREMENT = re.compile(
    r"^GRAPH_REQUIREMENTS index=(\d+) label=([a-z-]+) "
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
    r"^GRAPH_MAPPED index=(\d+) type=(\d+) coherent=([01])$"
)
SYNC = re.compile(
    r"^GRAPH_SYNC index=(\d+) operation=(flush|invalidate) "
    r"coherent=([01]) driver_call=([01]) atom=(\d+) result=(-?\d+)$"
)
SUBMISSION = re.compile(
    r"^GRAPH_SUBMIT completed=(\d+) reused=([01]) result=0$"
)
OUTPUT = re.compile(
    r"^GRAPH_PASS stages=(\d+) submission=(\d+) "
    r"verified_words=(\d+) stageA=([0-9a-f]{16}) "
    r"stageB=([0-9a-f]{16}) output=([0-9a-f]{16})$"
)
DESTROY = re.compile(
    r"^GRAPH_DESTROY acquisitions=(\d+) buffers=(\d+) "
    r"memories=(\d+) maps=(\d+) shaders=(\d+) "
    r"pipelines=(\d+) submissions=(\d+) idle_result=(-?\d+)$"
)
FAULT = re.compile(
    r"^GRAPH_FAULT acquisition=(\d+) resource=([a-z-]+)$"
)


def sha256(path: Path) -> str:
    """Calculate exact bytes SHA-256 of one existing source/log/artifact."""
    assert path.is_file(), path

    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_sha256(path: Path) -> str:
    """Hash canonical source bytes independently of Windows LF checkout."""
    assert path.is_file(), path

    return hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")
    ).hexdigest()


def fnv64(words: list[int]) -> str:
    """Calculate FNV-1a64 of unsigned 32-bit little-endian output words."""
    hash_value = 14695981039346656037

    for word in words:
        for byte in struct.pack("<I", word):
            hash_value = (
                (hash_value ^ byte) * FNV_PRIME
            ) & MASK64

    return f"{hash_value:016x}"


def reference_generations() -> list[tuple[str, str, str]]:
    """Independently implement the three public SPIR-V shader formulas."""
    results = []

    for submission in range(4):
        inputs = [
            (
                (
                    (index * 0x9E3779B9)
                    ^ (index << 15)
                    ^ (index >> 3)
                    ^ 0xA5A5A5A5
                ) & MASK32
            ) ^ ((submission * 0x9E3779B9) & MASK32)
            for index in range(2048)
        ]

        stage_a = []

        for index, value in enumerate(inputs):
            affine = (value * 1664525 + 1013904223) & MASK32
            rotated = ((value << 7) | (value >> 25)) & MASK32
            stage_a.append(
                ((affine ^ rotated) + index * 2654435761) & MASK32
            )

        stage_b = []

        for index, value in enumerate(stage_a):
            neighbor = stage_a[
                (index & ~31) + ((index + 7) & 31)
            ]
            rotated = ((value << 11) | (value >> 21)) & MASK32
            stage_b.append(
                ((value + neighbor) ^ rotated ^ 0x6C8E9CF5) & MASK32
            )

        stage_c = []

        for a, b in zip(stage_a, stage_b, strict=True):
            rotated = ((b << 3) | (b >> 29)) & MASK32
            stage_c.extend((
                a, b, a ^ b,
                ((a * 2246822519 + b * 3266489917) ^ rotated) & MASK32,
            ))

        results.append((
            fnv64(stage_a),
            fnv64(stage_b),
            fnv64(stage_c),
        ))

    assert results == [
        (
            "de56d6d0aa66c6ec", "c1259ad4f024499a",
            "05de70c62f8d7cfd",
        ),
        (
            "66ee1b4b1385e0e3", "27fb4f34e64d89a7",
            "7509bfa6ed0455d8",
        ),
        (
            "69b8f65ae2e8be72", "d8038be5522a0b58",
            "e50a539d4459e45b",
        ),
        (
            "8fb63465a93a5631", "ac37a5c3b35fc366",
            "07d63cfd9773324d",
        ),
    ]

    return results


def memory_profile(
    manifest: dict, device: str
) -> tuple[dict[int, int], dict[int, tuple[int, int]]]:
    """Use only independently archived memory types of the running ABI."""
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

    for line in path.read_text(encoding="utf-8").splitlines():
        heap = re.fullmatch(
            r"DEVICE_HEAP index=(\d+) size=(\d+) flags=0x([0-9a-f]{8})",
            line,
        )

        if heap:
            index, size, flags = heap.groups()
            heaps[int(index)] = int(size)

        memory_type = re.fullmatch(
            r"DEVICE_MEMORY_TYPE index=(\d+) heap=(\d+) "
            r"flags=0x([0-9a-f]{8})",
            line,
        )

        if memory_type:
            index, heap_index, flags = memory_type.groups()
            types[int(index)] = (int(heap_index), int(flags, 16))

    assert len(heaps) == 2
    assert len(types) >= 4

    if device == "redmagic-armv7":
        assert heaps[0] == 4294967296
    elif device == "redmagic-arm64":
        assert heaps[0] == 11584221184

    return heaps, types


def require_api(rows: list[str], api: str, stages: int) -> None:
    """Verify the requested Vulkan version and explicit shader topology."""
    instances = [
        row for row in rows if row.startswith("SELECT_INSTANCE_API=")
    ]
    devices = [
        row for row in rows if row.startswith("SELECT_DEVICE_API=")
    ]
    selected = [
        row for row in rows if row.startswith("GRAPH_SELECTED_DEVICE ")
    ]

    assert len(instances) == len(devices) == len(selected) == 1
    assert instances[0].startswith(f"SELECT_INSTANCE_API={api} ")
    assert devices[0].startswith(f"SELECT_DEVICE_API={api} ")
    assert (
        f"requested_api={api} effective_api={api} stages={stages} "
        f"descriptors={stages + 1} "
    ) in selected[0]
    assert (
        "workgroup=32 groups=64 features=0 extensions=0"
    ) in selected[0]


def verify_requirements(
    manifest: dict,
    rows: list[str],
    device: str,
    stages: int,
    noncoherent: bool,
) -> tuple[list[re.Match], int]:
    """Check Vulkan requirements, type masks, exact heaps and two CPU maps."""
    count = stages + 3
    descriptors = stages + 1
    output_size = 32768 if stages == 3 else 8192
    expected_sizes = (
        [8192] * (descriptors - 1)
        + [output_size, 8192, output_size]
    )
    expected_labels = (
        ["gpu-input", "stage-a", "stage-b", "stage-c"][:descriptors]
        + ["staging-upload", "staging-download"]
    )

    requirements = [
        REQUIREMENT.fullmatch(row)
        for row in rows if row.startswith("GRAPH_REQUIREMENTS ")
    ]
    placements = [
        PLACEMENT.fullmatch(row)
        for row in rows if row.startswith("GRAPH_PLAN_BUFFER ")
    ]
    heap_lines = [
        HEAP.fullmatch(row)
        for row in rows if row.startswith("GRAPH_PLAN_HEAP ")
    ]
    maps = [
        MAPPED.fullmatch(row)
        for row in rows if row.startswith("GRAPH_MAPPED ")
    ]

    assert len(requirements) == len(placements) == count
    assert len(heap_lines) == len(maps) == 2
    assert all(
        item is not None
        for item in requirements + placements + heap_lines + maps
    )

    heaps, types = memory_profile(manifest, device)
    reserved = {index: 0 for index in heaps}

    for index in range(count):
        req_index, label, size, alignment, bits = (
            requirements[index].groups()
        )
        plan_index, plan_label, type_index, heap, allocation, coherent = (
            placements[index].groups()
        )

        assert int(req_index) == int(plan_index) == index
        assert label == plan_label == expected_labels[index]
        assert int(size) == int(allocation)
        assert int(allocation) >= expected_sizes[index]
        assert int(alignment) > 0
        assert int(alignment) & (int(alignment) - 1) == 0

        type_id = int(type_index)
        heap_id = int(heap)

        assert type_id in types
        assert heap_id == types[type_id][0]
        assert int(bits, 16) & (1 << type_id)
        assert int(coherent) == int(bool(types[type_id][1] & 4))
        assert not (types[type_id][1] & (32 | 64))

        if index >= descriptors:
            assert types[type_id][1] & 2

            if noncoherent:
                assert not (types[type_id][1] & 4)

        reserved[heap_id] += int(allocation)

    for index in range(2):
        entry = maps[index]
        buffer = descriptors + index
        placement = placements[buffer]

        assert int(entry.group(1)) == buffer
        assert int(entry.group(2)) == int(placement.group(3))
        assert int(entry.group(3)) == int(placement.group(6))

    for record in heap_lines:
        index, actual, declared = record.groups()
        heap_id = int(index)

        assert heap_id in heaps
        assert int(actual) == reserved[heap_id]
        assert int(declared) == heaps[heap_id]
        assert int(actual) <= int(declared)

    assert sum(reserved.values()) >= sum(expected_sizes)

    atom_lines = [
        row for row in rows if row.startswith("ACCEPT_COMPUTE ")
    ]

    assert len(atom_lines) == 1
    atom = re.search(
        r"non_coherent_atom=(\d+)$", atom_lines[0]
    )

    assert atom is not None and int(atom.group(1)) >= 1

    return placements, int(atom.group(1))


def verify_gpu(
    manifest: dict,
    rows: list[str],
    device: str,
    stages: int,
    mode: str,
    reference: list[tuple[str, str, str]],
) -> None:
    """Validate exact graph topology and every real GPU command submission."""
    requested_api = "1.1" if mode == "required11" else "1.0"

    require_api(rows, requested_api, stages)

    buffer_count = stages + 3
    descriptors = stages + 1
    acquisitions = 16 + 4 * stages
    output_bytes = 32768 if stages == 3 else 8192
    verified_words = 12288 if stages == 3 else 2048
    noncoherent = mode == "noncoherent10"

    request_lines = [
        row for row in rows if row.startswith("GRAPH_REQUEST ")
    ]
    ready = [
        row for row in rows if row.startswith("GRAPH_READY ")
    ]

    assert request_lines == [
        f"GRAPH_REQUEST stages={stages} buffers={buffer_count} "
        f"descriptors={descriptors} mapped=2 output_bytes={output_bytes} "
        f"requested_noncoherent={int(noncoherent)}"
    ]
    assert ready == [
        f"GRAPH_READY acquisitions={acquisitions} "
        f"buffers={buffer_count} descriptors={descriptors} "
        f"stages={stages} maps=2 core=1.0"
    ]

    placements, atom = verify_requirements(
        manifest, rows, device, stages, noncoherent
    )

    submits = [
        SUBMISSION.fullmatch(row)
        for row in rows if row.startswith("GRAPH_SUBMIT ")
    ]
    passes = [
        OUTPUT.fullmatch(row)
        for row in rows if row.startswith("GRAPH_PASS ")
    ]
    syncs = [
        SYNC.fullmatch(row)
        for row in rows if row.startswith("GRAPH_SYNC ")
    ]
    destroyed = [
        DESTROY.fullmatch(row)
        for row in rows if row.startswith("GRAPH_DESTROY ")
    ]

    assert len(submits) == len(passes) == 4
    assert len(syncs) == 8
    assert len(destroyed) == 1
    assert all(
        record is not None
        for record in submits + passes + syncs + destroyed
    )

    for submission in range(4):
        assert submits[submission].groups() == (
            str(submission + 1), str(int(submission > 0))
        )

        values = tuple(passes[submission].groups())
        a, b, c = reference[submission]

        expected_a = EMPTY_HASH if stages == 2 else a
        expected_b = EMPTY_HASH if stages == 1 else b
        expected_output = (
            a if stages == 1
            else (b if stages == 2 else c)
        )

        assert values == (
            str(stages), str(submission), str(verified_words),
            expected_a, expected_b, expected_output,
        )

        for offset, buffer_index, operation in (
            (0, descriptors, "flush"),
            (1, descriptors + 1, "invalidate"),
        ):
            match = syncs[2 * submission + offset]
            group_index, group_operation, coherent, called, size, result = (
                match.groups()
            )
            expected_coherent = int(
                placements[buffer_index].group(6)
            )

            assert int(group_index) == buffer_index
            assert group_operation == operation
            assert int(coherent) == expected_coherent
            assert int(called) == 1 - expected_coherent
            assert int(size) == atom
            assert int(result) == 0

    assert tuple(map(int, destroyed[0].groups())) == (
        acquisitions, buffer_count, buffer_count,
        2, stages, stages, 4, 0,
    )
    assert rows[-1].startswith("GRAPH_DESTROY ")


def verify_memory_rejection(
    manifest: dict,
    rows: list[str],
    device: str,
    stages: int,
) -> None:
    """Reject a forced staging memory type unsupported by this real GPU."""
    require_api(rows, "1.0", stages)

    buffer_count = stages + 3
    descriptor_count = stages + 1

    assert any(
        row == (
            f"GRAPH_REQUEST stages={stages} buffers={buffer_count} "
            f"descriptors={descriptor_count} mapped=2 "
            f"output_bytes={32768 if stages == 3 else 8192} "
            "requested_noncoherent=1"
        )
        for row in rows
    )
    assert (
        "GRAPH_CREATE_FAILED result=-8 fail_after=0" in rows
    )
    assert "GRAPH_NO_COMPATIBLE_CONFIGURATION" in rows
    assert not any(
        row.startswith("GRAPH_PASS ") or row.startswith("GRAPH_SUBMIT ")
        for row in rows
    )

    requirements = [
        REQUIREMENT.fullmatch(row)
        for row in rows if row.startswith("GRAPH_REQUIREMENTS ")
    ]

    assert len(requirements) == buffer_count
    assert all(value is not None for value in requirements)

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
    index, bits, forbidden = rejected[0].groups()

    assert int(index) in (descriptor_count, descriptor_count + 1)
    assert int(forbidden, 16) & 4
    assert int(bits, 16) == int(
        requirements[int(index)].group(5), 16
    )

    _, types = memory_profile(manifest, device)

    assert not any(
        (int(bits, 16) & (1 << memory_type))
        and (flags & 2)
        and not (flags & int(forbidden, 16))
        for memory_type, (_, flags) in types.items()
    )

    destroyed = [
        DESTROY.fullmatch(row)
        for row in rows if row.startswith("GRAPH_DESTROY ")
    ]

    assert len(destroyed) == 1 and destroyed[0] is not None
    assert tuple(map(int, destroyed[0].groups())) == (
        1 + buffer_count, buffer_count, 0, 0, 0, 0, 0, 0,
    )


def verify_device_logs(
    manifest: dict,
    reference: list[tuple[str, str, str]],
) -> None:
    """Verify all 72 original GPU and unsupported-memory trials."""
    records = manifest["device_runs"]

    assert len(records) == 72
    assert {
        (entry["device"], entry["stages"], entry["mode"], entry["optimization"])
        for entry in records
    } == {
        (device, stage, mode, opt)
        for device in DEVICES
        for stage in (1, 2, 3)
        for mode in MODES
        for opt in OPTIMIZATIONS
    }

    success_count = 0
    rejection_count = 0

    for entry in records:
        assert set(entry) == {
            "device", "abi", "stages", "mode", "optimization",
            "file", "exit_code", "sha256",
        }

        device = entry["device"]
        stages = entry["stages"]
        mode = entry["mode"]
        optimization = entry["optimization"]
        filename = f"{device}-stage{stages}-{mode}-{optimization}.log"

        assert entry["file"] == filename
        assert entry["abi"] == DEVICES[device]
        assert sha256(ARCHIVE / filename) == entry["sha256"]

        rows = (ARCHIVE / filename).read_text(
            encoding="utf-8"
        ).splitlines()

        unsupported = mode == "noncoherent10" and device.startswith(
            "redmagic"
        )
        expected_exit = int(unsupported)

        assert entry["exit_code"] == expected_exit
        assert rows[:6] == [
            "TEST=vulkan-core10-dynamic-graph",
            f"STAGES={stages}",
            f"MODE={mode}",
            f"OPTIMIZATION={optimization}",
            f"ABI={DEVICES[device]}",
            f"ADB_EXIT_CODE={expected_exit}",
        ]

        if unsupported:
            verify_memory_rejection(
                manifest, rows[6:], device, stages
            )
            rejection_count += 1
        else:
            verify_gpu(
                manifest, rows[6:], device,
                stages, mode, reference
            )
            success_count += 1

        print(
            f"PASS {device} stages={stages} {mode} -{optimization}: "
            + (
                "exact unsupported HOST_VISIBLE noncoherent memory type"
                if unsupported
                else "4 real GPU submissions with independent CPU reference"
            )
        )

    assert success_count == 60
    assert rejection_count == 12


def fault_names(stages: int) -> tuple[str, ...]:
    """List every genuine acquired Vulkan handle in exact creation order."""
    buffers = stages + 3

    return (
        "device",
        *("buffer",) * buffers,
        *("memory",) * buffers,
        "mapped-buffer", "mapped-buffer",
        "descriptor-layout", "pipeline-layout",
        *(
            name for _ in range(stages)
            for name in ("shader-module", "compute-pipeline")
        ),
        "descriptor-pool", "descriptor-set",
        "command-pool", "command-buffer", "fence",
    )


def expected_destroy(
    stages: int,
    point: int,
) -> tuple[int, ...]:
    """Independently derive all live Vulkan allocations after fault injection."""
    buffers = stages + 3
    shader_first = 2 * buffers + 6

    return (
        point,
        min(max(point - 1, 0), buffers),
        min(max(point - buffers - 1, 0), buffers),
        min(max(point - 2 * buffers - 1, 0), 2),
        sum(
            point >= shader_first + 2 * stage
            for stage in range(stages)
        ),
        sum(
            point >= shader_first + 2 * stage + 1
            for stage in range(stages)
        ),
        0,
        0,
    )


def verify_faults(manifest: dict) -> None:
    """Check 72 genuine partial-resource acquisition states per process ABI."""
    records = manifest["fault_runs"]

    assert len(records) == 3
    assert {entry["device"] for entry in records} == set(DEVICES)

    for entry in records:
        device = entry["device"]
        filename = f"{device}-faults.log"

        assert entry["file"] == filename
        assert entry["abi"] == DEVICES[device]
        assert entry["fault_points"] == 72
        assert sha256(ARCHIVE / filename) == entry["sha256"]

        rows = (ARCHIVE / filename).read_text(
            encoding="utf-8"
        ).splitlines()

        assert rows[:3] == [
            "TEST=vulkan-core10-dynamic-graph-faults",
            f"ABI={DEVICES[device]}",
            "STAGE_COUNTS=1,2,3",
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
        assert len(blocks) == 72

        expected_cases = [
            (stages, point)
            for stages in (1, 2, 3)
            for point in range(1, 17 + 4 * stages)
        ]

        for (stages, point), block in zip(
            expected_cases, blocks, strict=True
        ):
            assert block[0] == (
                f"FAULT_CASE stages={stages} point={point} exit=42"
            )

            names = fault_names(stages)
            assert len(names) == 16 + 4 * stages

            faults = [
                FAULT.fullmatch(row)
                for row in block if row.startswith("GRAPH_FAULT ")
            ]
            assert len(faults) == 1 and faults[0] is not None
            assert int(faults[0].group(1)) == point
            assert faults[0].group(2) == names[point - 1]

            destroyed = [
                DESTROY.fullmatch(row)
                for row in block if row.startswith("GRAPH_DESTROY ")
            ]
            assert len(destroyed) == 1
            assert destroyed[0] is not None
            assert tuple(map(int, destroyed[0].groups())) == (
                expected_destroy(stages, point)
            )

            assert (
                f"GRAPH_CREATE_FAILED result=-3 fail_after={point}"
            ) in block
            assert "GRAPH_NO_COMPATIBLE_CONFIGURATION" in block
            assert not any(
                row.startswith("GRAPH_SUBMIT ")
                or row.startswith("GRAPH_PASS ")
                for row in block
            )

        print(
            f"PASS {device}: 72 genuine Vulkan resource acquisition faults "
            "for variable 4/5/6-buffer graph ownership"
        )


def verify_invalid(manifest: dict) -> None:
    """Check exactly nine malformed commands per real Android ABI target."""
    records = manifest["invalid_runs"]

    assert len(records) == 3
    assert {item["device"] for item in records} == set(DEVICES)
    names = (
        "missing-stage-count",
        "zero-stages",
        "four-stages",
        "zero-submissions",
        "seventeen-submissions",
        "invalid-version",
        "zero-fault",
        "fault-over-limit",
        "missing-shader",
    )

    for entry in records:
        filename = f"{entry['device']}-negative-controls.log"

        assert entry["file"] == filename
        assert entry["abi"] == DEVICES[entry["device"]]
        assert entry["invalid_cases"] == 9
        assert sha256(ARCHIVE / filename) == entry["sha256"]

        rows = (ARCHIVE / filename).read_text(
            encoding="utf-8"
        ).splitlines()

        assert rows == [
            "TEST=vulkan-core10-dynamic-graph-invalid-inputs",
            f"ABI={entry['abi']}",
            "INVALID_CASES=9",
            *(f"INVALID_CASE={name} exit=2" for name in names),
        ]

    print("PASS all 27 malformed CLI controls on real ARM64 and ARMv7 processes")


def verify_native(manifest: dict) -> None:
    """Audit original optimized/unoptimized C memory policy test observations."""
    records = manifest["native_runs"]

    assert len(records) == 2
    assert {item["optimization"] for item in records} == {"O0", "O2"}
    reference = None

    for entry in records:
        optimization = entry["optimization"]
        filename = f"vulkan-graph-host-{optimization}.log"

        assert entry["file"] == filename
        assert entry["exit_code"] == 0
        assert sha256(ARCHIVE / filename) == entry["sha256"]

        rows = (ARCHIVE / filename).read_text(
            encoding="utf-8"
        ).splitlines()

        assert rows[:4] == [
            "TEST=vulkan-core10-dynamic-graph-host",
            f"OPTIMIZATION={optimization}",
            "GPU_DRIVER_LINKED=0",
            "EXIT_CODE=0",
        ]

        passes = tuple(
            line for line in rows if line.startswith("PASS_GRAPH_")
        )

        assert passes == HOST_PASSES

        if reference is None:
            reference = rows[4:]
        else:
            assert reference == rows[4:]

    print("PASS both native C dynamic memory policies at O0/O2: 4,5,6,7 buffers")


def verify_manifest(manifest: dict) -> list[tuple[str, str, str]]:
    """Audit exact source hashes and independent multi-stage CPU oracle."""
    assert manifest["date"] == "2026-10-31"
    assert manifest["scope"] == (
        "variable-size public synthetic Vulkan 1.0 Compute graph, "
        "one to three GPU stages with capability-driven memory"
    )
    assert manifest["source_hash_policy"] == (
        "UTF-8 with CRLF normalized to LF"
    )
    assert manifest["android_minimum_api"] == 24
    assert manifest["compiled_abis"] == ["arm64-v8a", "armeabi-v7a"]
    assert manifest["physical_vulkan10_only_device_tested"] is False
    assert manifest["submissions_per_run"] == 4
    assert manifest["shader_optimizations"] == list(OPTIMIZATIONS)
    assert manifest["modes"] == list(MODES)

    assert manifest["device_stage_topology"] == {
        str(stage): {
            "buffers": stage + 3,
            "descriptors": stage + 1,
            "pipelines": stage,
            "mapped_staging": 2,
            "acquisition_points": 16 + 4 * stage,
            "verified_words_per_submission": (
                12288 if stage == 3 else 2048
            ),
        }
        for stage in (1, 2, 3)
    }

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

    reference = reference_generations()

    print("PASS 18 canonical source SHA-256 and four independent CPU generations")

    return reference


def verify_artifacts(manifest: dict, directory: Path) -> None:
    """Verify ten real NDK/host/shader build artifact byte identities."""
    assert directory.is_dir()

    for name, digest in manifest["artifact_sha256"].items():
        assert sha256(directory / name) == digest, name

    print("PASS ten exact Android ARM64/ARMv7, SPIR-V and native C binary builds")


def verify_mutations(
    manifest: dict,
    reference: list[tuple[str, str, str]],
) -> None:
    """Reject forged Vulkan API, GPU output, cache and memory-placement logs."""
    record = next(
        item for item in manifest["device_runs"]
        if item["device"] == "pixel-arm64"
        and item["stages"] == 3
        and item["mode"] == "noncoherent10"
        and item["optimization"] == "O"
    )
    original = (ARCHIVE / record["file"]).read_text(
        encoding="utf-8"
    ).splitlines()[6:]

    mutations = (
        (
            "SELECT_INSTANCE_API=1.0 ",
            "SELECT_INSTANCE_API=1.1 ",
        ),
        (
            "GRAPH_SYNC index=4 operation=flush coherent=0 driver_call=1",
            "GRAPH_SYNC index=4 operation=flush coherent=0 driver_call=0",
        ),
        (
            "GRAPH_PLAN_BUFFER index=4 label=staging-upload type=1",
            "GRAPH_PLAN_BUFFER index=4 label=staging-upload type=5",
        ),
        (
            "GRAPH_SUBMIT completed=4 reused=1",
            "GRAPH_SUBMIT completed=4 reused=0",
        ),
        (
            "output=07d63cfd9773324d",
            "output=0000000000000000",
        ),
    )

    for original_text, substitute in mutations:
        corrupted = [
            row.replace(original_text, substitute)
            for row in original
        ]

        assert corrupted != original

        try:
            verify_gpu(
                manifest, corrupted,
                "pixel-arm64", 3, "noncoherent10", reference
            )
        except AssertionError:
            print(
                "PASS corruption rejected", original_text.split()[0]
            )
        else:
            raise AssertionError(
                f"Forged Vulkan GPU graph evidence accepted: {original_text}"
            )

    assert expected_destroy(3, 28) == (
        28, 6, 6, 2, 3, 3, 0, 0,
    )

    print("PASS exact resource ownership ledger independently recalculated")


def main() -> None:
    """Verify all original real-device and native Vulkan graph observations."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--artifacts", type=Path,
        help="Directory with exact ten native Android/host/SPIR-V artifacts",
    )
    arguments = parser.parse_args()

    manifest = json.loads(
        (ARCHIVE / "manifest.json").read_text(encoding="utf-8")
    )

    reference = verify_manifest(manifest)
    verify_native(manifest)
    verify_device_logs(manifest, reference)
    verify_faults(manifest)
    verify_invalid(manifest)

    if arguments.artifacts is not None:
        verify_artifacts(
            manifest, arguments.artifacts.resolve(strict=True)
        )

    verify_mutations(manifest, reference)

    expected = {
        record["file"]
        for record in (
            manifest["device_runs"]
            + manifest["fault_runs"]
            + manifest["invalid_runs"]
            + manifest["native_runs"]
        )
    }

    assert len(expected) == 80
    assert {path.name for path in ARCHIVE.glob("*.log")} == expected

    print(
        "PASS dynamic Vulkan1.0 graph: 60 real GPU runs, "
        "12 exact incompatible noncoherent controls, "
        "216 Vulkan acquisition-failure cleanups, "
        "27 invalid inputs; real ARM64 and ARMv7 GPU data"
    )


if __name__ == "__main__":
    main()
