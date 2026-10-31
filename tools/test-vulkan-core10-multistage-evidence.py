#!/usr/bin/env python3
"""Audit portable noncryptographic Vulkan 1.0 three-stage GPU evidence.

Reads original Android ARM64 Adreno/Mali logs, native six-buffer allocator
tests, exact build hashes and independent real Vulkan memory-type surveys.
A pure Python CPU reference verifies every public stage-A/stage-B/result
checksum without executing GPU code, ADB or cryptographic computations.

Both physical GPUs support newer Vulkan versions, so these results verify
a Vulkan 1.0 requested-instance execution path, not a physical device
restricted to Vulkan 1.0. ARMv7 builds are checked by artifact hashes;
this archive contains only observed ARM64 multistage GPU executions.
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
    / "vulkan-core-multistage-20261031"
)
DEVICES = ("redmagic-arm64", "pixel-arm64")
MODES = ("default10", "explicit10", "required11", "noncoherent10")
OPTIMIZATIONS = ("O0", "O")
ROLES = (
    "gpu-input", "stage-a", "stage-b", "gpu-output",
    "staging-upload", "staging-download",
)
SIZES = (8192, 8192, 8192, 32768, 8192, 32768)
CPU_PASSES = (
    "PASS_MULTISTAGE_SIX all_resources capacity_and_roles",
    "PASS_MULTISTAGE_BACKTRACK six_requests_exact_placement",
    "PASS_MULTISTAGE_NONCOHERENT compatible_and_explicit_rejection",
    "PASS_MULTISTAGE_ERRORS count alignment heap capacity untouched",
    "PASS_MULTISTAGE_MEMORY_POLICY no_vulkan_driver",
)
REQUEST = re.compile(
    r"^MULTISTAGE_REQUIREMENTS index=(\d+) "
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
    r"^MULTISTAGE_SYNC index=(\d+) "
    r"operation=(flush|invalidate) coherent=([01]) "
    r"driver_call=([01]) atom=(\d+) result=(-?\d+)$"
)
SELECT_INSTANCE = re.compile(
    r"^SELECT_INSTANCE_API=(\d+)\.(\d+) "
    r"loader_minor=(\d+) cap=1.1$"
)
SELECT_DEVICE = re.compile(
    r"^SELECT_DEVICE_API=(\d+)\.(\d+) "
    r"reported_minor=(\d+) instance_minor=(\d+) cap=1.1$"
)
PASS = re.compile(
    r"^PASS_CORE10_MULTISTAGE elements=2048 verified_words=12288 "
    r"stageA=([0-9a-f]{16}) stageB=([0-9a-f]{16}) "
    r"output=([0-9a-f]{16}) stages=3 gpu_buffers=4 "
    r"staging_buffers=2 copies=2 enabled_features=0 extensions=0$"
)


def sha256(path: Path) -> str:
    """Return the exact byte-level SHA-256 of an archived or built artifact."""
    assert path.is_file(), path

    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_sha256(path: Path) -> str:
    """Hash source bytes consistently across LF and Windows CRLF worktrees."""
    assert path.is_file(), path

    return hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")
    ).hexdigest()


def cpu_checksums() -> dict[str, str]:
    """Independently calculate the three synthetic SPIR-V stage checksums."""
    mask = 0xFFFFFFFF
    fnv_offset = 14695981039346656037
    fnv_prime = 1099511628211

    def input_word(index: int) -> int:
        return (
            index * 0x9E3779B9
            ^ (index << 15)
            ^ (index >> 3)
            ^ 0xA5A5A5A5
        ) & mask

    def a_word(index: int) -> int:
        value = input_word(index)
        affine = (value * 1664525 + 1013904223) & mask
        rotated = ((value << 7) | (value >> 25)) & mask

        return ((affine ^ rotated) + index * 2654435761) & mask

    stage_a = [a_word(index) for index in range(2048)]
    stage_b = []

    for index, value in enumerate(stage_a):
        neighbor = stage_a[
            (index & ~31) + ((index + 7) & 31)
        ]
        rotated = ((value << 11) | (value >> 21)) & mask
        stage_b.append(
            ((value + neighbor) ^ rotated ^ 0x6C8E9CF5) & mask
        )

    output = []

    for a, b in zip(stage_a, stage_b, strict=True):
        rotated = ((b << 3) | (b >> 29)) & mask
        output.extend((
            a, b, a ^ b,
            ((a * 2246822519 + b * 3266489917) ^ rotated) & mask,
        ))

    def fnv(words: list[int]) -> str:
        value = fnv_offset

        for word in words:
            for byte in struct.pack("<I", word):
                value = ((value ^ byte) * fnv_prime) & 0xFFFFFFFFFFFFFFFF

        return f"{value:016x}"

    return {
        "stageA": fnv(stage_a),
        "stageB": fnv(stage_b),
        "output": fnv(output),
    }


def memory_profile(device: str) -> tuple[dict, dict]:
    """Read independently archived real GPU heap/type properties."""
    name = device.split("-")[0]
    path = (
        ROOT / "tests" / "fixtures" / "android-gpu"
        / "vulkan-heap-20261031" / f"{name}-allocation.log"
    )
    assert path.is_file()

    heaps = {}
    types = {}

    for line in path.read_text(encoding="utf-8").splitlines():
        heap = re.fullmatch(
            r"DEVICE_HEAP index=(\d+) size=(\d+) "
            r"flags=0x([0-9a-f]{8})",
            line,
        )

        if heap:
            index, size, flags = heap.groups()
            heaps[int(index)] = {
                "capacity": int(size),
                "flags": int(flags, 16),
            }

        memory_type = re.fullmatch(
            r"DEVICE_MEMORY_TYPE index=(\d+) heap=(\d+) "
            r"flags=0x([0-9a-f]{8})",
            line,
        )

        if memory_type:
            index, heap_index, flags = memory_type.groups()
            types[int(index)] = {
                "heap": int(heap_index),
                "flags": int(flags, 16),
            }

    assert len(heaps) == 2
    assert len(types) >= 4

    return heaps, types


def verify_success(
    body: list[str],
    device: str,
    mode: str,
    checksums: dict[str, str],
) -> None:
    """Verify every buffer's actual memory mask, API choice and GPU output."""
    expected_minor = 1 if mode == "required11" else 0

    instance = [
        SELECT_INSTANCE.fullmatch(row)
        for row in body
        if row.startswith("SELECT_INSTANCE_API=")
    ]
    physical = [
        SELECT_DEVICE.fullmatch(row)
        for row in body
        if row.startswith("SELECT_DEVICE_API=")
    ]

    assert len(instance) == len(physical) == 1
    assert instance[0] is not None and physical[0] is not None
    assert instance[0].group(1) == physical[0].group(1) == "1"
    assert int(instance[0].group(2)) == expected_minor
    assert int(physical[0].group(2)) == expected_minor
    assert int(physical[0].group(4)) == expected_minor
    assert int(physical[0].group(3)) >= expected_minor

    selection = [
        row for row in body
        if row.startswith("MULTISTAGE_DEVICE_API")
    ]

    assert len(selection) == 1
    assert (
        f"effective=1.{expected_minor} requested=1.{expected_minor}"
    ) in selection[0]
    assert (
        "shared_bytes=128 storage_bindings=4 "
        "enabled_features=0 extensions=0"
    ) in selection[0]

    requirements = [
        REQUEST.fullmatch(row)
        for row in body
        if row.startswith("MULTISTAGE_REQUIREMENTS")
    ]
    plans = [
        PLAN.fullmatch(row)
        for row in body
        if row.startswith("MULTISTAGE_PLAN_BUFFER")
    ]
    heap_records = [
        HEAP.fullmatch(row)
        for row in body
        if row.startswith("MULTISTAGE_PLAN_HEAP")
    ]
    syncs = [
        SYNC.fullmatch(row)
        for row in body
        if row.startswith("MULTISTAGE_SYNC")
    ]

    assert len(requirements) == len(plans) == 6
    assert len(syncs) == len(heap_records) == 2
    assert all(
        record is not None
        for record in requirements + plans + heap_records + syncs
    )

    heaps, types = memory_profile(device)
    used = {index: 0 for index in heaps}

    for index in range(6):
        req_index, req_label, size, alignment, bits = (
            requirements[index].groups()
        )
        plan_index, plan_label, memory_type, heap, bytes_allocated, coherent = (
            plans[index].groups()
        )

        assert int(req_index) == int(plan_index) == index
        assert req_label == plan_label == ROLES[index]
        assert int(size) == int(bytes_allocated) == SIZES[index]
        assert int(alignment) == 64

        type_index = int(memory_type)
        heap_index = int(heap)

        assert type_index in types
        assert heap_index == types[type_index]["heap"]
        assert int(bits, 16) & (1 << type_index)
        assert int(coherent) == int(
            bool(types[type_index]["flags"] & 4)
        )

        if index >= 4:
            assert types[type_index]["flags"] & 2
            if mode == "noncoherent10":
                assert not types[type_index]["flags"] & 4

        assert not types[type_index]["flags"] & (32 | 64)
        used[heap_index] += int(bytes_allocated)

    assert sum(used.values()) == sum(SIZES) == 98304

    for record in heap_records:
        heap_index, reserved, declared = record.groups()
        index = int(heap_index)

        assert index in heaps
        assert int(reserved) == used[index]
        assert int(declared) == heaps[index]["capacity"]
        assert int(reserved) <= int(declared)

    staging = [
        re.fullmatch(
            r"^MULTISTAGE_STAGING_MAPPED upload_type=(\d+) "
            r"download_type=(\d+) upload_coherent=([01]) "
            r"download_coherent=([01])$",
            row,
        )
        for row in body
        if row.startswith("MULTISTAGE_STAGING_MAPPED")
    ]

    assert len(staging) == 1 and staging[0] is not None
    assert int(staging[0].group(1)) == int(plans[4].group(3))
    assert int(staging[0].group(2)) == int(plans[5].group(3))
    assert int(staging[0].group(3)) == int(plans[4].group(6))
    assert int(staging[0].group(4)) == int(plans[5].group(6))

    atom_row = next(
        row for row in body
        if row.startswith("ACCEPT_COMPUTE")
    )
    match_atom = re.search(r"non_coherent_atom=(\d+)", atom_row)

    assert match_atom is not None
    atom = int(match_atom.group(1))

    for index, operation, record in (
        (4, "flush", syncs[0]),
        (5, "invalidate", syncs[1]),
    ):
        given_index, given_operation, coherent, driver_call, size, result = (
            record.groups()
        )
        expected_coherent = int(plans[index].group(6))

        assert int(given_index) == index
        assert given_operation == operation
        assert int(coherent) == expected_coherent
        assert int(driver_call) == 1 - expected_coherent
        assert int(size) == atom
        assert int(result) == 0

    output = [
        PASS.fullmatch(row)
        for row in body
        if row.startswith("PASS_CORE10_MULTISTAGE")
    ]

    assert len(output) == 1 and output[0] is not None
    assert dict(
        zip(("stageA", "stageB", "output"), output[0].groups(), strict=True)
    ) == checksums
    assert body[-1].startswith("PASS_CORE10_MULTISTAGE")


