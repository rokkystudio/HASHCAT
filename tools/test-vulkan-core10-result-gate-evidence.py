#!/usr/bin/env python3
"""Verify real Android Vulkan result-gate diagnostics and honest provenance.

The archived driver observations must prove real Vulkan NOT_READY,
TIMEOUT, signaled GPU buffer copies and exact memory synchronization
before any application-only negative test result is introduced.
A strict host policy must then reject every attempted resubmission
and retain its first terminal error. The offline verifier also checks
exact ELF/source SHA-256 identities and rejects falsified GPU, policy
and cleanup records. No Vulkan device loss or HASHCAT workload runs.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = ROOT / "tests/fixtures/android-gpu/vulkan-core10-result-gate-20261101"

DEVICES = {
    "pixel-arm64": ("arm64-v8a", ("prefer", "noncoherent")),
}
TERMINALS = {
    "device-lost": (-4, -4),
    "device-memory": (-2, -2),
    "unexpected-positive": (5, -3),
}
PURE_CASES = (
    ("timeout_from_status", "fence_status", 2, -3),
    ("not_ready_from_wait", "fence_wait", 1, -3),
    ("incomplete_transfer", "transfer", 5, -3),
    ("host_memory_error", "transfer", -1, -1),
    ("invalid_site", "invalid", 0, -3),
)
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
    """Hash the complete bytes of one native binary or saved GPU transcript."""
    assert path.is_file(), path

    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_sha256(path: Path) -> str:
    """Hash checked-out source after normalizing Windows CRLF to LF."""
    return hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")
    ).hexdigest()


def verify_trial(
    lines: list[str],
    trial: int,
    api: str,
    memory_mode: str,
    terminal: str,
) -> None:
    """Verify one real VkDevice then one strictly simulated fatal status.

    Consumes every actual native event in its required order: instance
    and device creation, genuine NOT_READY and TIMEOUT, two completed
    VkCmdCopyBuffer submissions, deliberately synthetic terminal
    classification, three refused host submission requests, and
    VkDevice cleanup before VkInstance. No post-terminal queue copy,
    output mutation or policy reactivation can be accepted.
    """
    assert trial in (0, 1)
    assert api in ("1.0", "1.1")
    assert memory_mode in ("prefer", "noncoherent")
    assert terminal in TERMINALS
    assert len(lines) == 36, (len(lines), trial, terminal)

    code, expected_error = TERMINALS[terminal]
    cursor = 0

    def take() -> str:
        """Read one actual Vulkan/policy event in source order."""
        nonlocal cursor
        assert cursor < len(lines), (trial, cursor)
        value = lines[cursor]
        cursor += 1

        return value

    def expect(expected: str) -> None:
        """Require the exact next recorded event, including result codes."""
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

    instance = INSTANCE_OPEN.fullmatch(take())
    assert instance is not None
    assert instance.group(1) == instance.group(2) == api
    device_count = int(instance.group(3))
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
    assert candidate is not None and candidate.group(1)
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
    assert int(allocated.group(2)) == (
        0 if memory_mode == "prefer" else 1
    )
    assert 0 <= int(allocated.group(3)) < 32
    assert 0 <= int(allocated.group(4)) < 32
    coherent_src = int(allocated.group(5))
    coherent_dst = int(allocated.group(6))
    assert int(allocated.group(7)) == atom

    if memory_mode == "noncoherent":
        assert coherent_src == coherent_dst == 0

    expect(
        "RESULT_GATE_OPEN state=active first_error=0 allowed=0 blocked=0"
    )
    expect(
        f"RESULT_GATE_REAL_DRIVER trial={trial}"
        " site=fence_status observed=1"
    )
    expect(
        "RESULT_GATE_OBSERVE site=fence_status observed=1"
        " state=active nonfatal=1"
    )
    expect(
        f"RESULT_GATE_REAL_DRIVER trial={trial}"
        " site=fence_wait observed=2"
    )
    expect(
        "RESULT_GATE_OBSERVE site=fence_wait observed=2"
        " state=active nonfatal=2"
    )

    for round, words in enumerate((7, 1024)):
        size = words * 4
        flushes = round + 1 if coherent_src == 0 else 0
        invalidates = round + 1 if coherent_dst == 0 else 0

        expect(
            f"RESULT_GATE_ALLOW count={round + 1} state=active"
        )
        expect(
            f"TRANSFER_COPY index=0 bytes={size}"
            f" copies={round + 1}"
            f" flushes={flushes} invalidates={invalidates}"
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
            f"RESULT_GATE_GPU_VERIFIED trial={trial}"
            f" round={round} words={words} bytes={size}"
            " fence=0 match=1 shaders=0"
        )

    expect(
        f"RESULT_GATE_TEST_INJECT trial={trial} mode={terminal}"
        f" origin=application_only observed={code} driver_error=0"
    )
    expect(
        f"RESULT_GATE_TERMINAL site=transfer observed={code}"
        f" first_error={expected_error}"
        " state=failed allowed=2 blocked=0"
    )

    for attempt in (1, 2, 3):
        expect(
            f"RESULT_GATE_BLOCK count={attempt}"
            f" error={expected_error} state=failed"
        )
        expect(
            f"RESULT_GATE_REFUSED trial={trial}"
            f" attempt={attempt} error={expected_error}"
            " copies=2 bytes=4124 output_unchanged=1024"
        )

    expect(
        f"RESULT_GATE_STICKY trial={trial}"
        f" first_error={expected_error}"
        " later_success_ignored=1 blocked=3"
    )
    expect(
        "TRANSFER_CLOSE index=0 bytes=4096 copies=2"
        " copied_bytes=4124"
        f" flushes={2 if coherent_src == 0 else 0}"
        f" invalidates={2 if coherent_dst == 0 else 0}"
        " wait_result=0"
    )
    expect(
        f"RESULT_GATE_CLOSE previous=failed first_error={expected_error}"
        " allowed=2 blocked=3 nonfatal=2 successes=4 state=closed"
    )
    expect(
        f"DEVICE_CATALOG_CLOSE devices={device_count} selections=1"
    )
    expect(
        f"RESULT_GATE_TRIAL_PASS trial={trial} mode={terminal}"
        " injected=1 real_driver_loss=0 completed_copies=2"
        " verified_words=1031 verified_bytes=4124 allowed=2"
        " blocked=3 nonfatal=2 successes=4 shaders=0"
    )

    assert cursor == len(lines)


def verify_host_control(
    lines: list[str],
    name: str,
    site: str,
    observed: int,
    expected_error: int,
) -> None:
    """Verify a host-only unexpected VkResult cannot trigger GPU submission."""
    assert lines == [
        "RESULT_GATE_OPEN state=active first_error=0 allowed=0 blocked=0",
        f"RESULT_GATE_TERMINAL site={site} observed={observed}"
        f" first_error={expected_error}"
        " state=failed allowed=0 blocked=0",
        f"RESULT_GATE_BLOCK count=1 error={expected_error} state=failed",
        f"RESULT_GATE_CLOSE previous=failed first_error={expected_error}"
        " allowed=0 blocked=1 nonfatal=0 successes=0 state=closed",
        f"RESULT_GATE_POLICY_ONLY_PASS case={name}"
        f" origin=synthetic observed={observed}"
        f" first_error={expected_error} blocked=1 closed=1",
    ]


def verify_native(
    events: list[str],
    api: str,
    memory_mode: str,
    terminal: str,
) -> None:
    """Verify both real GPU trials, five policy controls and the summary."""
    assert len(events) == 98, len(events)

    verify_trial(events[:36], 0, api, memory_mode, terminal)
    verify_trial(events[36:72], 1, api, memory_mode, terminal)

    for i, (name, site, observed, error) in enumerate(PURE_CASES):
        verify_host_control(
            events[72 + i * 5:77 + i * 5],
            name, site, observed, error,
        )

    expect = (
        f"RESULT_GATE_DIAGNOSTIC_PASS api={api}"
        f" mode={0 if memory_mode == 'prefer' else 1}"
        f" terminal={terminal} trials=2 injected=2"
        " actual_driver_loss=0 real_copies=4"
        " verified_words=2062 verified_bytes=8248"
        " nonfatal=4 blocked=6 host_controls=5 shaders=0"
    )
    assert events[97] == expect

    assert not any(
        event.startswith((
            "BACKEND_VULKAN_", "JOB_PORT_SUBMIT ",
            "KERN_RUN ", "FENCE_DRIVER_LOST ",
        ))
        for event in events
    )


def verify_cli(lines: list[str], abi: str) -> None:
    """Require six malformed native Android commands to return code 2."""
    assert lines == [
        "TEST=vulkan-core10-result-gate-invalid-inputs",
        f"ABI={abi}",
        "INVALID_CASES=6",
        "INVALID_CASE=missing-options exit=2",
        "INVALID_CASE=unsupported-api exit=2",
        "INVALID_CASE=unknown-memory exit=2",
        "INVALID_CASE=unknown-terminal exit=2",
        "INVALID_CASE=wrong-flag exit=2",
        "INVALID_CASE=extra-argument exit=2",
    ]


def corrupt(
    lines: list[str],
    prefix: str,
    original: str,
    replacement: str,
) -> list[str]:
    """Falsify exactly one complete native Vulkan result-gate event."""
    changed = lines.copy()
    candidates = [
        i for i, line in enumerate(lines)
        if line.startswith(prefix) and original in line
    ]
    assert candidates, (prefix, original)
    index = candidates[0]
    changed[index] = changed[index].replace(original, replacement, 1)
    assert changed != lines

    return changed


def verify_mutations(manifest: dict) -> None:
    """Reject forged driver results, resubmissions, ownership and provenance."""
    record = next(
        item for item in manifest["runs"]
        if item["terminal_mode"] == "device-lost"
        and item["api"] == "1.0"
        and item["memory_mode"] == "noncoherent"
    )
    original = (ARCHIVE / record["file"]).read_text(
        encoding="utf-8"
    ).splitlines()[6:]

    attacks = (
        ("RESULT_GATE_REAL_DRIVER ", "observed=1", "observed=0"),
        ("RESULT_GATE_REAL_DRIVER ", "observed=2", "observed=0"),
        ("RESULT_GATE_OBSERVE site=fence_status observed=1", "nonfatal=1", "nonfatal=0"),
        ("RESULT_GATE_OBSERVE site=fence_wait observed=2", "nonfatal=2", "nonfatal=1"),
        ("RESULT_GATE_OPEN ", "state=active", "state=failed"),
        ("RESULT_GATE_ALLOW ", "state=active", "state=failed"),
        ("TRANSFER_OPEN ", "coherent_src=0", "coherent_src=1"),
        ("TRANSFER_OPEN ", "coherent_dst=0", "coherent_dst=1"),
        ("TRANSFER_OPEN ", "atom=64", "atom=128"),
        ("TRANSFER_COPY ", "bytes=28", "bytes=32"),
        ("TRANSFER_COPY ", "flushes=1", "flushes=0"),
        ("TRANSFER_COPY ", "invalidates=1", "invalidates=0"),
        ("TRANSFER_COPY ", "fence=signaled", "fence=pending"),
        ("RESULT_GATE_OBSERVE site=transfer observed=0", "successes=1", "successes=0"),
        ("RESULT_GATE_GPU_VERIFIED ", "match=1", "match=0"),
        ("RESULT_GATE_TEST_INJECT ", "origin=application_only", "origin=real_driver"),
        ("RESULT_GATE_TEST_INJECT ", "driver_error=0", "driver_error=1"),
        ("RESULT_GATE_TEST_INJECT ", "observed=-4", "observed=0"),
        ("RESULT_GATE_TERMINAL site=transfer observed=-4", "first_error=-4", "first_error=0"),
        ("RESULT_GATE_BLOCK ", "error=-4", "error=0"),
        ("RESULT_GATE_REFUSED ", "output_unchanged=1024", "output_unchanged=1023"),
        ("RESULT_GATE_REFUSED ", "copies=2", "copies=3"),
        ("RESULT_GATE_REFUSED ", "error=-4", "error=0"),
        ("RESULT_GATE_STICKY ", "later_success_ignored=1", "later_success_ignored=0"),
        ("RESULT_GATE_CLOSE ", "blocked=3", "blocked=2"),
        ("TRANSFER_CLOSE ", "flushes=2", "flushes=1"),
        ("TRANSFER_CLOSE ", "invalidates=2", "invalidates=1"),
        ("TRANSFER_CLOSE ", "wait_result=0", "wait_result=-4"),
        ("DEVICE_CATALOG_CLOSE ", "selections=1", "selections=0"),
        ("RESULT_GATE_POLICY_ONLY_PASS ", "closed=1", "closed=0"),
        ("RESULT_GATE_DIAGNOSTIC_PASS ", "actual_driver_loss=0", "actual_driver_loss=1"),
        ("RESULT_GATE_DIAGNOSTIC_PASS ", "real_copies=4", "real_copies=5"),
    )

    for prefix, before, after in attacks:
        forged = corrupt(original, prefix, before, after)

        try:
            verify_native(
                forged, "1.0", "noncoherent", "device-lost"
            )
        except AssertionError:
            print("PASS rejected false Vulkan gate evidence", prefix.strip())
        else:
            raise AssertionError(f"Accepted fraudulent Vulkan result: {prefix}")

    incorrect_order = original.copy()
    injection = next(
        i for i, line in enumerate(incorrect_order)
        if line.startswith("RESULT_GATE_TEST_INJECT ")
    )
    copied = next(
        i for i, line in enumerate(incorrect_order)
        if line.startswith("RESULT_GATE_GPU_VERIFIED ")
    )
    incorrect_order[injection], incorrect_order[copied] = (
        incorrect_order[copied], incorrect_order[injection]
    )

    try:
        verify_native(
            incorrect_order, "1.0", "noncoherent", "device-lost"
        )
    except AssertionError:
        print("PASS rejected GPU copy after terminal policy injection")
    else:
        raise AssertionError("Accepted GPU copy after simulated device loss")

    premature_instance_close = original.copy()
    device_close = next(
        i for i, line in enumerate(premature_instance_close)
        if line.startswith("TRANSFER_CLOSE ")
    )
    instance_close = next(
        i for i, line in enumerate(premature_instance_close)
        if line.startswith("DEVICE_CATALOG_CLOSE ")
    )
    premature_instance_close[device_close], premature_instance_close[instance_close] = (
        premature_instance_close[instance_close],
        premature_instance_close[device_close],
    )

    try:
        verify_native(
            premature_instance_close, "1.0", "noncoherent", "device-lost"
        )
    except AssertionError:
        print("PASS rejected VkInstance close before VkDevice")
    else:
        raise AssertionError("Accepted incorrect GPU object lifetime")

    invalid_row = manifest["invalid_cli"][0]
    invalid = (ARCHIVE / invalid_row["file"]).read_text(
        encoding="utf-8"
    ).splitlines()
    forged_cli = corrupt(
        invalid, "INVALID_CASE=unknown-terminal", "exit=2", "exit=0"
    )

    try:
        verify_cli(forged_cli, invalid_row["abi"])
    except AssertionError:
        print("PASS rejected invalid terminal argument falsely accepted")
    else:
        raise AssertionError("Accepted malformed terminal option")

    print("PASS 35 deliberately falsified GPU/result-gate records rejected")


def inspect_elf(
    readelf: Path,
    executable: Path,
    abi: str,
) -> None:
    """Verify Android API24 ELF identity and core Vulkan 1.0 dependencies."""
    def read(*options: str) -> str:
        """Return read-only Android llvm-readelf inspection output."""
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

    for name in (
        "vkGetInstanceProcAddr", "vkCreateDevice", "vkCreateFence",
        "vkGetFenceStatus", "vkWaitForFences", "vkCreateBuffer",
        "vkAllocateMemory", "vkCmdCopyBuffer", "vkQueueSubmit",
        "vkFlushMappedMemoryRanges", "vkInvalidateMappedMemoryRanges",
        "vkDestroyDevice",
    ):
        assert re.search(rf"UND\s+{name}\b", symbols), name

    assert not re.search(r"UND\s+vkEnumerateInstanceVersion\b", symbols)


def main() -> None:
    """Verify twelve physical GPU runs, host policy and native ELF hashes."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--artifacts", type=Path,
        help="Directory containing exact ARM64 and ARMv7 Android binaries",
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
    assert manifest["requested_vulkan_apis"] == ["1.0", "1.1"]
    assert manifest["terminal_modes"] == list(TERMINALS)
    assert manifest["actual_device_loss_returned_by_driver"] is False
    assert manifest["actual_driver_allocation_error_tested"] is False
    assert manifest["application_injected_terminal_status"] is True
    assert manifest["policy_enforced_inside_production_hashcat"] is False
    assert manifest["gpu_shaders"] == 0
    assert manifest["cryptographic_or_password_inputs"] is False
    assert manifest["android_abi_hardware_tested"] == ["arm64-v8a"]
    assert manifest["android_armv7_binary_built_but_not_run"] is True
    assert manifest["redmagic_not_connected_for_this_milestone"] is True

    assert len(manifest["source_sha256"]) == 15
    for source, digest in manifest["source_sha256"].items():
        assert source_sha256(ROOT / source) == digest, source

    assert len(manifest["runs"]) == 12
    assert len(manifest["invalid_cli"]) == 1
    assert len(list(ARCHIVE.glob("*.log"))) == 13

    expected = {
        (device, api, mode, terminal)
        for device, (_, modes) in DEVICES.items()
        for api in ("1.0", "1.1")
        for mode in modes
        for terminal in TERMINALS
    }

    seen = set()
    sessions = copies = words = byte_total = 0
    nonfatal = timeouts = terminal_events = refused = noncoherent = 0

    for row in manifest["runs"]:
        key = (
            row["device"], row["api"],
            row["memory_mode"], row["terminal_mode"],
        )

        assert key in expected and key not in seen, key
        seen.add(key)

        abi, modes = DEVICES[row["device"]]
        assert row["abi"] == abi
        assert row["api"] in ("1.0", "1.1")
        assert row["memory_mode"] in modes
        assert (row["test_only_observed"], row["retained_first_error"]) == (
            TERMINALS[row["terminal_mode"]]
        )
        assert row["native_exit_code"] == 0
        assert row["real_device_sessions"] == 2
        assert row["actual_gpu_copies"] == 4
        assert row["verified_words"] == 2062
        assert row["verified_bytes"] == 8248
        assert row["real_not_ready"] == 2
        assert row["real_timeout"] == 2
        assert row["host_only_terminal_events"] == 2
        assert row["blocked_resubmission_requests"] == 6
        assert row["host_only_controls"] == 5
        assert row["noncoherent_copies"] == (
            4 if row["memory_mode"] == "noncoherent" else 0
        )

        path = ARCHIVE / row["file"]
        data = path.read_bytes()
        assert data and data.endswith(b"\n") and b"\r" not in data
        assert b"46281FDAS002AL" not in data and b"91809037" not in data
        assert sha256(path) == row["sha256"]
        lines = path.read_text(encoding="utf-8").splitlines()

        assert lines[:6] == [
            "TEST=vulkan-core10-result-gate",
            f"ABI={row['abi']}",
            f"API={row['api']}",
            f"MEMORY_MODE={row['memory_mode']}",
            f"TERMINAL={row['terminal_mode']}",
            "ADB_EXIT_CODE=0",
        ]

        verify_native(
            lines[6:], row["api"],
            row["memory_mode"], row["terminal_mode"],
        )

        sessions += row["real_device_sessions"]
        copies += row["actual_gpu_copies"]
        words += row["verified_words"]
        byte_total += row["verified_bytes"]
        nonfatal += row["real_not_ready"]
        timeouts += row["real_timeout"]
        terminal_events += row["host_only_terminal_events"]
        refused += row["blocked_resubmission_requests"]
        noncoherent += row["noncoherent_copies"]

    assert seen == expected
    assert sessions == 24
    assert copies == 48
    assert words == 24744
    assert byte_total == 98976
    assert nonfatal == timeouts == terminal_events == 24
    assert refused == 72
    assert noncoherent == 24

    controls = manifest["invalid_cli"][0]
    assert controls["device"] == "pixel-arm64"
    assert controls["abi"] == "arm64-v8a"
    assert controls["rejected_requests"] == 6

    invalid_path = ARCHIVE / controls["file"]
    assert sha256(invalid_path) == controls["sha256"]
    verify_cli(
        invalid_path.read_text(encoding="utf-8").splitlines(),
        controls["abi"],
    )

    if args.artifacts is not None:
        directory = args.artifacts.resolve(strict=True)
        assert set(manifest["native_elf_sha256"]) == {"arm64", "armv7"}

        for abi, digest in manifest["native_elf_sha256"].items():
            executable = directory / f"vulkan-core10-result-gate-{abi}"
            assert sha256(executable) == digest

            if args.readelf is not None:
                inspect_elf(
                    args.readelf.resolve(strict=True), executable, abi
                )

        print("PASS exact native Android ARM64 and ARMv7 binaries")

        if args.readelf is not None:
            print("PASS actual Android API24 ELF and Vulkan core1.0 dependencies")

    verify_mutations(manifest)

    print("PASS 12 real Pixel Android GPU API/memory/host-terminal runs")
    print("PASS 24 actual VkDevice sessions with 48 public integer GPU copies")
    print("PASS 24744 verified words, 98976 bytes, 24 real NOT_READY and TIMEOUT each")
    print("PASS 24 explicitly synthetic terminal events, 72 refused submissions")
    print("PASS 24 genuinely noncoherent GPU flush/invalidate pairs")
    print("PASS 60 host-only negative site controls and six malformed CLI rejections")


if __name__ == "__main__":
    main()
