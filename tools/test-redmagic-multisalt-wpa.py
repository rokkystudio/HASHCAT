#!/usr/bin/env python3
"""Verify real Adreno GPU WPA 22000 results for four independent SSIDs.

All reference WPA PMKID and EAPOL MIC values are regenerated using Python
cryptographic primitives, never the GPU implementation under test.
The verifier only reads committed fixtures; it never contacts Android.
"""

from __future__ import annotations

import runpy
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "tests" / "fixtures" / "android-gpu" / "redmagic-multisalt-wpa"


def read(name: str) -> str:
    """Read a required GPU evidence file without guessing missing results."""
    path = FIXTURES / name
    if not path.is_file():
        raise FileNotFoundError(f"Missing saved multi-salt GPU result: {path}")
    return path.read_text(encoding="utf-8", errors="replace")


def check_records() -> None:
    """Compare the four parsed SSIDs and MICs with independent CPU references."""
    generator = runpy.run_path(str(ROOT / "tools" / "generate-m22000-multisalt-vectors.py"))
    expected = generator["build_records"]()
    actual = read("multisalt.hash").splitlines()
    assert actual == expected
    assert len({record.split("*")[5] for record in actual}) == 4
    assert [record.split("*")[1] for record in actual] == ["01", "02", "02", "02"]
    for version, record in enumerate(actual):
        generator["verify_record"](record, version)
    print("PASS independent MIC/PMKID CPU references for four SSIDs")


def check_positive() -> None:
    """Require GPU completion, all four recovered records and the exact passphrase."""
    log = read("match.log")
    assert read("match.exit").strip() == "0"
    assert read("match.recovered").splitlines() == ["hashcat!"] * 4
    for fragment in (
        "Hash.Mode........: 22000",
        "Hashes: 4 digests; 4 unique digests, 4 unique salts",
        "Status...........: Cracked",
        "Recovered........: 4/4 (100.00%)",
        "Candidate.Engine.: Device Generator",
        "Guess.Mask.......: hashcat! [8]",
    ):
        assert fragment in log, f"Missing GPU positive evidence: {fragment}"
    print("PASS RedMagic GPU multi-salt positive: four of four recovered")


def check_negative() -> None:
    """Require GPU exhaustion with zero recovered records for the wrong passphrase."""
    log = read("wrong.log")
    assert read("wrong.exit").strip() == "1"
    assert not (FIXTURES / "wrong.recovered").exists()
    for fragment in (
        "Hash.Mode........: 22000",
        "Hashes: 4 digests; 4 unique digests, 4 unique salts",
        "Status...........: Exhausted",
        "Recovered........: 0/4 (0.00%)",
        "Candidate.Engine.: Device Generator",
        "Guess.Mask.......: hashcat2 [8]",
    ):
        assert fragment in log, f"Missing GPU negative evidence: {fragment}"
    assert "Status...........: Cracked" not in log
    print("PASS RedMagic GPU multi-salt negative: four of four rejected")


def main() -> None:
    """Verify CPU references and both positive and negative GPU test outcomes."""
    check_records()
    check_positive()
    check_negative()
    print("REDMAGIC FOUR-SALT WPA GPU REGRESSION VERIFIED")


if __name__ == "__main__":
    main()