def verify_no_noncoherent(
    body: list[str],
    device: str,
) -> None:
    """Require a real incompatible host-type mask and no silent GPU fallback."""
    assert any(
        "SELECT_INSTANCE_API=1.0 " in row for row in body
    )
    assert (
        "NO_COMPATIBLE_VULKAN_MULTISTAGE_CONFIGURATION"
        in body
    )
    assert not any(
        row.startswith("PASS_CORE10_MULTISTAGE")
        for row in body
    )
    assert not any(
        row.startswith("MULTISTAGE_SYNC")
        for row in body
    )

    rejections = [
        re.fullmatch(
            r"REJECT_MULTISTAGE_MEMORY_TYPE index=(\d+) "
            r"role=1 bits=0x([0-9a-f]{8}) "
            r"forbidden=0x([0-9a-f]{8})",
            row,
        )
        for row in body if row.startswith("REJECT_MULTISTAGE_MEMORY_TYPE")
    ]

    assert len(rejections) == 1 and rejections[0] is not None
    index, bits, forbidden = rejections[0].groups()
    index = int(index)

    assert index in (4, 5)
    assert int(forbidden, 16) & 4

    requests = [
        REQUEST.fullmatch(row)
        for row in body
        if row.startswith("MULTISTAGE_REQUIREMENTS")
    ]

    assert len(requests) == 6
    assert all(item is not None for item in requests)
    assert int(bits, 16) == int(requests[index].group(5), 16)

    _, types = memory_profile(device)

    assert not any(
        (int(bits, 16) & (1 << type_index))
        and (type_info["flags"] & 2)
        and not (type_info["flags"] & int(forbidden, 16))
        for type_index, type_info in types.items()
    )


