#!/usr/bin/env python3
"""Validate archived RedMagic bcrypt firmware and compiler diagnostics.

The checks require the original public bcrypt vector, the read-only firmware
profile and vendor ELF hashes, a compiler SIGSEGV trace with a matching ELF
Build ID, and a separate device self-test rejection after global S-box kernel
initialization. This validator executes no GPU workload or Android program.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "tests" / "fixtures" / "android-gpu" / "redmagic-selected-modes-20261030"
MANIFEST = FIXTURES / "m03200-diagnostic-variants.json"


def read(path: Path) -> str:
    """Read a required archived text file using its original Unicode contents."""
    if not path.is_file():
        raise FileNotFoundError(f"Missing RedMagic bcrypt evidence: {path}")
    return path.read_text(encoding="utf-8", errors="replace")


def sha256(path: Path) -> str:
    """Compute the SHA-256 digest of a required archived or tracked input file."""
    if not path.is_file():
        raise FileNotFoundError(f"Missing reference file: {path}")
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify_source_identity(manifest: dict) -> None:
    """Require the recorded device, mode, and stable bcrypt reference digests."""
    assert manifest["device"] == "RedMagic NX709S"
    assert manifest["gpu"] == "QUALCOMM Adreno(TM)"
    assert manifest["adb_serial"] == "91809037"
    assert manifest["hash_mode"] == 3200
    assert manifest["frontend_sha256"] == "4ab04450ab7357f97e18f0e66497dd4776c4300fb7412e4b88eb8713a8ebbad9"
    assert manifest["input_sha256"] == sha256(FIXTURES / "m03200.hash")
    assert manifest["input_sha256"] == "6acc77110fb4f798b60f6189f6daddb3bb3d4579db2ff2edc9a15768f6a5dfa4"

    stock = json.loads(read(FIXTURES / "manifest.json"))["artifacts"]
    assert manifest["stock_kernel_sha256"] == stock["m03200-pure.cl"]
    assert manifest["stock_module_sha256"] == stock["module_03200.so"]
    assert manifest["stock_kernel_sha256"] == sha256(ROOT / "source" / "OpenCL" / "m03200-pure.cl")
    print("PASS bcrypt diagnostic identity and original public vector")


def verify_driver_profile(manifest: dict) -> None:
    """Verify the archived firmware profile against stock logs and crash ELF IDs."""
    profile_path = FIXTURES / "m03200-adreno-driver-profile-20261031.json"
    profile = json.loads(read(profile_path))

    assert profile["observation_date"] == "2026-10-31"
    assert profile["observation_type"] == (
        "read-only Android system properties, vendor ELF metadata, and SHA-256 hashes"
    )

    device = profile["device"]

    assert device["model"] == "NX709S"
    assert device["product_name"] == "NX709S-UN"
    assert device["manufacturer"] == "nubia"
    assert device["system_locale"] == "zh-CN"
    assert device["firmware_region"] == "unverified"
    assert "adb_serial" not in profile_path.read_text(encoding="utf-8")

    system = profile["system"]
    vendor = profile["vendor"]

    assert system == {
        "android_release": "13",
        "build_incremental": "eng.nubia.20241204.165456",
        "security_patch": "2024-11-01",
    }
    assert vendor == {
        "android_release": "12",
        "build_incremental": "eng.nubia.20241204.170923",
        "security_patch": "2024-11-01",
    }

    opencl = profile["opencl"]

    assert opencl["advertised_version"] == "OpenCL 3.0"
    assert opencl["platform"] == "QUALCOMM"
    assert opencl["reported_compiler_build_date"] == "2022-07-19"
    assert opencl["reported_commit"] == "21eaafda81"
    assert opencl["reported_changeid"] == "I578ccc16f0"

    stock_log = read(FIXTURES / "m03200-pure-failure.log")

    assert (
        "OpenCL API (OpenCL 3.0 QUALCOMM build: "
        f"commit #{opencl['reported_commit']} "
        f"changeid #{opencl['reported_changeid']} "
        "Date: 07/19/22"
    ) in stock_log

    libraries = profile["libraries"]
    opencl_path = "/vendor/lib64/libOpenCL.so"
    compiler_path = "/vendor/lib64/libllvm-qcom.so"

    assert set(libraries) == {opencl_path, compiler_path}

    expected = {
        opencl_path: (
            163216,
            "10f08bc7b8df69587447c2b8514230341867f8aac127b277654d73333227b713",
            "4252b0a3fc7297cc93dfa7a31c208d1f",
        ),
        compiler_path: (
            30529480,
            "3325d6d70c6142facfe0ae5977ac2918b1433f61fed76352b6650d69f5df3685",
            "ef9dddff519158ff428e86aef013d6e9",
        ),
    }

    for library, (size, digest, elf_id) in expected.items():
        assert libraries[library] == {
            "size_bytes": size,
            "sha256": digest,
            "elf_build_id": elf_id,
        }
        assert re.fullmatch(r"[0-9a-f]{64}", digest)
        assert re.fullmatch(r"[0-9a-f]{32}", elf_id)

    observed_crash = profile["runtime_verification"]
    crash_build_id = libraries[compiler_path]["elf_build_id"]

    assert observed_crash["archived_crash_compiler_elf_build_id"] == crash_build_id
    assert observed_crash["crash_archive"] == (
        "m03200-cl-opt-disable-failure.crash.txt"
    )
    assert observed_crash["selftest_exit_code"] == int(
        read(FIXTURES / "m03200-pure-failure.exit").strip()
    )
    assert observed_crash["optimizer_disabled_compiler_crash_exit_code"] == (
        manifest["variants"]["cl_opt_disable"]["exit_code"]
    )
    assert observed_crash["gpu_execution_during_this_observation"] is False

    crash = read(FIXTURES / observed_crash["crash_archive"])

    assert (
        f"{compiler_path} (llvm::ValueHandleBase::AddToUseList()+56) "
        f"(BuildId: {crash_build_id})"
    ) in crash

    print(
        "PASS RedMagic NX709S host firmware, both OpenCL ELF fingerprints, "
        "and exact archived Qualcomm compiler crash Build ID"
    )


def verify_compiler_crash(manifest: dict) -> None:
    """Require the Qualcomm compiler SIGSEGV before a bcrypt device self-test."""
    variant = manifest["variants"]["cl_opt_disable"]
    assert variant["exit_code"] == 139
    assert variant["jit_options"] == "-D FIXED_LOCAL_SIZE=8 -cl-opt-disable"
    assert variant["module_sha256"] == "19e786f0b804b4719cdd558de4e06fecdc744ee9d24132d35bbfad413fbe9be7"
    stem = "m03200-cl-opt-disable-failure"
    log = read(FIXTURES / f"{stem}.log")
    crash = read(FIXTURES / f"{stem}.crash.txt")
    assert read(FIXTURES / f"{stem}.exit").strip() == str(variant["exit_code"])
    assert f"BCRYPT_ADRENO_COMPILER_DIAGNOSTIC: {variant['jit_options']}" in log
    assert "Starting self-test" not in log
    assert "Status...........: Cracked" not in log
    assert not (FIXTURES / f"{stem}.recovered").exists()
    for fragment in (
        "Fatal signal 11 (SIGSEGV)",
        "/vendor/lib64/libllvm-qcom.so",
        "llvm::CloneFunctionInto",
        "llvm::QGPUSplitCallGraphPass::runOnModule",
    ):
        assert fragment in crash, fragment
    print("PASS Adreno bcrypt no-optimization JIT compiler SIGSEGV: exit 139")


def verify_global_sbox_selftest(manifest: dict) -> None:
    """Require normal kernel initialization followed by bcrypt self-test rejection."""
    variant = manifest["variants"]["global_sboxes"]
    assert variant["exit_code"] == 245
    assert variant["kernel_sha256"] == "89c2073e9f2b3136cb2daa900daab9ad88a43e091823ff85271fc6088bd65c07"
    assert variant["helper_source_sha256"] == "44051bd7b7d45940c59cda92119015b5ddbf1b294f08927297623bb8ed149676"
    assert variant["helper_header_sha256"] == "5ec8b0d6cbb23455beac2bfd248e7267e4763e674e7f5d536089bb77d57b3c59"
    assert variant["module_sha256"] == manifest["stock_module_sha256"]

    stem = "m03200-global-scratch-failure"
    log = read(FIXTURES / f"{stem}.log")
    assert read(FIXTURES / f"{stem}.exit").strip() == str(variant["exit_code"])
    for fragment in (
        "OpenCL API (OpenCL 3.0 QUALCOMM",
        "Initialized device kernels and memory",
        "Starting self-test.",
        "* Device #1: ATTENTION! OpenCL kernel self-test failed.",
        "Aborting session due to kernel self-test failure.",
    ):
        assert fragment in log, (stem, fragment)
    assert "build failed" not in log
    assert "Status...........: Cracked" not in log
    assert not (FIXTURES / f"{stem}.recovered").exists()
    print("PASS Adreno bcrypt global-S-box kernel initialization, self-test rejected: exit 245")


def main() -> None:
    """Validate archived driver provenance and both bcrypt failure signatures."""
    manifest = json.loads(read(MANIFEST))
    verify_source_identity(manifest)
    verify_driver_profile(manifest)
    verify_compiler_crash(manifest)
    verify_global_sbox_selftest(manifest)
    print("REDMAGIC BCRYPT FIRMWARE, COMPILER AND MEMORY DIAGNOSTICS VERIFIED")


if __name__ == "__main__":
    main()
