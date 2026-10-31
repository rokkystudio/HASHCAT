#!/usr/bin/env python3
"""Verify original public Vulkan 1.0 runtime GPU and cleanup observations.

Independently recalculates four generations of synthetic 32-bit stage-A/B/C
outputs, validates real GPU memory type masks and heap capacities by process
ABI, and checks ordered command reuse and noncoherent cache management.

Also verifies 84 real Vulkan acquisition/cleanup fault trials, native
six-buffer C allocator test parity, original log identities and exact binary
provenance. This offline verifier never executes ADB, Vulkan or hashcat.
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
    ROOT / "tests/fixtures/android-gpu/vulkan-runtime-20261031"
)
DEVICES = {
    "redmagic-arm64": "arm64-v8a",
    "redmagic-armv7": "armeabi-v7a",
    "pixel-arm64": "arm64-v8a",
}
MODES = ("default10", "explicit10", "required11", "noncoherent10")
OPTIMIZATIONS = ("O0", "O")
ROLES = (
    "gpu-input", "stage-a", "stage-b", "gpu-output",
    "staging-upload", "staging-download",
)
REQUEST_BYTES = (8192, 8192, 8192, 32768, 8192, 32768)
FNV_OFFSET = 14695981039346656037
FNV_PRIME = 1099511628211
MASK32 = 0xFFFFFFFF
MASK64 = 0xFFFFFFFFFFFFFFFF
NATIVE_PASSES = (
    "PASS_MULTISTAGE_SIX all_resources capacity_and_roles",
    "PASS_MULTISTAGE_BACKTRACK six_requests_exact_placement",
    "PASS_MULTISTAGE_NONCOHERENT compatible_and_explicit_rejection",
    "PASS_MULTISTAGE_ERRORS count alignment heap capacity untouched",
    "PASS_MULTISTAGE_MEMORY_POLICY no_vulkan_driver",
)
FAULT_NAMES = (
    "device", *("buffer",) * 6, *("memory",) * 6,
    "mapped-staging", "mapped-staging",
    "descriptor-layout", "pipeline-layout",
    "shader-module", "compute-pipeline",
    "shader-module", "compute-pipeline",
    "shader-module", "compute-pipeline",
    "descriptor-pool", "descriptor-set",
    "command-pool", "command-buffer", "fence",
)
STAGE_HASHES = (
    (
        "de56d6d0aa66c6ec",
        "c1259ad4f024499a",
        "05de70c62f8d7cfd",
    ),
    (
        "66ee1b4b1385e0e3",
        "27fb4f34e64d89a7",
        "7509bfa6ed0455d8",
    ),
    (
        "69b8f65ae2e8be72",
        "d8038be5522a0b58",
        "e50a539d4459e45b",
    ),
    (
        "8fb63465a93a5631",
        "ac37a5c3b35fc366",
        "07d63cfd9773324d",
    ),
)

REQUIREMENTS = re.compile(
    r"^RUNTIME_REQUIREMENTS index=(\d+) "
    r"label=([a-z-]+) size=(\d+) alignment=(\d+) "
    r"bits=0x([0-9a-f]{8})$"
)
PLAN = re.compile(
    r"^MULTISTAGE_PLAN_BUFFER index=(\d+) "
    r"label=([a-z-]+) type=(\d+) heap=(\d+) "
    r"bytes=(\d+) coherent=([01])$"
)
HEAP = re.compile(
    r"^MULTISTAGE_PLAN_HEAP index=(\d+) "
    r"reserved=(\d+) declared=(\d+)$"
)
SYNC = re.compile(
    r"^RUNTIME_SYNC index=(\d+) operation=(flush|invalidate) "
    r"coherent=([01]) driver_call=([01]) atom=(\d+) result=(-?\d+)$"
)
SUBMIT = re.compile(
    r"^RUNTIME_SUBMIT complete=(\d+) command_reused=([01]) result=0$"
)
GPU_PASS = re.compile(
    r"^RUNTIME_PASS submission=(\d+) verified_words=12288 "
    r"stageA=([0-9a-f]{16}) stageB=([0-9a-f]{16}) "
    r"output=([0-9a-f]{16})$"
)
DESTROY = re.compile(
    r"^RUNTIME_DESTROY acquired=(\d+) buffers=(\d+) "
    r"allocations=(\d+) mapped=(\d+) shaders=(\d+) "
    r"pipelines=(\d+) submissions=(\d+) idle_result=(-?\d+)$"
)
INJECT = re.compile(
    r"^RUNTIME_INJECT_FAULT acquisition=(\d+) resource=([a-z-]+)$"
)


def sha256(path: Path) -> str:
    """Return an exact original file's SHA-256, rejecting missing artifacts."""
    assert path.is_file(), path

    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_sha256(path: Path) -> str:
    """Verify source identity regardless of Windows CRLF checkout policy."""
    assert path.is_file(), path

    return hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")
    ).hexdigest()