def verify_logs(manifest: dict, checksums: dict[str, str]) -> None:
    """Validate all 16 real GPU/memory trials without running ADB."""
    records = manifest["device_runs"]

    assert len(records) == 16
    assert {
        (entry["device"], entry["mode"], entry["optimization"])
        for entry in records
    } == {
        (device, mode, opt)
        for device in DEVICES
        for mode in MODES
        for opt in OPTIMIZATIONS
    }

    for entry in records:
        assert set(entry) == {
            "device", "mode", "optimization", "file",
            "sha256", "exit_code",
        }
        device = entry["device"]
        mode = entry["mode"]
        opt = entry["optimization"]
        name = f"{device}-{mode}-{opt}.log"

        assert entry["file"] == name
        assert sha256(ARCHIVE / name) == entry["sha256"]

        rows = (ARCHIVE / name).read_text(
            encoding="utf-8"
        ).splitlines()

        assert rows[:5] == [
            "TEST=portable-vulkan1.0-multistage",
            f"MODE={mode}",
            f"OPTIMIZATION={opt}",
            "ABI=arm64-v8a",
            f"ADB_EXIT_CODE={entry['exit_code']}",
        ]

        body = rows[5:]
        expected_exit = (
            1 if device == "redmagic-arm64" and mode == "noncoherent10"
            else 0
        )

        assert entry["exit_code"] == expected_exit

        if expected_exit == 0:
            verify_success(body, device, mode, checksums)
        else:
            verify_no_noncoherent(body, device)

        print(
            f"PASS {device} {mode} -{opt}: "
            + (
                "12,288 independently verified GPU words"
                if expected_exit == 0
                else "precise incompatible noncoherent memory rejection"
            )
        )


