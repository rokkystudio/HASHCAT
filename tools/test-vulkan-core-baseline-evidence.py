#!/usr/bin/env python3
"""Audit Vulkan 1.0-by-default GPU compute and explicit Vulkan 1.1 evidence.

The original Android logs contain real GPU dispatches using a SPIR-V 1.0
integer shader, two staging copies and four automatically planned allocations.
The validator confirms exact runtime API selection, independent CPU checksum,
driver-reported memory-type compatibility, physical heap assignments and
separate Vulkan loader versus physical-device versions. It additionally
validates actual Vulkan 1.0 noncoherent flush/invalidate on one phone and
a precise incompatible-memory rejection on another.

Native C loader policy tests prove that Vulkan 1.0 is supported when the
Vulkan 1.1 vkEnumerateInstanceVersion entry point is absent. No real
physical GPU limited to Vulkan 1.0 is claimed to have been tested.

The validator runs entirely offline: it invokes neither Vulkan, ADB,
hashcat nor any password-processing code.
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
    ROOT / "tests" / "fixtures" / "android-gpu"
    / "vulkan-core-baseline-20261031"
)
MANIFEST = ARCHIVE / "manifest.json"
DEVICES = ("redmagic", "pixel")
OPTIMIZATIONS = ("O0", "O")
MODES = ("default10", "explicit10", "required11")
ROLES = ("gpu-input", "gpu-output", "staging-upload", "staging-download")
NATIVE_MODES = ("O0", "O2")
EXPECTED_CPU_PASSES = (
    "PASS_CORE11_LOADER dynamic_query core10_missing version_errors",
    "PASS_CORE11_INSTANCE default10 newer_loader_never_raises_api explicit11",
    "PASS_CORE11_DEVICE cap11 physical_version_independent lower_minimum",
    "PASS_CORE11_POLICY all three groups without Vulkan driver",
)

REQUIREMENTS = re.compile(
    r"^CORE10_BUFFER_REQUIREMENTS index=(\d+) "
    r"role=(gpu-input|gpu-output|staging-upload|staging-download) "
    r"size=(\d+) alignment=(\d+) bits=0x([0-9a-f]{8})$"
)
PLAN = re.compile(
    r"^PLAN_BUFFER index=(\d+) "
    r"label=(gpu-input|gpu-output|staging-upload|staging-download) "
    r"type=(\d+) heap=(\d+) bytes=(\d+) coherent=([01])$"
)
HEAP = re.compile(
    r"^PLAN_HEAP index=(\d+) reserved=(\d+) declared=(\d+)$"
)
MAPPED = re.compile(
    r"^CORE10_STAGING_MAPPED input_type=(\d+) output_type=(\d+) "
    r"input_coherent=([01]) output_coherent=([01])$"
)
STAGING_REQUEST = re.compile(
    r"^CORE10_STAGING_REQUEST mode=(auto|noncoherent) "
    r"input_bits=0x([0-9a-f]{8}) output_bits=0x([0-9a-f]{8})$"
)
STAGING_SYNC = re.compile(
    r"^CORE10_STAGING_SYNC index=(\d+) "
    r"operation=(flush|invalidate) coherent=([01]) "
    r"driver_call=([01]) atom=(\d+) result=(-?\d+)$"
)
GPU_PASS = re.compile(
    r"^PASS_CORE11_COMPUTE device=(\d+) queue=(\d+) "
    r"words=2048 checksum=([0-9a-f]{16}) "
    r"gpu_buffers=2 staging_buffers=2 copies=2 "
    r"optional_features=0 extensions=0$"
)
LOADER = re.compile(
    r"^VULKAN_LOADER_VERSION=(\d+)\.(\d+)\.(\d+) "
    r"source=(vkEnumerateInstanceVersion|core10_missing_entry_point)$"
)
SELECT_INSTANCE = re.compile(
    r"^SELECT_INSTANCE_API=(\d+)\.(\d+) "
    r"loader_minor=(\d+) cap=1.1$"
)
SELECT_DEVICE = re.compile(
    r"^SELECT_DEVICE_API=(\d+)\.(\d+) "
    r"reported_minor=(\d+) instance_minor=(\d+) cap=1.1$"
)
FEATURES = re.compile(
    r"^CORE11_OPTIONAL_FEATURES reported_shaderInt64=([01]) "
    r"reported_shaderInt16=([01]) enabled=0 extensions=0 "
    r"effective_api=(\d+)\.(\d+)$"
)


def sha256(path: Path) -> str:
    """Hash exact saved GPU evidence bytes or an optionally built artifact."""
    assert path.is_file(), f"Vulkan core-version evidence absent: {path}"

    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_sha256(path: Path) -> str:
    """Hash UTF-8 source with CRLF normalized to LF across Windows checkouts."""
    assert path.is_file(), f"Vulkan portable source absent: {path}"

    return hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")
    ).hexdigest()


def independent_cpu_checksum() -> str:
    """Calculate public SPIR-V stage-A FNV-1a without calling GPU or C code."""
    value64 = 14695981039346656037
    mask32 = 0xFFFFFFFF
    mask64 = 0xFFFFFFFFFFFFFFFF

    for index in range(2048):
        input_word = (index * 1664525) & mask32
        affine = (input_word * 1664525 + 1013904223) & mask32
        rotated = (
            ((input_word << 7) | (input_word >> 25)) & mask32
        )
        output_word = (
            (affine ^ rotated) + index * 2654435761
        ) & mask32

        for byte in struct.pack("<I", output_word):
            value64 = ((value64 ^ byte) * 1099511628211) & mask64

    return f"{value64:016x}"


def previous_memory_properties(
    device: str, abi: str = "arm64-v8a"
) -> tuple[dict, dict]:
    """Read a real Vulkan memory survey from the same Android process ABI.

    ARMv7 may report different heap capacities from ARM64 even for the same
    physical GPU. All resource checks use values observed by that ABI,
    never the memory budget of another architecture.
    """
    if abi == "armeabi-v7a":
        assert device == "redmagic"
        path = ARCHIVE / "redmagic-armv7-memory-properties.log"
    else:
        assert abi == "arm64-v8a"
        path = (
            ROOT / "tests" / "fixtures" / "android-gpu"
            / "vulkan-heap-20261031" / f"{device}-allocation.log"
        )

    assert path.is_file()

    heap_rows = {}
    type_rows = {}

    for line in path.read_text(encoding="utf-8").splitlines():
        match = re.fullmatch(
            r"DEVICE_HEAP index=(\d+) size=(\d+) flags=0x([0-9a-f]{8})",
            line,
        )

        if match:
            index, bytes_capacity, flags = match.groups()
            heap_rows[int(index)] = {
                "capacity": int(bytes_capacity),
                "flags": int(flags, 16),
            }

        match = re.fullmatch(
            r"DEVICE_MEMORY_TYPE index=(\d+) "
            r"heap=(\d+) flags=0x([0-9a-f]{8})",
            line,
        )

        if match:
            index, heap, flags = match.groups()
            type_rows[int(index)] = {
                "heap": int(heap),
                "flags": int(flags, 16),
            }

    assert len(heap_rows) >= 1
    assert len(type_rows) >= 1

    return heap_rows, type_rows


def verify_manifest(manifest: dict) -> str:
    """Check unchanged base modules, exact build provenance and workload."""
    assert manifest["date"] == "2026-10-31"
    assert manifest["scope"] == (
        "synthetic Vulkan 1.0 default Compute with staging, optional "
        "explicit Vulkan 1.1, and real Adreno/Mali device results"
    )
    assert manifest["source_hash_policy"] == (
        "UTF-8 source with CRLF normalized to LF"
    )
    assert manifest["toolchains"] == {
        "ndk": "29.0.14206865",
        "clang": "21.0.0",
        "native_gcc": "MSYS2 MinGW GCC 16.2.0",
        "shader_target": "vulkan1.0",
        "validated_shader_targets": ["vulkan1.0", "vulkan1.1"],
        "android_targets": [
            "aarch64-linux-android24",
            "armv7a-linux-androideabi24",
        ],
    }
    assert manifest["workload"] == {
        "default_instance_api": "1.0",
        "optional_instance_api": "1.1",
        "minimum_spirv_version": "1.0",
        "local_size": [32, 1, 1],
        "workgroups": [64, 1, 1],
        "words": 2048,
        "buffer_count": 4,
        "bytes_per_buffer": 8192,
        "copies_per_dispatch": 2,
        "enabled_optional_device_features": 0,
        "enabled_extensions": 0,
        "minimum_android_api": 24,
        "supported_android_abis": [
            "arm64-v8a", "armeabi-v7a",
        ],
        "noncoherent_staging_test": {
            "requested_core_api": "1.0",
            "host_visible_required": True,
            "host_coherent_forbidden": True,
            "unsupported_memory_exit_code": 1,
        },
    }

    expected_sources = {
        "tools/vulkan-core11-policy.c",
        "tools/vulkan-core11-policy.h",
        "tools/vulkan-core11-compute-diagnostic.c",
        "tools/test-vulkan-core11-policy-host.c",
        "tools/build-vulkan-core-baseline.ps1",
        "tools/run-vulkan-core-baseline.ps1",
        "tools/test-vulkan-core-baseline-evidence.py",
        "tools/vulkan-auto-config.c",
        "tools/vulkan-auto-config.h",
        "tools/vulkan-memory-heap-policy.c",
        "tools/vulkan-memory-heap-policy.h",
        "tools/vulkan-memory-heap-probe.c",
        "tools/vulkan-pipeline-stage-a.comp",
    }

    assert set(manifest["source_sha256"]) == expected_sources

    for name, digest in manifest["source_sha256"].items():
        assert re.fullmatch(r"[0-9a-f]{64}", digest)
        assert source_sha256(ROOT / name) == digest, name

    previous = json.loads((
        ROOT / "tests" / "fixtures" / "android-gpu"
        / "vulkan-heap-20261031" / "manifest.json"
    ).read_text(encoding="utf-8"))

    for name in (
        "tools/vulkan-auto-config.c",
        "tools/vulkan-auto-config.h",
        "tools/vulkan-memory-heap-policy.c",
        "tools/vulkan-memory-heap-policy.h",
    ):
        assert manifest["source_sha256"][name] == (
            previous["source_sha256"][name]
        )

    assert set(manifest["artifact_sha256"]) == {
        "vulkan-core-baseline-arm64",
        "vulkan-core-baseline-armv7",
        "vulkan-heap-probe-armv7",
        "stage-a-O0-vk1.0.spv",
        "stage-a-O-vk1.0.spv",
        "vulkan-core11-policy-O0.exe",
        "vulkan-core11-policy-O2.exe",
    }

    for digest in manifest["artifact_sha256"].values():
        assert re.fullmatch(r"[0-9a-f]{64}", digest)

    assert manifest["host_imports"] == ["KERNEL32.dll", "msvcrt.dll"]
    assert manifest["native_policy_uses_vulkan_driver"] is False
    assert manifest["real_physical_vulkan10_only_device_tested"] is False
    assert set(manifest["device_profiles"]) == set(DEVICES)

    checksum = independent_cpu_checksum()

    assert checksum == manifest["expected_gpu_fnv1a64"]
    assert checksum == "00d6d6e4837edebc"

    print("PASS Vulkan 1.0 baseline source and earlier memory policy SHA-256 identities")
    print("PASS native API tests have no Vulkan/OpenCL driver imports")
    print(f"PASS independent CPU GPU-output reference FNV-1a: {checksum}")

    return checksum


def verify_host_policy(manifest: dict) -> None:
    """Require two native CPU executions with identical Vulkan1.0 policy."""
    records = manifest["host_policy_runs"]

    assert len(records) == 2
    assert {entry["optimization"] for entry in records} == set(NATIVE_MODES)

    compared = None

    for entry in records:
        assert set(entry) == {
            "optimization", "file", "sha256", "exit_code",
        }
        assert entry["exit_code"] == 0
        assert entry["file"] == (
            f"vulkan-core11-policy-{entry['optimization']}.log"
        )
        assert sha256(ARCHIVE / entry["file"]) == entry["sha256"]

        lines = (ARCHIVE / entry["file"]).read_text(
            encoding="utf-8"
        ).splitlines()

        assert lines[:4] == [
            "TEST=vulkan-core-api-1.0-baseline",
            f"OPTIMIZATION={entry['optimization']}",
            "VULKAN_DRIVER_LINKED=0",
            "EXIT_CODE=0",
        ]
        assert tuple(lines[4:]) == EXPECTED_CPU_PASSES

        if compared is None:
            compared = lines[4:]
        else:
            assert compared == lines[4:]

        print(
            f"PASS native Vulkan API policy -{entry['optimization']}: "
            "Vulkan 1.0 missing entry point and explicit 1.1 requirement"
        )


def verify_device_log(
    lines: list[str],
    profile: dict,
    device: str,
    mode: str,
    optimization: str,
    oracle: str,
    staging_mode: str = "auto",
    abi: str = "arm64-v8a",
) -> None:
    """Validate the actual Vulkan API, buffer types and cache synchronization."""
    assert lines[:4] == [
        "TEST=vulkan-core1.0-baseline-optional-1.1",
        f"MODE={mode}",
        f"OPTIMIZATION={optimization}",
        "ADB_EXIT_CODE=0",
    ]

    body = lines[4:]
    expected = 1 if mode == "required11" else 0

    loader = [LOADER.fullmatch(line) for line in body
              if line.startswith("VULKAN_LOADER_VERSION")]
    instance = [SELECT_INSTANCE.fullmatch(line) for line in body
                if line.startswith("SELECT_INSTANCE_API")]
    selected = [SELECT_DEVICE.fullmatch(line) for line in body
                if line.startswith("SELECT_DEVICE_API")]
    optional = [FEATURES.fullmatch(line) for line in body
                if line.startswith("CORE11_OPTIONAL_FEATURES")]

    assert len(loader) == len(instance) == len(selected) == len(optional) == 1
    assert all(item[0] is not None
               for item in (loader, instance, selected, optional))

    loader_major, loader_minor, _, source = loader[0].groups()

    assert int(loader_major) == 1
    assert int(loader_minor) >= expected
    assert source == "vkEnumerateInstanceVersion"

    instance_major, instance_minor, reported_loader_minor = (
        instance[0].groups()
    )

    assert instance_major == "1"
    assert int(instance_minor) == expected
    assert int(reported_loader_minor) == int(loader_minor)

    device_major, device_minor, reported_device_minor, physical_instance = (
        selected[0].groups()
    )

    assert device_major == "1"
    assert int(device_minor) == expected
    assert int(physical_instance) == expected
    assert int(reported_device_minor) >= expected

    features64, features16, effective_major, effective_minor = (
        optional[0].groups()
    )

    assert features64 in ("0", "1") and features16 in ("0", "1")
    assert (effective_major, effective_minor) == ("1", str(expected))

    candidates = [
        line for line in body if line.startswith("VULKAN_CANDIDATE")
    ]

    assert len(candidates) == 1

    for expected_value in (
        f"name={profile['gpu']}",
        f"vendor={profile['vendor_id']}",
        f"device={profile['device_id']}",
        f"api={profile['vulkan_api_version']}",
        f"driver={profile['driver_version']}",
    ):
        assert expected_value in candidates[0], expected_value

    assert len([
        line for line in body if line.startswith("ACCEPT_COMPUTE index=")
    ]) == 1

    requirements = [
        REQUIREMENTS.fullmatch(line)
        for line in body if line.startswith("CORE10_BUFFER_REQUIREMENTS")
    ]
    plans = [
        PLAN.fullmatch(line)
        for line in body if line.startswith("PLAN_BUFFER")
    ]
    mapped = [
        MAPPED.fullmatch(line)
        for line in body if line.startswith("CORE10_STAGING_MAPPED")
    ]
    heaps = [
        HEAP.fullmatch(line)
        for line in body if line.startswith("PLAN_HEAP")
    ]
    staging_request = [
        STAGING_REQUEST.fullmatch(line)
        for line in body if line.startswith("CORE10_STAGING_REQUEST")
    ]
    staging_sync = [
        STAGING_SYNC.fullmatch(line)
        for line in body if line.startswith("CORE10_STAGING_SYNC")
    ]

    assert len(staging_request) == 1
    assert staging_request[0] is not None
    assert staging_request[0].group(1) == staging_mode
    assert len(staging_sync) == 2
    assert all(row is not None for row in staging_sync)

    assert len(requirements) == len(plans) == 4
    assert len(mapped) == 1
    assert all(row is not None for row in requirements + plans + mapped + heaps)

    heap_info, memory_types = previous_memory_properties(device, abi)

    assert len(heaps) == len(heap_info)

    reserved = {index: 0 for index in heap_info}

    for index in range(4):
        req_index, req_role, bytes_size, alignment, bits = (
            requirements[index].groups()
        )
        plan_index, plan_role, memory_type, heap, allocated, coherent = (
            plans[index].groups()
        )

        assert int(req_index) == int(plan_index) == index
        assert req_role == plan_role == ROLES[index]
        assert int(bytes_size) >= 8192
        assert int(alignment) > 0
        assert int(alignment) & (int(alignment) - 1) == 0
        assert int(allocated) == int(bytes_size)

        type_index = int(memory_type)
        heap_index = int(heap)

        assert type_index in memory_types
        assert int(bits, 16) & (1 << type_index)
        assert heap_index == memory_types[type_index]["heap"]
        assert int(coherent) == int(
            (memory_types[type_index]["flags"] & 4) != 0
        )

        if index >= 2:
            assert memory_types[type_index]["flags"] & 2

        assert not (memory_types[type_index]["flags"] & (32 | 64))
        reserved[heap_index] += int(allocated)

    mapped_input, mapped_output, coherent_in, coherent_out = (
        mapped[0].groups()
    )

    assert int(mapped_input) == int(plans[2].group(3))
    assert int(mapped_output) == int(plans[3].group(3))
    assert int(coherent_in) == int(plans[2].group(6))
    assert int(coherent_out) == int(plans[3].group(6))

    assert int(staging_request[0].group(2), 16) == int(
        requirements[2].group(5), 16
    )
    assert int(staging_request[0].group(3), 16) == int(
        requirements[3].group(5), 16
    )

    compute = next(
        row for row in body if row.startswith("ACCEPT_COMPUTE index=")
    )
    cache_atom = re.search(r"non_coherent_atom=(\d+)", compute)

    assert cache_atom is not None
    assert int(cache_atom.group(1)) > 0

    for index, operation, entry in (
        (2, "flush", staging_sync[0]),
        (3, "invalidate", staging_sync[1]),
    ):
        recorded_index, recorded_operation, coherent, calls, atom, result = (
            entry.groups()
        )
        coherent_expected = int(plans[index].group(6))

        assert int(recorded_index) == index
        assert recorded_operation == operation
        assert int(coherent) == coherent_expected
        assert int(calls) == 1 - coherent_expected
        assert int(atom) == int(cache_atom.group(1))
        assert int(result) == 0

        if staging_mode == "noncoherent":
            assert coherent_expected == 0

    for row in heaps:
        heap_index, bytes_reserved, declared = row.groups()
        index = int(heap_index)

        assert index in heap_info
        assert int(declared) == heap_info[index]["capacity"]
        assert int(bytes_reserved) == reserved[index]
        assert int(bytes_reserved) <= int(declared)

    assert sum(reserved.values()) == 4 * 8192

    successes = [GPU_PASS.fullmatch(line)
                 for line in body
                 if line.startswith("PASS_CORE11_COMPUTE")]

    assert len(successes) == 1 and successes[0] is not None

    device_index, queue, words_digest = successes[0].groups()

    assert device_index == "0" and queue == "0"
    assert words_digest == oracle
    assert lines[-1].startswith("PASS_CORE11_COMPUTE")


def verify_device_runs(manifest: dict, checksum: str) -> None:
    """Require all twelve real GPU computations with the expected API scope."""
    runs = manifest["device_runs"]

    assert len(runs) == 12
    assert {
        (item["device"], item["mode"], item["optimization"])
        for item in runs
    } == {
        (device, mode, optimization)
        for device in DEVICES
        for mode in MODES
        for optimization in OPTIMIZATIONS
    }

    for item in runs:
        assert set(item) == {
            "device", "mode", "optimization", "file", "sha256", "exit_code",
        }
        assert item["exit_code"] == 0

        filename = (
            f"{item['device']}-{item['mode']}-{item['optimization']}.log"
        )

        assert item["file"] == filename
        assert sha256(ARCHIVE / filename) == item["sha256"]

        lines = (ARCHIVE / filename).read_text(
            encoding="utf-8"
        ).splitlines()

        verify_device_log(
            lines,
            manifest["device_profiles"][item["device"]],
            item["device"],
            item["mode"],
            item["optimization"],
            checksum,
        )

        print(
            f"PASS {item['device']}: {item['mode']} "
            f"-{item['optimization']}, 2048 exact GPU words, "
            "no optional Vulkan features"
        )


def verify_armv7_memory_survey(manifest: dict) -> None:
    """Audit a separate real 32-bit heap survey with Vulkan 1.0.

    RedMagic's ARMv7 Vulkan loader reports a smaller device-local heap
    than its ARM64 loader. The test validates real type flags, buffer masks,
    four allocated/bound buffers and their architecture-specific capacities.
    """
    record = manifest["armv7_memory_survey"]

    assert set(record) == {
        "device", "abi", "file", "sha256", "exit_code",
    }
    assert record["device"] == "redmagic"
    assert record["abi"] == "armeabi-v7a"
    assert record["file"] == "redmagic-armv7-memory-properties.log"
    assert record["exit_code"] == 0
    assert sha256(ARCHIVE / record["file"]) == record["sha256"]

    lines = (ARCHIVE / record["file"]).read_text(
        encoding="utf-8"
    ).splitlines()

    assert lines[:2] == [
        "TEST=vulkan1.0-android24-armv7-heap-survey",
        "ADB_EXIT_CODE=0",
    ]

    assert lines[-1] == (
        "PASS_VULKAN_HEAP_PROBE device_index=0 "
        "buffers=4 allocations=4 mapped_staging=2 gpu_dispatches=0"
    )

    heap32, types32 = previous_memory_properties(
        "redmagic", "armeabi-v7a"
    )
    heap64, types64 = previous_memory_properties(
        "redmagic", "arm64-v8a"
    )

    assert len(heap32) == 2
    assert len(types32) == 9
    assert types32 == types64
    assert heap32[0]["capacity"] == 4294967296
    assert heap64[0]["capacity"] == 11584221184
    assert heap32[1]["capacity"] == heap64[1]["capacity"] == 268435456

    assert (
        "DEVICE_HEAP_PROFILE memory_types=9 memory_heaps=2 "
        "max_allocations=4096"
    ) in lines

    requirements = [
        re.fullmatch(
            r"ACTUAL_REQUIREMENTS index=(\d+) "
            r"label=(gpu-input|gpu-output|staging-upload|staging-download) "
            r"size=(\d+) alignment=(\d+) "
            r"memory_bits=0x([0-9a-f]{8})",
            row,
        )
        for row in lines
        if row.startswith("ACTUAL_REQUIREMENTS")
    ]
    selected = [
        PLAN.fullmatch(row)
        for row in lines
        if row.startswith("PLAN_BUFFER")
    ]
    mapped = [
        re.fullmatch(
            r"MAPPED_STAGING index=(\d+) coherent=([01]) bytes=(\d+)",
            row,
        )
        for row in lines
        if row.startswith("MAPPED_STAGING")
    ]
    reserved = [
        HEAP.fullmatch(row)
        for row in lines
        if row.startswith("PLAN_HEAP")
    ]

    assert len(requirements) == len(selected) == 4
    assert len(mapped) == len(reserved) == 2
    assert all(
        row is not None for row in (
            requirements + selected + mapped + reserved
        )
    )

    totals = {heap: 0 for heap in heap32}

    for index in range(4):
        required = requirements[index].groups()
        plan = selected[index].groups()

        req_index, req_role, req_size, alignment, mask = required
        plan_index, plan_role, type_index, heap_index, bytes_allocated, coherent = plan

        assert int(req_index) == int(plan_index) == index
        assert req_role == plan_role == ROLES[index]
        assert int(req_size) == int(bytes_allocated) == 8192
        assert int(alignment) == 64

        type_index = int(type_index)
        heap_index = int(heap_index)

        assert type_index in types32
        assert int(mask, 16) & (1 << type_index)
        assert heap_index == types32[type_index]["heap"]
        assert int(coherent) == int(
            (types32[type_index]["flags"] & 4) != 0
        )

        if index >= 2:
            assert types32[type_index]["flags"] & 2

        totals[heap_index] += int(bytes_allocated)

    assert totals == {0: 32768, 1: 0}

    for row in reserved:
        heap_index, amount, declared = row.groups()
        heap_index = int(heap_index)

        assert heap_index in heap32
        assert int(amount) == totals[heap_index]
        assert int(declared) == heap32[heap_index]["capacity"]

    for index, row in enumerate(mapped, start=2):
        mapped_index, coherent, bytes_mapped = row.groups()

        assert int(mapped_index) == index
        assert int(coherent) == int(selected[index].group(6))
        assert int(bytes_mapped) == 8192

    print(
        "PASS independent ARMv7 Vulkan 1.0 memory survey: "
        "4-GiB heap versus ARM64 11.58-GB heap, 4 allocations"
    )


def verify_armv7_runs(manifest: dict, checksum: str) -> None:
    """Validate real ARMv7 Vulkan 1.0/1.1 Compute on an ABI-capable device.

    Uses exactly the same independent SPIR-V 1.0 arithmetic reference and
    per-buffer Vulkan type/heap checks as the ARM64 runs. Architecture is
    explicit evidence metadata, not a GPU vendor or model selection rule.
    """
    runs = manifest["armv7_gpu_runs"]

    assert len(runs) == 6
    assert {
        (item["device"], item["mode"], item["optimization"])
        for item in runs
    } == {
        ("redmagic", mode, optimization)
        for mode in MODES
        for optimization in OPTIMIZATIONS
    }

    for record in runs:
        assert set(record) == {
            "device", "abi", "mode", "optimization", "file",
            "sha256", "exit_code",
        }
        assert record["abi"] == "armeabi-v7a"
        assert record["exit_code"] == 0

        name = (
            f"redmagic-armv7-{record['mode']}-"
            f"{record['optimization']}.log"
        )

        assert record["file"] == name
        assert sha256(ARCHIVE / name) == record["sha256"]

        lines = (ARCHIVE / name).read_text(
            encoding="utf-8"
        ).splitlines()

        verify_device_log(
            lines,
            manifest["device_profiles"]["redmagic"],
            "redmagic",
            record["mode"],
            record["optimization"],
            checksum,
            abi="armeabi-v7a",
        )

        print(
            "PASS real ARMv7 Android API24 build on RedMagic: "
            f"{record['mode']} -{record['optimization']}, "
            "2,048 exact GPU outputs"
        )


def verify_noncoherent_runs(manifest: dict, checksum: str) -> None:
    """Verify real noncoherent GPU transfers or exact unsupported memory types.

    Each archived trial explicitly asks for HOST_VISIBLE without
    HOST_COHERENT on both staging buffers. A successful Vulkan 1.0 dispatch
    must contain real flush/invalidate calls and valid CPU results.
    A device without such memory must explicitly reject an allocation type;
    the verifier independently confirms that no eligible type exists.
    """
    original_runs = manifest["noncoherent_runs"]
    armv7_runs = manifest["armv7_noncoherent_runs"]

    assert len(original_runs) == 4
    assert {
        (item["device"], item["optimization"])
        for item in original_runs
    } == {
        (device, optimization)
        for device in DEVICES
        for optimization in OPTIMIZATIONS
    }
    assert len(armv7_runs) == 2
    assert {
        (item["device"], item["abi"], item["optimization"])
        for item in armv7_runs
    } == {
        ("redmagic", "armeabi-v7a", optimization)
        for optimization in OPTIMIZATIONS
    }

    runs = original_runs + armv7_runs

    supported = 0
    incompatible = 0

    for record in runs:
        assert record["exit_code"] in (0, 1)

        device = record["device"]
        optimization = record["optimization"]
        abi = record.get("abi", "arm64-v8a")

        if abi == "armeabi-v7a":
            assert set(record) == {
                "device", "abi", "optimization", "file",
                "sha256", "exit_code",
            }
            assert device == "redmagic"
            filename = f"{device}-armv7-noncoherent10-{optimization}.log"
        else:
            assert abi == "arm64-v8a"
            assert set(record) == {
                "device", "optimization", "file",
                "sha256", "exit_code",
            }
            filename = f"{device}-noncoherent10-{optimization}.log"

        assert record["file"] == filename
        assert sha256(ARCHIVE / filename) == record["sha256"]

        lines = (ARCHIVE / filename).read_text(
            encoding="utf-8"
        ).splitlines()

        assert lines[:4] == [
            "TEST=vulkan-core1.0-noncoherent-staging",
            "MODE=noncoherent10",
            f"OPTIMIZATION={optimization}",
            f"ADB_EXIT_CODE={record['exit_code']}",
        ]

        body = lines[4:]
        selection = [
            row for row in body if row.startswith("SELECT_INSTANCE_API")
        ]

        assert len(selection) == 1
        assert selection[0].startswith("SELECT_INSTANCE_API=1.0 ")

        requests = [
            STAGING_REQUEST.fullmatch(row)
            for row in body
            if row.startswith("CORE10_STAGING_REQUEST")
        ]

        assert len(requests) == 1
        assert requests[0] is not None
        assert requests[0].group(1) == "noncoherent"

        if record["exit_code"] == 0:
            normalized = [
                "TEST=vulkan-core1.0-baseline-optional-1.1",
                "MODE=default10",
                f"OPTIMIZATION={optimization}",
                "ADB_EXIT_CODE=0",
            ] + body

            verify_device_log(
                normalized,
                manifest["device_profiles"][device],
                device,
                "default10",
                optimization,
                checksum,
                staging_mode="noncoherent",
            )

            print(
                f"PASS {device} -{optimization}: actual HOST_VISIBLE "
                "noncoherent Vulkan 1.0 flush/dispatch/fence/invalidate"
            )
            supported += 1

            continue

        assert not any(
            row.startswith("PASS_CORE11_COMPUTE")
            for row in body
        )
        assert not any(
            row.startswith("CORE10_STAGING_MAPPED")
            for row in body
        )
        assert not any(
            row.startswith("CORE10_STAGING_SYNC")
            for row in body
        )
        assert (
            "NO_CORE11_COMPATIBLE_SYNTHETIC_COMPUTE_CONFIGURATION"
            in body
        )
        assert any(
            row.startswith("REJECT_VULKAN_CALL ")
            and row.endswith("result=-8")
            for row in body
        )

        actual_rejects = [
            re.fullmatch(
                r"REJECT_ALLOCATION_TYPE index=(\d+) role=1 "
                r"bits=0x([0-9a-f]{8}) "
                r"forbidden=0x([0-9a-f]{8})",
                row,
            )
            for row in body
            if row.startswith("REJECT_ALLOCATION_TYPE")
        ]

        assert len(actual_rejects) == 1
        assert actual_rejects[0] is not None

        reject_index, reject_bits, forbidden = (
            actual_rejects[0].groups()
        )
        reject_index = int(reject_index)

        assert reject_index in (2, 3)
        assert int(forbidden, 16) & 4
        assert int(reject_bits, 16) == int(
            requests[0].group(2 if reject_index == 2 else 3), 16
        )

        _, memory_types = previous_memory_properties(device, abi)

        for request_index in range(2, reject_index + 1):
            bits = int(
                requests[0].group(
                    2 if request_index == 2 else 3
                ), 16
            )
            compatible = [
                index for index, memory in memory_types.items()
                if bits & (1 << index)
                and memory["flags"] & 2
                and not memory["flags"] & int(forbidden, 16)
            ]

            if request_index < reject_index:
                assert compatible
            else:
                assert not compatible

        incompatible += 1

        print(
            f"PASS {device} -{optimization}: explicit "
            "VK_ERROR_FEATURE_NOT_PRESENT, no compatible noncoherent "
            "staging type and no automatic fallback"
        )

    assert supported > 0
    assert incompatible > 0

    print(
        "PASS actual noncoherent Vulkan 1.0 memory on a compatible GPU, "
        "explicit rejection on an incompatible GPU"
    )


def verify_invalids(manifest: dict) -> None:
    """Verify all invalid CLI controls on both ARM64 and actual ARMv7 runs."""
    native_runs = manifest["negative_controls"]
    armv7_runs = manifest["armv7_negative_controls"]

    assert len(native_runs) == 2
    assert {entry["device"] for entry in native_runs} == set(DEVICES)
    assert len(armv7_runs) == 1
    assert armv7_runs[0]["device"] == "redmagic"
    assert armv7_runs[0]["abi"] == "armeabi-v7a"

    runs = native_runs + armv7_runs

    expected = [
        "TEST=vulkan-core1.0-baseline-optional-1.1",
        "INVALID_INPUT_CONTROLS=4",
        "INVALID no-shader exit=2",
        "INVALID bad-flag exit=2",
        "INVALID unsupported-api exit=2",
        "INVALID missing-file exit=2",
    ]

    for entry in runs:
        abi = entry.get("abi", "arm64-v8a")

        if abi == "armeabi-v7a":
            assert set(entry) == {
                "device", "abi", "file", "sha256", "exit_code",
            }
            name = "redmagic-armv7-negative-controls.log"
        else:
            assert set(entry) == {
                "device", "file", "sha256", "exit_code",
            }
            name = f"{entry['device']}-negative-controls.log"

        assert entry["exit_code"] == 2
        assert entry["file"] == name
        assert sha256(ARCHIVE / entry["file"]) == entry["sha256"]

        assert (ARCHIVE / entry["file"]).read_text(
            encoding="utf-8"
        ).splitlines() == expected

    print("PASS twelve real-device ARM64/ARMv7 invalid CLI control rejections")


def verify_artifacts(manifest: dict, directory: Path) -> None:
    """Check five exact ARM64/SPIR-V/native-GCC artifacts if supplied."""
    assert directory.is_dir()

    for name, digest in manifest["artifact_sha256"].items():
        assert sha256(directory / name) == digest, name

    print("PASS seven exact ARM64/ARMv7 Android24, memory survey, SPIR-V 1.0 and native C artifact SHA-256 identities")


def verify_corruptions(manifest: dict, checksum: str) -> None:
    """Reject a forged instance version, incompatible memory or bad GPU output."""
    record = next(
        item for item in manifest["device_runs"]
        if item["device"] == "redmagic"
        and item["mode"] == "default10"
        and item["optimization"] == "O"
    )
    lines = (ARCHIVE / record["file"]).read_text(
        encoding="utf-8"
    ).splitlines()
    profile = manifest["device_profiles"][record["device"]]

    alterations = (
        (
            "SELECT_INSTANCE_API=1.0 ",
            "SELECT_INSTANCE_API=1.1 ",
            "unrequested Vulkan 1.1 instance",
        ),
        (
            "PLAN_BUFFER index=2 label=staging-upload type=6 ",
            "PLAN_BUFFER index=2 label=staging-upload type=0 ",
            "incompatible host staging memory",
        ),
        (
            f"checksum={checksum}",
            "checksum=0000000000000000",
            "wrong GPU readback checksum",
        ),
    )

    for original, replacement, label in alterations:
        changed = [row.replace(original, replacement) for row in lines]

        assert changed != lines, label

        try:
            verify_device_log(
                changed, profile, record["device"],
                record["mode"], record["optimization"], checksum,
            )
        except AssertionError:
            print(f"PASS negative control rejected {label}")
        else:
            raise AssertionError(f"Incorrect Vulkan baseline accepted: {label}")


def verify_noncoherent_corruption(manifest: dict, checksum: str) -> None:
    """Reject forged successful noncoherent logs that omit real cache calls."""
    record = next(
        item for item in manifest["noncoherent_runs"]
        if item["exit_code"] == 0 and item["optimization"] == "O"
    )
    original = (ARCHIVE / record["file"]).read_text(
        encoding="utf-8"
    ).splitlines()

    forged = [
        row.replace("driver_call=1", "driver_call=0")
        if row.startswith("CORE10_STAGING_SYNC index=2")
        else row
        for row in original
    ]

    assert forged != original

    normalized = [
        "TEST=vulkan-core1.0-baseline-optional-1.1",
        "MODE=default10",
        "OPTIMIZATION=O",
        "ADB_EXIT_CODE=0",
    ] + forged[4:]

    try:
        verify_device_log(
            normalized,
            manifest["device_profiles"][record["device"]],
            record["device"],
            "default10",
            "O",
            checksum,
            staging_mode="noncoherent",
        )
    except AssertionError:
        print(
            "PASS negative control rejected missing real noncoherent flush"
        )
    else:
        raise AssertionError(
            "Forged Vulkan 1.0 noncoherent cache operation was accepted"
        )


def main() -> None:
    """Audit CPU-only API selection and real Vulkan 1.0/1.1 GPU results."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--artifacts", type=Path,
        help="Directory with six ARM64/ARMv7 API24, shader and native artifacts",
    )
    args = parser.parse_args()

    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))

    assert "adb_serial" not in MANIFEST.read_text(encoding="utf-8")

    checksum = verify_manifest(manifest)

    verify_host_policy(manifest)
    verify_armv7_memory_survey(manifest)
    verify_device_runs(manifest, checksum)
    verify_armv7_runs(manifest, checksum)
    verify_noncoherent_runs(manifest, checksum)
    verify_invalids(manifest)

    if args.artifacts is not None:
        verify_artifacts(manifest, args.artifacts.resolve(strict=True))

    verify_corruptions(manifest, checksum)
    verify_noncoherent_corruption(manifest, checksum)

    expected_files = {
        item["file"]
        for item in (
            manifest["host_policy_runs"]
            + manifest["device_runs"]
            + manifest["armv7_gpu_runs"]
            + manifest["noncoherent_runs"]
            + manifest["armv7_noncoherent_runs"]
            + manifest["negative_controls"]
            + manifest["armv7_negative_controls"]
            + [manifest["armv7_memory_survey"]]
        )
    }

    assert {path.name for path in ARCHIVE.glob("*.log")} == expected_files

    print(
        "PASS Vulkan 1.0 Android API24 ARM64/ARMv7 GPU compute, "
        "noncoherent flush/invalidate, explicit incompatibility, "
        "and optional Vulkan 1.1; no physical Vulkan 1.0-only "
        "device or Android 7.0 device was tested"
    )


if __name__ == "__main__":
    main()
