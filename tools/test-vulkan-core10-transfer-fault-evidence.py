#!/usr/bin/env python3
"""Independently verify real Android Vulkan partial-resource fault lifecycles.

Audits exact Vulkan 1.0/1.1 instance and device capability events,
real acquired-resource interruptions, per-handle teardown cardinality,
native process exit 42 and absence of GPU copy submissions. Deliberate
log alterations demonstrate error detection. Verification itself never
opens a GPU, runs HASHCAT or executes a cryptographic workload.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = ROOT / "tests/fixtures/android-gpu/vulkan-core10-transfer-faults-20261101"

DEVICES = {
    "redmagic-arm64": "arm64-v8a",
    "redmagic-armv7": "armeabi-v7a",
    "pixel-arm64": "arm64-v8a",
}
RESOURCES = (
    "device",
    "command_pool",
    "command_buffer",
    "fence",
    "source_buffer",
    "source_memory",
    "source_mapping",
    "destination_buffer",
    "destination_memory",
    "destination_mapping",
)


def sha256(path: Path) -> str:
    """Hash all exact bytes of an existing executable or Android transcript."""
    assert path.is_file(), path

    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_sha256(path: Path) -> str:
    """Hash current source after normalizing Windows CRLF to LF."""
    return hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")
    ).hexdigest()


def expected_release(point: int) -> str:
    """Describe exactly the Vulkan handles acquired at one fault point."""
    assert 1 <= point <= len(RESOURCES)

    mapped = 2 if point >= 10 else 1 if point >= 7 else 0
    buffers = 2 if point >= 8 else 1 if point >= 5 else 0
    memory = 2 if point >= 9 else 1 if point >= 6 else 0
    fence = int(point >= 4)
    command = int(point >= 3)
    pool = int(point >= 2)

    return (
        f"TRANSFER_RELEASE point={point} acquired={point}"
        f" mapped={mapped} buffers={buffers} memory={memory}"
        f" fence={fence} command={command} pool={pool} device=1"
    )


def verify_trial(
    segment: list[str],
    api: str,
    trial: int,
    point: int,
) -> None:
    """Verify one ordered VkInstance/VkDevice partial acquisition teardown.

    The expected counts are derived from ten fixed API resource
    acquisition steps. Each real VkDevice is destroyed before its
    owning VkInstance, with no copied GPU data or shader dispatch.
    """
    assert len(segment) == 14, (len(segment), segment)
    assert api in ("1.0", "1.1")
    assert trial in (0, 1)
    assert 1 <= point <= len(RESOURCES)

    loader = re.fullmatch(
        r"VULKAN_LOADER_VERSION=1\.(\d+)\.(\d+)"
        r" source=vkEnumerateInstanceVersion",
        segment[0],
    )
    assert loader is not None
    assert int(loader.group(1)) >= int(api.split(".")[1])

    assert segment[1] == (
        f"SELECT_INSTANCE_API={api} loader_minor={loader.group(1)} cap=1.1"
    )

    opened = re.fullmatch(
        r"DEVICE_CATALOG_OPEN requested_api=(1\.[01])"
        r" instance_api=(1\.[01]) devices=(\d+)"
        r" extensions=0 layers=0",
        segment[2],
    )
    assert opened is not None
    assert opened.group(1) == opened.group(2) == api
    count = int(opened.group(3))
    assert count >= 1

    selected_api = re.fullmatch(
        rf"SELECT_DEVICE_API={re.escape(api)} reported_minor=(\d+)"
        r" instance_minor=(\d+) cap=1\.1",
        segment[3],
    )
    assert selected_api is not None
    assert int(selected_api.group(1)) >= int(api.split(".")[1])
    assert int(selected_api.group(2)) == int(api.split(".")[1])

    candidate = re.fullmatch(
        r"VULKAN_CANDIDATE index=0 name=(.+)"
        r" vendor=0x([0-9a-f]+) device=0x([0-9a-f]+)"
        r" api=1\.(\d+)\.(\d+) driver=0x([0-9a-f]+)",
        segment[4],
    )
    assert candidate is not None
    assert candidate.group(1)
    assert int(candidate.group(4)) >= int(api.split(".")[1])

    capability = re.fullmatch(
        r"ACCEPT_COMPUTE index=0 queue_family=(\d+)"
        r" local_max=(\d+) invocations_max=(\d+)"
        r" groups_max=(\d+) shared_max=(\d+)"
        r" storage_range_max=(\d+) descriptor_max=(\d+)"
        r" non_coherent_atom=(\d+)",
        segment[5],
    )
    assert capability is not None
    family = int(capability.group(1))
    assert int(capability.group(2)) >= 1
    assert int(capability.group(3)) >= 1
    assert int(capability.group(4)) >= 1
    assert int(capability.group(6)) >= 4096
    assert int(capability.group(7)) >= 2
    atom = int(capability.group(8))
    assert atom > 0 and (atom & (atom - 1)) == 0

    assert segment[6] == (
        f"DEVICE_CATALOG_SELECT index=0 queue_family={family}"
        f" effective_api={api} selections=1"
    )
    assert segment[7] == (
        f"TRANSFER_INVALID_OPEN trial={trial} rejected=7 owned=0"
    )
    assert segment[8] == (
        f"TRANSFER_INJECT_FAULT point={point} acquisition={point}"
        f" resource={RESOURCES[point - 1]} result=-3"
    )
    assert segment[9] == (
        "TRANSFER_OPEN_FAILED index=0 mode=0 result=-3"
    )
    assert segment[10] == (
        "TRANSFER_CLOSE index=0 bytes=0 copies=0"
        " copied_bytes=0 flushes=0 invalidates=0 wait_result=0"
    )
    assert segment[11] == expected_release(point)
    assert segment[12] == (
        f"TRANSFER_FAULT_TRIAL trial={trial}"
        f" point={point} injected=1 owned=0"
    )
    assert segment[13] == (
        f"DEVICE_CATALOG_CLOSE devices={count} selections=1"
    )


def verify_fault_body(
    lines: list[str],
    api: str,
    point: int,
) -> None:
    """Verify both physical-device lifecycles and native fault completion."""
    assert len(lines) == 29, len(lines)

    verify_trial(lines[:14], api, 0, point)
    verify_trial(lines[14:28], api, 1, point)

    assert lines[28] == (
        f"TRANSFER_DIAGNOSTIC_FAULT api={api} mode=0 point={point}"
        " trials=2 injected=2 copies=0 shaders=0"
    )

    for prefix in (
        "TRANSFER_COPY ",
        "TRANSFER_VERIFIED ",
        "TRANSFER_OPEN ",
        "JOB_PORT_SUBMIT ",
        "BACKEND_VULKAN_SESSION_READY ",
    ):
        assert not any(item.startswith(prefix) for item in lines), prefix


def verify_cli(lines: list[str], abi: str) -> None:
    """Require five exact malformed native command lines to exit with code 2."""
    assert lines == [
        "TEST=vulkan-core10-transfer-fault-invalid-inputs",
        f"ABI={abi}",
        "INVALID_CASES=5",
        "INVALID_CASE=zero-fault exit=2",
        "INVALID_CASE=fault-out-of-range exit=2",
        "INVALID_CASE=fault-nonnumeric exit=2",
        "INVALID_CASE=fault-noncoherent-mode exit=2",
        "INVALID_CASE=fault-extra-argument exit=2",
    ]


def corrupt(
    lines: list[str],
    prefix: str,
    old: str,
    new: str,
) -> list[str]:
    """Produce an altered full transcript with exactly one false event field."""
    result = lines.copy()
    matches = [
        i for i, line in enumerate(lines)
        if line.startswith(prefix) and old in line
    ]
    assert matches, (prefix, old)
    target = matches[0]
    result[target] = result[target].replace(old, new, 1)
    assert result != lines

    return result


def verify_mutations(manifest: dict) -> None:
    """Reject false cleanup counts, impossible fault results and bad ordering."""
    record = next(
        item for item in manifest["runs"]
        if item["device"] == "pixel-arm64"
        and item["api"] == "1.0"
        and item["fault_point"] == 10
    )
    original = (ARCHIVE / record["file"]).read_text(
        encoding="utf-8"
    ).splitlines()[5:]

    mutations = (
        ("TRANSFER_INJECT_FAULT ", "point=10", "point=9"),
        ("TRANSFER_INJECT_FAULT ", "acquisition=10", "acquisition=9"),
        ("TRANSFER_INJECT_FAULT ", "resource=destination_mapping", "resource=source_mapping"),
        ("TRANSFER_INJECT_FAULT ", "result=-3", "result=0"),
        ("TRANSFER_OPEN_FAILED ", "result=-3", "result=0"),
        ("TRANSFER_CLOSE ", "wait_result=0", "wait_result=-4"),
        ("TRANSFER_RELEASE ", "acquired=10", "acquired=9"),
        ("TRANSFER_RELEASE ", "mapped=2", "mapped=1"),
        ("TRANSFER_RELEASE ", "buffers=2", "buffers=1"),
        ("TRANSFER_RELEASE ", "memory=2", "memory=1"),
        ("TRANSFER_RELEASE ", "fence=1", "fence=0"),
        ("TRANSFER_RELEASE ", "command=1", "command=0"),
        ("TRANSFER_RELEASE ", "pool=1", "pool=0"),
        ("TRANSFER_RELEASE ", "device=1", "device=0"),
        ("TRANSFER_FAULT_TRIAL ", "injected=1", "injected=0"),
        ("TRANSFER_FAULT_TRIAL ", "owned=0", "owned=1"),
        ("DEVICE_CATALOG_CLOSE ", "selections=1", "selections=0"),
        ("TRANSFER_DIAGNOSTIC_FAULT ", "copies=0", "copies=1"),
        ("TRANSFER_DIAGNOSTIC_FAULT ", "shaders=0", "shaders=1"),
        ("TRANSFER_INVALID_OPEN ", "rejected=7", "rejected=6"),
    )

    for prefix, old, new in mutations:
        modified = corrupt(original, prefix, old, new)

        try:
            verify_fault_body(modified, "1.0", 10)
        except AssertionError:
            print("PASS rejected corrupted Vulkan fault evidence", prefix.strip())
        else:
            raise AssertionError(f"Accepted false Vulkan fault teardown: {prefix}")

    misordered = original.copy()
    close_index = next(
        i for i, line in enumerate(misordered)
        if line.startswith("DEVICE_CATALOG_CLOSE ")
    )
    release_index = next(
        i for i, line in enumerate(misordered)
        if line.startswith("TRANSFER_RELEASE ")
    )
    misordered[close_index], misordered[release_index] = (
        misordered[release_index], misordered[close_index]
    )

    try:
        verify_fault_body(misordered, "1.0", 10)
    except AssertionError:
        print("PASS rejected instance closure before logical-device release")
    else:
        raise AssertionError("Accepted incorrect Vulkan instance/device order")

    for point in (1, 4, 7):
        record = next(
            item for item in manifest["runs"]
            if item["device"] == "redmagic-arm64"
            and item["api"] == "1.0"
            and item["fault_point"] == point
        )
        lines = (ARCHIVE / record["file"]).read_text(
            encoding="utf-8"
        ).splitlines()[5:]

        mutated = corrupt(
            lines, "TRANSFER_RELEASE ", "device=1", "device=0"
        )

        try:
            verify_fault_body(mutated, "1.0", point)
        except AssertionError:
            print("PASS rejected false early Vulkan device release", point)
        else:
            raise AssertionError(f"Accepted false release at resource {point}")

    cli = next(
        item for item in manifest["invalid_cli"]
        if item["device"] == "pixel-arm64"
    )
    lines = (ARCHIVE / cli["file"]).read_text(
        encoding="utf-8"
    ).splitlines()

    modified = corrupt(
        lines, "INVALID_CASE=fault-noncoherent-mode",
        "exit=2", "exit=0",
    )

    try:
        verify_cli(modified, cli["abi"])
    except AssertionError:
        print("PASS rejected false accepted malformed fault request")
    else:
        raise AssertionError("Accepted invalid native fault argument")

    print("PASS 25 deliberately corrupted Vulkan fault records rejected")


def inspect_elf(
    readelf: Path,
    binary: Path,
    abi: str,
) -> None:
    """Statically check the native Android Vulkan 1.0 loader and ABI symbols."""
    def read(*arguments: str) -> str:
        """Run only read-only llvm-readelf against an existing ELF."""
        value = subprocess.run(
            [str(readelf), "-W", *arguments, str(binary)],
            check=True, capture_output=True, text=True, encoding="utf-8",
        )
        return value.stdout

    header = read("--file-header")
    notes = read("--notes")
    dynamic = read("--dynamic")
    symbols = read("--dyn-syms")

    expected_elf = "ELF64" if abi == "arm64" else "ELF32"
    expected_machine = "AArch64" if abi == "arm64" else "ARM"

    assert re.search(rf"Class:\s+{expected_elf}\b", header)
    assert re.search(rf"Machine:\s+{expected_machine}\b", header)
    assert "NT_ANDROID_TYPE_IDENT" in notes
    assert "description data: 18 00 00 00" in notes
    assert "Shared library: [libvulkan.so]" in dynamic

    for method in (
        "vkGetInstanceProcAddr", "vkCreateDevice", "vkCreateBuffer",
        "vkAllocateMemory", "vkMapMemory", "vkCmdCopyBuffer",
        "vkQueueSubmit", "vkWaitForFences", "vkDestroyDevice",
    ):
        assert re.search(rf"UND\s+{method}\b", symbols), method

    assert not re.search(r"UND\s+vkEnumerateInstanceVersion\b", symbols)


def main() -> None:
    """Verify all 120 actual partial Vulkan resource lifecycle interruptions."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--artifacts", type=Path,
        help="Directory containing both exact ARM64/ARMv7 transfer ELF files",
    )
    parser.add_argument(
        "--readelf", type=Path,
        help="Android NDK llvm-readelf for read-only ELF symbol verification",
    )
    args = parser.parse_args()

    if args.readelf is not None and args.artifacts is None:
        parser.error("--readelf requires --artifacts")

    manifest = json.loads(
        (ARCHIVE / "manifest.json").read_text(encoding="utf-8")
    )

    assert manifest["minimum_android_api"] == 24
    assert manifest["vulkan_apis"] == ["1.0", "1.1"]
    assert manifest["failure_result"] == "VK_ERROR_INITIALIZATION_FAILED"
    assert manifest["injected_native_exit"] == 42
    assert manifest["driver_fault_or_device_loss_simulated"] is False
    assert manifest["vulkan_gpu_copies_in_fault_runs"] == 0
    assert manifest["hashcat_backend_connected"] is False
    assert manifest["password_or_cryptographic_inputs"] is False
    assert len(manifest["source_sha256"]) == 13

    for source, digest in manifest["source_sha256"].items():
        assert source_sha256(ROOT / source) == digest, source

    assert manifest["fault_points"] == {
        str(i): name for i, name in enumerate(RESOURCES, start=1)
    }

    assert len(manifest["runs"]) == 60
    assert len(manifest["invalid_cli"]) == 3

    seen = set()
    total = 0

    for row in manifest["runs"]:
        key = (row["device"], row["api"], row["fault_point"])
        assert key not in seen
        seen.add(key)
        assert DEVICES[row["device"]] == row["abi"]
        assert row["api"] in ("1.0", "1.1")
        assert row["resource"] == RESOURCES[row["fault_point"] - 1]
        assert row["native_exit"] == 42
        assert row["explicit_injections"] == 2
        assert row["partial_vulkan_device_lifecycles"] == 2
        assert row["gpu_copy_submissions"] == 0

        path = ARCHIVE / row["file"]
        data = path.read_bytes()
        assert data and data.endswith(b"\n") and b"\r" not in data
        assert b"91809037" not in data and b"46281FDAS002AL" not in data
        assert sha256(path) == row["sha256"]
        lines = path.read_text(encoding="utf-8").splitlines()

        assert lines[:5] == [
            "TEST=vulkan-core10-transfer-faults",
            f"ABI={row['abi']}",
            f"API={row['api']}",
            f"FAULT_POINT={row['fault_point']}",
            "ADB_EXIT_CODE=42",
        ]

        verify_fault_body(lines[5:], row["api"], row["fault_point"])

        total += row["partial_vulkan_device_lifecycles"]

    assert total == 120
    assert len(seen) == 60
    assert seen == {
        (device, api, point)
        for device in DEVICES
        for api in ("1.0", "1.1")
        for point in range(1, 11)
    }

    for row in manifest["invalid_cli"]:
        assert DEVICES[row["device"]] == row["abi"]
        assert row["rejected_commands"] == 5
        path = ARCHIVE / row["file"]
        assert sha256(path) == row["sha256"]
        verify_cli(
            path.read_text(encoding="utf-8").splitlines(), row["abi"]
        )

    assert len(list(ARCHIVE.glob("*.log"))) == 63

    if args.artifacts is not None:
        directory = args.artifacts.resolve(strict=True)

        for abi, digest in manifest["native_elf_sha256"].items():
            binary = directory / f"vulkan-core10-transfer-{abi}"

            assert sha256(binary) == digest, abi

            if args.readelf is not None:
                inspect_elf(
                    args.readelf.resolve(strict=True), binary, abi
                )

        print("PASS exact Android ARM64/ARMv7 real fault-injection ELF identities")

        if args.readelf is not None:
            print("PASS Android API24 ELF and native Vulkan core 1.0 resources")

    verify_mutations(manifest)

    print("PASS 60 Android device/API/fault-point observations")
    print("PASS 120 real VkDevice partial resource acquisition/cleanup trials")
    print("PASS exact VkDevice before VkInstance release and zero GPU copies")
    print("PASS 15 malformed native fault options explicitly rejected")


if __name__ == "__main__":
    main()
