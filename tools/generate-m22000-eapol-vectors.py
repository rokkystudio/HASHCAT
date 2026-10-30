#!/usr/bin/env python3
"""Generate deterministic synthetic WPA*02 EAPOL GPU correctness fixtures.

Builds a minimal, structurally valid EAPOL-Key message with the MIC field
zeroed (the representation used by hashcat) and derives its expected MIC
using Python hashlib/HMAC and PyCryptodome AES-CMAC, never GPU code.

The three key descriptors exercise hashcat mode 22000 AUX1 (HMAC-MD5),
AUX2 (HMAC-SHA1 truncated to 16 bytes), and AUX3 (AES-CMAC) respectively.
The vectors contain invented MAC addresses, nonces, SSID and passphrase,
not packet captures or credentials collected from a real network.
"""

from __future__ import annotations

import argparse
import hashlib
import hmac
from pathlib import Path

from Crypto.Cipher import AES
from Crypto.Hash import CMAC


PASSWORD = "hashcat!"
SSID = b"hashcat-essid"
AP = bytes.fromhex("fc690c158264")
STATION = bytes.fromhex("f4747f87f9f4")
ANONCE = bytes(range(0x10, 0x30))
SNONCE = bytes(range(0x40, 0x60))
MESSAGE_PAIR = "00"


def make_eapol(keyver: int) -> bytes:
    """Construct a 99-byte EAPOL-Key frame with the on-wire MIC field empty.

    EAPOL header: version (2), type (3), big-endian payload size (95).
    Key descriptor: WPA 254 for keyver 1, RSN 2 for keyvers 2 and 3.
    The key_info low two bits encode the version; the SNonce is the
    descriptor nonce, and the 16-byte MIC at offset 81 remains zero.
    """
    if keyver not in (1, 2, 3):
        raise ValueError(f"Unsupported EAPOL key version: {keyver}")

    descriptor = 254 if keyver == 1 else 2
    packet = bytearray(99)
    packet[0:2] = bytes((2, 3))
    packet[2:4] = (95).to_bytes(2, "big")
    packet[4] = descriptor
    packet[5:7] = (0x0100 | keyver).to_bytes(2, "big")
    packet[7:9] = (16).to_bytes(2, "big")
    packet[9:17] = (1).to_bytes(8, "big")
    packet[17:49] = SNONCE
    packet[97:99] = (0).to_bytes(2, "big")

    assert len(packet) == 99
    assert packet[81:97] == bytes(16)
    assert (int.from_bytes(packet[5:7], "big") & 3) == keyver
    return bytes(packet)


def derive_kck(keyver: int, pmk: bytes) -> bytes:
    """Derive the first 16 bytes of the pairwise key expansion PRF output.

    WPA keyvers 1/2 use HMAC-SHA1 with a zero-ended label and counter;
    keyver 3 uses HMAC-SHA256 over the 102-byte 802.11w expansion block.
    Sorting exactly matches the MAC and nonce ordering in module_22000.c.
    """
    macs = min(AP, STATION) + max(AP, STATION)
    nonces = min(ANONCE, SNONCE) + max(ANONCE, SNONCE)
    label = b"Pairwise key expansion"
    assert len(label) == 22 and len(macs) == 12 and len(nonces) == 64

    if keyver in (1, 2):
        pke = label + b"\x00" + macs + nonces + b"\x00"
        assert len(pke) == 100
        return hmac.new(pmk, pke, hashlib.sha1).digest()[:16]

    if keyver == 3:
        pke = b"\x01\x00" + label + macs + nonces + b"\x80\x01"
        assert len(pke) == 102
        return hmac.new(pmk, pke, hashlib.sha256).digest()[:16]

    raise ValueError(f"Unsupported EAPOL key version: {keyver}")


def calculate_mic(keyver: int, key: bytes, eapol: bytes) -> bytes:
    """Compute the EAPOL MIC using the key version's reference primitive."""
    if keyver == 1:
        return hmac.new(key, eapol, hashlib.md5).digest()
    if keyver == 2:
        return hmac.new(key, eapol, hashlib.sha1).digest()[:16]
    if keyver == 3:
        cmac = CMAC.new(key, ciphermod=AES)
        cmac.update(eapol)
        return cmac.digest()
    raise ValueError(f"Unsupported EAPOL key version: {keyver}")


def build_record(keyver: int) -> tuple[str, bytes, bytes]:
    """Produce a WPA*02 line, independently computed KCK and expected MIC."""
    pmk = hashlib.pbkdf2_hmac("sha1", PASSWORD.encode("utf-8"), SSID, 4096, 32)
    kck = derive_kck(keyver, pmk)
    eapol = make_eapol(keyver)
    mic = calculate_mic(keyver, kck, eapol)
    assert len(mic) == 16

    fields = (
        "WPA", "02", mic.hex(), AP.hex(), STATION.hex(), SSID.hex(),
        ANONCE.hex(), eapol.hex(), MESSAGE_PAIR,
    )
    record = "*".join(fields)
    assert len(fields) == 9 and len(fields[6]) == 64
    assert 198 <= len(fields[7]) <= 512 and len(fields[8]) == 2
    assert bytes.fromhex(fields[7])[81:97] == bytes(16)
    return record, kck, mic


def verify_record(record: str, keyver: int) -> None:
    """Reparse a fixture and verify its MIC with independently parsed fields."""
    fields = record.split("*")
    assert len(fields) == 9 and fields[:2] == ["WPA", "02"]
    assert fields[3] == AP.hex() and fields[4] == STATION.hex()
    assert fields[5] == SSID.hex() and fields[6] == ANONCE.hex()
    eapol = bytes.fromhex(fields[7])
    assert len(eapol) == 99 and eapol[81:97] == bytes(16)
    assert (int.from_bytes(eapol[5:7], "big") & 3) == keyver
    pmk = hashlib.pbkdf2_hmac("sha1", PASSWORD.encode(), bytes.fromhex(fields[5]), 4096, 32)
    kck = derive_kck(keyver, pmk)
    assert hmac.compare_digest(calculate_mic(keyver, kck, eapol), bytes.fromhex(fields[2]))


def main() -> None:
    """Write all three verified synthetic EAPOL records to the given folder."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, default=Path("build/diagnostic-multimode/redmagic-eapol-vectors"))
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)

    for keyver, suffix in ((1, "aux1"), (2, "aux2"), (3, "aux3")):
        record, kck, mic = build_record(keyver)
        verify_record(record, keyver)
        path = args.output_dir / f"m22000-eapol-{suffix}.hash"
        path.write_text(record + "\n", encoding="ascii", newline="\n")
        assert path.read_text(encoding="ascii").strip() == record
        print(f"VERIFIED synthetic keyver={keyver} {suffix}, KCK={kck.hex()}, MIC={mic.hex()}, record={path}")


if __name__ == "__main__":
    main()
