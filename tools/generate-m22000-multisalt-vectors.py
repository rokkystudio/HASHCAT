#!/usr/bin/env python3
"""Generate four synthetic WPA mode-22000 records with distinct SSIDs.

The reference cryptography is performed on the CPU using hashlib, hmac
and PyCryptodome AES-CMAC. The output contains invented credentials, frames,
MACs and nonces only. No captured wireless data is read.
"""

from __future__ import annotations

import argparse
import hashlib
import hmac
from pathlib import Path

from Crypto.Cipher import AES
from Crypto.Hash import CMAC


PASSWORD = b"hashcat!"
SAMPLES = (
    (0, b"lab-pmkid-01", "021122334450", "02aabbccdd60"),
    (1, b"lab-eapol-md5", "021122334451", "02aabbccdd61"),
    (2, b"lab-eapol-sha1", "021122334452", "02aabbccdd62"),
    (3, b"lab-eapol-cmac", "021122334453", "02aabbccdd63"),
)


def derive_kck(keyver: int, pmk: bytes, ap: bytes, station: bytes, anonce: bytes, snonce: bytes) -> bytes:
    """Derive 16 bytes of key confirmation material for the specified WPA key version."""
    ordered = min(ap, station) + max(ap, station) + min(anonce, snonce) + max(anonce, snonce)
    label = b"Pairwise key expansion"
    if keyver in (1, 2):
        return hmac.new(pmk, label + b"\x00" + ordered + b"\x00", hashlib.sha1).digest()[:16]
    if keyver == 3:
        return hmac.new(pmk, b"\x01\x00" + label + ordered + b"\x80\x01", hashlib.sha256).digest()[:16]
    raise ValueError(f"Invalid EAPOL key version: {keyver}")


def reference_mic(keyver: int, key: bytes, frame: bytes) -> bytes:
    """Calculate the 16-byte EAPOL MIC with HMAC-MD5, SHA1 or AES-CMAC."""
    if keyver == 1:
        return hmac.new(key, frame, hashlib.md5).digest()
    if keyver == 2:
        return hmac.new(key, frame, hashlib.sha1).digest()[:16]
    if keyver == 3:
        cmac = CMAC.new(key, ciphermod=AES)
        cmac.update(frame)
        return cmac.digest()
    raise ValueError(f"Invalid EAPOL key version: {keyver}")


def make_eapol(keyver: int, snonce: bytes) -> bytes:
    """Build a deterministic 99-byte EAPOL-Key frame with an empty MIC field."""
    frame = bytearray(99)
    frame[0:4] = bytes.fromhex("0203005f")
    frame[4] = 254 if keyver == 1 else 2
    frame[5:7] = (0x100 | keyver).to_bytes(2, "big")
    frame[7:9] = (16).to_bytes(2, "big")
    frame[9:17] = (1).to_bytes(8, "big")
    frame[17:49] = snonce
    assert frame[81:97] == bytes(16) and len(frame) == 99
    return bytes(frame)


def make_record(keyver: int, ssid: bytes, ap: bytes, station: bytes) -> str:
    """Build a PMKID or EAPOL record from independently computed reference values."""
    pmk = hashlib.pbkdf2_hmac("sha1", PASSWORD, ssid, 4096, 32)
    if keyver == 0:
        pmkid = hmac.new(pmk, b"PMK Name" + ap + station, hashlib.sha1).digest()[:16]
        return f"WPA*01*{pmkid.hex()}*{ap.hex()}*{station.hex()}*{ssid.hex()}***"
    anonce = bytes(range(0x10 + keyver, 0x30 + keyver))
    snonce = bytes(range(0x40 + keyver, 0x60 + keyver))
    frame = make_eapol(keyver, snonce)
    key = derive_kck(keyver, pmk, ap, station, anonce, snonce)
    mic = reference_mic(keyver, key, frame)
    return "*".join(("WPA", "02", mic.hex(), ap.hex(), station.hex(), ssid.hex(), anonce.hex(), frame.hex(), "00"))


def verify_record(record: str, keyver: int, password: bytes = PASSWORD) -> None:
    """Reparse a generated record and verify its MIC or PMKID against CPU reference crypto."""
    fields = record.split("*")
    assert fields[0] == "WPA" and len(fields[2]) == 32
    ap = bytes.fromhex(fields[3])
    station = bytes.fromhex(fields[4])
    ssid = bytes.fromhex(fields[5])
    pmk = hashlib.pbkdf2_hmac("sha1", password, ssid, 4096, 32)
    if keyver == 0:
        assert len(fields) == 9 and fields[1] == "01"
        digest = hmac.new(pmk, b"PMK Name" + ap + station, hashlib.sha1).digest()[:16]
    else:
        assert len(fields) == 9 and fields[1] == "02"
        anonce = bytes.fromhex(fields[6])
        frame = bytes.fromhex(fields[7])
        assert len(anonce) == 32 and len(frame) == 99 and frame[81:97] == bytes(16)
        assert (int.from_bytes(frame[5:7], "big") & 3) == keyver
        snonce = frame[17:49]
        key = derive_kck(keyver, pmk, ap, station, anonce, snonce)
        digest = reference_mic(keyver, key, frame)
    assert hmac.compare_digest(digest, bytes.fromhex(fields[2])), f"Reference mismatch: keyver={keyver}"


def build_records() -> list[str]:
    """Return four validated WPA records with unique SSIDs and unique MAC pairs."""
    records: list[str] = []
    for keyver, ssid, ap_hex, station_hex in SAMPLES:
        record = make_record(keyver, ssid, bytes.fromhex(ap_hex), bytes.fromhex(station_hex))
        verify_record(record, keyver)
        records.append(record)
    assert len({record.split("*")[5] for record in records}) == 4
    return records


def main() -> None:
    """Write the deterministic multi-salt fixture and report all reference MICs."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, default=Path("build/diagnostic-multimode/redmagic-multisalt-wpa/multisalt.hash"))
    args = parser.parse_args()
    records = build_records()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(records) + "\n", encoding="ascii", newline="\n")
    assert args.output.read_text(encoding="ascii").splitlines() == records
    for i, record in enumerate(records):
        print(f"REFERENCE[{i}] keyver={SAMPLES[i][0]} ssid={bytes.fromhex(record.split('*')[5])!r} mic={record.split('*')[2]}")
    print(f"SYNTHETIC_MULTISALT_RECORDS: {len(records)} -> {args.output}")


if __name__ == "__main__":
    main()
