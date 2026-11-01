#!/usr/bin/env python3
"""Independently audit genuine Android Vulkan fence status transitions.

Verifies captured Vulkan core 1.0/1.1 GPU driver results: initial and
reset fence VK_NOT_READY, zero/1ms VK_TIMEOUT, actual signaled
VK_SUCCESS after queue copies, exact host memory synchronization,
correct resource ownership and public integer comparisons. Also checks
the source and native Android ELF identities and rejects deliberately
corrupted transcripts. Does not invoke ADB or run GPU commands.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = ROOT / "tests/fixtures/android-gpu/vulkan-core10-transfer-fence-status-20261101"

DEVICES = {
    "redmagic-arm64": ("arm64-v8a", ("prefer",)),
    "redmagic-armv7": ("armeabi-v7a", ("prefer",)),
    "pixel-arm64": ("arm64-v8a", ("prefer", "noncoherent")),
}
WORDS = (1, 7, 64, 257, 1024)

INSTANCE_OPEN = re.compile(
    r"DEVICE_CATALOG_OPEN requested_api=(1\.[01])"
    r" instance_api=(1\.[01]) devices=(\d+)"
    r" extensions=0 layers=0"
)
COMPUTE_LIMITS = re.compile(
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
    """Return SHA-256 of all exact bytes in one file."""
    assert path.is_file(), path

    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_sha256(path: Path) -> str:
    """Return SHA-256 for a checked-out source normalized from CRLF to LF."""
    return hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")
    ).hexdigest()


def verify_trial(
    events: list[str],
    trial: int,
    api: str,
    mode: str,
) -> None:
    """Verify one real Vulkan instance, fence lifecycle and five GPU copies.

    Every driver event is consumed in the exact resource order. The
    unsignaled fence genuinely reports VK_NOT_READY and VK_TIMEOUT;
    each completed copy must report VK_SUCCESS before a host fence
    reset and the next unsignaled status query. Resource cleanup
    follows the final fence check and precedes VkInstance destruction.
    """
    assert len(events) == 40, (len(events), events)
    assert trial in (0, 1)
    assert api in ("1.0", "1.1")
    assert mode in ("prefer", "noncoherent")

    cursor = 0

    def take() -> str:
        """Consume the next exact recorded Vulkan driver event."""
        nonlocal cursor
        assert cursor < len(events), (trial, cursor)
        value = events[cursor]
        cursor += 1

        return value

    def expect(expected: str) -> None:
        """Require exact equality for the next source-owned event."""
        value = take()
        assert value == expected, (trial, cursor - 1, value, expected)

    loader = re.fullmatch(
        r"VULKAN_LOADER_VERSION=1\.(\d+)\.(\d+)"
        r" source=vkEnumerateInstanceVersion",
        take(),
    )
    assert loader is not None
    minor = int(loader.group(1))
    assert minor >= int(api.split(".")[1])

    expect(
        f"SELECT_INSTANCE_API={api} loader_minor={minor} cap=1.1"
    )

    opened = INSTANCE_OPEN.fullmatch(take())
    assert opened is not None
    assert opened.group(1) == opened.group(2) == api
    device_count = int(opened.group(3))
    assert device_count > 0

    selection = re.fullmatch(
        rf"SELECT_DEVICE_API={re.escape(api)}"
        r" reported_minor=(\d+) instance_minor=(\d+) cap=1\.1",
        take(),
    )
    assert selection is not None
    assert int(selection.group(1)) >= int(api.split(".")[1])
    assert int(selection.group(2)) == int(api.split(".")[1])

    candidate = re.fullmatch(
        r"VULKAN_CANDIDATE index=0 name=(.+)"
        r" vendor=0x([0-9a-f]+) device=0x([0-9a-f]+)"
        r" api=1\.(\d+)\.(\d+) driver=0x([0-9a-f]+)",
        take(),
    )
    assert candidate is not None
    assert candidate.group(1)
    assert int(candidate.group(4)) >= int(api.split(".")[1])

    limits = COMPUTE_LIMITS.fullmatch(take())
    assert limits is not None
    family = int(limits.group(1))
    assert int(limits.group(2)) >= 1
    assert int(limits.group(3)) >= 1
    assert int(limits.group(4)) >= 1
    assert int(limits.group(6)) >= 4096
    assert int(limits.group(7)) >= 2
    atom = int(limits.group(8))
    assert atom > 0 and (atom & (atom - 1)) == 0

    expect(
        f"DEVICE_CATALOG_SELECT index=0 queue_family={family}"
        f" effective_api={api} selections=1"
    )

    allocated = BUFFER_OPEN.fullmatch(take())
    assert allocated is not None
    assert int(allocated.group(1)) == family
    memory_code = 0 if mode == "prefer" else 1
    assert int(allocated.group(2)) == memory_code
    assert 0 <= int(allocated.group(3)) < 32
    assert 0 <= int(allocated.group(4)) < 32
    src_coherent = int(allocated.group(5))
    dst_coherent = int(allocated.group(6))
    assert int(allocated.group(7)) == atom

    if mode == "noncoherent":
        assert src_coherent == dst_coherent == 0

    expect(
        f"FENCE_SESSION_OPEN trial={trial}"
        f" mode={memory_code} owner=1 copies=0"
    )
    expect(
        f"FENCE_UNSIGNALED trial={trial} step=0"
        " status=1 wait_zero=2"
    )
    expect(
        f"FENCE_TIMED_WAIT trial={trial}"
        " timeout_ns=1000000 result=2"
    )
    expect(
        f"FENCE_PENDING_NO_COPY trial={trial}"
        " output_unchanged=1024 copies=0"
    )

    for round, words in enumerate(WORDS):
        size = words * 4
        flushes = round + 1 if src_coherent == 0 else 0
        invalidates = round + 1 if dst_coherent == 0 else 0

        expect(
            f"TRANSFER_COPY index=0 bytes={size}"
            f" copies={round + 1} flushes={flushes}"
            f" invalidates={invalidates} fence=signaled shaders=0"
        )
        expect(
            f"FENCE_SIGNALED trial={trial} round={round}"
            " status=0 wait_zero=0"
        )
        expect(
            f"FENCE_COPY_VERIFIED trial={trial} round={round}"
            f" words={words} bytes={size} valid=1"
        )
        expect(
            f"FENCE_RESET trial={trial} round={round} result=0"
        )
        expect(
            f"FENCE_UNSIGNALED trial={trial} step={round + 1}"
            " status=1 wait_zero=2"
        )

    expect(
        f"FENCE_TRIAL_PASS trial={trial}"
        " copies=5 verified_words=1353 copied_bytes=5412"
        " not_ready=6 zero_timeouts=6 bounded_timeouts=1"
        " signaled=5 zero_success=5 resets=5 shaders=0"
    )
    expect(
        "TRANSFER_CLOSE index=0 bytes=4096 copies=5"
        " copied_bytes=5412"
        f" flushes={5 if src_coherent == 0 else 0}"
        f" invalidates={5 if dst_coherent == 0 else 0}"
        " wait_result=0"
    )
    expect(
        f"DEVICE_CATALOG_CLOSE devices={device_count} selections=1"
    )

    assert cursor == len(events)


def verify_native(
    events: list[str],
    api: str,
    memory_mode: str,
) -> None:
    """Validate both complete Vulkan driver trials and native summary."""
    assert len(events) == 81, len(events)
    assert memory_mode in ("prefer", "noncoherent")

    verify_trial(events[:40], 0, api, memory_mode)
    verify_trial(events[40:80], 1, api, memory_mode)

    expect = (
        f"FENCE_DIAGNOSTIC_PASS api={api}"
        f" mode={0 if memory_mode == 'prefer' else 1}"
        " trials=2 copies=10 verified_words=2706 copied_bytes=10824"
        " not_ready=12 zero_timeouts=12 bounded_timeouts=2"
        " signaled=10 zero_success=10 resets=10 shaders=0"
    )
    assert events[80] == expect

    assert not any(
        event.startswith(("BACKEND_VULKAN_", "JOB_PORT_SUBMIT ",
                          "KERN_RUN ", "RECOVERY_FAULT_CONFIRMED "))
        for event in events
    )


def verify_cli(lines: list[str], abi: str) -> None:
    """Require all five malformed Android commands to return exit 2."""
    assert lines == [
        "TEST=vulkan-core10-transfer-fence-status-invalid-inputs",
        f"ABI={abi}",
        "INVALID_CASES=5",
        "INVALID_CASE=missing-options exit=2",
        "INVALID_CASE=unsupported-api exit=2",
        "INVALID_CASE=unsupported-memory exit=2",
        "INVALID_CASE=wrong-flag exit=2",
        "INVALID_CASE=extra-parameter exit=2",
    ]


def corrupt(
    events: list[str],
    prefix: str,
    old: str,
    new: str,
) -> list[str]:
    """Produce exactly one explicitly false native Vulkan event."""
    result = events.copy()
    positions = [
        index for index, event in enumerate(events)
        if event.startswith(prefix) and old in event
    ]
    assert positions, (prefix, old)
    index = positions[0]
    result[index] = result[index].replace(old, new, 1)
    assert result != events

    return result


def verify_mutations(manifest: dict) -> None:
    """Reject dishonest VkResult, fence, copy, memory and cleanup records."""
    row = next(
        item for item in manifest["runs"]
        if item["device"] == "pixel-arm64"
        and item["api"] == "1.0"
        and item["memory_mode"] == "noncoherent"
    )
    data = (ARCHIVE / row["file"]).read_text(
        encoding="utf-8"
    ).splitlines()[5:]

    mutations = (
        ("FENCE_UNSIGNALED ", "status=1", "status=0"),
        ("FENCE_UNSIGNALED ", "wait_zero=2", "wait_zero=0"),
        ("FENCE_TIMED_WAIT ", "timeout_ns=1000000", "timeout_ns=0"),
        ("FENCE_TIMED_WAIT ", "result=2", "result=0"),
        ("FENCE_PENDING_NO_COPY ", "output_unchanged=1024", "output_unchanged=1023"),
        ("FENCE_PENDING_NO_COPY ", "copies=0", "copies=1"),
        ("TRANSFER_OPEN ", "coherent_src=0", "coherent_src=1"),
        ("TRANSFER_OPEN ", "coherent_dst=0", "coherent_dst=1"),
        ("TRANSFER_OPEN ", "atom=64", "atom=128"),
        ("TRANSFER_COPY ", "bytes=4", "bytes=8"),
        ("TRANSFER_COPY ", "flushes=1", "flushes=0"),
        ("TRANSFER_COPY ", "invalidates=1", "invalidates=0"),
        ("TRANSFER_COPY ", "fence=signaled", "fence=pending"),
        ("FENCE_SIGNALED ", "status=0", "status=1"),
        ("FENCE_SIGNALED ", "wait_zero=0", "wait_zero=2"),
        ("FENCE_COPY_VERIFIED ", "valid=1", "valid=0"),
        ("FENCE_COPY_VERIFIED ", "words=1", "words=2"),
        ("FENCE_RESET ", "result=0", "result=-4"),
        ("FENCE_TRIAL_PASS ", "not_ready=6", "not_ready=5"),
        ("FENCE_TRIAL_PASS ", "zero_timeouts=6", "zero_timeouts=5"),
        ("FENCE_TRIAL_PASS ", "bounded_timeouts=1", "bounded_timeouts=0"),
        ("FENCE_TRIAL_PASS ", "signaled=5", "signaled=4"),
        ("FENCE_TRIAL_PASS ", "resets=5", "resets=4"),
        ("TRANSFER_CLOSE ", "flushes=5", "flushes=4"),
        ("TRANSFER_CLOSE ", "invalidates=5", "invalidates=4"),
        ("TRANSFER_CLOSE ", "wait_result=0", "wait_result=-4"),
        ("DEVICE_CATALOG_CLOSE ", "selections=1", "selections=0"),
        ("FENCE_DIAGNOSTIC_PASS ", "copies=10", "copies=9"),
        ("FENCE_DIAGNOSTIC_PASS ", "shaders=0", "shaders=1"),
    )

    for prefix, old, new in mutations:
        forged = corrupt(data, prefix, old, new)
        try:
            verify_native(forged, "1.0", "noncoherent")
        except AssertionError:
            print("PASS rejected forged Vulkan driver status", prefix.strip())
        else:
            raise AssertionError(f"Accepted falsified status: {prefix}")

    premature_close = data.copy()
    device = next(
        index for index, event in enumerate(data)
        if event.startswith("TRANSFER_CLOSE ")
    )
    instance = next(
        index for index, event in enumerate(data)
        if event.startswith("DEVICE_CATALOG_CLOSE ")
    )
    premature_close[device], premature_close[instance] = (
        premature_close[instance], premature_close[device]
    )

    try:
        verify_native(premature_close, "1.0", "noncoherent")
    except AssertionError:
        print("PASS rejected VkInstance destruction before VkDevice")
    else:
        raise AssertionError("Accepted premature Vulkan instance destruction")

    early_reset = data.copy()
    completed = next(
        index for index, event in enumerate(data)
        if event.startswith("FENCE_SIGNALED ")
    )
    reset = next(
        index for index, event in enumerate(data)
        if event.startswith("FENCE_RESET ")
    )
    early_reset[completed], early_reset[reset] = (
        early_reset[reset], early_reset[completed]
    )

    try:
        verify_native(early_reset, "1.0", "noncoherent")
    except AssertionError:
        print("PASS rejected fence reset before completion")
    else:
        raise AssertionError("Accepted incorrect fence status/reset order")

    cli = next(
        item for item in manifest["invalid_cli"]
        if item["device"] == "pixel-arm64"
    )
    invalid = (ARCHIVE / cli["file"]).read_text(
        encoding="utf-8"
    ).splitlines()
    forged = corrupt(
        invalid, "INVALID_CASE=unsupported-api",
        "exit=2", "exit=0"
    )

    try:
        verify_cli(forged, cli["abi"])
    except AssertionError:
        print("PASS rejected falsely accepted invalid Android command")
    else:
        raise AssertionError("Accepted an invalid Vulkan CLI as successful")

    print("PASS 32 deliberate Vulkan fence-status evidence corruptions rejected")


def inspect_elf(
    readelf: Path,
    executable: Path,
    abi: str,
) -> None:
    """Statically validate Android API24 and core Vulkan 1.0 fence imports."""
    def read(*arguments: str) -> str:
        """Return read-only Android llvm-readelf output."""
        return subprocess.run(
            [str(readelf), "-W", *arguments, str(executable)],
            check=True, capture_output=True, text=True,
            encoding="utf-8",
        ).stdout

    header = read("--file-header")
    notes = read("--notes")
    dynamic = read("--dynamic")
    symbols = read("--dyn-syms")

    expected_class = "ELF64" if abi == "arm64" else "ELF32"
    expected_machine = "AArch64" if abi == "arm64" else "ARM"

    assert re.search(rf"Class:\s+{expected_class}\b", header)
    assert re.search(rf"Machine:\s+{expected_machine}\b", header)
    assert "NT_ANDROID_TYPE_IDENT" in notes
    assert "description data: 18 00 00 00" in notes
    assert "Shared library: [libvulkan.so]" in dynamic

    for symbol in (
        "vkGetInstanceProcAddr", "vkCreateDevice", "vkCreateFence",
        "vkGetFenceStatus", "vkWaitForFences", "vkResetFences",
        "vkCreateBuffer", "vkAllocateMemory", "vkMapMemory",
        "vkCmdCopyBuffer", "vkQueueSubmit",
        "vkFlushMappedMemoryRanges", "vkInvalidateMappedMemoryRanges",
        "vkDestroyDevice",
    ):
        assert re.search(rf"UND\s+{symbol}\b", symbols), symbol

    assert not re.search(r"UND\s+vkEnumerateInstanceVersion\b", symbols)


def main() -> None:
    """Verify eight real GPU status logs, 15 invalid inputs and all identities."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--artifacts", type=Path,
        help="Exact Android ARM64/ARMv7 fence-status executable directory",
    )
    parser.add_argument(
        "--readelf", type=Path,
        help="Android NDK llvm-readelf for static Vulkan ABI inspection",
    )
    args = parser.parse_args()

    if args.readelf is not None and args.artifacts is None:
        parser.error("--readelf requires --artifacts")

    manifest = json.loads(
        (ARCHIVE / "manifest.json").read_text(encoding="utf-8")
    )

    assert manifest["minimum_android_api"] == 24
    assert manifest["apis"] == ["1.0", "1.1"]
    assert manifest["vk_not_ready"] == 1
    assert manifest["vk_timeout"] == 2
    assert manifest["vk_success"] == 0
    assert manifest["real_driver_results_injected"] is False
    assert manifest["vk_error_device_lost_induced"] is False
    assert manifest["production_hashcat_backend_connected"] is False
    assert manifest["gpu_shader_dispatches"] == 0
    assert manifest["password_or_cryptographic_inputs"] is False
    assert manifest["vulkan10_only_hardware_tested"] is False
    assert manifest["android_api24_os_tested"] is False

    assert len(manifest["source_sha256"]) == 13
    for source, digest in manifest["source_sha256"].items():
        assert source_sha256(ROOT / source) == digest, source

    assert len(manifest["runs"]) == 8
    assert len(manifest["invalid_cli"]) == 3
    assert len(list(ARCHIVE.glob("*.log"))) == 11

    expected = {
        (device, api, policy)
        for device, (_, policies) in DEVICES.items()
        for api in ("1.0", "1.1")
        for policy in policies
    }

    seen = set()
    sessions = pending = timeout_zero = timeout_bounded = 0
    copies = verified_words = verified_bytes = noncoherent_copies = 0

    for row in manifest["runs"]:
        key = (
            row["device"], row["api"], row["memory_mode"]
        )
        assert key in expected and key not in seen
        seen.add(key)

        abi, modes = DEVICES[row["device"]]
        assert row["abi"] == abi
        assert row["memory_mode"] in modes
        assert row["native_exit_code"] == 0
        assert row["vkdevice_sessions"] == 2
        assert row["real_not_ready"] == 12
        assert row["real_zero_timeout"] == 12
        assert row["real_bounded_timeout"] == 2
        assert row["real_signaled_success"] == 10
        assert row["fence_resets"] == 10
        assert row["gpu_copies"] == 10
        assert row["verified_words"] == 2706
        assert row["verified_bytes"] == 10824
        assert row["noncoherent_copies"] == (
            10 if row["memory_mode"] == "noncoherent" else 0
        )

        logfile = ARCHIVE / row["file"]
        bytes_in_log = logfile.read_bytes()
        assert bytes_in_log and bytes_in_log.endswith(b"\n")
        assert b"\r" not in bytes_in_log
        assert b"91809037" not in bytes_in_log
        assert b"46281FDAS002AL" not in bytes_in_log
        assert sha256(logfile) == row["sha256"]

        lines = logfile.read_text(encoding="utf-8").splitlines()
        assert lines[:5] == [
            "TEST=vulkan-core10-transfer-fence-status",
            f"ABI={row['abi']}",
            f"API={row['api']}",
            f"MEMORY_MODE={row['memory_mode']}",
            "ADB_EXIT_CODE=0",
        ]

        verify_native(
            lines[5:], row["api"], row["memory_mode"]
        )

        sessions += row["vkdevice_sessions"]
        pending += row["real_not_ready"]
        timeout_zero += row["real_zero_timeout"]
        timeout_bounded += row["real_bounded_timeout"]
        copies += row["gpu_copies"]
        verified_words += row["verified_words"]
        verified_bytes += row["verified_bytes"]
        noncoherent_copies += row["noncoherent_copies"]

    assert seen == expected
    assert sessions == 16
    assert pending == 96
    assert timeout_zero == 96
    assert timeout_bounded == 16
    assert copies == 80
    assert verified_words == 21648
    assert verified_bytes == 86592
    assert noncoherent_copies == 20

    seen_controls = set()

    for row in manifest["invalid_cli"]:
        assert row["device"] in DEVICES
        assert row["device"] not in seen_controls
        seen_controls.add(row["device"])
        assert row["abi"] == DEVICES[row["device"]][0]
        assert row["invalid_requests"] == 5

        logfile = ARCHIVE / row["file"]
        assert sha256(logfile) == row["sha256"]
        verify_cli(
            logfile.read_text(encoding="utf-8").splitlines(), row["abi"]
        )

    assert seen_controls == set(DEVICES)

    if args.artifacts is not None:
        directory = args.artifacts.resolve(strict=True)
        assert set(manifest["native_elf_sha256"]) == {"arm64", "armv7"}

        for abi, digest in manifest["native_elf_sha256"].items():
            executable = directory / f"vulkan-core10-transfer-fence-status-{abi}"
            assert sha256(executable) == digest

            if args.readelf is not None:
                inspect_elf(
                    args.readelf.resolve(strict=True), executable, abi
                )

        print("PASS exact ARM64/ARMv7 Vulkan real-fence-status ELF identities")

        if args.readelf is not None:
            print("PASS actual Vulkan 1.0 fence/copy imports and Android API24 notes")

    verify_mutations(manifest)

    print("PASS 8 real Android GPU API/memory fence status records")
    print("PASS 16 VkDevice lifecycles, 96 VK_NOT_READY and 112 VK_TIMEOUT")
    print("PASS 80 completed queue buffer copies, 21648 verified integer words")
    print("PASS 86592 bytes and 20 synchronized noncoherent GPU copies")
    print("PASS 15 malformed Android native fence CLI requests")


if __name__ == "__main__":
    main()
