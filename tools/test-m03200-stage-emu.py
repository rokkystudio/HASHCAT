#!/usr/bin/env python3
"""Validate bcrypt 3200 CPU stage states against the public hashcat self-test.

The test compiles the current unmodified Blowfish helper functions from
source/OpenCL/inc_cipher_blowfish.cl with a minimal native CPU harness. It
performs the same initial expansion, 2**cost key-schedule rounds, and final
ciphertext operations as m03200_init, m03200_loop, and m03200_comp.

The computed bcrypt digest is compared against the independently verified
public test hash. The stage record contains exact intermediate P-array words,
selected S-box words, and SHA-256 digests of the complete intermediate state.
No GPU backend or hashcat attack is executed.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "source" / "OpenCL" / "inc_cipher_blowfish.cl"
KERNEL = ROOT / "source" / "OpenCL" / "m03200-pure.cl"
TEMPLATE = ROOT / "tools" / "m03200-stage-emu.c.in"
VECTOR = ROOT / "tests" / "fixtures" / "android-gpu" / "redmagic-selected-modes-20261030" / "m03200.hash"
REFERENCE = VECTOR.parent / "m03200-cpu-stage-reference.json"
STAGES = ("INIT", "LOOP1", "LOOP_FINAL")
BUFFERS = {"P": 18, "S0": 256, "S1": 256, "S2": 256, "S3": 256}
BCRYPT_ALPHABET = "./ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789"
STANDARD_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"


def decode_bcrypt64(value: str, count: int) -> bytes:
    """Decode bcrypt's RFC-4648 bit ordering with its 64-character alphabet."""
    assert all(char in BCRYPT_ALPHABET for char in value), "Invalid bcrypt alphabet"
    translated = value.translate(str.maketrans(BCRYPT_ALPHABET, STANDARD_ALPHABET))
    decoded = base64.b64decode(translated + "=" * ((-len(translated)) % 4), validate=True)
    assert len(decoded) == count, (value, len(decoded), count)
    return decoded


def read_vector() -> tuple[int, bytes, bytes]:
    """Extract cost, salt bytes, and first 23 digest bytes from the public vector."""
    record = VECTOR.read_text(encoding="ascii").strip()
    assert record == "$2a$05$MBCzKhG1KhezLh.0LRa0Kuw12nLJtpHy6DIaU.JAnqJUDYspHC.Ou"
    match = re.fullmatch(r"\$2a\$(\d\d)\$([./A-Za-z0-9]{22})([./A-Za-z0-9]{31})", record)
    assert match is not None, "Invalid bcrypt record structure"
    cost = int(match.group(1))
    assert cost == 5
    return cost, decode_bcrypt64(match.group(2), 16), decode_bcrypt64(match.group(3), 23)


def generate_native_source() -> str:
    """Insert the exact Blowfish helper bodies into the diagnostic CPU template."""
    source = SOURCE.read_text(encoding="utf-8")
    expected_includes = (
        '#include "inc_vendor.h"',
        '#include "inc_types.h"',
        '#include "inc_platform.h"',
        '#include "inc_common.h"',
        '#include "inc_cipher_blowfish.h"',
    )
    for directive in expected_includes:
        assert source.splitlines().count(directive) == 1, directive
    result = "\n".join(
        line for line in source.splitlines() if line not in expected_includes
    )
    assert result.count("DECLSPEC void blowfish_set_key_salt") == 1
    assert result.count("DECLSPEC void blowfish_encrypt") == 1
    assert result.count("DECLSPEC void expand_key") == 1
    assert "BCRYPT_AVOID_BANK_CONFLICTS" in result
    template = TEMPLATE.read_text(encoding="utf-8")
    assert template.count("@@BLOWFISH_SOURCE@@") == 1
    return template.replace("@@BLOWFISH_SOURCE@@", result)


def execute(argv: list[str], timeout: int = 90) -> str:
    """Execute a host compiler or CPU harness and surface its complete failure."""
    process = subprocess.run(
        argv, cwd=ROOT, capture_output=True, text=True, timeout=timeout,
    )
    if process.returncode:
        raise RuntimeError(
            f"Command exit {process.returncode}: {argv}\n"
            f"stderr:\n{process.stderr[-8000:]}\nstdout:\n{process.stdout[-2000:]}"
        )
    return process.stdout


