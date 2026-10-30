#!/usr/bin/env python3
"""Verify archived Pixel GPU evidence for modes 500, 3200 and 5600.

The validator reads committed synthetic self-test vectors and device logs only.
It does not contact ADB or execute hashcat. The md5crypt vector is recomputed
with an independent Python implementation. The bcrypt and NetNTLMv2 vectors
are checked against their public hashcat module self-test records because no
independent verifier was available in the Windows test environment.
"""

from __future__ import annotations

import hashlib
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1] / "tests" / "fixtures" / "android-gpu" / "Pixel_9_Pro_XL-selected-modes-20261030"
CANDIDATE = "hashcat"
CRYPT64 = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"


def read(name: str) -> str:
    """Read one retained UTF-8 fixture and reject a missing file."""
    path = ROOT / name
    if not path.is_file():
        raise FileNotFoundError(f"Missing Pixel selected-mode evidence: {path}")
    return path.read_text(encoding="utf-8")


def crypt64(value: int, count: int) -> str:
    """Encode little-endian 6-bit groups using the traditional crypt alphabet."""
    out = []
    for _ in range(count):
        out.append(CRYPT64[value & 0x3F])
        value >>= 6
    return "".join(out)


def md5crypt(password: str, salt: str) -> str:
    """Compute Unix md5crypt ($1$) for the supplied ASCII password and salt."""
    password_bytes = password.encode("utf-8")
    salt = salt.split("$", 1)[0][:8]
    salt_bytes = salt.encode("ascii")
    magic = b"$1$"

    primary = hashlib.md5()
    primary.update(password_bytes)
    primary.update(magic)
    primary.update(salt_bytes)

    alternate = hashlib.md5(password_bytes + salt_bytes + password_bytes).digest()
    remaining = len(password_bytes)
    while remaining > 0:
        primary.update(alternate[: min(16, remaining)])
        remaining -= 16

    length = len(password_bytes)
    while length > 0:
        if length & 1:
            primary.update(b"\x00")
        else:
            primary.update(password_bytes[:1])
        length >>= 1

    digest = primary.digest()
    for index in range(1000):
        current = hashlib.md5()
        if index & 1:
            current.update(password_bytes)
        else:
            current.update(digest)
        if index % 3:
            current.update(salt_bytes)
        if index % 7:
            current.update(password_bytes)
        if index & 1:
            current.update(digest)
        else:
            current.update(password_bytes)
        digest = current.digest()

    encoded = "".join(
        (
            crypt64((digest[0] << 16) | (digest[6] << 8) | digest[12], 4),
            crypt64((digest[1] << 16) | (digest[7] << 8) | digest[13], 4),
            crypt64((digest[2] << 16) | (digest[8] << 8) | digest[14], 4),
            crypt64((digest[3] << 16) | (digest[9] << 8) | digest[15], 4),
            crypt64((digest[4] << 16) | (digest[10] << 8) | digest[5], 4),
            crypt64(digest[11], 2),
        )
    )
    return f"$1${salt}${encoded}"


def verify_success(mode: int, kernel: str) -> None:
    """Require exit 0, exact plaintext recovery and a completed GPU crack log."""
    stem = f"m{mode:05d}-{kernel}"
    log = read(f"{stem}.log")
    assert read(f"{stem}.exit").strip() == "0", stem
    assert read(f"{stem}.recovered").strip() == CANDIDATE, stem
    for expected in (
        f"Hash.Mode........: {mode} ",
        "Status...........: Cracked",
        "Recovered........: 1/1 (100.00%)",
        "Candidate.Engine.: Device Generator",
    ):
        assert expected in log, (mode, expected)
    print(f"PASS Pixel mode {mode} ({kernel}): GPU recovered exact self-test plaintext")


def verify_mode_500() -> None:
    """Verify the OpenSSL-equivalent md5crypt vector and the pure-kernel Mali crash."""
    expected = "$1$38652870$DUjsu4TTlTsOe/xxZ05uf/"
    assert read("m00500.hash").strip() == expected
    assert md5crypt(CANDIDATE, "38652870") == expected
    verify_success(500, "optimized")

    crash = read("m00500-pure-crash.log")
    for expected_crash in (
        "Fatal signal 6 (SIGABRT)",
        "hashcat -m 500 -a 3 -w 1",
        "Scudo ERROR: internal map failure (error desc=Out of memory)",
        "llvm::createValhallPreRAMachineScheduler",
        "clcc::Kernel::build_kernel",
        "clLinkProgram",
    ):
        assert expected_crash in crash, expected_crash
    print("PASS Pixel mode 500 pure diagnostic: Mali compiler SIGABRT evidence retained")


def main() -> None:
    """Validate all selected-mode Pixel fixtures and their runtime manifest."""
    manifest = json.loads(read("manifest.json"))
    assert manifest["device"] == "Pixel 9 Pro XL"
    assert manifest["gpu"] == "Mali-G715"
    assert manifest["candidate"] == CANDIDATE
    assert manifest["frontend_sha256"] == "4ab04450ab7357f97e18f0e66497dd4776c4300fb7412e4b88eb8713a8ebbad9"

    verify_mode_500()

    assert read("m03200.hash").strip() == "$2a$05$MBCzKhG1KhezLh.0LRa0Kuw12nLJtpHy6DIaU.JAnqJUDYspHC.Ou"
    verify_success(3200, "pure")

    assert read("m05600.hash").strip() == (
        "0UL5G37JOI0SX::6VB1IS0KA74:ebe1afa18b7fbfa6:"
        "aab8bf8675658dd2a939458a1077ba08:"
        "010100000000000031c8aa092510945398b9f7b7dde1a9fb00000000f7876f2b04b700"
    )
    verify_success(5600, "pure")

    print("ALL THREE PIXEL SELECTED-MODE GPU CASES VERIFIED")


if __name__ == "__main__":
    main()
