#!/usr/bin/env python3
"""Independently verify full Android HASHCAT Vulkan-discovery observations.

Reads previously captured backend-information transcripts and SHA-256
identities. Optionally inspects already-built ELF files using llvm-readelf.
Never launches HASHCAT, sends Android commands or performs GPU workloads.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = ROOT / "tests/fixtures/android-gpu/vulkan-hashcat-frontend-20261101"
BUILD = ROOT / "build/diagnostic-multimode/vulkan-hashcat-frontend-20261101"


def sha256(path: Path) -> str:
    """Return the SHA-256 digest of a complete existing file."""
    assert path.is_file(), path

    return hashlib.sha256(path.read_bytes()).hexdigest()


def source_sha256(path: Path) -> str:
    """Return the LF-normalized SHA-256 digest of a UTF-8 source file."""
    return hashlib.sha256(
        path.read_bytes().replace(b"\r\n", b"\n")
    ).hexdigest()


def verify_native_info(
    lines: list[str],
    abi: str,
    vendor: str,
    gpu_name: str,
    driver_minor: int,
) -> None:
    """Validate a real hashcat -I Vulkan lifecycle and its normal OpenCL GPU.

    Requires exactly one Vulkan 1.0 instance opened and closed, a matching
    nonzero physical-device count, the unchanged OpenCL GPU listing and
    the actual success exit marker. No Vulkan hashing workload is inferred
    from device discovery.
    """
    assert len(lines) >= 21
    assert lines[0] == "hashcat (v7.1.2) starting in backend information mode"
    assert lines[-1] == "HC_NATIVE_EXIT=0"

    loader = [
        re.fullmatch(
            r"VULKAN_LOADER_VERSION=1\.(\d+)\.(\d+) "
            r"source=vkEnumerateInstanceVersion",
            line,
        )
        for line in lines
        if line.startswith("VULKAN_LOADER_VERSION=")
    ]
    assert len(loader) == 1 and loader[0] is not None
    assert int(loader[0].group(1)) == driver_minor

    exact = (
        f"SELECT_INSTANCE_API=1.0 loader_minor={driver_minor} cap=1.1",
        "DEVICE_CATALOG_OPEN requested_api=1.0 instance_api=1.0 "
        "devices=1 extensions=0 layers=0",
        "BACKEND_VULKAN_CATALOG_READY devices=1 requested_api=1.0 scheduled=0",
        "Vulkan 1.0 discovery found 1 physical device(s); "
        "Vulkan Compute scheduling is not enabled.",
        "OpenCL Info:",
        "OpenCL Platform ID #1",
        "  Backend Device ID #01",
        "    Type...........: GPU",
        f"  Vendor..: {vendor}",
        f"    Name...........: {gpu_name}",
        "DEVICE_CATALOG_CLOSE devices=1 selections=0",
    )

    for line in exact:
        assert lines.count(line) == 1, line

    assert sum(item.startswith("DEVICE_CATALOG_OPEN ") for item in lines) == 1
    assert sum(item.startswith("DEVICE_CATALOG_CLOSE ") for item in lines) == 1
    assert sum(item.startswith("BACKEND_VULKAN_CATALOG_READY ") for item in lines) == 1

    indexes = [lines.index(line) for line in exact[:7]]
    assert indexes == sorted(indexes), indexes

    assert lines.index("OpenCL Info:") < lines.index(
        "DEVICE_CATALOG_CLOSE devices=1 selections=0"
    )

    assert not any(
        item.startswith(("DEVICE_CATALOG_SELECT ", "MULTI_SUBMIT ",
                         "JOB_PORT_SUBMIT ", "MULTIGRAPH_PASS "))
        for item in lines
    ), abi


def verify_mutations(records: list[dict]) -> None:
    """Reject deliberate false success, Vulkan ownership and OpenCL reports."""
    record = next(
        item for item in records if item["device"] == "redmagic-arm64"
    )
    original = (ARCHIVE / record["file"]).read_text(
        encoding="utf-8"
    ).splitlines()[4:]

    alterations = (
        (
            "DEVICE_CATALOG_OPEN requested_api=1.0 instance_api=1.0 devices=1",
            "DEVICE_CATALOG_OPEN requested_api=1.0 instance_api=1.0 devices=0",
        ),
        (
            "BACKEND_VULKAN_CATALOG_READY devices=1 requested_api=1.0 scheduled=0",
            "BACKEND_VULKAN_CATALOG_READY devices=1 requested_api=1.0 scheduled=1",
        ),
        (
            "DEVICE_CATALOG_CLOSE devices=1 selections=0",
            "DEVICE_CATALOG_CLOSE devices=1 selections=1",
        ),
        ("HC_NATIVE_EXIT=0", "HC_NATIVE_EXIT=255"),
        ("SELECT_INSTANCE_API=1.0 loader_minor=3", "SELECT_INSTANCE_API=1.1 loader_minor=3"),
        ("  Vendor..: QUALCOMM", "  Vendor..: UNKNOWN"),
        ("    Type...........: GPU", "    Type...........: CPU"),
        ("Vulkan Compute scheduling is not enabled.", "Vulkan Compute scheduling is enabled."),
    )

    for source, substitute in alterations:
        altered = [
            line.replace(source, substitute) for line in original
        ]
        assert altered != original, source

        try:
            verify_native_info(
                altered, record["abi"], record["opencl_vendor"],
                record["gpu_name"], record["loader_minor"],
            )
        except AssertionError:
            print("PASS rejected altered native event", source.split()[0])
        else:
            raise AssertionError(f"Accepted incorrect hardware evidence: {source}")

    reordered = original.copy()
    begin = reordered.index("DEVICE_CATALOG_OPEN requested_api=1.0 "
                            "instance_api=1.0 devices=1 extensions=0 layers=0")
    close = reordered.index("DEVICE_CATALOG_CLOSE devices=1 selections=0")
    reordered[begin], reordered[close] = reordered[close], reordered[begin]

    try:
        verify_native_info(
            reordered, record["abi"], record["opencl_vendor"],
            record["gpu_name"], record["loader_minor"],
        )
    except AssertionError:
        print("PASS rejected Vulkan instance teardown before creation")
    else:
        raise AssertionError("Accepted out-of-order Vulkan instance ownership")


def inspect_elf(readelf: Path, exe: Path, module: Path, abi: str) -> None:
    """Check Android API 24, exact ABI and Vulkan 1.0 ELF dependencies.

    Runs only the static llvm-readelf inspector on existing ELF files;
    never executes the inspected HASHCAT binary or its plugin.
    """
    def inspect(path: Path, *arguments: str) -> str:
        """Return successful llvm-readelf inspection output as text."""
        result = subprocess.run(
            [str(readelf), "-W", *arguments, str(path)],
            check=True, capture_output=True, text=True, encoding="utf-8",
        )
        return result.stdout

    header = inspect(exe, "--file-header")
    notes = inspect(exe, "--notes")
    dynamic = inspect(exe, "--dynamic")
    symbols = inspect(exe, "--dyn-syms")
    plugin_dynamic = inspect(module, "--dynamic")

    elf_class = "ELF64" if abi == "arm64-v8a" else "ELF32"
    machine = "AArch64" if abi == "arm64-v8a" else "ARM"

    assert re.search(rf"Class:\s+{elf_class}\b", header)
    assert re.search(rf"Machine:\s+{machine}\b", header)
    assert "NT_ANDROID_TYPE_IDENT" in notes
    assert "description data: 18 00 00 00" in notes
    assert "Shared library: [libvulkan.so]" in dynamic
    assert "UND vkGetInstanceProcAddr" in symbols
    assert not re.search(r"UND\s+vkEnumerateInstanceVersion\b", symbols)
    assert "Shared library: [libvulkan.so]" not in plugin_dynamic


def main() -> None:
    """Verify archived native logs, source digests and optional real ELF data."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--artifacts", type=Path,
        help="Directory containing full Android ABI-specific HASHCAT builds",
    )
    parser.add_argument(
        "--readelf", type=Path,
        help="Path to llvm-readelf for static inspection of full ARM ELF binaries",
    )
    args = parser.parse_args()

    if args.readelf is not None and args.artifacts is None:
        parser.error("--readelf requires --artifacts")

    manifest = json.loads(
        (ARCHIVE / "manifest.json").read_text(encoding="utf-8")
    )

    assert manifest["capture_source_commit"] == "32382a5487c470f1c4e2dfe4b96a58b307830c49"
    assert "Current checked-out source" in manifest["source_sha256_scope"]
    assert manifest["target_android_api"] == 24
    assert manifest["required_vulkan_api"] == "1.0"
    assert manifest["hashcat_vulkan_kernel_scheduling"] is False
    assert manifest["full_hashcat_android_frontend_linked"] is True
    assert manifest["release_archives_modified"] is False
    assert len(manifest["runs"]) == 3
    assert len(manifest["source_sha256"]) >= 9

    for source, digest in manifest["source_sha256"].items():
        assert source_sha256(ROOT / source) == digest, source

    expected = {
        "redmagic-arm64": ("arm64-v8a", "QUALCOMM", "QUALCOMM Adreno(TM)", 3),
        "redmagic-armv7": ("armeabi-v7a", "QUALCOMM", "QUALCOMM Adreno(TM)", 3),
        "pixel-arm64": ("arm64-v8a", "ARM", "Mali-G715 r0p0", 4),
    }

    for record in manifest["runs"]:
        abi, vendor, gpu_name, minor = expected[record["device"]]
        assert record["abi"] == abi
        assert record["opencl_vendor"] == vendor
        assert record["gpu_name"] == gpu_name
        assert record["loader_minor"] == minor

        path = ARCHIVE / record["file"]
        data = path.read_bytes()

        assert data.endswith(b"\n") and b"\r" not in data
        assert sha256(path) == record["sha256"]
        assert b"91809037" not in data and b"46281FDAS002AL" not in data
        lines = path.read_text(encoding="utf-8").splitlines()

        assert lines[:4] == [
            "TEST=vulkan-hashcat-real-frontend",
            f"ABI={abi}",
            "COMMAND=-I",
            "ADB_EXIT_CODE=0",
        ]

        verify_native_info(lines[4:], abi, vendor, gpu_name, minor)

    assert len(list(ARCHIVE.glob("*.log"))) == 3

    if args.artifacts is not None:
        folder = args.artifacts.resolve(strict=True)

        for abi in ("arm64-v8a", "armeabi-v7a"):
            item = manifest["binaries"][abi]
            binary = folder / abi / "hashcat"
            assert sha256(binary) == item["sha256"], abi

            modules = list((folder / abi / "modules").glob("*.so"))
            bridges = list((folder / abi / "bridges").glob("*.so"))
            opencl = list((folder / abi / "OpenCL").rglob("*"))
            assert len(modules) == item["modules"] == 581
            assert len(bridges) == item["bridges"] == 4
            assert sum(p.is_file() for p in opencl) == item["opencl_files"] == 1595

            if args.readelf is not None:
                inspect_elf(
                    args.readelf.resolve(strict=True), binary,
                    folder / abi / "modules/module_00000.so", abi,
                )

        print("PASS two exact full HASHCAT Android ARM64/ARMv7 binary identities")

        if args.readelf is not None:
            print("PASS API 24 ELF notes, Vulkan 1.0 symbols, isolated plugin linking")

    verify_mutations(manifest["runs"])
    print("PASS three real full HASHCAT -I Android GPU sessions")
    print("PASS Vulkan 1.0 instance creation, OpenCL coexistence and cleanup")
    print("PASS nine rejected manipulated evidence records")
    print("PASS exact source identities", len(manifest["source_sha256"]))


if __name__ == "__main__":
    main()
