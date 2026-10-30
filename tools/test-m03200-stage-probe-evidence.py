#!/usr/bin/env python3
"""Validate the RedMagic bcrypt standalone OpenCL probe's archived first run.

The first launch loaded the diagnostic executable but failed to locate the
OpenCL ICD. The test verifies the unmodified bcrypt input and helper source,
staged probe hashes, exact device exit and error, and confirms that the
archived evidence contains no program build or GPU execution claim.

No Android connection, OpenCL program, or hashcat workload is started.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = ROOT / "tests" / "fixtures" / "android-gpu" / "redmagic-selected-modes-20261030"
MANIFEST = ARCHIVE / "m03200-standalone-probe-manifest.json"


def sha256(path: Path) -> str:
    """Hash the complete source or public test vector without normalizing bytes."""
    if not path.is_file():
        raise FileNotFoundError(f"Missing archived probe source: {path}")
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read(path: Path) -> str:
    """Read an exact archived diagnostic file using UTF-8 text decoding."""
    if not path.is_file():
        raise FileNotFoundError(f"Missing archived probe evidence: {path}")
    return path.read_text(encoding="utf-8")


def main() -> None:
    """Require the documented OpenCL loader failure and original source identity."""
    manifest = json.loads(read(MANIFEST))

    assert manifest["device"] == "RedMagic NX709S"
    assert manifest["adb_serial"] == "91809037"
    assert manifest["gpu"] == "QUALCOMM Adreno(TM)"
    assert manifest["hash_mode"] == 3200
    assert manifest["candidate"] == "hashcat"

    sources = {
        "public_input_sha256": ARCHIVE / "m03200.hash",
        "stock_kernel_sha256": ROOT / "source" / "OpenCL" / "m03200-pure.cl",
        "helper_source_sha256": ROOT / "source" / "OpenCL" / "inc_cipher_blowfish.cl",
        "standalone_probe_source_sha256": ROOT / "tools" / "m03200-stage-probe.cl",
        "staged_runner_sha256": ROOT / "tools" / "run-redmagic-bcrypt-stage-probe.sh",
    }

    for label, path in sources.items():
        assert manifest[label] == sha256(path), label

    original = json.loads(read(ARCHIVE / "manifest.json"))["artifacts"]
    assert manifest["stock_module_sha256"] == original["module_03200.so"]
    assert manifest["stock_kernel_sha256"] == original["m03200-pure.cl"]
    assert re.fullmatch("[0-9a-f]{64}", manifest["staged_arm64_probe_binary_sha256"])

    clang = manifest["host_preflight"]
    assert clang["hashcat_opencl_probe_syntax"] == ["CL1.2", "CL3.0"]
    assert clang["android_arm64_gnu_c11_warnings"] == "none after selecting OpenCL 1.2 host API"
    assert clang["opencl_syntax_warnings"] == "none with -Werror"
    assert clang["real_gpu_execution_confirmed"] is False

    state = manifest["first_run"]
    assert state["exit_code"] == 2
    assert state["opencl_program_build_attempted"] is False
    assert state["gpu_kernels_executed"] is False
    assert state["gpu_intermediate_snapshots_written"] is False

    exit_code = read(ARCHIVE / state["archived_exit"]).strip()
    log = read(ARCHIVE / state["archived_log"])

    assert exit_code == str(state["exit_code"])
    assert log.strip() == 'OpenCL dlopen: dlopen failed: library "libOpenCL.so" not found'
    for fragment in (
        "PROBE_BUILD_BEGIN",
        "PROBE_BUILD_SUCCESS",
        "PROBE_INIT_BEGIN",
        "STAGE_SAVED",
        "PROBE_COMPLETE",
    ):
        assert fragment not in log, fragment

    helper = read(ROOT / "tools" / "m03200-stage-probe.cl")
    assert '#include "inc_cipher_blowfish.cl"' in helper
    for function in ("bcrypt_stage_init", "bcrypt_stage_loop", "bcrypt_stage_comp"):
        assert helper.count(f"void {function}") == 1, function
    runner = read(ROOT / "tools" / "run-redmagic-bcrypt-stage-probe.sh")
    assert "--self-test-disable" not in runner

    print("PASS RedMagic standalone bcrypt probe: exact source hashes and public vector")
    print("PASS standalone OpenCL ICD discovery failure: exit 2, before program build")
    print("PASS no GPU execution or captured intermediate state claimed")
    print("REDMAGIC BCRYPT STANDALONE PROBE EVIDENCE VERIFIED")


if __name__ == "__main__":
    main()