def verify_native(manifest: dict) -> None:
    """Check both real C six-buffer planner modes without driver linkage."""
    records = manifest["native_runs"]

    assert len(records) == 2
    assert {
        entry["optimization"] for entry in records
    } == {"O0", "O2"}

    previous = None

    for entry in records:
        assert set(entry) == {
            "optimization", "file", "sha256", "exit_code",
        }
        assert entry["exit_code"] == 0
        name = f"vulkan-multistage-heap-{entry['optimization']}.log"

        assert entry["file"] == name
        assert sha256(ARCHIVE / name) == entry["sha256"]

        lines = (ARCHIVE / name).read_text(
            encoding="utf-8"
        ).splitlines()

        assert lines[:4] == [
            "TEST=vulkan-multistage-memory-plan-host",
            f"OPTIMIZATION={entry['optimization']}",
            "VULKAN_DRIVER_LINKED=0",
            "EXIT_CODE=0",
        ]

        passes = tuple(
            row for row in lines
            if row.startswith("PASS_MULTISTAGE_")
        )

        assert passes == CPU_PASSES

        if previous is not None:
            assert lines[4:] == previous

        previous = lines[4:]

        print(
            f"PASS native six-buffer planner -{entry['optimization']}: "
            "exact roles, fallback backtracking, noncoherent memory and failures"
        )


def verify_manifest(manifest: dict) -> dict[str, str]:
    """Audit expected source, binary identities and pure Python GPU oracle."""
    assert manifest["date"] == "2026-10-31"
    assert manifest["scope"] == (
        "public synthetic Vulkan 1.0 three-stage GPU staging "
        "with six planned allocations, no cryptographic compute"
    )
    assert manifest["minimum_android_api"] == 24
    assert manifest["compiled_abis"] == ["arm64-v8a", "armeabi-v7a"]
    assert manifest["real_multistage_abis"] == ["arm64-v8a"]
    assert manifest["physical_vulkan10_only_device_tested"] is False
    assert manifest["source_hash_policy"] == "UTF-8 CRLF normalized to LF"

    sources = {
        "tools/vulkan-core10-multistage-diagnostic.c",
        "tools/vulkan-multistage-memory-plan.c",
        "tools/vulkan-multistage-memory-plan.h",
        "tools/test-vulkan-multistage-memory-plan-host.c",
        "tools/build-vulkan-core10-multistage.ps1",
        "tools/run-vulkan-core10-multistage.ps1",
        "tools/test-vulkan-core10-multistage-evidence.py",
        "tools/vulkan-auto-config.c",
        "tools/vulkan-auto-config.h",
        "tools/vulkan-core11-policy.c",
        "tools/vulkan-core11-policy.h",
        "tools/vulkan-pipeline-stage-a.comp",
        "tools/vulkan-pipeline-stage-b.comp",
        "tools/vulkan-pipeline-stage-c.comp",
    }

    assert set(manifest["source_sha256"]) == sources

    for name, digest in manifest["source_sha256"].items():
        assert re.fullmatch(r"[0-9a-f]{64}", digest)
        assert source_sha256(ROOT / name) == digest, name

    artifacts = {
        "vulkan-core10-multistage-arm64",
        "vulkan-core10-multistage-armv7",
        "vulkan-multistage-heap-O0.exe",
        "vulkan-multistage-heap-O2.exe",
        *(f"stage-{stage}-{opt}-vk1.0.spv"
          for stage in "abc" for opt in ("O0", "O")),
    }

    assert set(manifest["artifact_sha256"]) == artifacts
    assert all(
        re.fullmatch(r"[0-9a-f]{64}", digest)
        for digest in manifest["artifact_sha256"].values()
    )

    oracle = cpu_checksums()

    assert oracle == manifest["cpu_checksums"]
    assert oracle == {
        "stageA": "de56d6d0aa66c6ec",
        "stageB": "c1259ad4f024499a",
        "output": "05de70c62f8d7cfd",
    }

    print("PASS standalone CPU oracle across three synthetic shader stages")
    print("PASS all 14 source SHA-256 identifiers and ten exact build digests")

    return oracle


