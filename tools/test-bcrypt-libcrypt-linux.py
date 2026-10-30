#!/usr/bin/env python3
"""Verify the archived bcrypt GPU self-test input with Linux libcrypt.

The test requires a libcrypt implementation supporting bcrypt $2a$ records.
Both positive and negative candidates are checked against the same record.
"""

from __future__ import annotations

import ctypes
import ctypes.util
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
REDMAGIC = ROOT / "tests" / "fixtures" / "android-gpu" / "redmagic-selected-modes-20261030"
PIXEL = ROOT / "tests" / "fixtures" / "android-gpu" / "Pixel_9_Pro_XL-selected-modes-20261030"


def main() -> None:
    """Check both bcrypt candidates against the Linux crypt(3) result."""
    if not sys.platform.startswith("linux"):
        raise RuntimeError("This reference test requires Linux libcrypt")

    name = ctypes.util.find_library("crypt")
    if name is None:
        raise RuntimeError("Linux libcrypt is unavailable")

    library = ctypes.CDLL(name)
    library.crypt.argtypes = (ctypes.c_char_p, ctypes.c_char_p)
    library.crypt.restype = ctypes.c_char_p

    record = (REDMAGIC / "m03200.hash").read_text(encoding="ascii").strip()
    pixel_record = (PIXEL / "m03200.hash").read_text(encoding="ascii").strip()
    assert record == pixel_record
    assert record.startswith("$2a$05$")

    for candidate, must_match in (("hashcat", True), ("hashcat2", False)):
        output = library.crypt(candidate.encode("utf-8"), record.encode("ascii"))
        if output is None or output.startswith(b"*"):
            raise RuntimeError("crypt(3) did not calculate a bcrypt record")
        assert (output.decode("ascii") == record) == must_match, candidate
        print(f"PASS independent Linux libcrypt bcrypt reference: {candidate} {'accepted' if must_match else 'rejected'}")


if __name__ == "__main__":
    main()
