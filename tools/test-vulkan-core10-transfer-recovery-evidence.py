#!/usr/bin/env python3
"""Independently validate real Android Vulkan transfer recovery sessions.

Checks original LF-normalized Android driver evidence, every test-defined
fault and resource release, two fresh VkDevices per retained VkInstance,
each fence-completed GPU buffer copy and noncoherent cache operation,
plus exact ELF/source SHA-256 identities. Rejects deliberately altered
transcripts without opening a GPU or executing HASHCAT.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = ROOT / "tests/fixtures/android-gpu/vulkan-core10-transfer-recovery-20261101"

DEVICES = {
    "redmagic-arm64": ("arm64-v8a", ("prefer",)),
    "redmagic-armv7": ("armeabi-v7a", ("prefer",)),
    "pixel-arm64": ("arm64-v8a", ("prefer", "noncoherent")),
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
WORDS = (1, 7, 64, 257, 1024)

OPEN = re.compile(
    r"DEVICE_CATALOG_OPEN requested_api=(1\.[01])"
    r" instance_api=(1\.[01]) devices=(\d+)"
    r" extensions=0 layers=0"
)
ACCEPT = re.compile(
    r"ACCEPT_COMPUTE index=0 queue_family=(\d+)"
    r" local_max=(\d+) invocations_max=(\d+)"
    r" groups_max=(\d+) shared_max=(\d+)"
    r" storage_range_max=(\d+) descriptor_max=(\d+)"
    r" non_coherent_atom=(\d+)"
)
BUFFER_OPEN = re.compile(
    r"TRANSFER_OPEN index=0 queue_family=(\d+) bytes=4096"
    r" mode=([01]) src_type=(\d+) dst_type=(\d+)"
    r" coherent_src=([01]) coherent_dst=([01]) atom=(\d+)"
    r" shaders=0 features=0 extensions=0"
)


def sha256(path: Path) -> str:
    """Return SHA-256 of one complete archived transcript or native ELF."""
    assert path.is_file(), path

    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_sha256(path: Path) -> str:
    """Return canonical LF-normalized SHA-256 for a current source file."""
    return hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")
    ).hexdigest()


def release_event(point: int) -> str:
    """Derive exact resource counts from the ten real Vulkan acquisitions."""
    assert 1 <= point <= len(RESOURCES)

    mapped = 2 if point >= 10 else 1 if point >= 7 else 0
    buffers = 2 if point >= 8 else 1 if point >= 5 else 0
    memory = 2 if point >= 9 else 1 if point >= 6 else 0

    return (
        f"TRANSFER_RELEASE point={point} acquired={point}"
        f" mapped={mapped} buffers={buffers} memory={memory}"
        f" fence={int(point >= 4)} command={int(point >= 3)}"
        f" pool={int(point >= 2)} device=1"
    )


def verify_trial(
    lines: list[str],
    trial: int,
    api: str,
    memory_mode: str,
    point: int,
) -> None:
    """Verify fault, two real recoveries and VkDevice-before-instance close.

    The native log must contain a real Vulkan loader/physical-device
    selection, one exact failed acquisition, two independent successful
    VkDevice reopens with five and one fence-separated copies respectively,
    complete cache synchronization and explicit owner reset before the
    caller-owned VkInstance is closed. No GPU kernel is dispatched.
    """
    assert trial in (0, 1)
    assert api in ("1.0", "1.1")
    assert memory_mode in ("prefer", "noncoherent")
    assert 1 <= point <= 10
    assert len(lines) == 36, len(lines)

    cursor = 0

    def take() -> str:
        """Consume one exact native driver event from the trial."""
        nonlocal cursor
        assert cursor < len(lines), (trial, point, cursor)
        current = lines[cursor]
        cursor += 1

        return current

    def expect(expected: str) -> None:
        """Require one complete event at the prescribed ownership position."""
        value = take()
        assert value == expected, (trial, point, cursor - 1, value, expected)

    loader = re.fullmatch(
        r"VULKAN_LOADER_VERSION=1\.(\d+)\.(\d+)"
        r" source=vkEnumerateInstanceVersion",
        take(),
    )
    assert loader is not None
    assert int(loader.group(1)) >= int(api.split(".")[1])

    expect(
        f"SELECT_INSTANCE_API={api} loader_minor={loader.group(1)} cap=1.1"
    )

    instance = OPEN.fullmatch(take())
    assert instance is not None
    assert instance.group(1) == instance.group(2) == api
    count = int(instance.group(3))
    assert count > 0

    selected_api = re.fullmatch(
        rf"SELECT_DEVICE_API={re.escape(api)}"
        r" reported_minor=(\d+) instance_minor=(\d+) cap=1\.1",
        take(),
    )
    assert selected_api is not None
    assert int(selected_api.group(1)) >= int(api.split(".")[1])
    assert int(selected_api.group(2)) == int(api.split(".")[1])

    candidate = re.fullmatch(
        r"VULKAN_CANDIDATE index=0 name=(.+)"
        r" vendor=0x([0-9a-f]+) device=0x([0-9a-f]+)"
        r" api=1\.(\d+)\.(\d+) driver=0x([0-9a-f]+)",
        take(),
    )
    assert candidate is not None
    assert candidate.group(1)
    assert int(candidate.group(4)) >= int(api.split(".")[1])

    accepted = ACCEPT.fullmatch(take())
    assert accepted is not None
    family = int(accepted.group(1))
    assert int(accepted.group(2)) >= 1
    assert int(accepted.group(3)) >= 1
    assert int(accepted.group(4)) >= 1
    assert int(accepted.group(6)) >= 4096
    assert int(accepted.group(7)) >= 2
    atom = int(accepted.group(8))
    assert atom > 0 and (atom & (atom - 1)) == 0

    expect(
        f"DEVICE_CATALOG_SELECT index=0 queue_family={family}"
        f" effective_api={api} selections=1"
    )

    expect(
        f"TRANSFER_INJECT_FAULT point={point}"
        f" acquisition={point} resource={RESOURCES[point - 1]} result=-3"
    )
    expect("TRANSFER_OPEN_FAILED index=0 mode=" +
           ("0" if memory_mode == "prefer" else "1") + " result=-3")
    expect(
        "TRANSFER_CLOSE index=0 bytes=0 copies=0 copied_bytes=0"
        " flushes=0 invalidates=0 wait_result=0"
    )
    expect(release_event(point))
    expect(
        f"RECOVERY_FAULT_CONFIRMED trial={trial} point={point}"
        " result=-3 owner=0 instance=1 copies=0"
    )

    for phase in (0, 1):
        opened = BUFFER_OPEN.fullmatch(take())
        assert opened is not None
        assert int(opened.group(1)) == family
        assert int(opened.group(2)) == (
            0 if memory_mode == "prefer" else 1
        )
        assert 0 <= int(opened.group(3)) < 32
        assert 0 <= int(opened.group(4)) < 32

        coherent_src = int(opened.group(5))
        coherent_dst = int(opened.group(6))
        assert int(opened.group(7)) == atom

        if memory_mode == "noncoherent":
            assert coherent_src == coherent_dst == 0

        expect(
            f"RECOVERY_REOPEN trial={trial} phase={phase}"
            f" point={point} owner=1 acquisitions=10 copied=0"
        )

        words_for_phase = WORDS if phase == 0 else (1024,)
        copied_words = 0

        for round, words in enumerate(words_for_phase):
            copied_words += words
            bytes_count = words * 4
            flushes = (round + 1) if coherent_src == 0 else 0
            invalidates = (round + 1) if coherent_dst == 0 else 0

            expect(
                f"TRANSFER_COPY index=0 bytes={bytes_count}"
                f" copies={round + 1} flushes={flushes}"
                f" invalidates={invalidates} fence=signaled shaders=0"
            )
            expect(
                f"RECOVERY_VERIFIED trial={trial} phase={phase}"
                f" round={round} words={words}"
                f" bytes={bytes_count} valid=1 shaders=0"
            )

        expected_copies = len(words_for_phase)
        bytes_total = copied_words * 4
        flushes_total = expected_copies if coherent_src == 0 else 0
        invalidates_total = expected_copies if coherent_dst == 0 else 0

        expect(
            f"RECOVERY_PHASE_PASS trial={trial} phase={phase}"
            f" copies={expected_copies} verified_words={copied_words}"
            f" copied_bytes={bytes_total} flushes={flushes_total}"
            f" invalidates={invalidates_total} shaders=0"
        )
        expect(
            f"TRANSFER_CLOSE index=0 bytes=4096 copies={expected_copies}"
            f" copied_bytes={bytes_total} flushes={flushes_total}"
            f" invalidates={invalidates_total} wait_result=0"
        )
        expect(
            f"RECOVERY_PHASE_CLOSED trial={trial} phase={phase}"
            " owner=0 instance=1"
        )

    expect(
        f"DEVICE_CATALOG_CLOSE devices={count} selections=1"
    )
    expect(
        f"RECOVERY_TRIAL_PASS trial={trial} fault={point}"
        " failed=1 reopened=2 copies=6"
        " verified_words=2377 copied_bytes=9508 shaders=0"
    )
    assert cursor == len(lines)


def verify_native_body(
    lines: list[str],
    api: str,
    memory_mode: str,
    point: int,
) -> None:
    """Require two full native Vulkan recovery trials and exact summary."""
    assert len(lines) == 73, len(lines)

    verify_trial(lines[:36], 0, api, memory_mode, point)
    verify_trial(lines[36:72], 1, api, memory_mode, point)

    assert lines[72] == (
        f"RECOVERY_DIAGNOSTIC_PASS api={api}"
        f" mode={0 if memory_mode == 'prefer' else 1}"
        f" point={point} trials=2 faults=2 reopened=4"
        " copies=12 verified_words=4754 copied_bytes=19016 shaders=0"
    )

    assert not any(
        line.startswith(("MULTI_SUBMIT ", "JOB_PORT_SUBMIT ",
                         "KERN_RUN ", "BACKEND_VULKAN_SESSION_READY "))
        for line in lines
    )


def verify_cli(lines: list[str], abi: str) -> None:
    """Verify six malformed recovery requests rejected by the native CLI."""
    assert lines == [
        "TEST=vulkan-core10-transfer-recovery-invalid-inputs",
        f"ABI={abi}",
        "INVALID_CASES=6",
        "INVALID_CASE=missing-arguments exit=2",
        "INVALID_CASE=unsupported-api exit=2",
        "INVALID_CASE=zero-fault exit=2",
        "INVALID_CASE=fault-out-of-range exit=2",
        "INVALID_CASE=fault-nonnumeric exit=2",
        "INVALID_CASE=unsupported-memory-name exit=2",
    ]


def corrupt(
    lines: list[str],
    prefix: str,
    old: str,
    new: str,
) -> list[str]:
    """Create exactly one dishonest native event in a full driver transcript."""
    changed = lines.copy()
    indexes = [
        i for i, line in enumerate(lines)
        if line.startswith(prefix) and old in line
    ]
    assert indexes, (prefix, old)
    changed[indexes[0]] = changed[indexes[0]].replace(old, new, 1)
    assert changed != lines

    return changed


def verify_mutations(manifest: dict) -> None:
    """Demonstrate detection of false successful recovery and bad cleanup."""
    record = next(
        row for row in manifest["runs"]
        if row["device"] == "pixel-arm64"
        and row["api"] == "1.0"
        and row["memory_mode"] == "noncoherent"
        and row["fault_point"] == 10
    )
    original = (ARCHIVE / record["file"]).read_text(
        encoding="utf-8"
    ).splitlines()[6:]

    mutations = (
        ("TRANSFER_INJECT_FAULT ", "point=10", "point=9"),
        ("TRANSFER_INJECT_FAULT ", "resource=destination_mapping", "resource=source_mapping"),
        ("TRANSFER_OPEN_FAILED ", "result=-3", "result=0"),
        ("TRANSFER_RELEASE ", "acquired=10", "acquired=9"),
        ("TRANSFER_RELEASE ", "mapped=2", "mapped=1"),
        ("TRANSFER_RELEASE ", "buffers=2", "buffers=1"),
        ("TRANSFER_RELEASE ", "memory=2", "memory=1"),
        ("TRANSFER_RELEASE ", "device=1", "device=0"),
        ("RECOVERY_FAULT_CONFIRMED ", "owner=0", "owner=1"),
        ("RECOVERY_FAULT_CONFIRMED ", "instance=1", "instance=0"),
        ("TRANSFER_OPEN ", "coherent_src=0", "coherent_src=1"),
        ("TRANSFER_OPEN ", "coherent_dst=0", "coherent_dst=1"),
        ("TRANSFER_OPEN ", "atom=64", "atom=128"),
        ("RECOVERY_REOPEN ", "acquisitions=10", "acquisitions=9"),
        ("RECOVERY_REOPEN ", "copied=0", "copied=1"),
        ("TRANSFER_COPY ", "fence=signaled", "fence=pending"),
        ("TRANSFER_COPY ", "flushes=1", "flushes=0"),
        ("TRANSFER_COPY ", "invalidates=1", "invalidates=0"),
        ("RECOVERY_VERIFIED ", "valid=1", "valid=0"),
        ("RECOVERY_VERIFIED ", "words=1", "words=2"),
        ("RECOVERY_PHASE_PASS ", "verified_words=1353", "verified_words=1352"),
        ("RECOVERY_PHASE_PASS ", "flushes=5", "flushes=4"),
        ("RECOVERY_PHASE_PASS ", "invalidates=5", "invalidates=4"),
        ("TRANSFER_CLOSE ", "wait_result=0", "wait_result=-4"),
        ("RECOVERY_PHASE_CLOSED ", "owner=0", "owner=1"),
        ("RECOVERY_TRIAL_PASS ", "copies=6", "copies=5"),
        ("DEVICE_CATALOG_CLOSE ", "selections=1", "selections=0"),
        ("RECOVERY_DIAGNOSTIC_PASS ", "shaders=0", "shaders=1"),
    )

    for prefix, old, new in mutations:
        modified = corrupt(original, prefix, old, new)

        try:
            verify_native_body(
                modified, "1.0", "noncoherent", 10
            )
        except AssertionError:
            print("PASS rejected false GPU recovery evidence", prefix.strip())
        else:
            raise AssertionError(f"Accepted falsified recovery event: {prefix}")

    wrong_order = original.copy()

    reopen = next(
        i for i, line in enumerate(wrong_order)
        if line.startswith("RECOVERY_REOPEN ")
    )
    instance_close = next(
        i for i, line in enumerate(wrong_order)
        if line.startswith("DEVICE_CATALOG_CLOSE ")
    )

    wrong_order[reopen], wrong_order[instance_close] = (
        wrong_order[instance_close], wrong_order[reopen]
    )

    try:
        verify_native_body(wrong_order, "1.0", "noncoherent", 10)
    except AssertionError:
        print("PASS rejected instance destruction before recovered VkDevice")
    else:
        raise AssertionError("Accepted prematurely closed recovery VkInstance")

    wrong_phase = original.copy()
    phases = [
        i for i, line in enumerate(wrong_phase)
        if line.startswith("RECOVERY_PHASE_CLOSED ")
    ]
    assert len(phases) == 4

    wrong_phase[phases[0]], wrong_phase[phases[1]] = (
        wrong_phase[phases[1]], wrong_phase[phases[0]]
    )

    try:
        verify_native_body(wrong_phase, "1.0", "noncoherent", 10)
    except AssertionError:
        print("PASS rejected impossible recovered-session lifecycle order")
    else:
        raise AssertionError("Accepted swapped recovered session closures")

    control = next(
        row for row in manifest["invalid_cli"]
        if row["device"] == "pixel-arm64"
    )
    control_lines = (ARCHIVE / control["file"]).read_text(
        encoding="utf-8"
    ).splitlines()
    wrong_cli = corrupt(
        control_lines, "INVALID_CASE=fault-out-of-range",
        "exit=2", "exit=0",
    )
    try:
        verify_cli(wrong_cli, control["abi"])
    except AssertionError:
        print("PASS rejected fabricated successful invalid recovery request")
    else:
        raise AssertionError("Accepted corrupted recovery CLI control")

    print("PASS 31 deliberate GPU fault-recovery evidence corruptions rejected")


def inspect_elf(
    readelf: Path,
    executable: Path,
    abi: str,
) -> None:
    """Statically verify Android API24, Vulkan core1.0 and actual process ABI."""
    def read(*options: str) -> str:
        """Inspect the ELF using the supplied existing Android NDK tool."""
        return subprocess.run(
            [str(readelf), "-W", *options, str(executable)],
            check=True, capture_output=True, text=True,
            encoding="utf-8",
        ).stdout

    header = read("--file-header")
    notes = read("--notes")
    dynamic = read("--dynamic")
    symbols = read("--dyn-syms")

    elf_class = "ELF64" if abi == "arm64" else "ELF32"
    machine = "AArch64" if abi == "arm64" else "ARM"

    assert re.search(rf"Class:\s+{elf_class}\b", header)
    assert re.search(rf"Machine:\s+{machine}\b", header)
    assert "NT_ANDROID_TYPE_IDENT" in notes
    assert "description data: 18 00 00 00" in notes
    assert "Shared library: [libvulkan.so]" in dynamic

    for symbol in (
        "vkGetInstanceProcAddr", "vkCreateDevice", "vkCreateBuffer",
        "vkAllocateMemory", "vkMapMemory", "vkCmdCopyBuffer",
        "vkQueueSubmit", "vkWaitForFences",
        "vkFlushMappedMemoryRanges", "vkInvalidateMappedMemoryRanges",
        "vkDestroyDevice",
    ):
        assert re.search(rf"UND\s+{symbol}\b", symbols), symbol

    assert not re.search(r"UND\s+vkEnumerateInstanceVersion\b", symbols)


def main() -> None:
    """Verify all real Android Vulkan recovery lifecycles and native binaries."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--artifacts", type=Path,
        help="Directory with exact built Android ARM64 and ARMv7 recovery ELFs",
    )
    parser.add_argument(
        "--readelf", type=Path,
        help="Android NDK llvm-readelf for independent static ELF inspection",
    )
    args = parser.parse_args()

    if args.readelf is not None and args.artifacts is None:
        parser.error("--readelf requires --artifacts")

    manifest = json.loads(
        (ARCHIVE / "manifest.json").read_text(encoding="utf-8")
    )

    assert manifest["minimum_android_api"] == 24
    assert manifest["apis"] == ["1.0", "1.1"]
    assert manifest["injected_failure_result"] == "VK_ERROR_INITIALIZATION_FAILED"
    assert manifest["fault_driver_returned_error_simulated"] is False
    assert manifest["physical_gpu_device_loss_tested"] is False
    assert manifest["production_hashcat_backend_connected"] is False
    assert manifest["gpu_shader_dispatches"] == 0
    assert manifest["password_or_cryptographic_inputs"] is False
    assert manifest["no_vulkan10_only_hardware"] is True
    assert manifest["no_android_api24_os_hardware"] is True
    assert len(manifest["source_sha256"]) == 13

    for path, digest in manifest["source_sha256"].items():
        assert source_sha256(ROOT / path) == digest, path

    assert manifest["fault_points"] == {
        str(point): resource
        for point, resource in enumerate(RESOURCES, start=1)
    }
    assert len(manifest["runs"]) == 80
    assert len(manifest["invalid_cli"]) == 3

    expected = {
        (device, api, mode, point)
        for device, (_, modes) in DEVICES.items()
        for api in ("1.0", "1.1")
        for mode in modes
        for point in range(1, 11)
    }
    seen = set()
    faults = recovered = copies = words = bytes_total = noncoherent = 0

    for row in manifest["runs"]:
        key = (
            row["device"], row["api"],
            row["memory_mode"], row["fault_point"],
        )
        assert key in expected and key not in seen, key
        seen.add(key)

        abi, modes = DEVICES[row["device"]]
        assert row["abi"] == abi
        assert row["memory_mode"] in modes
        assert row["acquired_resource"] == RESOURCES[row["fault_point"] - 1]
        assert row["native_exit_code"] == 0
        assert row["injected_faults"] == 2
        assert row["recovered_vkdevices"] == 4
        assert row["shared_vkinstances"] == 2
        assert row["gpu_copies"] == 12
        assert row["verified_words"] == 4754
        assert row["verified_bytes"] == 19016
        assert row["noncoherent_copies"] == (
            12 if row["memory_mode"] == "noncoherent" else 0
        )

        path = ARCHIVE / row["file"]
        data = path.read_bytes()
        assert data and data.endswith(b"\n") and b"\r" not in data, path
        assert b"91809037" not in data and b"46281FDAS002AL" not in data
        assert sha256(path) == row["sha256"]
        lines = path.read_text(encoding="utf-8").splitlines()

        assert lines[:6] == [
            "TEST=vulkan-core10-transfer-recovery",
            f"ABI={row['abi']}",
            f"API={row['api']}",
            f"MEMORY_MODE={row['memory_mode']}",
            f"FAULT_POINT={row['fault_point']}",
            "ADB_EXIT_CODE=0",
        ]

        verify_native_body(
            lines[6:], row["api"],
            row["memory_mode"], row["fault_point"],
        )

        faults += row["injected_faults"]
        recovered += row["recovered_vkdevices"]
        copies += row["gpu_copies"]
        words += row["verified_words"]
        bytes_total += row["verified_bytes"]
        noncoherent += row["noncoherent_copies"]

    assert seen == expected
    assert faults == 160
    assert recovered == 320
    assert copies == 960
    assert words == 380320
    assert bytes_total == 1521280
    assert noncoherent == 240

    assert len(list(ARCHIVE.glob("*.log"))) == 83

    controls = set()

    for row in manifest["invalid_cli"]:
        assert row["device"] in DEVICES
        assert row["device"] not in controls
        controls.add(row["device"])
        assert row["abi"] == DEVICES[row["device"]][0]
        assert row["invalid_requests"] == 6

        path = ARCHIVE / row["file"]
        assert sha256(path) == row["sha256"]

        verify_cli(
            path.read_text(encoding="utf-8").splitlines(), row["abi"]
        )

    assert controls == set(DEVICES)

    if args.artifacts is not None:
        directory = args.artifacts.resolve(strict=True)
        assert set(manifest["native_elf_sha256"]) == {"arm64", "armv7"}

        for abi, digest in manifest["native_elf_sha256"].items():
            binary = directory / f"vulkan-core10-transfer-recovery-{abi}"
            assert sha256(binary) == digest, abi

            if args.readelf is not None:
                inspect_elf(
                    args.readelf.resolve(strict=True), binary, abi
                )

        print("PASS both exact Android ARM64/ARMv7 recovery ELF SHA-256")

        if args.readelf is not None:
            print("PASS actual Vulkan 1.0 symbols and Android API 24 ELF notes")

    verify_mutations(manifest)

    print("PASS 80 actual Android GPU API/memory/fault recovery invocations")
    print("PASS 160 controlled acquisition failures followed by 320 fresh VkDevices")
    print("PASS 160 persistent VkInstance lifecycles and 960 completed GPU copies")
    print("PASS 380320 verified public integer words and 1521280 bytes")
    print("PASS 240 genuinely noncoherent flush/invalidate GPU transfers")
    print("PASS 18 rejected malformed Android fault-recovery commands")


if __name__ == "__main__":
    main()