def verify_artifacts(manifest: dict, directory: Path) -> None:
    """Compare all ten exact, reproducibly built device/CPU/SPIR-V artifacts."""
    assert directory.is_dir()

    for name, digest in manifest["artifact_sha256"].items():
        assert sha256(directory / name) == digest, name

    print("PASS ten exact compiled artifacts: ARM64, ARMv7, six SPIR-V and C CPU tests")


def verify_corruptions(manifest: dict, checksums: dict[str, str]) -> None:
    """Reject forged API versions, incompatible GPU memory and fake GPU data."""
    entry = next(
        item for item in manifest["device_runs"]
        if item["device"] == "pixel-arm64"
        and item["mode"] == "noncoherent10"
        and item["optimization"] == "O"
    )
    original = (ARCHIVE / entry["file"]).read_text(
        encoding="utf-8"
    ).splitlines()[5:]

    mutations = (
        ("SELECT_INSTANCE_API=1.0 ", "SELECT_INSTANCE_API=1.1 "),
        ("MULTISTAGE_SYNC index=4 operation=flush coherent=0 driver_call=1",
         "MULTISTAGE_SYNC index=4 operation=flush coherent=0 driver_call=0"),
        ("MULTISTAGE_PLAN_BUFFER index=4 label=staging-upload type=1",
         "MULTISTAGE_PLAN_BUFFER index=4 label=staging-upload type=3"),
        ("stageA=de56d6d0aa66c6ec", "stageA=0000000000000000"),
    )

    for before, after in mutations:
        changed = [row.replace(before, after) for row in original]

        assert changed != original

        try:
            verify_success(
                changed, "pixel-arm64", "noncoherent10", checksums
            )
        except AssertionError:
            print(f"PASS corruption control rejected {before.split()[0]}")
        else:
            raise AssertionError(
                f"Forged Vulkan 1.0 three-stage result accepted: {before}"
            )


def main() -> None:
    """Verify all archived real-device and CPU-only public Vulkan trials."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--artifacts", type=Path,
        help="Directory with ten exact ARM64/ARMv7, SPIR-V and C artifacts",
    )
    args = parser.parse_args()

    manifest = json.loads(
        (ARCHIVE / "manifest.json").read_text(encoding="utf-8")
    )

    checksums = verify_manifest(manifest)

    verify_native(manifest)
    verify_logs(manifest, checksums)

    if args.artifacts is not None:
        verify_artifacts(
            manifest, args.artifacts.resolve(strict=True)
        )

    verify_corruptions(manifest, checksums)

    known_logs = {
        record["file"]
        for record in (
            manifest["device_runs"] + manifest["native_runs"]
        )
    }

    assert {path.name for path in ARCHIVE.glob("*.log")} == known_logs

    print(
        "PASS Vulkan 1.0 six-buffer three-stage GPU: "
        "14 real ARM64 GPU executions, two explicit memory rejections, "
        "two native C tests; ARMv7 execution and physical Vulkan 1.0 "
        "hardware are not claimed"
    )


if __name__ == "__main__":
    main()
