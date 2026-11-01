#!/usr/bin/env python3
"""Independently validate real Vulkan 1.0 public GPU buffer transfers.

Verifies LF-only Android GPU evidence, source and executable identities,
true VkBuffer/VkDevice/VkInstance lifecycle ordering, fence-completed
public integer copies, noncoherent flush/invalidate and genuine memory
capability refusals. Rejects deliberately corrupted evidence without
launching ADB, HASHCAT, Vulkan, shaders or cryptographic operations.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = ROOT / "tests/fixtures/android-gpu/vulkan-core10-transfer-20261101"
WORDS = (1, 7, 64, 257, 1024)

OPEN = re.compile(
    r"DEVICE_CATALOG_OPEN requested_api=(1\.[01]) instance_api=(1\.[01])"
    r" devices=(\d+) extensions=0 layers=0"
)
ACCEPT = re.compile(
    r"ACCEPT_COMPUTE index=0 queue_family=(\d+) local_max=(\d+)"
    r" invocations_max=(\d+) groups_max=(\d+) shared_max=(\d+)"
    r" storage_range_max=(\d+) descriptor_max=(\d+) non_coherent_atom=(\d+)"
)
TRANSFER_OPEN = re.compile(
    r"TRANSFER_OPEN index=0 queue_family=(\d+) bytes=4096 mode=(\d)"
    r" src_type=(\d+) dst_type=(\d+) coherent_src=(\d)"
    r" coherent_dst=(\d) atom=(\d+)"
    r" shaders=0 features=0 extensions=0"
)
UNSUPPORTED_MEMORY = re.compile(
    r"TRANSFER_MEMORY_UNAVAILABLE index=0 buffer=([01])"
    r" compatible=0x([0-9a-f]{8}) mode=1 result=-8"
)


def sha256(path: Path) -> str:
    """Hash the exact contents of an existing native binary or log."""
    assert path.is_file(), path

    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_sha256(path: Path) -> str:
    """Verify source identity after normalizing Windows CRLF to LF."""
    return hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")
    ).hexdigest()


def exact(lines: list[str], message: str) -> None:
    """Require a single native diagnostic event with the exact payload."""
    assert lines.count(message) == 1, message


def verify_common_trial(
    lines: list[str],
    trial: int,
    api: str,
    memory_mode: str,
) -> tuple[int, int, int]:
    """Validate exact API discovery and actual physical-device selection."""
    assert lines
    first = re.fullmatch(
        r"VULKAN_LOADER_VERSION=1\.(\d+)\.(\d+)"
        r" source=vkEnumerateInstanceVersion",
        lines[0],
    )
    assert first is not None, lines[0]
    loader_minor = int(first.group(1))
    assert loader_minor >= int(api.split(".")[1])

    assert lines[1] == (
        f"SELECT_INSTANCE_API={api} loader_minor={loader_minor} cap=1.1"
    )
    instance = OPEN.fullmatch(lines[2])
    assert instance is not None
    assert instance.group(1) == instance.group(2) == api
    count = int(instance.group(3))
    assert count > 0

    device_api = re.fullmatch(
        rf"SELECT_DEVICE_API={re.escape(api)}"
        r" reported_minor=(\d+) instance_minor=(\d+) cap=1\.1",
        lines[3],
    )
    assert device_api is not None
    assert int(device_api.group(1)) >= int(api.split(".")[1])
    assert int(device_api.group(2)) == int(api.split(".")[1])

    candidate = re.fullmatch(
        r"VULKAN_CANDIDATE index=0 name=(.+) vendor=0x([0-9a-f]+)"
        r" device=0x([0-9a-f]+) api=1\.(\d+)\.(\d+)"
        r" driver=0x([0-9a-f]+)",
        lines[4],
    )
    assert candidate is not None and candidate.group(1)
    assert int(candidate.group(4)) >= int(api.split(".")[1])

    selected = ACCEPT.fullmatch(lines[5])
    assert selected is not None
    family = int(selected.group(1))
    assert int(selected.group(2)) >= 1
    assert int(selected.group(3)) >= 1
    assert int(selected.group(4)) >= 1
    assert int(selected.group(6)) >= 4096
    assert int(selected.group(7)) >= 2

    atom = int(selected.group(8))
    assert atom > 0 and (atom & (atom - 1)) == 0

    assert lines[6] == (
        f"DEVICE_CATALOG_SELECT index=0 queue_family={family}"
        f" effective_api={api} selections=1"
    )
    assert lines[7] == (
        f"TRANSFER_INVALID_OPEN trial={trial} rejected=7 owned=0"
    )

    return count, family, atom


def verify_success_trial(
    lines: list[str],
    trial: int,
    api: str,
    memory_mode: str,
) -> None:
    """Verify five actual copies, coherence and ordered Vulkan destruction."""
    assert len(lines) == 23, len(lines)

    count, family, atom = verify_common_trial(
        lines, trial, api, memory_mode
    )
    opened = TRANSFER_OPEN.fullmatch(lines[8])

    assert opened is not None
    assert int(opened.group(1)) == family
    assert int(opened.group(2)) == (0 if memory_mode == "prefer" else 1)
    assert 0 <= int(opened.group(3)) < 32
    assert 0 <= int(opened.group(4)) < 32
    source_coherent = int(opened.group(5))
    destination_coherent = int(opened.group(6))
    assert int(opened.group(7)) == atom

    if memory_mode == "noncoherent":
        assert source_coherent == destination_coherent == 0

    assert lines[9] == (
        f"TRANSFER_INVALID_ACTIVE trial={trial} rejected=6 copies=0"
    )

    for round, words in enumerate(WORDS):
        size = words * 4
        flushes = (round + 1) if source_coherent == 0 else 0
        invalidates = (round + 1) if destination_coherent == 0 else 0

        assert lines[10 + 2 * round] == (
            f"TRANSFER_COPY index=0 bytes={size} copies={round + 1}"
            f" flushes={flushes} invalidates={invalidates}"
            " fence=signaled shaders=0"
        )
        assert lines[11 + 2 * round] == (
            f"TRANSFER_VERIFIED trial={trial} round={round}"
            f" words={words} bytes={size} valid=1"
        )

    assert lines[20] == (
        f"TRANSFER_TRIAL_PASS trial={trial}"
        f" mode={0 if memory_mode == 'prefer' else 1}"
        " copies=5 verified_words=1353 copied_bytes=5412"
        " invalid=13 shaders=0"
    )
    assert lines[21] == (
        "TRANSFER_CLOSE index=0 bytes=4096 copies=5"
        " copied_bytes=5412"
        f" flushes={5 if source_coherent == 0 else 0}"
        f" invalidates={5 if destination_coherent == 0 else 0}"
        " wait_result=0"
    )
    assert lines[22] == (
        f"DEVICE_CATALOG_CLOSE devices={count} selections=1"
    )
    assert len(lines[23:]) == 0, lines[23:]


def verify_unsupported_trial(
    lines: list[str],
    trial: int,
    api: str,
    memory_mode: str,
) -> None:
    """Require explicit incompatible noncoherent memory, never fake GPU success."""
    assert memory_mode == "noncoherent"
    assert len(lines) == 13, len(lines)

    count, _, _ = verify_common_trial(
        lines, trial, api, memory_mode
    )

    denial = UNSUPPORTED_MEMORY.fullmatch(lines[8])
    assert denial is not None
    assert int(denial.group(2), 16) > 0
    assert lines[9] == "TRANSFER_OPEN_FAILED index=0 mode=1 result=-8"
    assert lines[10] == (
        "TRANSFER_CLOSE index=0 bytes=0 copies=0"
        " copied_bytes=0 flushes=0 invalidates=0 wait_result=0"
    )
    assert lines[11] == (
        f"TRANSFER_UNSUPPORTED_TRIAL trial={trial}"
        " mode=noncoherent result=-8 owned=0"
    )
    assert lines[12] == (
        f"DEVICE_CATALOG_CLOSE devices={count} selections=1"
    )
    assert len(lines[13:]) == 0, lines[13:]


def verify_native_log(
    lines: list[str],
    api: str,
    memory_mode: str,
    outcome: str,
) -> None:
    """Verify complete native Vulkan buffer lifecycles and process result."""
    assert outcome in ("transferred", "unsupported")
    assert memory_mode in ("prefer", "noncoherent")
    assert api in ("1.0", "1.1")

    indexes = [
        i for i, line in enumerate(lines)
        if line.startswith("VULKAN_LOADER_VERSION=")
    ]
    assert len(indexes) == 2
    assert indexes[0] == 0
    summary = lines[-1]

    if outcome == "transferred":
        assert summary == (
            f"TRANSFER_DIAGNOSTIC_PASS api={api}"
            f" mode={0 if memory_mode == 'prefer' else 1}"
            " trials=2 copies=10 verified_words=2706"
            " copied_bytes=10824 invalid=26 shaders=0"
        )
        assert sum(
            line.startswith("TRANSFER_COPY ") for line in lines
        ) == 10
        assert sum(
            line.startswith("TRANSFER_VERIFIED ") for line in lines
        ) == 10
    else:
        assert memory_mode == "noncoherent"
        assert summary == (
            f"TRANSFER_DIAGNOSTIC_UNSUPPORTED api={api}"
            " mode=1 trials=2 refusals=2 copied_bytes=0 shaders=0"
        )
        assert not any(
            line.startswith(("TRANSFER_COPY ", "TRANSFER_VERIFIED ",
                             "TRANSFER_OPEN ", "TRANSFER_TRIAL_PASS "))
            for line in lines
        )

    first = lines[:indexes[1]]
    second = lines[indexes[1]:-1]

    if outcome == "transferred":
        verify_success_trial(first, 0, api, memory_mode)
        verify_success_trial(second, 1, api, memory_mode)
    else:
        verify_unsupported_trial(first, 0, api, memory_mode)
        verify_unsupported_trial(second, 1, api, memory_mode)

    assert not any(
        line.startswith(("BACKEND_VULKAN_", "JOB_PORT_SUBMIT ",
                         "KERN_RUN ", "MULTI_SUBMIT "))
        for line in lines
    )


def verify_cli(lines: list[str], abi: str) -> None:
    """Check all three native Android malformed-input exit codes."""
    assert lines == [
        "TEST=vulkan-core10-transfer-invalid-inputs",
        f"ABI={abi}",
        "INVALID_CASES=3",
        "INVALID_CASE=missing-minimum exit=2",
        "INVALID_CASE=unsupported-api exit=2",
        "INVALID_CASE=unknown-memory exit=2",
    ]


def corrupt(
    lines: list[str],
    prefix: str,
    old: str,
    new: str,
) -> list[str]:
    """Make exactly one explicit dishonest change to a full native log."""
    changed = lines.copy()
    indexes = [
        i for i, line in enumerate(lines)
        if line.startswith(prefix) and old in line
    ]
    assert indexes
    i = indexes[0]
    changed[i] = changed[i].replace(old, new, 1)
    assert changed != lines

    return changed


def verify_mutations(manifest: dict) -> None:
    """Reject false GPU copies, sync, device teardown and driver refusals."""
    success = next(
        row for row in manifest["runs"]
        if row["device"] == "pixel-arm64"
        and row["api"] == "1.0"
        and row["memory_mode"] == "noncoherent"
    )
    lines = (ARCHIVE / success["file"]).read_text(
        encoding="utf-8"
    ).splitlines()[5:]

    variants = (
        ("TRANSFER_OPEN ", "coherent_src=0", "coherent_src=1"),
        ("TRANSFER_OPEN ", "coherent_dst=0", "coherent_dst=1"),
        ("TRANSFER_OPEN ", "src_type=1", "src_type=99"),
        ("TRANSFER_OPEN ", "atom=64", "atom=128"),
        ("TRANSFER_COPY ", "fence=signaled", "fence=pending"),
        ("TRANSFER_COPY ", "flushes=1", "flushes=0"),
        ("TRANSFER_COPY ", "invalidates=1", "invalidates=0"),
        ("TRANSFER_VERIFIED ", "valid=1", "valid=0"),
        ("TRANSFER_VERIFIED ", "words=1", "words=2"),
        ("TRANSFER_TRIAL_PASS ", "verified_words=1353", "verified_words=1352"),
        ("TRANSFER_CLOSE ", "flushes=5", "flushes=4"),
        ("TRANSFER_CLOSE ", "invalidates=5", "invalidates=4"),
        ("TRANSFER_CLOSE ", "wait_result=0", "wait_result=-4"),
        ("DEVICE_CATALOG_CLOSE ", "selections=1", "selections=0"),
        ("TRANSFER_DIAGNOSTIC_PASS ", "shaders=0", "shaders=1"),
    )

    for prefix, old, new in variants:
        modified = corrupt(lines, prefix, old, new)
        try:
            verify_native_log(
                modified, success["api"], success["memory_mode"],
                success["outcome"],
            )
        except AssertionError:
            print("PASS rejected false GPU transfer evidence", prefix.strip())
        else:
            raise AssertionError(f"Accepted corrupted native GPU event: {prefix}")

    reversed_teardown = lines.copy()
    first_close = next(
        i for i, line in enumerate(reversed_teardown)
        if line.startswith("TRANSFER_CLOSE ")
    )
    first_catalog = next(
        i for i, line in enumerate(reversed_teardown)
        if line.startswith("DEVICE_CATALOG_CLOSE ")
    )
    reversed_teardown[first_close], reversed_teardown[first_catalog] = (
        reversed_teardown[first_catalog], reversed_teardown[first_close]
    )
    try:
        verify_native_log(
            reversed_teardown, success["api"], success["memory_mode"],
            success["outcome"],
        )
    except AssertionError:
        print("PASS rejected VkInstance close before VkDevice")
    else:
        raise AssertionError("Accepted Vulkan instance lifetime violation")

    unsupported = next(
        row for row in manifest["runs"]
        if row["device"] == "redmagic-arm64"
        and row["api"] == "1.0"
        and row["memory_mode"] == "noncoherent"
    )
    refusal = (ARCHIVE / unsupported["file"]).read_text(
        encoding="utf-8"
    ).splitlines()[5:]
    for prefix, old, new in (
        ("TRANSFER_MEMORY_UNAVAILABLE ", "result=-8", "result=0"),
        ("TRANSFER_OPEN_FAILED ", "result=-8", "result=0"),
        ("TRANSFER_DIAGNOSTIC_UNSUPPORTED ", "refusals=2", "refusals=1"),
    ):
        changed = corrupt(refusal, prefix, old, new)
        try:
            verify_native_log(
                changed, unsupported["api"], unsupported["memory_mode"],
                unsupported["outcome"],
            )
        except AssertionError:
            print("PASS rejected false noncoherent support", prefix.strip())
        else:
            raise AssertionError("Accepted false driver capability refusal")

    cli = next(
        row for row in manifest["invalid_cli"]
        if row["device"] == "pixel-arm64"
    )
    original = (ARCHIVE / cli["file"]).read_text(
        encoding="utf-8"
    ).splitlines()
    edited = corrupt(
        original, "INVALID_CASE=unknown-memory",
        "exit=2", "exit=0",
    )
    try:
        verify_cli(edited, cli["abi"])
    except AssertionError:
        print("PASS rejected malformed native CLI falsely reported as valid")
    else:
        raise AssertionError("Accepted false native CLI success")


def inspect_elf(readelf: Path, file: Path, abi: str) -> None:
    """Inspect ELF32/ELF64 API 24 imports without executing the Android binary."""
    def run(*args: str) -> str:
        """Return successful llvm-readelf static inspection output."""
        result = subprocess.run(
            [str(readelf), "-W", *args, str(file)],
            check=True, capture_output=True, text=True, encoding="utf-8",
        )

        return result.stdout

    header = run("--file-header")
    note = run("--notes")
    dynamic = run("--dynamic")
    symbols = run("--dyn-syms")
    cls = "ELF64" if abi == "arm64" else "ELF32"
    machine = "AArch64" if abi == "arm64" else "ARM"

    assert re.search(rf"Class:\s+{cls}\b", header)
    assert re.search(rf"Machine:\s+{machine}\b", header)
    assert "NT_ANDROID_TYPE_IDENT" in note
    assert "description data: 18 00 00 00" in note
    assert "Shared library: [libvulkan.so]" in dynamic

    for name in (
        "vkGetInstanceProcAddr", "vkCreateDevice", "vkCreateBuffer",
        "vkAllocateMemory", "vkCmdCopyBuffer", "vkQueueSubmit",
        "vkWaitForFences", "vkFlushMappedMemoryRanges",
        "vkInvalidateMappedMemoryRanges", "vkDestroyDevice",
    ):
        assert re.search(rf"UND\s+{name}\b", symbols), name

    assert not re.search(r"UND\s+vkEnumerateInstanceVersion\b", symbols)


def main() -> None:
    """Validate 15 real Android observations, binaries and corruptions."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--artifacts", type=Path,
        help="Directory of the exact built Android ARM64/ARMv7 binaries",
    )
    parser.add_argument(
        "--readelf", type=Path,
        help="Android NDK llvm-readelf for static native symbol checks",
    )
    args = parser.parse_args()

    if args.readelf is not None and args.artifacts is None:
        parser.error("--readelf requires --artifacts")

    manifest = json.loads(
        (ARCHIVE / "manifest.json").read_text(encoding="utf-8")
    )

    assert manifest["minimum_android_api"] == 24
    assert manifest["apis"] == ["1.0", "1.1"]
    assert manifest["memory_modes"] == ["prefer", "noncoherent"]
    assert manifest["driver_capabilities_from_real_memory_type_bits"] is True
    assert manifest["vulkan_hashcat_backend_connected"] is False
    assert manifest["shader_dispatches"] == 0
    assert manifest["password_or_cryptographic_inputs"] is False

    assert len(manifest["source_sha256"]) == 13
    assert len(manifest["runs"]) == 12
    assert len(manifest["invalid_cli"]) == 3

    for source, digest in manifest["source_sha256"].items():
        assert source_sha256(ROOT / source) == digest, source

    seen = set()
    sessions = 0
    copies = 0
    verified_words = 0
    transferred_bytes = 0
    memory_refusals = 0

    for row in manifest["runs"]:
        key = (row["device"], row["api"], row["memory_mode"])
        assert key not in seen
        seen.add(key)

        path = ARCHIVE / row["file"]
        data = path.read_bytes()
        assert data and data.endswith(b"\n") and b"\r" not in data
        assert sha256(path) == row["sha256"]
        assert b"91809037" not in data and b"46281FDAS002AL" not in data

        lines = path.read_text(encoding="utf-8").splitlines()
        expected_exit = 0 if row["outcome"] == "transferred" else 3
        assert row["native_exit_code"] == expected_exit
        assert lines[:5] == [
            "TEST=vulkan-core10-transfer",
            f"ABI={row['abi']}",
            f"API={row['api']}",
            f"MEMORY_MODE={row['memory_mode']}",
            f"ADB_EXIT_CODE={expected_exit}",
        ]

        verify_native_log(
            lines[5:], row["api"], row["memory_mode"], row["outcome"]
        )

        assert row["device_sessions"] == (
            2 if row["outcome"] == "transferred" else 0
        )
        assert row["successful_copies"] == (
            10 if row["outcome"] == "transferred" else 0
        )
        assert row["verified_words"] == (
            2706 if row["outcome"] == "transferred" else 0
        )
        assert row["transferred_bytes"] == (
            10824 if row["outcome"] == "transferred" else 0
        )
        assert row["memory_refusals"] == (
            0 if row["outcome"] == "transferred" else 2
        )

        sessions += row["device_sessions"]
        copies += row["successful_copies"]
        verified_words += row["verified_words"]
        transferred_bytes += row["transferred_bytes"]
        memory_refusals += row["memory_refusals"]

    assert len(seen) == 12
    assert sessions == 16
    assert copies == 80
    assert verified_words == 21648
    assert transferred_bytes == 86592
    assert memory_refusals == 8

    assert len({
        row["device"] for row in manifest["runs"]
    }) == 3
    assert len(list(ARCHIVE.glob("*.log"))) == 15

    for row in manifest["invalid_cli"]:
        path = ARCHIVE / row["file"]
        assert sha256(path) == row["sha256"]
        assert row["rejected_commands"] == 3
        verify_cli(
            path.read_text(encoding="utf-8").splitlines(), row["abi"]
        )

    if args.artifacts is not None:
        directory = args.artifacts.resolve(strict=True)

        for abi, digest in manifest["native_elf_sha256"].items():
            executable = directory / f"vulkan-core10-transfer-{abi}"
            assert sha256(executable) == digest

            if args.readelf is not None:
                inspect_elf(
                    args.readelf.resolve(strict=True), executable, abi
                )

        print("PASS two exact Android ARM64/ARMv7 Vulkan transfer binaries")

        if args.readelf is not None:
            print("PASS actual API 24 ELF Vulkan 1.0 and host-memory imports")

    verify_mutations(manifest)

    print("PASS 12 real Android GPU API/memory observations")
    print("PASS 16 Vulkan device/instance sessions and 80 completed queue copies")
    print("PASS 21648 verified public integer words (86592 bytes)")
    print("PASS 8 real noncoherent capability refusals")
    print("PASS 9 malformed Android CLI requests")
    print("PASS 20 deliberate corrupted GPU evidence rejections")


if __name__ == "__main__":
    main()
