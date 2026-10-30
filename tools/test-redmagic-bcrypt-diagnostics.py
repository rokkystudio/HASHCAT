#!/usr/bin/env python3
"""Validate archived RedMagic bcrypt compiler and S-box storage diagnostics.

The checks require the original public bcrypt self-test vector, Qualcomm
compiler SIGSEGV evidence for one diagnostic JIT configuration, and a
separate failed device self-test after global S-box kernel initialization.
No GPU workload is launched by this validator.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path


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
    """Verify both archived bcrypt diagnostics without executing a GPU workload."""
    manifest = json.loads(read(MANIFEST))
    verify_source_identity(manifest)
    verify_compiler_crash(manifest)
    verify_global_sbox_selftest(manifest)
    print("REDMAGIC BCRYPT COMPILER AND MEMORY DIAGNOSTICS VERIFIED")


if __name__ == "__main__":
    main()
