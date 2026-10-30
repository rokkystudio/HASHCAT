#!/usr/bin/env python3
"""Verify that Pixel rejects wrong passwords and selects the right word among three.

The validator reads archived synthetic GPU test evidence only. It does not
contact ADB, load real credentials, or execute a cracking workload.
"""

from __future__ import annotations

import hashlib
from pathlib import Path

from Crypto.Hash import MD4


ROOT = Path(__file__).resolve().parents[1] / "tests" / "fixtures" / "android-gpu"
MODES = (0, 100, 1000, 1400)
RIGHT = "benchdemo2026"
WRONG = "benchdemo2027"
OTHER_WRONG = "benchdemo2025"


def digest_for(mode: int, password: str) -> str:
    """Calculate the reference digest using Python's independent hash primitives."""
    value = password.encode("utf-8")
    if mode == 0:
        return hashlib.md5(value).hexdigest()
    if mode == 100:
        return hashlib.sha1(value).hexdigest()
    if mode == 1000:
        return MD4.new(password.encode("utf-16le")).hexdigest()
    if mode == 1400:
        return hashlib.sha256(value).hexdigest()
    raise ValueError(f"Unsupported hashcat mode: {mode}")


def read(path: Path) -> str:
    """Read a retained UTF-8 result and reject a missing evidence file."""
    if not path.is_file():
        raise FileNotFoundError(f"Missing GPU evidence: {path}")
    return path.read_text(encoding="utf-8")


def verify_negative(mode: int) -> None:
    """Require exhaustion without any recovered hash for the wrong GPU candidate."""
    root = ROOT / "Pixel_9_Pro_XL-negative-vectors-20261030"
    stem = f"m{mode:05d}"
    log = read(root / f"{stem}.log")

    assert read(root / f"{stem}.exit").strip() == "1", stem
    assert read(root / f"{stem}.status").strip() == "PASS", stem
    for expected in (
        f"Hash.Mode........: {mode} ",
        "Status...........: Exhausted",
        "Recovered........: 0/1 (0.00%)",
        "Candidate.Engine.: Device Generator",
        f"Guess.Mask.......: {WRONG} [13]",
    ):
        assert expected in log, (mode, expected)
    assert "Status...........: Cracked" not in log, mode
    assert not (root / f"{stem}.cracked").exists(), mode

    # A different digest independently ensures the rejected candidate is wrong.
    assert digest_for(mode, WRONG) != digest_for(mode, RIGHT), mode
    print(f"PASS Pixel negative {mode}: wrong GPU candidate rejected (0/1)")


def verify_multi(mode: int) -> None:
    """Require exactly one recovered plaintext from a three-entry GPU wordlist."""
    root = ROOT / "Pixel_9_Pro_XL-multiple-candidates-20261030"
    stem = f"m{mode:05d}"
    log = read(root / f"{stem}.log")

    assert read(root / f"{stem}.exit").strip() == "0", stem
    assert read(root / f"{stem}.status").strip() == "PASS", stem
    assert read(root / f"{stem}.cracked").splitlines() == [RIGHT], stem
    for expected in (
        f"Hash.Mode........: {mode} ",
        "Status...........: Cracked",
        "Recovered........: 1/1 (100.00%)",
        "Candidate.Engine.: Device Generator",
        "Guess.Base.......: File",
    ):
        assert expected in log, (mode, expected)

    expected_hash = digest_for(mode, RIGHT)
    positive = ROOT / "Pixel_9_Pro_XL-known-vectors-20261030"
    assert read(positive / f"{stem}.cracked").strip() == f"{expected_hash}:{RIGHT}", stem
    print(f"PASS Pixel wordlist {mode}: selected exact correct candidate out of three")


def main() -> None:
    """Verify both candidate-rejection and candidate-selection evidence per mode."""
    wordlist = ROOT / "Pixel_9_Pro_XL-multiple-candidates-20261030" / "candidates.txt"
    assert read(wordlist).splitlines() == [WRONG, RIGHT, OTHER_WRONG]
    for mode in MODES:
        verify_negative(mode)
        verify_multi(mode)
    print("ALL EIGHT ADDITIONAL PIXEL GPU CANDIDATE TESTS VERIFIED")


if __name__ == "__main__":
    main()
