#!/usr/bin/env python3
"""Verify archived synthetic Android GPU cracking results against independent digests.

Reads committed synthetic GPU logs, exit markers and recovered lines under
``tests/fixtures/android-gpu`` without contacting ADB or modifying files.
The pixel NTLM reference uses the PyCryptodome ``Crypto.Hash.MD4`` module.
"""

from __future__ import annotations

import hashlib
import hmac
import runpy
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


def verify_redmagic_eapol() -> None:
    """Verify EAPOL AUX1–AUX3 GPU matches and independently computed MICs.

    The fixture generator uses only standard Python HMAC/PBKDF2 and
    PyCryptodome AES-CMAC. No GPU code participates in record generation.
    The AUX2 host runner log is archived as proof of the reported password;
    AUX1 and AUX3 additionally save an on-device recovered-word marker.
    """
    generator_path = Path(__file__).resolve().parent / "generate-m22000-eapol-vectors.py"
    generator = runpy.run_path(str(generator_path))
    root = ROOT / "redmagic-eapol-vectors"
    password = "hashcat!"

    for keyver, suffix in ((1, "aux1"), (2, "aux2"), (3, "aux3")):
        prefix = f"m22000-eapol-{suffix}"
        record = text_from(root / f"{prefix}.hash").strip()
        expected_record, kck, mic = generator["build_record"](keyver)
        generator["verify_record"](record, keyver)
        assert record == expected_record, suffix
        assert len(kck) == 16 and len(mic) == 16, suffix
        assert record.split("*")[2] == mic.hex(), suffix
        assert text_from(root / f"{prefix}-device.exit").strip() == "0", suffix
        verify_crack_log(text_from(root / f"{prefix}-device.log"), 22000, password)

        if keyver == 2:
            runner_output = text_from(root / f"{prefix}-runner.stdout.log").splitlines()
            assert "KNOWN_VECTOR_PASS mode=22000 exit=0" in runner_output
            assert "REDMAGIC_EAPOL_AUX2_ADB_EXIT:0" in runner_output
            assert password in runner_output
        else:
            assert text_from(root / f"{prefix}-recovered.txt").strip() == password, suffix

        print(f"PASS RedMagic WPA EAPOL keyver {keyver} ({suffix}): independently computed MIC and GPU recovered 1/1")

        negative_log = text_from(root / f"{prefix}-negative-device.log")
        assert text_from(root / f"{prefix}-negative-device.exit").strip() == "1", suffix
        for expected in (
            "Status...........: Exhausted",
            "Recovered........: 0/1 (0.00%)",
            "Candidate.Engine.: Device Generator",
            "Guess.Mask.......: hashcat2 [8]",
        ):
            if expected not in negative_log:
                raise AssertionError(f"Mode 22000 {suffix} negative control: missing {expected!r}")
        assert "Status...........: Cracked" not in negative_log, suffix
        print(f"PASS RedMagic WPA EAPOL keyver {keyver} ({suffix}) negative: wrong candidate rejected 0/1")


def main() -> None:
    """Validate archived positive and negative synthetic GPU cracking cases."""
    verify_pixel()
    verify_redmagic()
    verify_redmagic_eapol()
    print("ALL FOURTEEN GPU KNOWN-VECTOR CASES VERIFIED: ten matches and four rejected candidates")


if __name__ == "__main__":
    main()
