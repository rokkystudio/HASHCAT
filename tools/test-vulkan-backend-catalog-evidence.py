#!/usr/bin/env python3
"""Independently verify Vulkan discovery inside hashcat's backend_ctx.

Audits exact Android native logs, source/binary SHA-256 identities,
loader/device API versions, real Compute limits, correctly owned
instance lifecycles, rejected invalid calls and absence of production
hashcat GPU task scheduling. This test never runs ADB or Vulkan.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re

from vulkan_archive_provenance import source_sha256_at_archive_commit


ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = ROOT / "tests/fixtures/android-gpu/vulkan-backend-catalog-20261031"

OPEN = re.compile(
    r"^DEVICE_CATALOG_OPEN requested_api=(1\.[01]) "
    r"instance_api=(1\.[01]) devices=(\d+) extensions=0 layers=0$"
)
COMPUTE = re.compile(
    r"^ACCEPT_COMPUTE index=(\d+) queue_family=(\d+) "
    r"local_max=(\d+) invocations_max=(\d+) groups_max=(\d+) "
    r"shared_max=(\d+) storage_range_max=(\d+) "
    r"descriptor_max=(\d+) non_coherent_atom=(\d+)$"
)
CANDIDATE = re.compile(
    r"^VULKAN_CANDIDATE index=(\d+) name=(.+) "
    r"vendor=0x([0-9a-f]+) device=0x([0-9a-f]+) "
    r"api=1\.(\d+)\.(\d+) driver=0x([0-9a-f]+)$"
)


def sha256(path: Path) -> str:
    """Calculate SHA-256 for all bytes of one file."""
    return hashlib.sha256(path.read_bytes()).hexdigest()


def exact_line(lines: list[str], value: str) -> None:
    """Require one exact occurrence of a native event."""
    assert lines.count(value) == 1, value


def verify_success(
    lines: list[str],
    mode: str,
    physical_devices: list[int],
) -> None:
    """Audit both real hashcat backend_ctx Vulkan discovery lifecycles.

    Checks exact source-defined event order, two independent VkInstance
    creations, caller-specified core API, nonzero driver device count,
    declared public Compute limits and explicit queue selection. No Vulkan
    device is scheduled for hashcat's password-processing workload.
    """
    assert mode in ("1.0", "1.1")
    assert len(physical_devices) == 2
    assert len(lines) == 33, len(lines)

    for trial in range(2):
        segment = lines[trial * 16:trial * 16 + 16]

        loader = re.fullmatch(
            r"VULKAN_LOADER_VERSION=1\.(\d+)\.(\d+) "
            r"source=(vkEnumerateInstanceVersion|core10_missing_entry_point)",
            segment[0],
        )
        assert loader is not None
        loader_minor = int(loader.group(1))
        assert loader_minor >= int(mode.split(".")[1])

        assert re.fullmatch(
            rf"SELECT_INSTANCE_API={re.escape(mode)} "
            rf"loader_minor={loader_minor} cap=1\.1",
            segment[1],
        )

        opened = OPEN.fullmatch(segment[2])
        assert opened is not None
        assert opened.group(1) == opened.group(2) == mode
        device_count = int(opened.group(3))
        assert device_count == physical_devices[trial]
        assert device_count > 0

        assert segment[3] == (
            f"BACKEND_VULKAN_CATALOG_READY devices={device_count} "
            f"requested_api={mode} scheduled=0"
        )
        assert segment[4] == (
            "BACKEND_VULKAN_INVALID_CONTROLS rejected=6 selections=0"
        )

        assert segment[5] == segment[9]

        api = re.fullmatch(
            rf"SELECT_DEVICE_API={re.escape(mode)} "
            r"reported_minor=(\d+) instance_minor=(\d+) cap=1\.1",
            segment[9],
        )
        assert api is not None
        assert int(api.group(1)) >= int(mode.split(".")[1])
        assert int(api.group(2)) == int(mode.split(".")[1])

        assert segment[6] == segment[10]

        rejected = re.fullmatch(
            r"REJECT_COMPUTE dimension=0 local=4294967295 "
            r"max_local=(\d+) groups=64 max_groups=(\d+)",
            segment[7],
        )
        assert rejected is not None
        assert segment[8] == (
            "BACKEND_VULKAN_LIMIT_REJECT result=-8 selections=0"
        )

        candidate = CANDIDATE.fullmatch(segment[10])
        assert candidate is not None
        assert int(candidate.group(1)) == 0
        assert candidate.group(2)
        assert int(candidate.group(5)) >= int(mode.split(".")[1])

        compute = COMPUTE.fullmatch(segment[11])
        assert compute is not None
        assert int(compute.group(1)) == 0
        queue_family = int(compute.group(2))
        assert queue_family >= 0
        assert int(compute.group(3)) == int(rejected.group(1))
        assert int(compute.group(5)) == int(rejected.group(2))
        assert int(compute.group(3)) >= 32
        assert int(compute.group(4)) >= 32
        assert int(compute.group(5)) >= 64
        assert int(compute.group(7)) >= 8192
        assert int(compute.group(8)) >= 4
        atom = int(compute.group(9))
        assert atom > 0 and atom & (atom - 1) == 0

        assert segment[12] == (
            f"DEVICE_CATALOG_SELECT index=0 queue_family={queue_family} "
            f"effective_api={mode} selections=1"
        )
        assert segment[13] == (
            f"BACKEND_VULKAN_SELECT_PASS trial={trial} "
            f"devices={device_count} requested_api={mode} "
            f"effective_api={mode} scheduled=0"
        )
        assert segment[14] == (
            f"DEVICE_CATALOG_CLOSE devices={device_count} selections=1"
        )
        assert segment[15] == (
            f"BACKEND_VULKAN_CLOSE_PASS trial={trial}"
        )

    exact_line(
        lines,
        f"BACKEND_VULKAN_DIAGNOSTIC_PASS api={mode} "
        "trials=2 selections=2 invalid_controls=12 scheduled=0",
    )

    for forbidden in (
        "MULTI_SUBMIT", "MULTIGRAPH_PASS", "KERN_RUN",
        "PASSWORD", "HASH_CRACKED",
    ):
        assert not any(forbidden in line for line in lines), forbidden


def verify_cli(lines: list[str], abi: str) -> None:
    """Require all three Android command-line rejection controls."""
    assert lines == [
        "TEST=vulkan-backend-catalog-invalid-inputs",
        f"ABI={abi}",
        "INVALID_CASES=3",
        "INVALID_CASE=missing-minimum exit=2",
        "INVALID_CASE=unsupported-api exit=2",
        "INVALID_CASE=extra-argument exit=2",
    ]


def verify_mutations(manifest: dict) -> None:
    """Demonstrate rejection of altered owner, API and scheduling evidence."""
    target = next(
        item for item in manifest["runs"]
        if item["device"] == "redmagic-arm64" and item["mode"] == "1.0"
    )
    original = (ARCHIVE / target["file"]).read_text(
        encoding="utf-8"
    ).splitlines()[4:]

    changes = (
        (
            "DEVICE_CATALOG_OPEN requested_api=1.0 instance_api=1.0 devices=1",
            "DEVICE_CATALOG_OPEN requested_api=1.0 instance_api=1.0 devices=0",
        ),
        (
            "BACKEND_VULKAN_CATALOG_READY devices=1 requested_api=1.0 scheduled=0",
            "BACKEND_VULKAN_CATALOG_READY devices=1 requested_api=1.0 scheduled=1",
        ),
        (
            "BACKEND_VULKAN_INVALID_CONTROLS rejected=6 selections=0",
            "BACKEND_VULKAN_INVALID_CONTROLS rejected=5 selections=0",
        ),
        (
            "BACKEND_VULKAN_LIMIT_REJECT result=-8 selections=0",
            "BACKEND_VULKAN_LIMIT_REJECT result=0 selections=0",
        ),
        (
            "REJECT_COMPUTE dimension=0 local=4294967295",
            "REJECT_COMPUTE dimension=0 local=32",
        ),
        (
            "DEVICE_CATALOG_SELECT index=0 queue_family=0 effective_api=1.0 selections=1",
            "DEVICE_CATALOG_SELECT index=0 queue_family=0 effective_api=1.0 selections=2",
        ),
        (
            "BACKEND_VULKAN_SELECT_PASS trial=1 devices=1",
            "BACKEND_VULKAN_SELECT_PASS trial=0 devices=1",
        ),
        (
            "BACKEND_VULKAN_CLOSE_PASS trial=1",
            "BACKEND_VULKAN_CLOSE_PASS trial=0",
        ),
        (
            "BACKEND_VULKAN_DIAGNOSTIC_PASS api=1.0 trials=2 selections=2 invalid_controls=12 scheduled=0",
            "BACKEND_VULKAN_DIAGNOSTIC_PASS api=1.0 trials=2 selections=2 invalid_controls=12 scheduled=1",
        ),
        (
            "groups_max=65535",
            "groups_max=1",
        ),
    )

    for source, substitute in changes:
        corrupted = [
            row.replace(source, substitute) for row in original
        ]
        assert corrupted != original

        try:
            verify_success(corrupted, "1.0", target["physical_devices"])
        except AssertionError:
            print("PASS rejected corrupted backend event", source.split()[0])
        else:
            raise AssertionError(f"Accepted altered Vulkan event: {source}")

    reordered = original.copy()
    reopened = reordered.index("BACKEND_VULKAN_CLOSE_PASS trial=0")
    reordered[reopened], reordered[reopened - 2] = (
        reordered[reopened - 2], reordered[reopened]
    )

    try:
        verify_success(reordered, "1.0", target["physical_devices"])
    except AssertionError:
        print("PASS rejected invalid Vulkan catalogue teardown order")
    else:
        raise AssertionError("Accepted premature backend catalogue close")

    invalid = next(
        item for item in manifest["invalid_cli_runs"]
        if item["device"] == "pixel-arm64"
    )
    original_cli = (ARCHIVE / invalid["file"]).read_text(
        encoding="utf-8"
    ).splitlines()
    altered_cli = [
        row.replace("missing-minimum exit=2", "missing-minimum exit=0")
        for row in original_cli
    ]

    assert altered_cli != original_cli

    try:
        verify_cli(altered_cli, invalid["abi"])
    except AssertionError:
        print("PASS rejected corrupted Android CLI control")
    else:
        raise AssertionError("Accepted modified Android CLI exit code")


def main() -> None:
    """Validate physical Android discovery and exact native source identities."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--artifacts",
        type=Path,
        help="Directory with exact Android native objects and executables",
    )
    args = parser.parse_args()

    manifest = json.loads(
        (ARCHIVE / "manifest.json").read_text(encoding="utf-8")
    )

    assert manifest["capture_source_commit"] == "7eb41fda61fbc937dcce64f17645f8db0bb9501a"
    assert "Current checked-out source" in manifest["source_sha256_scope"]
    assert manifest["minimum_android_api"] == 24
    assert manifest["default_vulkan_api"] == "1.0"
    assert manifest["build_flag"] == "ENABLE_VULKAN_DISCOVERY=1"
    assert manifest["hashcat_vulkan_password_scheduler_enabled"] is False
    assert manifest["backend_device_list_unchanged"] is True
    assert manifest["tested_device_abis"] == {
        "redmagic-arm64": "arm64-v8a",
        "redmagic-armv7": "armeabi-v7a",
        "pixel-arm64": "arm64-v8a",
    }
    assert manifest["tested_apis"] == ["1.0", "1.1"]

    for name, digest in manifest["source_sha256"].items():
        assert source_sha256_at_archive_commit(
            ROOT, ARCHIVE / "manifest.json", ROOT / name
        ) == digest, name

    assert len(manifest["source_sha256"]) == 16
    assert len(manifest["runs"]) == 6
    assert len(manifest["invalid_cli_runs"]) == 3

    for run in manifest["runs"]:
        path = ARCHIVE / run["file"]
        data = path.read_bytes()

        assert data and data.endswith(b"\n") and b"\r" not in data
        assert sha256(path) == run["sha256"]
        lines = path.read_text(encoding="utf-8").splitlines()
        assert lines[:4] == [
            "TEST=vulkan-backend-catalog",
            f"ABI={run['abi']}",
            f"API={run['mode']}",
            "ADB_EXIT_CODE=0",
        ]
        assert run["lifecycles"] == 2
        assert run["invalid_backend_requests"] == 12
        assert run["unsupported_compute_workloads"] == 2
        verify_success(lines[4:], run["mode"], run["physical_devices"])

    for control in manifest["invalid_cli_runs"]:
        path = ARCHIVE / control["file"]
        assert sha256(path) == control["sha256"]
        assert control["cases"] == 3
        verify_cli(path.read_text(encoding="utf-8").splitlines(), control["abi"])

    assert len(list(ARCHIVE.glob("*.log"))) == 9

    if args.artifacts is not None:
        directory = args.artifacts.resolve(strict=True)
        assert len(manifest["artifact_sha256"]) == 4

        for name, digest in manifest["artifact_sha256"].items():
            assert sha256(directory / name) == digest, name

        print("PASS 4 exact Android ARM64/ARMv7 Vulkan backend build products")

    verify_mutations(manifest)

    print("PASS 16 exact source identities for real hashcat backend integration")
    print("PASS 6 Android device/API runs, 12 real Vulkan instance lifecycles")
    print("PASS 72 host invalid requests without production hashcat scheduling")
    print("PASS 12 real GPU Compute-limit refusals returned VK_ERROR_FEATURE_NOT_PRESENT")
    print("PASS 9 rejected Android CLI requests and 12 corrupted evidence controls")


if __name__ == "__main__":
    main()