def fnv64(words: list[int]) -> str:
    """Hash unsigned public 32-bit words in fixed little-endian byte order."""
    digest = FNV_OFFSET

    for word in words:
        for byte in struct.pack("<I", word):
            digest = ((digest ^ byte) * FNV_PRIME) & MASK64

    return f"{digest:016x}"


def reference_generations() -> list[tuple[str, str, str]]:
    """Compute stage A, B and C using independent scalar CPU expressions."""
    results = []

    for submission in range(4):
        inputs = [
            (
                index * 0x9E3779B9
                ^ (index << 15)
                ^ (index >> 3)
                ^ 0xA5A5A5A5
            ) & MASK32 ^ ((submission * 0x9E3779B9) & MASK32)
            for index in range(2048)
        ]

        stage_a = []

        for index, value in enumerate(inputs):
            affine = (value * 1664525 + 1013904223) & MASK32
            rotate = ((value << 7) | (value >> 25)) & MASK32
            stage_a.append(
                ((affine ^ rotate) + index * 2654435761) & MASK32
            )

        stage_b = []

        for index, value in enumerate(stage_a):
            start = index & ~31
            neighbor = stage_a[start + ((index + 7) & 31)]
            rotate = ((value << 11) | (value >> 21)) & MASK32
            stage_b.append(
                ((value + neighbor) ^ rotate ^ 0x6C8E9CF5) & MASK32
            )

        output = []

        for a, b in zip(stage_a, stage_b, strict=True):
            rotate = ((b << 3) | (b >> 29)) & MASK32
            output.extend((
                a,
                b,
                a ^ b,
                (
                    (a * 2246822519 + b * 3266489917) ^ rotate
                ) & MASK32,
            ))

        results.append((
            fnv64(stage_a),
            fnv64(stage_b),
            fnv64(output),
        ))

    return results


