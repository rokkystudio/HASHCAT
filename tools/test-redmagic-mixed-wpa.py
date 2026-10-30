#!/usr/bin/env python3
"""Validate archived RedMagic GPU results for a mixed four-record WPA 22000 batch.

The reference batch contains one synthetic PMKID and three independently
constructed synthetic EAPOL records using key versions 1, 2 and 3.
This check reads local evidence only; it never contacts Android or runs hashcat.
"""

from __future__ import annotations

import hashlib
import hmac
import runpy
from pathlib import Path


FIXTURES = Path(__file__).resolve().parents[1] / "tests" / "fixtures" / "android-gpu"
MIXED = FIXTURES / "redmagic-mixed-wpa"
EAPOL = FIXTURES / "redmagic-eapol-vectors"
PMKID = FIXTURES / "redmagic-known-vectors" / "m22000-pmkid.hash"


def read(path: Path) -> str:
    """Return one archived evidence file, rejecting missing results explicitly."""
    if not path.is_file():
        raise FileNotFoundError(f"Missing saved GPU evidence: {path}")
    return path.read_text(encoding="utf-8")


def verify_input() -> None:
    """Recompute the PMKID and three EAPOL MICs and compare the mixed input."""
    expected = [
        read(PMKID).strip(),
        read(EAPOL / "m22000-eapol-aux1.hash").strip(),
        read(EAPOL / "m22000-eapol-aux2.hash").strip(),
        read(EAPOL / "m22000-eapol-aux3.hash").strip(),
    ]
    pmkid_fields = expected[0].split("*")
    assert pmkid_fields[:2] == ["WPA", "01"]
    pmk = hashlib.pbkdf2_hmac(
        "sha1", b"hashcat!", bytes.fromhex(pmkid_fields[5]), 4096, 32
    )
    computed_pmkid = hmac.new(
        pmk,
        b"PMK Name" + bytes.fromhex(pmkid_fields[3]) + bytes.fromhex(pmkid_fields[4]),
        hashlib.sha1,
    ).digest()[:16].hex()
    assert computed_pmkid == pmkid_fields[2]

    generator = runpy.run_path(
        str(Path(__file__).resolve().parent / "generate-m22000-eapol-vectors.py")
    )
    for keyver, record in enumerate(expected[1:], start=1):
        generator["verify_record"](record, keyver)
        independently_computed, _kck, _mic = generator["build_record"](keyver)
        assert record == independently_computed

    actual = read(MIXED / "mixed.hash").splitlines()
    assert actual == expected, "Mixed batch differs from individually validated hashes"
    assert [record.split("*")[1] for record in actual] == ["01", "02", "02", "02"]
    assert len(set(actual)) == 4
    print("PASS mixed GPU input: one PMKID and three distinct EAPOL key versions")


def verify_positive() -> None:
    """Require four recovered digests, the exact plaintext and host success."""
    log = read(MIXED / "match.log")
    assert read(MIXED / "match.exit").strip() == "0"
    assert read(MIXED / "match.recovered").splitlines() == ["hashcat!"] * 4
    host_log = read(MIXED / "match-runner.stdout.log")
    assert "MIXED_WPA_MATCH_PASS mode=22000 case=match exit=0" in host_log
    assert "REDMAGIC_MIXED4_HOST_EXIT:0" in host_log
    for fragment in (
        "Status...........: Cracked",
        "Hash.Mode........: 22000",
        "Hashes: 4 digests; 4 unique digests, 1 unique salts",
        "Recovered........: 4/4 (100.00%)",
        "Progress.........: 1/1 (100.00%)",
        "Guess.Mask.......: hashcat! [8]",
        "Candidate.Engine.: Device Generator",
    ):
        assert fragment in log, f"Mixed positive GPU log missing {fragment!r}"
    print("PASS mixed GPU positive: exactly four recoveries with hashcat!")


def verify_negative() -> None:
    """Require exhausted GPU results, no recovered file and host success."""
    log = read(MIXED / "wrong.log")
    assert read(MIXED / "wrong.exit").strip() == "1"
    assert not (MIXED / "wrong.recovered").exists()
    host_log = read(MIXED / "wrong-runner.stdout.log")
    assert "MIXED_WPA_NEGATIVE_PASS mode=22000 exit=1" in host_log
    assert "REDMAGIC_MIXED_WRONG_HOST_EXIT:0" in host_log
    for fragment in (
        "Status...........: Exhausted",
        "Hash.Mode........: 22000",
        "Hashes: 4 digests; 4 unique digests, 1 unique salts",
        "Recovered........: 0/4 (0.00%)",
        "Guess.Mask.......: hashcat2 [8]",
        "Candidate.Engine.: Device Generator",
    ):
        assert fragment in log, f"Mixed negative GPU log missing {fragment!r}"
    assert "Status...........: Cracked" not in log
    print("PASS mixed GPU negative: all four rejected for hashcat2")


def main() -> None:
    """Verify input identity and both GPU results for the complete mixed batch."""
    verify_input()
    verify_positive()
    verify_negative()
    print("MIXED REDMAGIC WPA GPU BATCH VERIFIED: 4/4 match and 0/4 mismatch")


if __name__ == "__main__":
    main()
