#!/usr/bin/env python3
"""Independently verify public Vulkan 1.0 multi-stage GPU graph evidence.

Audits exact source/binary/log SHA-256 identities, storage/uniform descriptor
layout positions, memory requirements, stage ordering, GPU fence reuse,
coherent/noncoherent cache calls, all 2,048 output words represented by
independently calculated stage FNV-1a checksums, and rejected inputs.

This program does not run ADB, shaders, Vulkan or hashcat.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = ROOT / "tests/fixtures/android-gpu/vulkan-multiset-graph-20261031"
MASK32 = (1 << 32) - 1
MASK64 = (1 << 64) - 1

REQUIREMENTS = re.compile(
    r"^MULTI_REQUIREMENTS index=(\d+) label=([^ ]+) "
    r"size=(\d+) alignment=(\d+) bits=0x([0-9a-f]{8})$"
)
PLAN = re.compile(
    r"^GRAPH_PLAN_BUFFER index=(\d+) label=([^ ]+) "
    r"type=(\d+) heap=(\d+) bytes=(\d+) coherent=([01])$"
)
HEAP = re.compile(
    r"^GRAPH_PLAN_HEAP index=(\d+) reserved=(\d+) declared=(\d+)$"
)
COMPUTE = re.compile(
    r"^ACCEPT_COMPUTE index=(\d+) queue_family=(\d+) "
    r"local_max=(\d+) invocations_max=(\d+) groups_max=(\d+) "
    r"shared_max=(\d+) storage_range_max=(\d+) "
    r"descriptor_max=(\d+) non_coherent_atom=(\d+)$"
)
DESCRIPTOR = re.compile(
    r"^MULTI_DESCRIPTOR index=(\d+) set=(\d+) binding=(\d+) "
    r"type=(storage|uniform) buffer=(\d+) range=(\d+)$"
)
SYNC = re.compile(
    r"^MULTI_SYNC index=(\d+) operation=(flush|invalidate) "
    r"coherent=([01]) driver_call=([01]) atom=(\d+) result=0$"
)
PASS = re.compile(
    r"^MULTIGRAPH_PASS sets=(\d+) stages=(\d+) submission=(\d+) "
    r"stage=(\d+) words=2048 fnv=([0-9a-f]{16})$"
)


def sha256(path: Path) -> str:
    """Calculate the exact SHA-256 of the bytes stored at one path."""
    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_sha256(path: Path) -> str:
    """Hash a source with CRLF normalized to LF, as required by the manifest."""
    return hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")
    ).hexdigest()


def reference(stage: int, submission: int) -> str:
    """Calculate each output word independently with Python modulo 2**32."""
    assert 0 <= stage <= 2
    assert 0 <= submission <= 3

    factor = (1664525 + submission * 101) & MASK32
    bias = (1013904223 + submission * 97) & MASK32
    shift = 3 + 4 * submission
    salt = (0xA5A5A5A5 ^ (submission * 0x9E3779B9)) & MASK32
    digest = 14695981039346656037

    for index in range(2048):
        value = (
            index * 0x9E3779B9
            ^ (index << 15)
            ^ (index >> 3)
            ^ 0xA5A5A5A5
        ) & MASK32
        value ^= (submission * 0x9E3779B9) & MASK32

        for step in range(stage + 1):
            mixed = (
                value * (factor + step * 17)
                + bias + step * 12345
            ) & MASK32
            rotated = (
                (mixed << shift)
                | (mixed >> (32 - shift))
            ) & MASK32
            value = (
                (mixed ^ rotated)
                + (index ^ salt)
                + step * 0x9E3779B9
            ) & MASK32

        for byte in value.to_bytes(4, "little"):
            digest = ((digest ^ byte) * 1099511628211) & MASK64

    return f"{digest:016x}"


def matching(lines: list[str], pattern: re.Pattern[str]) -> list[re.Match[str]]:
    """Return only complete-line regular-expression matches, in log order."""
    return [
        match for line in lines
        if (match := pattern.fullmatch(line)) is not None
    ]


def exact_line(lines: list[str], value: str) -> None:
    """Require one exact log line without accepting a partial substitute."""
    assert lines.count(value) == 1, value


def verify_success(
    lines: list[str],
    sets: int,
    stages: int,
    mode: str,
    oracle: dict[tuple[int, int], str],
) -> None:
    """Validate a complete GPU graph run against declared resource contracts."""
    assert 1 <= sets <= 2
    assert 1 <= stages <= 3
    assert mode in ("default10", "explicit10", "required11", "noncoherent10")

    api = "1.1" if mode == "required11" else "1.0"
    buffers = 2 * stages + 3
    binding_count = stages + 2
    mapped_count = stages + 2
    acquisitions = 14 + 7 * stages + 2 * sets
    upload = stages + 1
    uniform = 2 * stages + 2

    assert any(row.startswith(f"SELECT_INSTANCE_API={api} ") for row in lines)
    assert any(row.startswith(f"SELECT_DEVICE_API={api} ") for row in lines)
    assert any(
        row.startswith("MULTIGRAPH_SELECTED_DEVICE index=0 ")
        and f"requested_api={api} effective_api={api} " in row
        and f"sets={sets} stages={stages} storage={stages + 1} " in row
        and "features=0 extensions=0" in row
        for row in lines
    )
    exact_line(
        lines,
        f"DESCRIPTOR_ACCEPT sets={sets} bindings={binding_count} "
        f"storage={stages + 1} uniform=1",
    )
    exact_line(
        lines,
        f"MULTIGRAPH_REQUEST sets={sets} stages={stages} "
        f"buffers={buffers} bindings={binding_count} "
        f"mapped={mapped_count} noncoherent={int(mode == 'noncoherent10')}",
    )
    exact_line(
        lines,
        f"MULTI_READY acquisitions={acquisitions} buffers={buffers} "
        f"sets={sets} bindings={binding_count} storage={stages + 1} "
        f"uniform=1 stages={stages} mapped={mapped_count} api=1.0",
    )
    exact_line(
        lines,
        f"MULTIGRAPH_ACCEPT stages={stages} sets={sets} "
        f"buffers={buffers} bytes=8192",
    )
    exact_line(
        lines,
        f"MULTI_DESTROY acquisitions={acquisitions} buffers={buffers} "
        f"memories={buffers} maps={mapped_count} layouts={sets} "
        f"sets={sets} shaders={stages} pipelines={stages} "
        "submissions=4 idle_result=0",
    )

    requirements = matching(lines, REQUIREMENTS)
    plan = matching(lines, PLAN)
    heaps = matching(lines, HEAP)
    compute = matching(lines, COMPUTE)
    descriptors = matching(lines, DESCRIPTOR)
    syncs = matching(lines, SYNC)
    passes = matching(lines, PASS)

    assert len(requirements) == buffers
    assert len(plan) == buffers
    assert len(heaps) >= 1
    assert len(compute) == 1
    assert int(compute[0].group(1)) == 0
    atom = int(compute[0].group(9))
    assert atom > 0
    assert len(descriptors) == binding_count
    assert len(syncs) == 4 * (stages + 2)
    assert len(passes) == 4 * stages

    for index, (requirement, planned) in enumerate(zip(requirements, plan)):
        assert int(requirement.group(1)) == index
        assert int(planned.group(1)) == index
        assert requirement.group(2) == planned.group(2)
        assert int(requirement.group(3)) == int(planned.group(5))
        assert int(requirement.group(4)) > 0
        memory_bits = int(requirement.group(5), 16)
        memory_type = int(planned.group(3))
        assert 0 <= memory_type < 32
        assert memory_bits & (1 << memory_type), (index, memory_bits, memory_type)
        assert int(planned.group(4)) >= 0
        assert (
            int(requirement.group(3)) == 8192
            if index != uniform else int(requirement.group(3)) >= 16
        )

    for heap_index, heap in enumerate(heaps):
        assert int(heap.group(1)) == heap_index
        reserved = sum(
            int(item.group(5)) for item in plan
            if int(item.group(4)) == heap_index
        )
        assert int(heap.group(2)) == reserved
        assert 0 <= reserved <= int(heap.group(3))

    for index, descriptor in enumerate(descriptors):
        assert int(descriptor.group(1)) == index

        if index <= stages:
            assert (
                int(descriptor.group(2)) ==
                (0 if index == 0 else sets - 1)
            )
            assert int(descriptor.group(3)) == 2 + index * 2
            assert descriptor.group(4) == "storage"
            assert int(descriptor.group(5)) == index
            assert int(descriptor.group(6)) == 8192
        else:
            assert int(descriptor.group(2)) == sets - 1
            assert int(descriptor.group(3)) == 10
            assert descriptor.group(4) == "uniform"
            assert int(descriptor.group(5)) == uniform
            assert int(descriptor.group(6)) == 16

    for layout in range(sets):
        expected_count = 1 if sets == 2 and layout == 0 else (
            binding_count - 1 if sets == 2 else binding_count
        )
        exact_line(
            lines,
            f"MULTI_SET_LAYOUT index={layout} bindings={expected_count}",
        )
        exact_line(lines, f"MULTI_SET_ALLOCATED index={layout}")

    for stage in range(stages):
        exact_line(
            lines,
            f"MULTIGRAPH_DISPATCH stage={stage} pipeline={stage} "
            f"input={stage} output={stage + 1} groups=64,1,1",
        )
        exact_line(
            lines,
            f"MULTIGRAPH_READBACK stage={stage} "
            f"gpu={stage + 1} host={upload + stage + 1}",
        )

        if stage + 1 < stages:
            exact_line(
                lines,
                f"MULTIGRAPH_BARRIER from={stage} "
                f"to={stage + 1} buffer={stage + 1}",
            )

    assert sum(line.startswith("MULTIGRAPH_BARRIER ") for line in lines) == stages - 1
    assert sum(line.startswith("MULTIGRAPH_DISPATCH ") for line in lines) == stages
    assert sum(line.startswith("MULTIGRAPH_READBACK ") for line in lines) == stages

    mapped = []
    for line in lines:
        match = re.fullmatch(
            r"MULTI_MAPPED index=(\d+) type=(\d+) coherent=([01])",
            line,
        )

        if match is not None:
            mapped.append(match)

    assert len(mapped) == mapped_count

    for step, item in enumerate(mapped):
        index = upload + step
        assert int(item.group(1)) == index
        assert int(item.group(2)) == int(plan[index].group(3))
        assert int(item.group(3)) == int(plan[index].group(6))

        if mode == "noncoherent10":
            assert int(item.group(3)) == 0

    for submission in range(4):
        exact_line(
            lines,
            f"MULTI_SUBMIT completed={submission + 1} "
            f"reused={int(submission != 0)} result=0",
        )

        expected_sync = (
            [(upload, "flush"), (uniform, "flush")]
            + [
                (upload + 1 + stage, "invalidate")
                for stage in range(stages)
            ]
        )

        for offset, (index, operation) in enumerate(expected_sync):
            sync = syncs[submission * (stages + 2) + offset]
            assert int(sync.group(1)) == index
            assert sync.group(2) == operation
            coherent = int(plan[index].group(6))
            assert int(sync.group(3)) == coherent
            assert int(sync.group(4)) == 1 - coherent
            assert int(sync.group(5)) == atom

            if mode == "noncoherent10":
                assert int(sync.group(3)) == 0
                assert int(sync.group(4)) == 1

        for stage in range(stages):
            match = passes[submission * stages + stage]
            assert int(match.group(1)) == sets
            assert int(match.group(2)) == stages
            assert int(match.group(3)) == submission
            assert int(match.group(4)) == stage
            assert match.group(5) == oracle[(submission, stage)]

    assert sum(row.startswith("MULTI_SUBMIT ") for row in lines) == 4
    assert sum(row.startswith("MULTIGRAPH_PASS ") for row in lines) == 4 * stages
    assert not any("MULTIGRAPH_MISMATCH" in row for row in lines)
    assert not any("MULTIGRAPH_NO_COMPATIBLE_CONFIGURATION" in row for row in lines)


def verify_incompatible(lines: list[str], sets: int, stages: int) -> None:
    """Require a precise Vulkan memory-type rejection without dispatching."""
    assert any(
        row.startswith("GRAPH_REJECT_MEMORY_TYPE index=")
        for row in lines
    )
    exact_line(lines, "MULTIGRAPH_CREATE_FAILED result=-8 fail_after=0")
    exact_line(lines, "MULTIGRAPH_NO_COMPATIBLE_CONFIGURATION")
    exact_line(
        lines,
        f"MULTIGRAPH_REQUEST sets={sets} stages={stages} "
        f"buffers={2 * stages + 3} bindings={stages + 2} "
        f"mapped={stages + 2} noncoherent=1",
    )

    assert not any(row.startswith("MULTIGRAPH_PASS ") for row in lines)
    assert not any(row.startswith("MULTI_SUBMIT ") for row in lines)
    assert not any(row.startswith("MULTIGRAPH_DISPATCH ") for row in lines)
    assert not any(row.startswith("MULTI_READY ") for row in lines)


def acquisition_ledger(sets: int, stages: int, point: int) -> dict[str, int]:
    """Calculate precisely which native Vulkan objects exist at a fault point."""
    buffers = 2 * stages + 3
    mapped = stages + 2

    objects = (
        ["device"]
        + ["buffer"] * buffers
        + ["memory"] * buffers
        + ["map"] * mapped
        + ["layout"] * sets
        + ["pipeline-layout"]
    )

    for _ in range(stages):
        objects.extend(("shader", "pipeline"))

    objects.extend(
        ["descriptor-pool"]
        + ["set"] * sets
        + ["command-pool", "command-buffer", "fence"]
    )

    assert len(objects) == 14 + 7 * stages + 2 * sets
    assert 1 <= point <= len(objects)

    return {
        kind: objects[:point].count(kind)
        for kind in ("buffer", "memory", "map", "layout", "set", "shader", "pipeline")
    }


def verify_faults(
    item: dict,
    supplied_lines: list[str] | None = None,
) -> int:
    """Audit actual or deliberately altered Vulkan partial-cleanup evidence."""
    path = ARCHIVE / item["file"]
    assert sha256(path) == item["sha256"]
    lines = (
        supplied_lines if supplied_lines is not None
        else path.read_text(encoding="utf-8").splitlines()
    )

    assert lines[:3] == [
        "TEST=vulkan-core10-multiset-graph-faults",
        f"ABI={item['abi']}",
        "CONFIGURATIONS=set1-stages1,set2-stages3",
    ]

    marker = re.compile(
        r"^FAULT_CASE sets=(\d+) stages=(\d+) point=(\d+) exit=42$"
    )
    markers = [
        (index, match)
        for index, line in enumerate(lines)
        if (match := marker.fullmatch(line)) is not None
    ]
    expected = (
        [(1, 1, point) for point in range(1, 24)]
        + [(2, 3, point) for point in range(1, 40)]
    )
    assert len(markers) == 62

    for index, ((position, match), (sets, stages, point)) in enumerate(
        zip(markers, expected)
    ):
        assert (
            int(match.group(1)),
            int(match.group(2)),
            int(match.group(3)),
        ) == (sets, stages, point)

        end = markers[index + 1][0] if index + 1 < len(markers) else len(lines)
        section = lines[position + 1:end]
        ledger = acquisition_ledger(sets, stages, point)

        assert sum(
            line.startswith(f"MULTI_INJECT_FAULT acquisition={point} resource=")
            for line in section
        ) == 1

        exact_line(
            section,
            f"MULTIGRAPH_CREATE_FAILED result=-3 fail_after={point}",
        )
        exact_line(section, "MULTIGRAPH_NO_COMPATIBLE_CONFIGURATION")
        exact_line(
            section,
            f"MULTI_DESTROY acquisitions={point} "
            f"buffers={ledger['buffer']} memories={ledger['memory']} "
            f"maps={ledger['map']} layouts={ledger['layout']} "
            f"sets={ledger['set']} shaders={ledger['shader']} "
            f"pipelines={ledger['pipeline']} submissions=0 idle_result=0",
        )

        assert not any(row.startswith("MULTI_SUBMIT ") for row in section)
        assert not any(row.startswith("MULTIGRAPH_PASS ") for row in section)
        assert not any(row.startswith("MULTIGRAPH_DISPATCH ") for row in section)

    assert item["set_stage_points"] == {"1:1": 23, "2:3": 39}

    return len(markers)


def verify_abi_heap_properties(manifest: dict) -> None:
    """Verify RedMagic ARM64 and ARMv7 retain distinct driver-local GPU heaps."""
    actual = {}

    for device in ("redmagic-arm64", "redmagic-armv7"):
        record = next(
            item for item in manifest["device_runs"]
            if item["device"] == device
            and item["sets"] == 2 and item["stages"] == 3
            and item["mode"] == "default10"
            and item["optimization"] == "O"
        )
        lines = (ARCHIVE / record["file"]).read_text(
            encoding="utf-8"
        ).splitlines()[7:]
        heaps = matching(lines, HEAP)
        assert len(heaps) == 2
        assert int(heaps[0].group(1)) == 0

        actual[device] = int(heaps[0].group(3))

    assert actual == {
        "redmagic-arm64": 11584221184,
        "redmagic-armv7": 4294967296,
    }

    print("PASS ABI-specific Adreno GPU heap capacities from native Vulkan")


def verify_mutations(
    manifest: dict,
    oracle: dict[tuple[int, int], str],
) -> None:
    """Reject altered Compute output, memory synchronization and cleanup logs."""
    target = next(
        item for item in manifest["device_runs"]
        if item["sets"] == 2 and item["stages"] == 3
        and item["mode"] == "default10"
        and item["optimization"] == "O"
    )
    original = (ARCHIVE / target["file"]).read_text(
        encoding="utf-8"
    ).splitlines()[7:]

    corruptions = (
        (
            "MULTIGRAPH_BARRIER from=1 to=2 buffer=2",
            "MULTIGRAPH_BARRIER from=1 to=2 buffer=0",
        ),
        (
            "MULTI_DESCRIPTOR index=4 set=1 binding=10 type=uniform",
            "MULTI_DESCRIPTOR index=4 set=1 binding=10 type=storage",
        ),
        (
            "MULTIGRAPH_PASS sets=2 stages=3 submission=3 stage=2 words=2048 fnv=007be5e7346adf5a",
            "MULTIGRAPH_PASS sets=2 stages=3 submission=3 stage=2 words=2048 fnv=0000000000000000",
        ),
        (
            "MULTI_SUBMIT completed=4 reused=1 result=0",
            "MULTI_SUBMIT completed=4 reused=0 result=0",
        ),
        (
            "MULTI_SYNC index=8 operation=flush coherent=1 driver_call=0",
            "MULTI_SYNC index=8 operation=flush coherent=1 driver_call=1",
        ),
    )

    for source, substitute in corruptions:
        altered = [
            row.replace(source, substitute)
            for row in original
        ]
        assert altered != original, source

        try:
            verify_success(altered, 2, 3, "default10", oracle)
        except AssertionError:
            print("PASS rejected corrupted evidence", source.split()[0])
        else:
            raise AssertionError(f"Accepted corrupted Vulkan record: {source}")

    pixel = next(
        item for item in manifest["device_runs"]
        if item["device"] == "pixel-arm64"
        and item["sets"] == 2 and item["stages"] == 3
        and item["mode"] == "noncoherent10"
        and item["optimization"] == "O"
    )
    pixel_lines = (ARCHIVE / pixel["file"]).read_text(
        encoding="utf-8"
    ).splitlines()[7:]
    cache_line = next(
        line for line in pixel_lines
        if line.startswith(
            "MULTI_SYNC index=8 operation=flush coherent=0 driver_call=1 "
        )
    )
    altered = [
        row.replace("driver_call=1", "driver_call=0")
        if row == cache_line else row
        for row in pixel_lines
    ]
    assert altered != pixel_lines

    try:
        verify_success(altered, 2, 3, "noncoherent10", oracle)
    except AssertionError:
        print("PASS rejected altered Pixel noncoherent uniform flush")
    else:
        raise AssertionError("Accepted missing Pixel Vulkan uniform flush")

    fault = next(
        item for item in manifest["fault_runs"]
        if item["device"] == "redmagic-armv7"
    )
    original_faults = (ARCHIVE / fault["file"]).read_text(
        encoding="utf-8"
    ).splitlines()
    destroy_line = next(
        line for line in original_faults
        if line.startswith(
            "MULTI_DESTROY acquisitions=39 buffers=9 memories=9"
        )
    )
    altered_faults = [
        row.replace("buffers=9 memories=9", "buffers=8 memories=9")
        if row == destroy_line else row
        for row in original_faults
    ]
    assert altered_faults != original_faults

    try:
        verify_faults(fault, altered_faults)
    except AssertionError:
        print("PASS rejected altered ARMv7 resource cleanup ledger")
    else:
        raise AssertionError("Accepted damaged Vulkan object teardown ledger")


def main() -> None:
    """Validate three hardware/ABI matrices, Vulkan faults and exact hashes."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--artifacts",
        type=Path,
        help="Directory containing the 14 exact native/SPIR-V build products",
    )
    args = parser.parse_args()

    manifest = json.loads(
        (ARCHIVE / "manifest.json").read_text(encoding="utf-8")
    )
    assert manifest["minimum_android_api"] == 24
    assert manifest["api_default"] == "1.0"
    assert manifest["physical_vulkan10_only_device_tested"] is False
    assert manifest["gpu_device_abi_tested"] == {
        "redmagic-arm64": "arm64-v8a",
        "redmagic-armv7": "armeabi-v7a",
        "pixel-arm64": "arm64-v8a",
    }

    for name, digest in manifest["source_sha256"].items():
        assert source_sha256(ROOT / name) == digest, name

    oracle = {
        (submission, stage): reference(stage, submission)
        for submission in range(4)
        for stage in range(3)
    }

    assert oracle[(0, 0)] == "da420145579e3ab0"
    assert oracle[(1, 1)] == "55af4d53d195f1b5"
    assert oracle[(3, 2)] == "007be5e7346adf5a"

    success_count = 0
    incompatible_count = 0
    verified_words = 0

    for item in manifest["device_runs"]:
        path = ARCHIVE / item["file"]
        assert sha256(path) == item["sha256"], item["file"]
        lines = path.read_text(encoding="utf-8").splitlines()
        assert lines[:7] == [
            "TEST=vulkan-core10-multiset-graph",
            f"SETS={item['sets']}",
            f"STAGES={item['stages']}",
            f"MODE={item['mode']}",
            f"OPTIMIZATION={item['optimization']}",
            f"ABI={item['abi']}",
            f"ADB_EXIT_CODE={item['exit_code']}",
        ]

        if item["exit_code"] == 0:
            verify_success(
                lines[7:], item["sets"], item["stages"],
                item["mode"], oracle
            )
            success_count += 1
            verified_words += 4 * 2048 * item["stages"]
        else:
            assert item["device"].startswith("redmagic")
            assert item["mode"] == "noncoherent10"
            assert item["exit_code"] == 1
            verify_incompatible(
                lines[7:], item["sets"], item["stages"]
            )
            incompatible_count += 1

    expected_invalid = (
        "missing-both", "zero-stages", "four-stages", "zero-sets",
        "three-sets", "five-submissions", "invalid-api", "zero-fault",
        "fault-over-limit", "missing-shader",
    )

    for item in manifest["invalid_runs"]:
        path = ARCHIVE / item["file"]
        assert sha256(path) == item["sha256"]
        lines = path.read_text(encoding="utf-8").splitlines()
        assert lines[:3] == [
            "TEST=vulkan-core10-multiset-graph-invalid-inputs",
            f"ABI={item['abi']}",
            f"INVALID_CASES={item['cases']}",
        ]
        assert lines[3:] == [
            f"INVALID_CASE={name} exit=2"
            for name in expected_invalid
        ]

    fault_count = sum(
        verify_faults(item) for item in manifest["fault_runs"]
    )

    assert len(manifest["device_runs"]) == 144
    assert success_count == 120, success_count
    assert incompatible_count == 24, incompatible_count
    assert verified_words == 1966080, verified_words
    assert fault_count == 186, fault_count
    assert len(manifest["invalid_runs"]) == 3
    assert len(manifest["fault_runs"]) == 3
    assert len(list(ARCHIVE.glob("*.log"))) == 150

    if args.artifacts is not None:
        directory = args.artifacts.resolve(strict=True)

        for name, digest in manifest["artifact_sha256"].items():
            assert sha256(directory / name) == digest, name

        assert len(manifest["artifact_sha256"]) == 14
        print("PASS all fourteen exact ARM64/ARMv7 and SPIR-V binaries")

    verify_abi_heap_properties(manifest)
    verify_mutations(manifest, oracle)

    print("PASS 19 canonical source identities")
    print("PASS 12 independent Python CPU stage/submission references")
    print("PASS 120 real GPU runs and 480 fence-separated submissions")
    print("PASS 1,966,080 real GPU stage words independently checked")
    print("PASS 12 actual Pixel noncoherent multi-stage GPU runs")
    print("PASS 24 explicit RedMagic noncoherent memory incompatibilities")
    print("PASS 186 Vulkan post-acquisition partial cleanup trials")
    print("PASS 30 malformed CLI controls and seven corrupted evidence rejections")


if __name__ == "__main__":
    main()
