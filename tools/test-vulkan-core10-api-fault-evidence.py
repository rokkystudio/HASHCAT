#!/usr/bin/env python3
"""Independently verify real Android Vulkan linked-API fault interception.

The native binary interposes exactly vkQueueSubmit and vkWaitForFences
using test-only ld.lld --wrap symbols. Before an injected return,
public GPU buffer copies and fence statuses originate from a real
Android driver. A negative test-generated VkResult then propagates
through the genuine transfer method, and subsequent Vulkan entry
calls are intercepted without reaching the driver. Independently
reopened VkDevices must complete real public GPU copies.

Verification reads archived LF-only logs, source files and Android
ELFs. It never runs HASHCAT, ADB, a GPU command or a cryptographic
workload, and never claims actual GPU device loss.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = ROOT / "tests/fixtures/android-gpu/vulkan-core10-api-fault-boundary-20261101"

DEVICES = {
    "redmagic-arm64": ("arm64-v8a", ("prefer",)),
    "redmagic-armv7": ("armeabi-v7a", ("prefer",)),
    "pixel-arm64": ("arm64-v8a", ("prefer", "noncoherent")),
}
SITES = ("submit", "wait")
ERRORS = {"device-lost": -4, "device-memory": -2}

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
    """Return SHA-256 over all exact bytes of an archived record or ELF."""
    assert path.is_file(), path

    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_sha256(path: Path) -> str:
    """Hash checked-out source after normalizing Windows CRLF to LF."""
    return hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")
    ).hexdigest()


def verify_buffer_open(
    event: str,
    family: int,
    mode: int,
    atom: int,
) -> tuple[int, int]:
    """Check exact device-memory compatibility for one new VkDevice.

    Reports whether the driver-selected source and destination memory
    actually have HOST_COHERENT. Required noncoherent mode must not
    silently choose coherent memory.
    """
    match = BUFFER_OPEN.fullmatch(event)
    assert match is not None, event
    assert int(match.group(1)) == family
    assert int(match.group(2)) == mode
    assert 0 <= int(match.group(3)) < 32
    assert 0 <= int(match.group(4)) < 32
    assert int(match.group(7)) == atom

    src = int(match.group(5))
    dst = int(match.group(6))

    if mode == 1:
        assert src == dst == 0

    return src, dst


def verify_trial(
    lines: list[str],
    trial: int,
    api: str,
    memory_mode: str,
    fault_site: str,
    error: int,
) -> None:
    """Verify every real Vulkan operation and each test-only API return.

    The ordered record proves real GPU copies before interception,
    precisely one VkResult generated at the selected linked Vulkan
    function, zero real driver calls after the injected result, six
    denied host/API submissions, complete old VkDevice release and
    creation of a fresh working VkDevice under the same VkInstance.
    The trial does not claim physical GPU device loss.
    """
    assert trial in (0, 1)
    assert api in ("1.0", "1.1")
    assert memory_mode in ("prefer", "noncoherent")
    assert fault_site in SITES
    assert error in (-4, -2)
    assert len(lines) == (59 if fault_site == "wait" else 58), len(lines)

    mode = 0 if memory_mode == "prefer" else 1
    source_site = "queue_submit" if fault_site == "submit" else "fence_wait"
    real_submissions = 1 if fault_site == "wait" else 0
    wait_attempts = 2 if fault_site == "wait" else 1
    cursor = 0

    def take() -> str:
        """Consume one exact event from the recorded driver/host sequence."""
        nonlocal cursor
        assert cursor < len(lines), (trial, cursor)
        event = lines[cursor]
        cursor += 1

        return event

    def expect(event: str) -> None:
        """Assert that the next saved event has exact values and ordering."""
        actual = take()
        assert actual == event, (trial, cursor - 1, actual, event)

    loader = re.fullmatch(
        r"VULKAN_LOADER_VERSION=1\.(\d+)\.(\d+)"
        r" source=vkEnumerateInstanceVersion",
        take(),
    )
    assert loader is not None
    loader_minor = int(loader.group(1))
    assert loader_minor >= int(api.split(".")[1])

    expect(
        f"SELECT_INSTANCE_API={api} loader_minor={loader_minor} cap=1.1"
    )

    instance = INSTANCE_OPEN.fullmatch(take())
    assert instance is not None
    assert instance.group(1) == instance.group(2) == api
    device_count = int(instance.group(3))
    assert device_count > 0

    select_api = re.fullmatch(
        rf"SELECT_DEVICE_API={re.escape(api)}"
        r" reported_minor=(\d+) instance_minor=(\d+) cap=1\.1",
        take(),
    )
    assert select_api is not None
    assert int(select_api.group(1)) >= int(api.split(".")[1])
    assert int(select_api.group(2)) == int(api.split(".")[1])

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

    coherent_src, coherent_dst = verify_buffer_open(
        take(), family, mode, atom
    )

    expect(
        "RESULT_GATE_OPEN state=active first_error=0"
        " allowed=0 blocked=0"
    )
    expect(
        f"API_FAULT_REAL_DRIVER trial={trial}"
        " site=fence_status observed=1"
    )
    expect(
        "RESULT_GATE_OBSERVE site=fence_status observed=1"
        " state=active nonfatal=1"
    )
    expect(
        f"API_FAULT_REAL_DRIVER trial={trial}"
        " site=fence_wait observed=2"
    )
    expect(
        "RESULT_GATE_OBSERVE site=fence_wait observed=2"
        " state=active nonfatal=2"
    )

    for round, words in enumerate((7, 1024)):
        size = words * 4
        flush = (round + 1) if coherent_src == 0 else 0
        invalidate = (round + 1) if coherent_dst == 0 else 0

        expect(
            f"RESULT_GATE_ALLOW count={round + 1} state=active"
        )
        expect(
            f"TRANSFER_COPY index=0 bytes={size}"
            f" copies={round + 1}"
            f" flushes={flush} invalidates={invalidate}"
            " fence=signaled shaders=0"
        )
        expect(
            f"RESULT_GATE_OBSERVE site=transfer observed=0"
            f" state=active successes={round * 2 + 1}"
        )
        expect(
            f"RESULT_GATE_OBSERVE site=fence_status observed=0"
            f" state=active successes={round * 2 + 2}"
        )
        expect(
            f"API_FAULT_GOOD_COPY trial={trial}"
            f" phase=before round={round}"
            f" words={words} bytes={size}"
            " fence=0 valid=1 shaders=0"
        )

    expect(
        f"API_FAULT_ARM site={source_site} result={error}"
        " origin=test_linker_wrap real_device_loss=0"
    )
    expect("RESULT_GATE_ALLOW count=3 state=active")

    if fault_site == "wait":
        expect(
            "API_FAULT_REAL_SUBMIT result=0 real_submissions=1"
        )
        expect(
            f"API_FAULT_INJECT site=fence_wait"
            f" result={error} real_driver_called=0"
            " prior_real_submission=1"
        )
    else:
        expect(
            f"API_FAULT_INJECT site=queue_submit"
            f" result={error} real_driver_called=0"
            " real_submissions=0"
        )

    expect(
        f"RESULT_GATE_TERMINAL site=transfer observed={error}"
        f" first_error={error} state=failed"
        " allowed=3 blocked=0"
    )
    expect(
        f"API_FAULT_RETURNED trial={trial} result={error}"
        " origin=test_linker_wrap real_driver_loss=0"
        " copies=2 output_unchanged=1024"
    )

    for attempt in (1, 2, 3):
        expect(
            f"RESULT_GATE_BLOCK count={attempt}"
            f" error={error} state=failed"
        )
        expect(
            f"API_FAULT_HOST_DENIED trial={trial}"
            f" attempt={attempt} error={error}"
            " copies=2 output_unchanged=1024"
        )

    for attempt in (1, 2):
        expect(
            f"API_FAULT_BLOCK site=queue_submit result={error}"
            f" real_driver_called=0 count={attempt}"
        )
        expect(
            f"API_FAULT_BOUNDARY_DENIED trial={trial}"
            f" site=queue_submit attempt={attempt}"
            f" result={error} real_driver_called=0"
        )

    expect(
        f"API_FAULT_BLOCK site=fence_wait result={error}"
        " real_driver_called=0 count=3"
    )
    expect(
        f"API_FAULT_BOUNDARY_DENIED trial={trial}"
        f" site=fence_wait attempt=1"
        f" result={error} real_driver_called=0"
    )
    expect(
        f"API_FAULT_COUNTS trial={trial}"
        f" submit_attempts=3 real_submissions={real_submissions}"
        f" wait_attempts={wait_attempts} real_waits=0"
        " injected=1 blocked=3 host_blocked=3 sticky=1"
    )
    expect(
        f"API_FAULT_DISARM site={source_site} result={error}"
        f" submit_attempts=3 real_submissions={real_submissions}"
        f" wait_attempts={wait_attempts} real_waits=0"
        " injected=1 blocked=3 real_device_loss=0"
    )

    expect(
        "TRANSFER_CLOSE index=0 bytes=4096 copies=2 copied_bytes=4124"
        f" flushes={3 if coherent_src == 0 else 0}"
        f" invalidates={2 if coherent_dst == 0 else 0}"
        " wait_result=0"
    )
    expect(
        f"RESULT_GATE_CLOSE previous=failed first_error={error}"
        " allowed=3 blocked=3 nonfatal=2 successes=4 state=closed"
    )
    expect(
        f"API_FAULT_OLD_DEVICE_CLOSED trial={trial}"
        f" instance=1 owner=0 sticky_error={error}"
    )

    fresh_src, fresh_dst = verify_buffer_open(
        take(), family, mode, atom
    )
    expect(
        "RESULT_GATE_OPEN state=active first_error=0"
        " allowed=0 blocked=0"
    )
    expect("RESULT_GATE_ALLOW count=1 state=active")
    expect(
        "TRANSFER_COPY index=0 bytes=256 copies=1"
        f" flushes={0 if fresh_src else 1}"
        f" invalidates={0 if fresh_dst else 1}"
        " fence=signaled shaders=0"
    )
    expect(
        "RESULT_GATE_OBSERVE site=transfer observed=0"
        " state=active successes=1"
    )
    expect(
        "RESULT_GATE_OBSERVE site=fence_status observed=0"
        " state=active successes=2"
    )
    expect(
        f"API_FAULT_GOOD_COPY trial={trial}"
        " phase=new round=0 words=64 bytes=256"
        " fence=0 valid=1 shaders=0"
    )
    expect(
        f"API_FAULT_NEW_DEVICE_PASS trial={trial}"
        " fresh_device=1 copies=1 words=64 bytes=256"
        " instance=1 valid=1"
    )
    expect(
        "TRANSFER_CLOSE index=0 bytes=4096 copies=1 copied_bytes=256"
        f" flushes={0 if fresh_src else 1}"
        f" invalidates={0 if fresh_dst else 1}"
        " wait_result=0"
    )
    expect(
        "RESULT_GATE_CLOSE previous=active first_error=0"
        " allowed=1 blocked=0 nonfatal=0 successes=2 state=closed"
    )
    expect(
        f"API_FAULT_NEW_DEVICE_CLOSED trial={trial}"
        " instance=1 owner=0"
    )
    expect(
        f"DEVICE_CATALOG_CLOSE devices={device_count} selections=1"
    )
    expect(
        f"API_FAULT_TRIAL_PASS trial={trial}"
        " injected=1 actual_driver_loss=0"
        " completed_gpu_copies=3 verified_words=1095"
        " verified_bytes=4380 blocked_host=3"
        " blocked_driver_calls=3 devices=2 instance=1 shaders=0"
    )

    assert cursor == len(lines)


def verify_native(
    events: list[str],
    api: str,
    memory_mode: str,
    fault_site: str,
    error: int,
) -> None:
    """Verify both complete GPU lifecycles and the exact native completion."""
    length = 59 if fault_site == "wait" else 58
    assert len(events) == length * 2 + 1, len(events)

    verify_trial(
        events[:length], 0, api,
        memory_mode, fault_site, error,
    )
    verify_trial(
        events[length:length * 2], 1, api,
        memory_mode, fault_site, error,
    )

    assert events[-1] == (
        f"API_FAULT_DIAGNOSTIC_PASS api={api}"
        f" mode={0 if memory_mode == 'prefer' else 1}"
        f" fault_site={fault_site} result={error}"
        " trials=2 injected=2 actual_driver_loss=0"
        " live_devices=4 completed_gpu_copies=6"
        " verified_words=2190 verified_bytes=8760"
        " blocked_host=6 blocked_driver_calls=6 shaders=0"
    )

    for banned in (
        "BACKEND_VULKAN_", "JOB_PORT_SUBMIT ",
        "KERN_RUN ", "ACTUAL_DEVICE_LOST ",
    ):
        assert not any(x.startswith(banned) for x in events), banned


def verify_cli(lines: list[str], abi: str) -> None:
    """Require all seven malformed native options to exit with code two."""
    assert lines == [
        "TEST=vulkan-core10-api-fault-boundary-invalid-inputs",
        f"ABI={abi}",
        "INVALID_CASES=7",
        "INVALID_CASE=missing-options exit=2",
        "INVALID_CASE=unsupported-api exit=2",
        "INVALID_CASE=invalid-memory exit=2",
        "INVALID_CASE=invalid-site exit=2",
        "INVALID_CASE=invalid-result exit=2",
        "INVALID_CASE=wrong-flag exit=2",
        "INVALID_CASE=extra-argument exit=2",
    ]


def corrupt(
    lines: list[str],
    prefix: str,
    original: str,
    replacement: str,
) -> list[str]:
    """Falsify one exact real or test-intercepted Vulkan event in a full log."""
    changed = lines.copy()
    positions = [
        i for i, value in enumerate(lines)
        if value.startswith(prefix) and original in value
    ]
    assert positions, (prefix, original)
    index = positions[0]
    changed[index] = changed[index].replace(original, replacement, 1)
    assert changed != lines

    return changed


def verify_mutations(manifest: dict) -> None:
    """Reject fabricated driver loss, copied data and post-terminal submits."""
    sample = next(
        row for row in manifest["runs"]
        if row["device"] == "pixel-arm64"
        and row["api"] == "1.0"
        and row["memory_mode"] == "noncoherent"
        and row["fault_site"] == "wait"
        and row["test_error_name"] == "device-lost"
    )
    original = (ARCHIVE / sample["file"]).read_text(
        encoding="utf-8"
    ).splitlines()[7:]

    changes = (
        ("API_FAULT_REAL_DRIVER ", "observed=1", "observed=0"),
        ("API_FAULT_REAL_DRIVER ", "observed=2", "observed=0"),
        ("RESULT_GATE_OBSERVE site=fence_status observed=1", "nonfatal=1", "nonfatal=0"),
        ("RESULT_GATE_OBSERVE site=fence_wait observed=2", "nonfatal=2", "nonfatal=1"),
        ("TRANSFER_OPEN ", "coherent_src=0", "coherent_src=1"),
        ("TRANSFER_OPEN ", "coherent_dst=0", "coherent_dst=1"),
        ("TRANSFER_OPEN ", "atom=64", "atom=128"),
        ("TRANSFER_COPY ", "bytes=28", "bytes=32"),
        ("TRANSFER_COPY ", "flushes=1", "flushes=0"),
        ("TRANSFER_COPY ", "invalidates=1", "invalidates=0"),
        ("TRANSFER_COPY ", "fence=signaled", "fence=pending"),
        ("API_FAULT_GOOD_COPY ", "valid=1", "valid=0"),
        ("API_FAULT_GOOD_COPY ", "words=7", "words=8"),
        ("API_FAULT_ARM ", "origin=test_linker_wrap", "origin=real_driver"),
        ("API_FAULT_ARM ", "real_device_loss=0", "real_device_loss=1"),
        ("API_FAULT_REAL_SUBMIT ", "result=0", "result=-4"),
        ("API_FAULT_INJECT ", "result=-4", "result=0"),
        ("API_FAULT_INJECT ", "real_driver_called=0", "real_driver_called=1"),
        ("API_FAULT_INJECT ", "prior_real_submission=1", "prior_real_submission=0"),
        ("RESULT_GATE_TERMINAL ", "first_error=-4", "first_error=0"),
        ("API_FAULT_RETURNED ", "real_driver_loss=0", "real_driver_loss=1"),
        ("API_FAULT_RETURNED ", "output_unchanged=1024", "output_unchanged=1023"),
        ("RESULT_GATE_BLOCK ", "error=-4", "error=0"),
        ("API_FAULT_HOST_DENIED ", "copies=2", "copies=3"),
        ("API_FAULT_BLOCK ", "real_driver_called=0", "real_driver_called=1"),
        ("API_FAULT_BOUNDARY_DENIED ", "real_driver_called=0", "real_driver_called=1"),
        ("API_FAULT_COUNTS ", "real_submissions=1", "real_submissions=0"),
        ("API_FAULT_COUNTS ", "wait_attempts=2", "wait_attempts=1"),
        ("API_FAULT_COUNTS ", "injected=1", "injected=0"),
        ("API_FAULT_COUNTS ", "blocked=3", "blocked=2"),
        ("API_FAULT_DISARM ", "real_device_loss=0", "real_device_loss=1"),
        ("API_FAULT_OLD_DEVICE_CLOSED ", "sticky_error=-4", "sticky_error=0"),
        ("TRANSFER_CLOSE ", "flushes=3", "flushes=2"),
        ("TRANSFER_CLOSE ", "invalidates=2", "invalidates=1"),
        ("TRANSFER_CLOSE ", "wait_result=0", "wait_result=-4"),
        ("API_FAULT_NEW_DEVICE_PASS ", "valid=1", "valid=0"),
        ("API_FAULT_NEW_DEVICE_PASS ", "instance=1", "instance=0"),
        ("RESULT_GATE_CLOSE previous=failed", "blocked=3", "blocked=2"),
        ("DEVICE_CATALOG_CLOSE ", "selections=1", "selections=0"),
        ("API_FAULT_TRIAL_PASS ", "actual_driver_loss=0", "actual_driver_loss=1"),
        ("API_FAULT_DIAGNOSTIC_PASS ", "actual_driver_loss=0", "actual_driver_loss=1"),
        ("API_FAULT_DIAGNOSTIC_PASS ", "completed_gpu_copies=6", "completed_gpu_copies=7"),
    )

    rejected = 0

    for prefix, old, new in changes:
        changed = corrupt(original, prefix, old, new)

        try:
            verify_native(
                changed, "1.0", "noncoherent", "wait", -4
            )
        except AssertionError:
            rejected += 1
        else:
            raise AssertionError(f"Accepted false Vulkan API record: {prefix}")

    mismatches = (
        ("API_FAULT_INJECT ", "API_FAULT_REAL_SUBMIT "),
        ("API_FAULT_GOOD_COPY ", "API_FAULT_RETURNED "),
        ("API_FAULT_NEW_DEVICE_PASS ", "API_FAULT_OLD_DEVICE_CLOSED "),
        ("TRANSFER_CLOSE ", "DEVICE_CATALOG_CLOSE "),
    )

    for first_prefix, second_prefix in mismatches:
        changed = original.copy()
        first = next(
            i for i, value in enumerate(changed)
            if value.startswith(first_prefix)
        )
        second = next(
            i for i, value in enumerate(changed)
            if value.startswith(second_prefix)
        )
        changed[first], changed[second] = changed[second], changed[first]

        try:
            verify_native(
                changed, "1.0", "noncoherent", "wait", -4
            )
        except AssertionError:
            rejected += 1
        else:
            raise AssertionError(
                f"Accepted invalid Vulkan ordering: {first_prefix}"
            )

    submit_row = next(
        row for row in manifest["runs"]
        if row["device"] == "redmagic-arm64"
        and row["api"] == "1.0"
        and row["fault_site"] == "submit"
        and row["test_error_name"] == "device-memory"
    )
    submit = (ARCHIVE / submit_row["file"]).read_text(
        encoding="utf-8"
    ).splitlines()[7:]
    changed = corrupt(
        submit, "API_FAULT_INJECT ", "real_submissions=0",
        "real_submissions=1",
    )

    try:
        verify_native(changed, "1.0", "prefer", "submit", -2)
    except AssertionError:
        rejected += 1
    else:
        raise AssertionError("Accepted real submission during SUBMIT injection")

    changed = corrupt(
        submit, "API_FAULT_COUNTS ", "wait_attempts=1",
        "wait_attempts=2",
    )

    try:
        verify_native(changed, "1.0", "prefer", "submit", -2)
    except AssertionError:
        rejected += 1
    else:
        raise AssertionError("Accepted a genuine wait after SUBMIT injection")

    negative_row = next(
        row for row in manifest["invalid_cli"]
        if row["device"] == "pixel-arm64"
    )
    negative = (ARCHIVE / negative_row["file"]).read_text(
        encoding="utf-8"
    ).splitlines()
    changed = corrupt(
        negative, "INVALID_CASE=invalid-site", "exit=2", "exit=0",
    )

    try:
        verify_cli(changed, negative_row["abi"])
    except AssertionError:
        rejected += 1
    else:
        raise AssertionError("Accepted malformed Vulkan fault-site option")

    assert rejected == len(changes) + len(mismatches) + 3

    print(
        f"PASS {rejected} deliberately falsified Vulkan API calls,"
        " driver-origin claims, output and cleanup records rejected"
    )


def inspect_elf(
    readelf: Path,
    executable: Path,
    abi: str,
) -> None:
    """Require native Vulkan 1.0 symbols and exact test-only linker interposition."""
    def read(*options: str) -> str:
        """Inspect the existing Android native ELF with read-only llvm-readelf."""
        return subprocess.run(
            [str(readelf), "-W", *options, str(executable)],
            check=True, capture_output=True, text=True,
            encoding="utf-8",
        ).stdout

    header = read("--file-header")
    notes = read("--notes")
    dynamic = read("--dynamic")
    symbols = read("--dyn-syms")
    all_symbols = read("--symbols")

    expected_class = "ELF64" if abi == "arm64" else "ELF32"
    expected_machine = "AArch64" if abi == "arm64" else "ARM"

    assert re.search(rf"Class:\s+{expected_class}\b", header)
    assert re.search(rf"Machine:\s+{expected_machine}\b", header)
    assert "NT_ANDROID_TYPE_IDENT" in notes
    assert "description data: 18 00 00 00" in notes
    assert "Shared library: [libvulkan.so]" in dynamic

    for function in (
        "vkGetInstanceProcAddr", "vkCreateDevice",
        "vkCreateBuffer", "vkAllocateMemory", "vkMapMemory",
        "vkCmdCopyBuffer", "vkQueueSubmit", "vkWaitForFences",
        "vkGetFenceStatus", "vkDeviceWaitIdle",
        "vkFlushMappedMemoryRanges", "vkInvalidateMappedMemoryRanges",
        "vkDestroyDevice",
    ):
        assert re.search(rf"UND\s+{function}\b", symbols), function

    for wrapper in (
        "__wrap_vkQueueSubmit", "__wrap_vkWaitForFences",
    ):
        assert re.search(
            rf"GLOBAL\s+DEFAULT\s+\d+\s+{wrapper}\b",
            all_symbols,
        ), wrapper

    assert not re.search(r"UND\s+vkEnumerateInstanceVersion\b", symbols)


def main() -> None:
    """Verify all real Android Vulkan API fault-boundary observations."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--artifacts", type=Path,
        help="Directory containing exact Android ARM64 and ARMv7 native ELFs",
    )
    parser.add_argument(
        "--readelf", type=Path,
        help="Android NDK llvm-readelf for static ABI and test-linker inspection",
    )
    args = parser.parse_args()

    if args.readelf is not None and args.artifacts is None:
        parser.error("--readelf requires --artifacts")

    manifest = json.loads(
        (ARCHIVE / "manifest.json").read_text(encoding="utf-8")
    )

    assert manifest["minimum_android_api"] == 24
    assert manifest["requested_vulkan_apis"] == ["1.0", "1.1"]
    assert manifest["test_intercept_sites"] == list(SITES)
    assert manifest["test_errors"] == ERRORS
    assert manifest["linker_interception"] == [
        "--wrap=vkQueueSubmit", "--wrap=vkWaitForFences",
    ]
    assert manifest["failure_is_returned_by_real_driver"] is False
    assert manifest["genuine_gpu_device_loss_tested"] is False
    assert manifest["actual_gpu_reset_or_hang_attempted"] is False
    assert manifest["hashcat_production_backend_connected"] is False
    assert manifest["gpu_shader_dispatches"] == 0
    assert manifest["cryptographic_or_password_inputs"] is False

    assert len(manifest["source_sha256"]) == 17

    for source, digest in manifest["source_sha256"].items():
        assert source_sha256(ROOT / source) == digest, source

    assert len(manifest["runs"]) == 32
    assert len(manifest["invalid_cli"]) == 3
    assert len(list(ARCHIVE.glob("*.log"))) == 35

    expected_cases = {
        (device, api, mode, site, error_name)
        for device, (_, memory_modes) in DEVICES.items()
        for api in ("1.0", "1.1")
        for mode in memory_modes
        for site in SITES
        for error_name in ERRORS
    }
    seen = set()

    totals = {key: 0 for key in manifest["totals"]}

    for row in manifest["runs"]:
        case = (
            row["device"], row["api"], row["memory_mode"],
            row["fault_site"], row["test_error_name"],
        )
        assert case in expected_cases and case not in seen, case
        seen.add(case)

        abi, modes = DEVICES[row["device"]]
        assert row["abi"] == abi
        assert row["memory_mode"] in modes
        assert row["test_error_result"] == ERRORS[row["test_error_name"]]
        assert row["actual_gpu_device_loss"] is False
        assert row["native_exit_code"] == 0
        assert row["test_only_interceptions"] == 2
        assert row["real_device_sessions"] == 4
        assert row["reused_parent_instances"] == 2
        assert row["completed_gpu_copies"] == 6
        assert row["verified_words"] == 2190
        assert row["verified_bytes"] == 8760
        assert row["host_policy_denials"] == 6
        assert row["api_boundary_denials"] == 6
        assert row["real_submissions_during_injected_wait"] == (
            2 if row["fault_site"] == "wait" else 0
        )
        assert row["noncoherent_completed_copies"] == (
            6 if row["memory_mode"] == "noncoherent" else 0
        )

        path = ARCHIVE / row["file"]
        data = path.read_bytes()
        assert data and data.endswith(b"\n") and b"\r" not in data
        assert b"91809037" not in data and b"46281FDAS002AL" not in data
        assert sha256(path) == row["sha256"]

        lines = path.read_text(encoding="utf-8").splitlines()

        assert lines[:7] == [
            "TEST=vulkan-core10-api-fault-boundary",
            f"ABI={row['abi']}",
            f"API={row['api']}",
            f"MEMORY_MODE={row['memory_mode']}",
            f"FAULT_SITE={row['fault_site']}",
            f"TEST_RESULT={row['test_error_name']}",
            "ADB_EXIT_CODE=0",
        ]

        verify_native(
            lines[7:], row["api"], row["memory_mode"],
            row["fault_site"], row["test_error_result"],
        )

        for key in totals:
            totals[key] += row[key]

    assert seen == expected_cases
    assert totals == manifest["totals"]
    assert totals == {
        "test_only_interceptions": 64,
        "real_device_sessions": 128,
        "reused_parent_instances": 64,
        "completed_gpu_copies": 192,
        "verified_words": 70080,
        "verified_bytes": 280320,
        "host_policy_denials": 192,
        "api_boundary_denials": 192,
        "real_submissions_during_injected_wait": 32,
        "noncoherent_completed_copies": 48,
    }

    seen_controls = set()

    for row in manifest["invalid_cli"]:
        assert row["device"] in DEVICES
        assert row["device"] not in seen_controls
        seen_controls.add(row["device"])
        assert row["abi"] == DEVICES[row["device"]][0]
        assert row["invalid_requests"] == 7

        path = ARCHIVE / row["file"]
        assert sha256(path) == row["sha256"]

        verify_cli(
            path.read_text(encoding="utf-8").splitlines(), row["abi"]
        )

    assert seen_controls == set(DEVICES)

    if args.artifacts is not None:
        directory = args.artifacts.resolve(strict=True)
        assert set(manifest["native_elf_sha256"]) == {"arm64", "armv7"}

        for abi, digest in manifest["native_elf_sha256"].items():
            executable = directory / f"vulkan-core10-api-fault-boundary-{abi}"

            assert sha256(executable) == digest, abi

            if args.readelf is not None:
                inspect_elf(
                    args.readelf.resolve(strict=True), executable, abi
                )

        print("PASS exact Android ARM64 and ARMv7 API fault ELF SHA-256")

        if args.readelf is not None:
            print("PASS real Vulkan 1.0 symbols, Android API24 and linker --wrap")

    verify_mutations(manifest)

    print("PASS 32 real Android GPU API/memory/linked-fault test invocations")
    print("PASS 64 synthetic API faults with actual driver-side call bypass")
    print("PASS 128 real VkDevice ownership lifecycles under 64 VkInstances")
    print("PASS 192 completed GPU copies and 70080 verified public integers")
    print("PASS 280320 bytes and 48 noncoherent synchronized GPU copies")
    print("PASS 192 denied host + 192 denied Vulkan call-boundary submissions")
    print("PASS 32 real queue submissions before injected fence-wait returns")
    print("PASS 21 malformed native Android option requests rejected")


if __name__ == "__main__":
    main()
