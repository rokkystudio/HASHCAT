#!/usr/bin/env python3
"""Run independent CPU controls for synthetic multi-salt WPA mode 22000 vectors.

The check requires four unique SSIDs, validates the correct MIC/PMKID for
each key version, and requires rejection of both a wrong passphrase and a
single-digit corrupted MIC. It performs no GPU, ADB or network operations.
"""

from __future__ import annotations

import runpy
from pathlib import Path


GENERATOR = Path(__file__).resolve().parent / "generate-m22000-multisalt-vectors.py"


def must_reject(verify, record: str, version: int, password: bytes) -> None:
    """Assert that the CPU verifier refuses an incorrect MIC or passphrase."""
    try:
        verify(record, version, password)
    except AssertionError:
        return
    raise AssertionError(f"CPU reference unexpectedly accepted key version {version}")


def main() -> None:
    """Check four CPU positives, four password negatives and four corrupted MICs."""
    module = runpy.run_path(str(GENERATOR))
    records = module["build_records"]()
    verify = module["verify_record"]
    assert len(records) == 4
    assert len({record.split("*")[5] for record in records}) == 4

    for version, record in enumerate(records):
        verify(record, version)
        must_reject(verify, record, version, b"hashcat2")
        parts = record.split("*")
        first = parts[2][0]
        parts[2] = ("1" if first != "1" else "2") + parts[2][1:]
        must_reject(verify, "*".join(parts), version, b"hashcat!")
        print(f"PASS synthetic multi-salt CPU reference: keyver={version}, ssid={bytes.fromhex(record.split('*')[5])!r}")

    print("CPU MULTISALT REFERENCES VERIFIED: 4 positive, 8 negative")


if __name__ == "__main__":
    main()
