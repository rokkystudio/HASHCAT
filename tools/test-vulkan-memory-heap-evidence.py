#!/usr/bin/env python3
"""Verify exact Vulkan heap planning and four real Android memory allocations.

The validator checks native C CPU-oracle results and the independently logged
Vulkan 1.0 memory heap properties from two real Android devices. It verifies
every actual buffer's memoryTypeBits, selected heap index, declared capacity,
allocation size, host mapping property and zero GPU dispatches.

It can also compare the locally rebuilt Windows/Android binaries with exact
SHA-256 identities, but never invokes ADB, Vulkan, hashcat or GPU computation.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = ROOT / "tests" / "fixtures" / "android-gpu" / "vulkan-heap-20261031"
MANIFEST = ARCHIVE / "manifest.json"
DEVICES = ("redmagic", "pixel")
OPTIMIZATIONS = ("O0", "O2")
ROLES = ("gpu-input", "gpu-output", "staging-upload", "staging-download")

HEAP_PROFILE = re.compile(
    r"^DEVICE_HEAP_PROFILE memory_types=(\d+) memory_heaps=(\d+) "
    r"max_allocations=(\d+)$"
)
HEAP = re.compile(
    r"^DEVICE_HEAP index=(\d+) size=(\d+) flags=0x([0-9a-f]{8})$"
)
TYPE = re.compile(
    r"^DEVICE_MEMORY_TYPE index=(\d+) heap=(\d+) flags=0x([0-9a-f]{8})$"
)
REQUIREMENTS = re.compile(
    r"^ACTUAL_REQUIREMENTS index=(\d+) "
    r"label=(gpu-input|gpu-output|staging-upload|staging-download) "
    r"size=(\d+) alignment=(\d+) memory_bits=0x([0-9a-f]{8})$"
)
PLAN = re.compile(
    r"^PLAN_BUFFER index=(\d+) "
    r"label=(gpu-input|gpu-output|staging-upload|staging-download) "
    r"type=(\d+) heap=(\d+) bytes=(\d+) coherent=([01])$"
)
HEAP_PLAN = re.compile(
    r"^PLAN_HEAP index=(\d+) reserved=(\d+) declared=(\d+)$"
)
ALLOCATED = re.compile(
    r"^ALLOCATED_BUFFER index=(\d+) type=(\d+) "
    r"heap=(\d+) bytes=(\d+)$"
)
MAPPED = re.compile(
    r"^MAPPED_STAGING index=(\d+) coherent=([01]) bytes=(\d+)$"
)
PASS = re.compile(
    r"^PASS_VULKAN_HEAP_PROBE device_index=(\d+) "
    r"buffers=4 allocations=4 mapped_staging=2 gpu_dispatches=0$"
)
EXHAUSTIVE = (
    "PASS_HEAP_EXHAUSTIVE layouts=64 heap_maps=8 masks=7 roles=4 sizes=2 "
    "cases=200704 accepted=78138 unsupported=24816 capacity=97750"
)
HOST_PASSES = (
    "PASS_HEAP_BACKTRACK alternate_type_prevents_false_incompatibility",
    "PASS_HEAP_FOUR_BUFFERS two_gpu_two_staging_exact_heap_sizes",
    "PASS_HEAP_NONCOHERENT_FALLBACK explicit_capacity_and_cache_flags",
    "PASS_HEAP_FAILURES capacity masks counts invalid_input untouched",
    "PASS_HEAP_BOUNDARIES bit31 unsigned64_capacity",
    EXHAUSTIVE,
    "PASS_HEAP_POLICY all six groups without Vulkan driver",
)


def sha256(path: Path) -> str:
    """Hash exact bytes of a compiled artifact or archived device log."""
    assert path.is_file(), f"Missing Vulkan heap evidence: {path}"

    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_digest(path: Path) -> str:
    """Hash canonical UTF-8 source with Windows CRLF normalized to LF."""
    assert path.is_file(), f"Missing Vulkan heap source: {path}"

    return hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")
    ).hexdigest()


def independent_oracle() -> tuple[int, int, int]:
    """Enumerate all 200,704 two-buffer heap-feasibility cases in Python.

    The oracle uses only the declared memory flags, heap assignments,
    compatibility masks and buffer sizes. It does not call the C algorithm.
    """
    classes = (1, 10, 6, 7)
    accepted = 0
    unsupported = 0
    capacity = 0

    for layout in range(64):
        flags = (
            classes[layout % 4],
            classes[(layout // 4) % 4],
            classes[(layout // 16) % 4],
        )

        for heap_layout in range(8):
            heaps = (
                heap_layout & 1,
                (heap_layout >> 1) & 1,
                (heap_layout >> 2) & 1,
            )

            for first_mask in range(1, 8):
                for second_mask in range(1, 8):
                    for role in range(4):
                        types_first = tuple(
                            index for index in range(3)
                            if first_mask & (1 << index)
                            and (not role & 1 or flags[index] & 2)
                        )
                        types_second = tuple(
                            index for index in range(3)
                            if second_mask & (1 << index)
                            and (not role & 2 or flags[index] & 2)
                        )

                        if not types_first or not types_second:
                            unsupported += 2
                            continue

                        for size_first, size_second in ((8, 8), (12, 4)):
                            feasible = any(
                                size_first <= (8, 12)[heaps[first]]
                                and size_second <= (8, 12)[heaps[second]]
                                and (
                                    heaps[first] != heaps[second]
                                    or size_first + size_second
                                    <= (8, 12)[heaps[first]]
                                )
                                for first in types_first
                                for second in types_second
                            )

                            if feasible:
                                accepted += 1
                            else:
                                capacity += 1

    assert accepted + unsupported + capacity == 200704

    return accepted, unsupported, capacity


def verify_provenance(manifest: dict) -> None:
    """Check exact C sources, toolchain identity and unchanged older modules."""
    assert manifest["date"] == "2026-10-31"
    assert manifest["scope"] == (
        "Vulkan 1.0 noncryptographic memory heap planning, host C exhaustive "
        "oracle and four real allocated/bound buffers on Android GPUs"
    )
    assert manifest["toolchains"] == {
        "native_gcc": "MSYS2 MinGW GCC 16.2.0",
        "native_target": "x86_64-w64-mingw32",
        "ndk_clang": "21.0.0",
        "ndk": "29.0.14206865",
        "android_target": "aarch64-linux-android26",
    }
    assert manifest["source_policy"] == "UTF-8 with CRLF normalized to LF"
    assert manifest["workload"] == {
        "vulkan_minimum": "1.0",
        "buffer_count": 4,
        "bytes_per_buffer": 8192,
        "roles": list(ROLES),
        "gpu_dispatches": 0,
        "host_optimizations": list(OPTIMIZATIONS),
        "exhaustive_cases": 200704,
        "expected_accepted": 78138,
        "expected_unsupported": 24816,
        "expected_capacity": 97750,
    }

    expected_sources = {
        "tools/vulkan-auto-config.c",
        "tools/vulkan-auto-config.h",
        "tools/vulkan-memory-heap-policy.c",
        "tools/vulkan-memory-heap-policy.h",
        "tools/vulkan-memory-heap-probe.c",
        "tools/test-vulkan-memory-heap-policy-host.c",
        "tools/build-vulkan-memory-heap-policy.ps1",
        "tools/run-vulkan-memory-heap-probe.ps1",
    }

    assert set(manifest["source_sha256"]) == expected_sources

    for name, digest in manifest["source_sha256"].items():
        assert re.fullmatch(r"[0-9a-f]{64}", digest)
        assert source_digest(ROOT / name) == digest, name

    previous = json.loads(
        (
            ROOT / "tests" / "fixtures" / "android-gpu"
            / "vulkan-autoconfig-20261031" / "manifest.json"
        ).read_text(encoding="utf-8")
    )

    for name in ("tools/vulkan-auto-config.c", "tools/vulkan-auto-config.h"):
        assert manifest["source_sha256"][name] == (
            previous["source_sha256"][name]
        )

    assert set(manifest["artifact_sha256"]) == {
        "vulkan-heap-policy-O0.exe",
        "vulkan-heap-policy-O2.exe",
        "vulkan-heap-probe-arm64",
    }

    for digest in manifest["artifact_sha256"].values():
        assert re.fullmatch(r"[0-9a-f]{64}", digest)

    assert manifest["host_imported_dlls"] == [
        "KERNEL32.dll", "msvcrt.dll"
    ]
    assert manifest["native_tests_use_gpu_driver"] is False
    assert manifest["android_gpu_dispatches"] == 0
    assert set(manifest["device_profiles"]) == set(DEVICES)

    print("PASS unchanged universal source SHA-256 and eight heap-policy source identities")
    print("PASS native Windows CPU tests do not import Vulkan/OpenCL libraries")


def verify_native_logs(manifest: dict) -> None:
    """Require matching exact C outcomes from both native optimization modes."""
    logs = manifest["host_runs"]

    assert len(logs) == 2
    assert {item["optimization"] for item in logs} == set(OPTIMIZATIONS)

    comparison = None

    for run in logs:
        assert set(run) == {
            "optimization", "file", "sha256", "exit_code",
        }
        assert run["exit_code"] == 0
        assert run["file"] == f"vulkan-heap-policy-{run['optimization']}.log"
        assert sha256(ARCHIVE / run["file"]) == run["sha256"]

        lines = (ARCHIVE / run["file"]).read_text(
            encoding="utf-8"
        ).splitlines()

        assert lines[:4] == [
            "TEST=synthetic-vulkan-heap-allocation-policy",
            f"OPTIMIZATION={run['optimization']}",
            "VULKAN_DRIVER_LINKED=0",
            "EXIT_CODE=0",
        ]

        passes = [line for line in lines if line.startswith("PASS_HEAP_")]

        assert tuple(passes) == HOST_PASSES
        assert lines[-1] == HOST_PASSES[-1]
        assert (
            "PLAN_BUFFER index=0 label=general-gpu type=1 "
            "heap=1 bytes=8 coherent=0"
        ) in lines
        assert (
            "PLAN_BUFFER index=1 label=restricted-gpu type=0 "
            "heap=0 bytes=8 coherent=0"
        ) in lines
        assert (
            "REJECT_ALLOCATION_COUNT existing=3 requested=2 maximum=4"
        ) in lines
        assert (
            "REJECT_HEAP_CAPACITY buffers=2 "
            "reason=no_combination_fits_declared_heaps"
        ) in lines

        if comparison is None:
            comparison = lines[4:]
        else:
            assert comparison == lines[4:], (
                "Host optimization changed the C heap selection behavior"
            )

        print(
            f"PASS native C -{run['optimization']}: 200704 heap plans, "
            "explicit backtracking, staging and allocation-count errors"
        )


def verify_device_log(lines: list[str], profile: dict) -> None:
    """Validate real memory heap indices and every allocated buffer resource."""
    assert lines[:2] == [
        "TEST=vulkan-1.0-memory-heap-probe", "ADB_EXIT_CODE=0",
    ]

    candidates = [
        line for line in lines if line.startswith("VULKAN_CANDIDATE")
    ]

    assert len(candidates) == 1

    for required in (
        f"name={profile['gpu']}",
        f"vendor={profile['vendor_id']}",
        f"device={profile['device_id']}",
        f"api={profile['vulkan_api_version']}",
        f"driver={profile['driver_version']}",
    ):
        assert required in candidates[0], required

    compute = [
        line for line in lines if line.startswith("ACCEPT_COMPUTE")
    ]

    assert len(compute) == 1
    assert "queue_family=0" in compute[0]

    profile_lines = [
        HEAP_PROFILE.fullmatch(line)
        for line in lines if line.startswith("DEVICE_HEAP_PROFILE")
    ]

    assert len(profile_lines) == 1 and profile_lines[0] is not None

    types_count, heaps_count, max_allocations = (
        int(value) for value in profile_lines[0].groups()
    )
    assert 1 <= types_count <= 32
    assert 1 <= heaps_count <= 16
    assert max_allocations >= 4

    heaps = [
        HEAP.fullmatch(line)
        for line in lines if line.startswith("DEVICE_HEAP index=")
    ]
    types = [
        TYPE.fullmatch(line)
        for line in lines if line.startswith("DEVICE_MEMORY_TYPE")
    ]
    requirements = [
        REQUIREMENTS.fullmatch(line)
        for line in lines if line.startswith("ACTUAL_REQUIREMENTS")
    ]
    planned = [
        PLAN.fullmatch(line)
        for line in lines if line.startswith("PLAN_BUFFER")
    ]
    heap_plans = [
        HEAP_PLAN.fullmatch(line)
        for line in lines if line.startswith("PLAN_HEAP")
    ]
    allocated = [
        ALLOCATED.fullmatch(line)
        for line in lines if line.startswith("ALLOCATED_BUFFER")
    ]
    mapped = [
        MAPPED.fullmatch(line)
        for line in lines if line.startswith("MAPPED_STAGING")
    ]

    assert len(heaps) == len(heap_plans) == heaps_count
    assert len(types) == types_count
    assert len(requirements) == len(planned) == len(allocated) == 4
    assert len(mapped) == 2
    assert all(
        item is not None
        for collection in (
            heaps, types, requirements, planned, heap_plans,
            allocated, mapped,
        )
        for item in collection
    )

    memory_heaps = {}
    allocations_per_heap = {i: 0 for i in range(heaps_count)}

    for heap, entry in enumerate(heaps):
        index, size, flags = entry.groups()

        assert int(index) == heap
        assert int(size) > 0
        assert int(flags, 16) >= 0

        memory_heaps[heap] = int(size)

        heap_index, reserved, declared = heap_plans[heap].groups()

        assert int(heap_index) == heap
        assert int(declared) == int(size)
        assert int(reserved) <= int(declared)

    memory_types = {}

    for index, entry in enumerate(types):
        recorded_index, heap, flags = entry.groups()

        assert int(recorded_index) == index
        assert int(heap) in memory_heaps

        memory_types[index] = {
            "heap": int(heap),
            "flags": int(flags, 16),
        }

    for index, role in enumerate(ROLES):
        req_index, req_role, size, alignment, bits = (
            requirements[index].groups()
        )
        plan_index, plan_role, selected_type, heap, planned_size, coherent = (
            planned[index].groups()
        )
        allocated_index, allocated_type, allocated_heap, allocated_size = (
            allocated[index].groups()
        )

        assert int(req_index) == int(plan_index) == int(allocated_index) == index
        assert req_role == plan_role == role
        assert int(size) >= 8192
        assert int(alignment) > 0
        assert int(alignment) & (int(alignment) - 1) == 0
        assert int(planned_size) == int(allocated_size) == int(size)

        selected_type = int(selected_type)
        heap = int(heap)

        assert 0 <= selected_type < types_count
        assert int(bits, 16) & (1 << selected_type)
        assert memory_types[selected_type]["heap"] == heap
        assert int(allocated_type) == selected_type
        assert int(allocated_heap) == heap
        assert int(coherent) == bool(
            memory_types[selected_type]["flags"] & 4
        )

        if index >= 2:
            assert memory_types[selected_type]["flags"] & 2

        assert not memory_types[selected_type]["flags"] & (32 | 64)

        allocations_per_heap[heap] += int(size)

    for heap in range(heaps_count):
        assert allocations_per_heap[heap] == int(
            heap_plans[heap].group(2)
        )
        assert allocations_per_heap[heap] <= memory_heaps[heap]

    for index, entry in enumerate(mapped):
        mapping_index, coherent, bytes_mapped = entry.groups()

        assert int(mapping_index) == index + 2
        assert int(bytes_mapped) == 8192
        assert int(coherent) == int(
            planned[index + 2].group(6)
        )

    passes = [
        PASS.fullmatch(line)
        for line in lines if line.startswith("PASS_VULKAN_HEAP_PROBE")
    ]

    assert len(passes) == 1 and passes[0] is not None

    candidate_index = re.search(
        r"^VULKAN_CANDIDATE index=(\d+) ", candidates[0]
    )

    assert candidate_index is not None
    assert passes[0].group(1) == candidate_index.group(1)
    assert all("gpu_dispatches=0" in line for line in (
        next(line for line in lines if line.startswith("PASS_VULKAN_HEAP_PROBE")),
    ))


def verify_actual_devices(manifest: dict) -> None:
    """Audit exact real Vulkan heap capacity and allocation/binding evidence."""
    runs = manifest["device_runs"]

    assert len(runs) == 2
    assert {run["device"] for run in runs} == set(DEVICES)

    for run in runs:
        assert set(run) == {
            "device", "file", "sha256", "exit_code",
        }
        assert run["exit_code"] == 0
        assert run["file"] == f"{run['device']}-allocation.log"
        assert sha256(ARCHIVE / run["file"]) == run["sha256"]

        lines = (ARCHIVE / run["file"]).read_text(
            encoding="utf-8"
        ).splitlines()

        verify_device_log(
            lines, manifest["device_profiles"][run["device"]],
        )

        print(
            f"PASS {run['device']} actual Vulkan buffer allocation: "
            "four buffers, two mappings, real heap limits, no GPU commands"
        )

    invalids = manifest["negative_runs"]

    assert len(invalids) == 2
    assert {run["device"] for run in invalids} == set(DEVICES)

    for run in invalids:
        assert set(run) == {"device", "file", "sha256", "exit_code"}
        assert run["exit_code"] == 2
        assert run["file"] == f"{run['device']}-negative-controls.log"
        assert sha256(ARCHIVE / run["file"]) == run["sha256"]

        assert (
            ARCHIVE / run["file"]
        ).read_text(encoding="utf-8").splitlines() == [
            "TEST=vulkan-1.0-memory-heap-probe",
            "INVALID unexpected-arguments exit=2",
        ]

    print("PASS two device invalid-argument rejections, exit code 2")


def verify_artifacts(manifest: dict, directory: Path) -> None:
    """Compare all three compiled binaries to their recorded raw SHA-256."""
    assert directory.is_dir()

    for name, digest in manifest["artifact_sha256"].items():
        assert sha256(directory / name) == digest, name

    print("PASS two native host and one Android ARM64 executable SHA-256")


def verify_corruption_controls(manifest: dict) -> None:
    """Reject a tampered heap reservation and a mismatched memory type."""
    first = manifest["device_runs"][0]
    original = (
        ARCHIVE / first["file"]
    ).read_text(encoding="utf-8").splitlines()
    profile = manifest["device_profiles"][first["device"]]

    changes = [
        (
            "PLAN_HEAP index=0 reserved=32768",
            "PLAN_HEAP index=0 reserved=32767",
            "invalid heap reservation",
        ),
        (
            "PLAN_BUFFER index=2 label=staging-upload type=6",
            "PLAN_BUFFER index=2 label=staging-upload type=0",
            "invalid staging memory type",
        ),
    ]

    for old, new, label in changes:
        mutated = [line.replace(old, new) for line in original]

        assert mutated != original, label

        try:
            verify_device_log(mutated, profile)
        except AssertionError:
            print(f"PASS negative control: {label} rejected")
        else:
            raise AssertionError(f"Corrupted Vulkan log accepted: {label}")


def main() -> None:
    """Verify CPU-only exhaustive tests and actual buffer-only Android evidence."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--artifacts", type=Path,
        help="Directory with the two rebuilt native CPU and ARM64 binaries",
    )
    args = parser.parse_args()
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))

    assert "adb_serial" not in MANIFEST.read_text(encoding="utf-8")

    verify_provenance(manifest)

    oracle = independent_oracle()

    assert oracle == (78138, 24816, 97750), oracle
    print(
        "PASS independent Python 200704-case heap oracle: "
        f"accepted={oracle[0]} unsupported={oracle[1]} capacity={oracle[2]}"
    )

    verify_native_logs(manifest)
    verify_actual_devices(manifest)

    if args.artifacts is not None:
        verify_artifacts(
            manifest, args.artifacts.resolve(strict=True)
        )

    verify_corruption_controls(manifest)

    expected_files = {
        run["file"]
        for run in (
            manifest["host_runs"]
            + manifest["device_runs"]
            + manifest["negative_runs"]
        )
    }
    assert {file.name for file in ARCHIVE.glob("*.log")} == expected_files

    print(
        "PASS C/independent Python heap planning and real Vulkan allocations; "
        "NO GPU COMPUTE, PASSWORDS OR HASHCAT BACKEND"
    )


if __name__ == "__main__":
    main()