def memory_profile(
    manifest: dict, device: str
) -> tuple[dict[int, int], dict[int, tuple[int, int]]]:
    """Load exact real Vulkan memory types and heap sizes from this process ABI."""
    source = manifest["heap_reference_sha256"][device]
    expected_path = {
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

    assert source["file"] == expected_path
    path = ROOT / source["file"]

    assert sha256(path) == source["sha256"]
    heaps = {}
    types = {}

    for row in path.read_text(encoding="utf-8").splitlines():
        heap = re.fullmatch(
            r"DEVICE_HEAP index=(\d+) size=(\d+) "
            r"flags=0x([0-9a-f]{8})",
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

    assert len(heaps) == 2
    assert len(types) >= 4

    if device == "redmagic-armv7":
        assert heaps[0] == 4294967296
    elif device == "redmagic-arm64":
        assert heaps[0] == 11584221184

    return heaps, types


def device_version(rows: list[str], expected: str) -> None:
    """Require the actually selected Vulkan instance and physical-device API."""
    instance = [
        row for row in rows if row.startswith("SELECT_INSTANCE_API=")
    ]
    device = [
        row for row in rows if row.startswith("SELECT_DEVICE_API=")
    ]
    selected = [
        row for row in rows if row.startswith("RUNTIME_SELECTED_DEVICE ")
    ]

    assert len(instance) == len(device) == len(selected) == 1
    assert instance[0].startswith(f"SELECT_INSTANCE_API={expected} ")
    assert device[0].startswith(f"SELECT_DEVICE_API={expected} ")
    assert (
        f"effective_api={expected} requested_api={expected}"
    ) in selected[0]
    assert (
        "workgroup=32 groups=64 storage_bindings=4 shared_bytes=128 "
        "features=0 extensions=0"
    ) in selected[0]


def check_memory_requests(
    rows: list[str],
    manifest: dict,
    device: str,
    noncoherent: bool,
) -> tuple[list[re.Match], int]:
    """Independently audit all six real Vulkan compatibility masks and heaps."""
    requirements = [
        REQUIREMENTS.fullmatch(row)
        for row in rows if row.startswith("RUNTIME_REQUIREMENTS ")
    ]
    plans = [
        PLAN.fullmatch(row)
        for row in rows if row.startswith("MULTISTAGE_PLAN_BUFFER ")
    ]
    heap_records = [
        HEAP.fullmatch(row)
        for row in rows if row.startswith("MULTISTAGE_PLAN_HEAP ")
    ]

    assert len(requirements) == len(plans) == 6
    assert len(heap_records) == 2
    assert all(
        value is not None
        for value in requirements + plans + heap_records
    )

    heaps, types = memory_profile(manifest, device)
    reserves = {index: 0 for index in heaps}

    for index in range(6):
        req_index, label, bytes_allocated, alignment, mask = (
            requirements[index].groups()
        )
        plan_index, plan_label, memory_type, heap_index, plan_bytes, coherent = (
            plans[index].groups()
        )

        assert int(req_index) == int(plan_index) == index
        assert label == plan_label == ROLES[index]
        assert int(bytes_allocated) == int(plan_bytes)
        assert int(bytes_allocated) >= REQUEST_BYTES[index]
        assert int(alignment) >= 1
        assert int(alignment) & (int(alignment) - 1) == 0

        type_index = int(memory_type)
        selected_heap = int(heap_index)

        assert type_index in types
        assert selected_heap == types[type_index][0]
        assert int(mask, 16) & (1 << type_index)
        assert int(coherent) == int(bool(types[type_index][1] & 4))
        assert types[type_index][1] & (32 | 64) == 0

        if index >= 4:
            assert types[type_index][1] & 2

            if noncoherent:
                assert types[type_index][1] & 4 == 0

        reserves[selected_heap] += int(plan_bytes)

    for heap in heap_records:
        index, reserved, declared = heap.groups()
        heap_index = int(index)

        assert heap_index in heaps
        assert int(reserved) == reserves[heap_index]
        assert int(declared) == heaps[heap_index]
        assert int(reserved) <= int(declared)

    assert sum(reserves.values()) >= sum(REQUEST_BYTES)

    atom_rows = [
        row for row in rows if row.startswith("ACCEPT_COMPUTE ")
    ]

    assert len(atom_rows) == 1
    atom = re.search(
        r"non_coherent_atom=(\d+)$", atom_rows[0]
    )

    assert atom is not None and int(atom.group(1)) >= 1

    return plans, int(atom.group(1))


def verify_success(
    rows: list[str],
    manifest: dict,
    device: str,
    mode: str,
    reference: list[tuple[str, str, str]],
) -> None:
    """Verify each GPU memory allocation, command reuse and CPU readback."""
    expected_api = "1.1" if mode == "required11" else "1.0"
    device_version(rows, expected_api)
    plans, atom = check_memory_requests(
        rows, manifest, device,
        noncoherent=mode == "noncoherent10",
    )

    assert sum(
        row.startswith("RUNTIME_READY acquisitions=28 buffers=6")
        for row in rows
    ) == 1
    assert sum(
        row.startswith("RUNTIME_STAGING_REQUEST mode=")
        for row in rows
    ) == 1

    staging = next(
        row for row in rows
        if row.startswith("RUNTIME_STAGING_REQUEST mode=")
    )
    expected_staging = (
        "noncoherent" if mode == "noncoherent10" else "auto"
    )

    assert staging.startswith(
        f"RUNTIME_STAGING_REQUEST mode={expected_staging} "
    )

    match = re.fullmatch(
        r"RUNTIME_STAGING_REQUEST mode=(auto|noncoherent) "
        r"upload_type=(\d+) download_type=(\d+) "
        r"upload_coherent=([01]) download_coherent=([01])",
        staging,
    )

    assert match is not None
    assert int(match.group(2)) == int(plans[4].group(3))
    assert int(match.group(3)) == int(plans[5].group(3))
    assert int(match.group(4)) == int(plans[4].group(6))
    assert int(match.group(5)) == int(plans[5].group(6))

    submit = [
        SUBMIT.fullmatch(row)
        for row in rows if row.startswith("RUNTIME_SUBMIT ")
    ]
    syncs = [
        SYNC.fullmatch(row)
        for row in rows if row.startswith("RUNTIME_SYNC ")
    ]
    completed = [
        GPU_PASS.fullmatch(row)
        for row in rows if row.startswith("RUNTIME_PASS ")
    ]
    destroy = [
        DESTROY.fullmatch(row)
        for row in rows if row.startswith("RUNTIME_DESTROY ")
    ]

    assert len(submit) == len(completed) == 4
    assert len(syncs) == 8
    assert len(destroy) == 1
    assert all(
        value is not None
        for value in submit + syncs + completed + destroy
    )

    for index in range(4):
        number, reused = submit[index].groups()

        assert int(number) == index + 1
        assert int(reused) == int(index > 0)
        assert int(completed[index].group(1)) == index
        assert tuple(completed[index].groups()[1:]) == reference[index]

        for cache_index, expected_buffer, expected_operation in (
            (2 * index, 4, "flush"),
            (2 * index + 1, 5, "invalidate"),
        ):
            stage = syncs[cache_index]
            buffer, operation, coherent, invoked, actual_atom, result = (
                stage.groups()
            )
            plan_coherent = int(plans[expected_buffer].group(6))

            assert int(buffer) == expected_buffer
            assert operation == expected_operation
            assert int(coherent) == plan_coherent
            assert int(invoked) == 1 - plan_coherent
            assert int(actual_atom) == atom
            assert int(result) == 0

    assert tuple(map(int, destroy[0].groups())) == (
        28, 6, 6, 2, 3, 3, 4, 0,
    )

    assert rows[-1].startswith("RUNTIME_DESTROY ")


def verify_unsupported_noncoherent(
    rows: list[str],
    manifest: dict,
    device: str,
) -> None:
    """Verify actual incompatible host-memory masks and no substituted type."""
    device_version(rows, "1.0")

    assert "RUNTIME_CREATE_FAILED result=-8 fault_after=0" in rows
    assert "RUNTIME_NO_COMPATIBLE_EXECUTION" in rows
    assert not any(row.startswith("RUNTIME_PASS ") for row in rows)
    assert not any(row.startswith("RUNTIME_SUBMIT ") for row in rows)
    assert not any(row.startswith("RUNTIME_SYNC ") for row in rows)

    requests = [
        REQUIREMENTS.fullmatch(row)
        for row in rows if row.startswith("RUNTIME_REQUIREMENTS ")
    ]

    assert len(requests) == 6
    assert all(request is not None for request in requests)

    rejection = [
        re.fullmatch(
            r"REJECT_MULTISTAGE_MEMORY_TYPE index=(\d+) "
            r"role=1 bits=0x([0-9a-f]{8}) "
            r"forbidden=0x([0-9a-f]{8})",
            row,
        )
        for row in rows
        if row.startswith("REJECT_MULTISTAGE_MEMORY_TYPE ")
    ]

    assert len(rejection) == 1 and rejection[0] is not None
    index, bits, forbidden = rejection[0].groups()

    assert int(index) in (4, 5)
    assert int(forbidden, 16) & 4
    assert int(bits, 16) == int(
        requests[int(index)].group(5), 16
    )

    _, types = memory_profile(manifest, device)

    assert not any(
        (int(bits, 16) & (1 << type_index))
        and (flags & 2)
        and not (flags & int(forbidden, 16))
        for type_index, (_, flags) in types.items()
    )

    destroyed = [
        DESTROY.fullmatch(row)
        for row in rows if row.startswith("RUNTIME_DESTROY ")
    ]

    assert len(destroyed) == 1 and destroyed[0] is not None
    assert tuple(map(int, destroyed[0].groups())) == (
        7, 6, 0, 0, 0, 0, 0, 0,
    )


def verify_devices(
    manifest: dict, reference: list[tuple[str, str, str]]
) -> None:
    """Audit all 24 original GPU or explicit incompatibility observations."""
    records = manifest["device_runs"]

    assert len(records) == 24
    assert {
        (record["device"], record["mode"], record["optimization"])
        for record in records
    } == {
        (device, mode, opt)
        for device in DEVICES
        for mode in MODES
        for opt in OPTIMIZATIONS
    }

    successes = 0
    incompatibilities = 0

    for record in records:
        assert set(record) == {
            "device", "abi", "mode", "optimization",
            "file", "exit_code", "sha256",
        }

        device = record["device"]
        mode = record["mode"]
        optimization = record["optimization"]
        filename = f"{device}-{mode}-{optimization}.log"

        assert record["file"] == filename
        assert record["abi"] == DEVICES[device]
        assert sha256(ARCHIVE / filename) == record["sha256"]

        rows = (ARCHIVE / filename).read_text(
            encoding="utf-8"
        ).splitlines()

        expected_exit = int(
            mode == "noncoherent10" and device.startswith("redmagic")
        )

        assert record["exit_code"] == expected_exit
        assert rows[:5] == [
            "TEST=vulkan-core10-reusable-runtime",
            f"MODE={mode}",
            f"OPTIMIZATION={optimization}",
            f"ABI={DEVICES[device]}",
            f"ADB_EXIT_CODE={expected_exit}",
        ]

        if expected_exit == 0:
            verify_success(
                rows[5:], manifest, device, mode, reference
            )
            successes += 1

            print(
                f"PASS {device} {mode} -{optimization}: "
                "4 sequential GPU submissions and 49,152 checked words"
            )
        else:
            verify_unsupported_noncoherent(
                rows[5:], manifest, device
            )
            incompatibilities += 1

            print(
                f"PASS {device} {mode} -{optimization}: "
                "explicit HOST_VISIBLE noncoherent incompatibility"
            )

    assert successes == 20
    assert incompatibilities == 4


def expected_fault_resources(
    step: int,
) -> tuple[int, int, int, int, int]:
    """Derive the exact live resource counts after one acquisition fault."""
    assert 1 <= step <= 28

    return (
        min(max(step - 1, 0), 6),
        min(max(step - 7, 0), 6),
        min(max(step - 13, 0), 2),
        sum(step >= point for point in (18, 20, 22)),
        sum(step >= point for point in (19, 21, 23)),
    )


def verify_faults(manifest: dict) -> None:
    """Audit every one of 28 partial Vulkan resource states on three ABIs."""
    records = manifest["fault_runs"]

    assert len(records) == 3
    assert {item["device"] for item in records} == set(DEVICES)
    assert len(FAULT_NAMES) == 28

    for record in records:
        device = record["device"]
        filename = f"{device}-faults.log"

        assert set(record) == {
            "device", "abi", "file", "sha256", "fault_points",
        }
        assert record["file"] == filename
        assert record["abi"] == DEVICES[device]
        assert record["fault_points"] == 28
        assert sha256(ARCHIVE / filename) == record["sha256"]

        lines = (ARCHIVE / filename).read_text(
            encoding="utf-8"
        ).splitlines()

        assert lines[:3] == [
            "TEST=vulkan-core10-runtime-faults",
            f"ABI={DEVICES[device]}",
            "FAULT_POINTS=28",
        ]

        blocks = []
        current = []

        for row in lines[3:]:
            if row.startswith("FAULT_POINT="):
                if current:
                    blocks.append(current)

                current = [row]
            else:
                assert current
                current.append(row)

        assert current
        blocks.append(current)
        assert len(blocks) == 28

        for step, block in enumerate(blocks, start=1):
            assert block[0] == f"FAULT_POINT={step} exit=1"

            injected = [
                INJECT.fullmatch(row)
                for row in block
                if row.startswith("RUNTIME_INJECT_FAULT ")
            ]

            assert len(injected) == 1
            assert injected[0] is not None
            assert int(injected[0].group(1)) == step
            assert injected[0].group(2) == FAULT_NAMES[step - 1]

            cleanup = [
                DESTROY.fullmatch(row)
                for row in block
                if row.startswith("RUNTIME_DESTROY ")
            ]

            assert len(cleanup) == 1 and cleanup[0] is not None
            fields = tuple(map(int, cleanup[0].groups()))

            assert fields == (
                step, *expected_fault_resources(step), 0, 0,
            )

            assert (
                f"RUNTIME_CREATE_FAILED result=-3 fault_after={step}"
            ) in block
            assert "RUNTIME_NO_COMPATIBLE_EXECUTION" in block
            assert not any(
                row.startswith("RUNTIME_SUBMIT ")
                or row.startswith("RUNTIME_PASS ")
                for row in block
            )

        print(
            f"PASS {device}: all 28 real Vulkan acquisition fault points "
            "and exact complete partial-resource cleanup"
        )


def verify_invalids(manifest: dict) -> None:
    """Verify five malformed CLI requests return 2 for every Android ABI."""
    records = manifest["invalid_runs"]

    assert len(records) == 3
    assert {item["device"] for item in records} == set(DEVICES)
    names = (
        "zero-submissions",
        "seventeen-submissions",
        "invalid-version",
        "invalid-fault",
        "missing-shader",
    )

    for record in records:
        device = record["device"]
        filename = f"{device}-negative-controls.log"

        assert record["file"] == filename
        assert record["abi"] == DEVICES[device]
        assert record["invalid_cases"] == 5
        assert sha256(ARCHIVE / filename) == record["sha256"]

        rows = (ARCHIVE / filename).read_text(
            encoding="utf-8"
        ).splitlines()

        assert rows == [
            "TEST=vulkan-core10-runtime-invalid-inputs",
            f"ABI={DEVICES[device]}",
            "INVALID_CASES=5",
            *(f"INVALID_CASE={name} exit=2" for name in names),
        ]

    print("PASS 15 invalid-input rejections on three real Android ABI targets")


def verify_native(manifest: dict) -> None:
    """Check exact independent six-buffer host C policy results at O0/O2."""
    records = manifest["native_runs"]

    assert len(records) == 2
    assert {record["optimization"] for record in records} == {"O0", "O2"}

    reference = None

    for record in records:
        optimization = record["optimization"]
        filename = f"vulkan-runtime-heap-{optimization}.log"

        assert record["file"] == filename
        assert record["exit_code"] == 0
        assert sha256(ARCHIVE / filename) == record["sha256"]

        rows = (ARCHIVE / filename).read_text(
            encoding="utf-8"
        ).splitlines()

        assert rows[:4] == [
            "TEST=vulkan-core10-runtime-heap-policy",
            f"OPTIMIZATION={optimization}",
            "GPU_DRIVER_LINKED=0",
            "EXIT_CODE=0",
        ]

        passes = tuple(
            line for line in rows
            if line.startswith("PASS_MULTISTAGE_")
        )

        assert passes == NATIVE_PASSES

        if reference is None:
            reference = rows[4:]
        else:
            assert reference == rows[4:]

    print("PASS O0/O2 native C memory planning, backtracking and negative controls")


def verify_manifest(manifest: dict) -> list[tuple[str, str, str]]:
    """Verify exact source provenance and independent four-generation CPU oracle."""
    assert manifest["date"] == "2026-10-31"
    assert manifest["scope"] == (
        "standalone Vulkan 1.0 resource owner, six buffer allocations, "
        "three public synthetic Compute stages and sequential reuse"
    )
    assert manifest["source_hash_policy"] == (
        "UTF-8 with CRLF normalized to LF"
    )
    assert manifest["workload"] == {
        "default_vulkan_api": "1.0",
        "optional_vulkan_api": "1.1",
        "minimum_android_api": 24,
        "spirv_version": "1.0",
        "local_size": 32,
        "group_count": 64,
        "storage_buffers": 4,
        "staging_buffers": 2,
        "shader_stages": 3,
        "copies_per_submission": 2,
        "verified_words_per_submission": 12288,
        "sequential_submissions": 4,
        "acquisition_fault_points": 28,
        "enabled_optional_features": 0,
        "enabled_extensions": 0,
    }

    sources = {
        "tools/vulkan-core10-runtime.h",
        "tools/vulkan-core10-runtime.c",
        "tools/vulkan-core10-runtime-diagnostic.c",
        "tools/build-vulkan-core10-runtime.ps1",
        "tools/run-vulkan-core10-runtime.ps1",
        "tools/archive-vulkan-core10-runtime-evidence.py",
        "tools/test-vulkan-core10-runtime-evidence.py",
        "tools/vulkan-auto-config.c",
        "tools/vulkan-auto-config.h",
        "tools/vulkan-core11-policy.c",
        "tools/vulkan-core11-policy.h",
        "tools/vulkan-multistage-memory-plan.c",
        "tools/vulkan-multistage-memory-plan.h",
        "tools/test-vulkan-multistage-memory-plan-host.c",
        "tools/vulkan-pipeline-stage-a.comp",
        "tools/vulkan-pipeline-stage-b.comp",
        "tools/vulkan-pipeline-stage-c.comp",
    }

    assert set(manifest["source_sha256"]) == sources

    for name, digest in manifest["source_sha256"].items():
        assert re.fullmatch(r"[0-9a-f]{64}", digest)
        assert source_sha256(ROOT / name) == digest, name

    artifacts = {
        "vulkan-runtime-arm64",
        "vulkan-runtime-armv7",
        "vulkan-runtime-heap-O0.exe",
        "vulkan-runtime-heap-O2.exe",
        *(
            f"stage-{stage}-{mode}-vk1.0.spv"
            for mode in OPTIMIZATIONS for stage in "abc"
        ),
    }

    assert set(manifest["artifact_sha256"]) == artifacts
    assert all(
        re.fullmatch(r"[0-9a-f]{64}", digest)
        for digest in manifest["artifact_sha256"].values()
    )
    assert set(manifest["heap_reference_sha256"]) == set(DEVICES)

    reference = reference_generations()

    assert tuple(reference) == STAGE_HASHES

    print("PASS 17 exact source identities and four independent CPU stage generations")

    return reference


def verify_corruptions(
    manifest: dict, reference: list[tuple[str, str, str]]
) -> None:
    """Detect fake GPU results, memory allocation types and fault cleanup."""
    entry = next(
        record for record in manifest["device_runs"]
        if record["device"] == "pixel-arm64"
        and record["mode"] == "noncoherent10"
        and record["optimization"] == "O"
    )
    original = (ARCHIVE / entry["file"]).read_text(
        encoding="utf-8"
    ).splitlines()[5:]

    mutations = (
        ("SELECT_INSTANCE_API=1.0 ", "SELECT_INSTANCE_API=1.1 "),
        ("RUNTIME_SYNC index=4 operation=flush coherent=0 driver_call=1",
         "RUNTIME_SYNC index=4 operation=flush coherent=0 driver_call=0"),
        ("MULTISTAGE_PLAN_BUFFER index=4 label=staging-upload type=1",
         "MULTISTAGE_PLAN_BUFFER index=4 label=staging-upload type=5"),
        ("RUNTIME_SUBMIT complete=4 command_reused=1",
         "RUNTIME_SUBMIT complete=4 command_reused=0"),
        ("stageB=27fb4f34e64d89a7", "stageB=0000000000000000"),
    )

    for old, new in mutations:
        corrupted = [row.replace(old, new) for row in original]

        assert corrupted != original

        try:
            verify_success(
                corrupted, manifest, "pixel-arm64",
                "noncoherent10", reference
            )
        except (AssertionError, StopIteration):
            print("PASS negative control rejected", old.split()[0])
        else:
            raise AssertionError(
                f"Forged Vulkan runtime GPU evidence was accepted: {old}"
            )

    record = next(
        item for item in manifest["fault_runs"]
        if item["device"] == "redmagic-arm64"
    )
    original_faults = (ARCHIVE / record["file"]).read_text(
        encoding="utf-8"
    ).splitlines()
    forged_faults = [
        row.replace("buffers=6 allocations=6", "buffers=6 allocations=5")
        if row.startswith("RUNTIME_DESTROY acquired=28")
        else row
        for row in original_faults
    ]

    assert forged_faults != original_faults

    # The structural verifier must reject any incorrect acquired/released
    # resource count for the last injected Vulkan object.
    last = next(
        row for row in forged_faults
        if row.startswith("RUNTIME_DESTROY acquired=28")
    )
    match = DESTROY.fullmatch(last)

    assert match is not None
    assert tuple(map(int, match.groups())) != (
        28, *expected_fault_resources(28), 0, 0,
    )

    print("PASS negative control rejected altered Vulkan cleanup count")


def verify_artifacts(manifest: dict, directory: Path) -> None:
    """Verify exact compiled Android/host executables and SPIR-V bytes."""
    assert directory.is_dir()

    for name, digest in manifest["artifact_sha256"].items():
        assert sha256(directory / name) == digest, name

    print("PASS 10 exact built artifacts: two Android ABIs, six SPIR-V, two CPU tests")


def main() -> None:
    """Run all offline real-hardware, source and fault-evidence validations."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--artifacts",
        type=Path,
        help="Directory with ten exact native and SPIR-V artifacts",
    )
    args = parser.parse_args()

    manifest = json.loads(
        (ARCHIVE / "manifest.json").read_text(encoding="utf-8")
    )

    reference = verify_manifest(manifest)
    verify_native(manifest)
    verify_devices(manifest, reference)
    verify_faults(manifest)
    verify_invalids(manifest)

    if args.artifacts is not None:
        verify_artifacts(
            manifest, args.artifacts.resolve(strict=True)
        )

    verify_corruptions(manifest, reference)

    expected_logs = {
        record["file"]
        for record in (
            manifest["device_runs"]
            + manifest["fault_runs"]
            + manifest["invalid_runs"]
            + manifest["native_runs"]
        )
    }

    assert len(expected_logs) == 32
    assert {path.name for path in ARCHIVE.glob("*.log")} == expected_logs

    print(
        "PASS Vulkan 1.0 reusable runtime: 20 real GPU runs "
        "(4 submissions each), 4 exact incompatible-memory trials, "
        "84 real resource acquisition cleanup faults, "
        "15 invalid CLI controls; ARM64 and ARMv7 tested"
    )


if __name__ == "__main__":
    main()
