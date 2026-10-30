#!/usr/bin/env python3
"""Verify archived synthetic Android GPU cracking results against independent digests.

Reads committed synthetic GPU logs, exit markers and recovered lines under
``tests/fixtures/android-gpu`` without contacting ADB or modifying files.
The pixel NTLM reference uses the PyCryptodome ``Crypto.Hash.MD4`` module.
"""

from __future__ import annotations

import hashlib
import hmac
from pathlib import Path

from Crypto.Hash import MD4


ROOT = Path(__file__).resolve().parents[1] / "tests" / "fixtures" / "android-gpu"


def text_from(path: Path) -> str:
    """Read one archived UTF-8 result file with a descriptive missing-file error."""
    if not path.is_file():
        raise FileNotFoundError(f"Missing saved GPU result: {path}")
    return path.read_text(encoding="utf-8")


def verify_crack_log(log: str, mode: int, candidate: str) -> None:
    """Require a real completed crack, one recovered hash and the GPU generator."""
    for expected in (
        "Status...........: Cracked",
        "Recovered........: 1/1 (100.00%)",
        "Candidate.Engine.: Device Generator",
        f"Guess.Mask.......: {candidate} [{len(candidate)}]",
    ):
        if expected not in log:
            raise AssertionError(f"Mode {mode}: missing log fact {expected!r}")


def verify_pixel() -> None:
    """Check four recorded Pixel GPU matches against Python-computed digests."""
    candidate = "benchdemo2026"
    encoded = candidate.encode("utf-8")
    references = {
        0: hashlib.md5(encoded).hexdigest(),
        100: hashlib.sha1(encoded).hexdigest(),
        1000: MD4.new(candidate.encode("utf-16le")).hexdigest(),
        1400: hashlib.sha256(encoded).hexdigest(),
    }
    root = ROOT / "Pixel_9_Pro_XL-known-vectors-20261030"
    for mode, digest in references.items():
        stem = f"m{mode:05d}"
        assert text_from(root / f"{stem}.exit").strip() == "0", mode
        assert text_from(root / f"{stem}.status").strip() == "PASS", mode
        assert text_from(root / f"{stem}.cracked").strip() == f"{digest}:{candidate}", mode
        verify_crack_log(text_from(root / f"{stem}.log"), mode, candidate)
        print(f"PASS Pixel mode {mode}: independently computed digest, GPU recovered 1/1")


def verify_redmagic() -> None:
    """Check RedMagic SHA-512 family and positive/negative WPA PMKID GPU evidence."""
    root = ROOT / "redmagic-known-vectors"
    candidate = "hashcat"
    digest = hashlib.sha512(candidate.encode("utf-8")).hexdigest()
    assert text_from(root / "m01700.hash").strip() == digest
    verify_crack_log(text_from(root / "m01700-device.log"), 1700, candidate)
    print("PASS RedMagic mode 1700: SHA-512 digest and recovered 1/1")

    sha512crypt = text_from(root / "m01800.hash").strip()
    assert sha512crypt == (
        "$6$72820166$U4DVzpcYxgw7MVVDGGvB2/H5lRistD5.Ah4upwENR5UtffLR4X4SxSzfREv8z6wVl0jRFX40/KnYVvK4829kD1"
    )
    assert text_from(root / "m01800-device.exit").strip() == "0"
    assert text_from(root / "m01800-recovered.txt").strip() == f"{sha512crypt}:{candidate}"
    verify_crack_log(text_from(root / "m01800-device.log"), 1800, candidate)
    print("PASS RedMagic mode 1800: OpenSSL-reference SHA-512crypt and recovered 1/1")

    password = "hashcat!"
    record = text_from(root / "m22000-pmkid.hash").strip()
    fields = record.split("*")
    assert fields[0:2] == ["WPA", "01"]
    pmkid, ap, station, ssid_hex = fields[2:6]
    ssid = bytes.fromhex(ssid_hex)
    pmk = hashlib.pbkdf2_hmac("sha1", password.encode("utf-8"), ssid, 4096, 32)
    actual_pmkid = hmac.new(
        pmk, b"PMK Name" + bytes.fromhex(ap) + bytes.fromhex(station), hashlib.sha1
    ).digest()[:16].hex()
    assert actual_pmkid == pmkid
    expected_output = f"{pmkid}:{ap}:{station}:{ssid.decode('ascii')}:{password}"
    assert text_from(root / "m22000-pmkid-device.exit").strip() == "0"
    assert text_from(root / "m22000-pmkid-recovered.txt").strip() == expected_output
    verify_crack_log(text_from(root / "m22000-pmkid-device.log"), 22000, password)
    print("PASS RedMagic mode 22000 PMKID: PBKDF2+HMAC reference, GPU recovered 1/1")

    negative_log = text_from(root / "m22000-pmkid-negative-device.log")
    assert text_from(root / "m22000-pmkid-negative-device.exit").strip() == "1"
    for expected in (
        "Status...........: Exhausted",
        "Recovered........: 0/1 (0.00%)",
        "Candidate.Engine.: Device Generator",
        "Guess.Mask.......: hashcat2 [8]",
    ):
        if expected not in negative_log:
            raise AssertionError(f"Mode 22000 negative control: missing {expected!r}")
    assert "Status...........: Cracked" not in negative_log
    print("PASS RedMagic mode 22000 PMKID negative: wrong candidate exhausts with 0/1")


def main() -> None:
    """Validate every saved GPU known-vector test and report their outcomes."""
    verify_pixel()
    verify_redmagic()
    print("ALL EIGHT GPU KNOWN-VECTOR CASES VERIFIED: seven matches and one rejected PMKID (EAPOL not covered)")


if __name__ == "__main__":
    main()