def parse_stages(output: str) -> dict:
    """Parse the expected native output and summarize full stage memory images."""
    lines = [line.strip() for line in output.splitlines()]
    expected = {"EXPANDED_KEY", "COMP_R"}
    expected.update(f"{stage}_{buffer}" for stage in STAGES for buffer in BUFFERS)
    records = {}

    for line in lines:
        key, separator, value = line.partition("=")
        assert separator == "=" and key in expected and key not in records, key
        assert re.fullmatch("[0-9a-f]+", value), key
        records[key] = value

    assert set(records) == expected, (sorted(set(records) - expected), sorted(expected - set(records)))
    assert len(records["EXPANDED_KEY"]) == 18 * 8
    assert len(records["COMP_R"]) == 6 * 8

    result = {
        "expanded_key_sha256": hashlib.sha256(bytes.fromhex(records["EXPANDED_KEY"])).hexdigest(),
        "stages": {},
        "final_ciphertext_hex": records["COMP_R"],
    }

    for stage in STAGES:
        values = {}
        for name, length in BUFFERS.items():
            raw = records[f"{stage}_{name}"]
            assert len(raw) == length * 8, (stage, name, len(raw))
            values[name] = bytes.fromhex(raw)

        combined = b"".join(values[name] for name in BUFFERS)
        result["stages"][stage.lower()] = {
            "state_sha256": hashlib.sha256(combined).hexdigest(),
            "p_words": [records[f"{stage}_P"][i:i + 8] for i in range(0, 18 * 8, 8)],
            "sbox_words": {
                name: [records[f"{stage}_{name}"][i:i + 8] for i in range(0, 256 * 8, 8)]
                for name in ("S0", "S1", "S2", "S3")
            },
            "sbox_sha256": {
                name: hashlib.sha256(values[name]).hexdigest()
                for name in ("S0", "S1", "S2", "S3")
            },
            "sbox_checkpoints": {
                name: [
                    records[f"{stage}_{name}"][index * 8:index * 8 + 8]
                    for index in (0, 1, 127, 255)
                ]
                for name in ("S0", "S1", "S2", "S3")
            },
        }

    assert len({result["stages"][s]["state_sha256"] for s in ("init", "loop1", "loop_final")}) == 3
    return result


def main() -> None:
    """Recompute positive and negative stage references and verify their digests."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc", help="Native C compiler executable")
    parser.add_argument(
        "--write-reference", action="store_true",
        help="Create the deterministic CPU reference record after verification",
    )
    args = parser.parse_args()

    cost, salt, expected = read_vector()
    code = generate_native_source()

    with tempfile.TemporaryDirectory(prefix="bcrypt-stage-cpu-") as path:
        directory = Path(path)
        cfile = directory / "bcrypt-stage.c"
        cfile.write_text(code, encoding="utf-8", newline="\n")

        candidates = {}
        for optimization in ("-O0", "-O2"):
            binary = directory / (
                f"bcrypt-stage-{optimization[1:]}" + (".exe" if os.name == "nt" else "")
            )
            execute([
                args.cc, "-std=c11", optimization, "-Wall", "-Wextra",
                str(cfile), "-o", str(binary),
            ], timeout=60)

            for candidate, should_match in (("hashcat", True), ("hashcat2", False)):
                stages = parse_stages(
                    execute([str(binary), candidate, salt.hex(), str(cost)])
                )
                computed = bytes.fromhex(stages["final_ciphertext_hex"])[:23]
                assert (computed == expected) == should_match, (
                    candidate, optimization, computed.hex(), expected.hex(),
                )
                if optimization == "-O0":
                    candidates[candidate] = stages
                else:
                    assert candidates[candidate] == stages, (
                        "CPU stage difference between host optimizations", candidate,
                    )
                print(
                    f"PASS CPU bcrypt {optimization} stages: {candidate}, "
                    f"23-byte digest {'matched' if should_match else 'rejected'}"
                )

    assert candidates["hashcat"]["stages"]["loop_final"]["state_sha256"] != (
        candidates["hashcat2"]["stages"]["loop_final"]["state_sha256"]
    )
    result = {
        "mode": 3200,
        "cost": cost,
        "source_sha256": hashlib.sha256(SOURCE.read_bytes()).hexdigest(),
        "kernel_sha256": hashlib.sha256(KERNEL.read_bytes()).hexdigest(),
        "input_sha256": hashlib.sha256(VECTOR.read_bytes()).hexdigest(),
        "salt_hex": salt.hex(),
        "expected_digest_hex": expected.hex(),
        "vectors": candidates,
    }

    if args.write_reference:
        assert not REFERENCE.exists(), f"Reference already exists: {REFERENCE}"
        REFERENCE.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n")
        print(f"REFERENCE_WRITTEN {REFERENCE.relative_to(ROOT)}")
    else:
        assert REFERENCE.is_file(), f"Missing CPU stage reference: {REFERENCE}"
        archived = json.loads(REFERENCE.read_text(encoding="utf-8"))
        assert archived == result, "CPU stage reference differs from the current OpenCL source"
        print("PASS CPU bcrypt init, loop1, loop-final reference matches archived data")

    print("BCRYPT 3200 CPU STAGE REFERENCE VERIFIED; GPU INTERMEDIATE STATES NOT YET CAPTURED")


if __name__ == "__main__":
    main()
