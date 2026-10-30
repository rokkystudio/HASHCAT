#!/usr/bin/env python3
"""Validate archived RedMagic md5crypt, bcrypt and NetNTLMv2 GPU trials.

The same synthetic inputs passed on Pixel 9 Pro XL. The evidence shows
md5crypt and NetNTLMv2 recovery on Adreno, and a bcrypt device
self-test failure on Adreno despite identical compiled inputs.
md5crypt and NetNTLMv2 are independently recomputed on the CPU.
The bcrypt hash is verified against hashcat's public self-test record.
"""

from __future__ import annotations

import hashlib
import hmac
import json
import re
import runpy
from pathlib import Path

from Crypto.Hash import MD4


ROOT = Path(__file__).resolve().parents[1]
REDMAGIC = ROOT / "tests" / "fixtures" / "android-gpu" / "redmagic-selected-modes-20261030"
PIXEL = ROOT / "tests" / "fixtures" / "android-gpu" / "Pixel_9_Pro_XL-selected-modes-20261030"
CASES = ((500, "optimized"), (3200, "pure"), (5600, "pure"))
PASSWORD = "hashcat"


def read(path: Path) -> str:
    """Read required archived GPU evidence or raise for missing files."""
    if not path.is_file():
        raise FileNotFoundError(f"Missing archived GPU evidence: {path}")
    return path.read_text(encoding="utf-8", errors="replace")


def verify_netntlmv2(record: str) -> None:
    """Compute the NetNTLMv2 HMAC-MD5 proof with a separate CPU implementation."""
    username, empty, domain, challenge, response, blob = record.split(":")
    assert empty == ""
    nt_hash = MD4.new(PASSWORD.encode("utf-16le")).digest()
    key = hmac.new(
        nt_hash, (username.upper() + domain).encode("utf-16le"), hashlib.md5
    ).digest()
    proof = hmac.new(
        key, bytes.fromhex(challenge) + bytes.fromhex(blob), hashlib.md5
    ).hexdigest()
    assert hmac.compare_digest(proof, response.lower())
    print("PASS independent NetNTLMv2 CPU reference")


def verify_bcrypt_selftest(record: str) -> None:
    """Require the bcrypt record to equal the hashcat module public self-test."""
    module = read(ROOT / "source" / "src" / "modules" / "module_03200.c")
    hash_match = re.search(r'static const char \*ST_HASH\s*=\s*"([^"]+)"', module)
    pass_match = re.search(r'static const char \*ST_PASS\s*=\s*"([^"]+)"', module)
    assert hash_match is not None and pass_match is not None
    assert record == hash_match.group(1)
    assert PASSWORD == pass_match.group(1)
    print("PASS bcrypt input equals public hashcat module self-test")


def verify_references() -> None:
    """Require unchanged cross-device test inputs and CPU cryptographic references."""
    md5crypt = runpy.run_path(str(ROOT / "tools" / "test-pixel-selected-modes.py"))[
        "md5crypt"
    ]
    for mode, _kernel in CASES:
        name = f"m{mode:05d}.hash"
        record = read(REDMAGIC / name).strip()
        assert record == read(PIXEL / name).strip(), name
        if mode == 500:
            assert md5crypt(PASSWORD, "38652870") == record
            print("PASS independent md5crypt CPU reference")
        elif mode == 3200:
            verify_bcrypt_selftest(record)
        elif mode == 5600:
            verify_netntlmv2(record)


def verify_success(mode: int, kernel: str) -> None:
    """Check a complete Adreno recovery with exact kernel path and exit 0."""
    stem = f"m{mode:05d}-{kernel}"
    log = read(REDMAGIC / f"{stem}.log")
    assert read(REDMAGIC / f"{stem}.exit").strip() == "0", stem
    assert read(REDMAGIC / f"{stem}.recovered").strip() == PASSWORD, stem
    feature = "Optimized Kernel" if kernel == "optimized" else "Pure Kernel"
    for fragment in (
        f"Hash.Mode........: {mode} ",
        "Status...........: Cracked",
        "Recovered........: 1/1 (100.00%)",
        "Candidate.Engine.: Device Generator",
        f"Guess.Mask.......: {PASSWORD} [7]",
        f"Kernel.Feature...: {feature}",
    ):
        assert fragment in log, (stem, fragment)
    print(f"PASS RedMagic mode {mode}: {kernel} GPU recovered 1/1, exit 0")


def verify_bcrypt_failure() -> None:
    """Check bcrypt self-test failure and matching artifacts with Pixel's successful test."""
    stem = "m03200-pure-failure"
    log = read(REDMAGIC / f"{stem}.log")
    assert read(REDMAGIC / f"{stem}.exit").strip() == "245"
    assert "Status...........: Cracked" not in log
    for fragment in (
        "* Device #1: ATTENTION! OpenCL kernel self-test failed.",
        "Aborting session due to kernel self-test failure.",
    ):
        assert fragment in log, fragment
    assert not (REDMAGIC / f"{stem}.recovered").exists()
    assert read(PIXEL / "m03200-pure.exit").strip() == "0"
    assert read(PIXEL / "m03200-pure.recovered").strip() == PASSWORD

    redmagic_artifacts = json.loads(read(REDMAGIC / "manifest.json"))["artifacts"]
    pixel_artifacts = json.loads(read(PIXEL / "manifest.json"))["artifacts"]
    for name in ("module_03200.so", "m03200-pure.cl"):
        assert redmagic_artifacts[name] == pixel_artifacts[name], name
    print("PASS bcrypt hardware divergence documented: RedMagic self-test exit 245, Pixel exit 0")


def verify_netntlmv2_pure_failure() -> None:
    """Require Adreno OpenCL a3-pure link failure while Pixel recovered the same hash."""
    stem = "m05600-pure-failure"
    log = read(REDMAGIC / f"{stem}.log")
    assert read(REDMAGIC / f"{stem}.exit").strip() == "251"
    assert "clLinkProgram(): CL_OUT_OF_HOST_MEMORY" in log
    assert "m05600_a3-pure.cl build failed" in log
    assert "Status...........: Cracked" not in log
    assert not (REDMAGIC / f"{stem}.recovered").exists()
    assert read(PIXEL / "m05600-pure.exit").strip() == "0"
    assert read(PIXEL / "m05600-pure.recovered").strip() == PASSWORD

    redmagic_artifacts = json.loads(read(REDMAGIC / "manifest.json"))["artifacts"]
    pixel_artifacts = json.loads(read(PIXEL / "manifest.json"))["artifacts"]
    for name in ("module_05600.so", "m05600_a3-pure.cl"):
        assert redmagic_artifacts[name] == pixel_artifacts[name], name
    print("PASS NetNTLMv2 kernel divergence: Adreno pure link exit 251, Pixel pure recovered 1/1")


def main() -> None:
    """Verify three cross-device vectors with two Adreno successes and two failures."""
    verify_references()
    verify_success(500, "optimized")
    verify_bcrypt_failure()
    verify_netntlmv2_pure_failure()
    verify_success(5600, "optimized")
    print("REDMAGIC THREE MODE CROSS-DEVICE EVIDENCE VERIFIED")


if __name__ == "__main__":
    main()
