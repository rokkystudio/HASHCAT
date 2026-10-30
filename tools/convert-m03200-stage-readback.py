#!/usr/bin/env python3
"""Convert native little-endian bcrypt 3200 probe snapshots to comparison JSON.

The input directory contains probe.log, probe.exit, and binary snapshots of
one synthetic candidate at the init, loop1 and loop_final stage boundaries,
followed by six final ciphertext words. Each complete intermediate snapshot
contains one 4240-byte bcrypt_tmp_t structure.

This tool reads existing files only; it never connects to Android or executes
hashcat. The --self-test mode generates CPU-only synthetic binaries in a
temporary directory to verify byte ordering, record structure and rejection
of incorrect input. The tool never labels synthetic data as a GPU capture.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import runpy
import struct
import tempfile


ROOT = Path(__file__).resolve().parents[1]
REFERENCE = (
    ROOT / "tests" / "fixtures" / "android-gpu"
    / "redmagic-selected-modes-20261030" / "m03200-cpu-stage-reference.json"
)
COMPARATOR = ROOT / "tools" / "compare-m03200-gpu-stages.py"
PROBE_SOURCE = ROOT / "tools" / "m03200-stage-probe.cl"
STAGES = ("init", "loop1", "loop_final")
BUFFERS = {"E": 18, "P": 18, "S0": 256, "S1": 256, "S2": 256, "S3": 256}
STAGE_WORDS = sum(BUFFERS.values())
STAGE_BYTES = STAGE_WORDS * 4
COMP_BYTES = 6 * 4


def load_reference() -> dict:
    """Read the complete CPU reference and verify the current probe source."""
    reference = json.loads(REFERENCE.read_text(encoding="utf-8"))
    assert reference["mode"] == 3200 and reference["cost"] == 5
    assert hashlib.sha256(PROBE_SOURCE.read_bytes()).hexdigest() == (
        "4c4fe7a6085f3f82b6579ec6e3c28d9aa25a31255583f0cf887ebce433be3e01"
    )
    return reference


def decode_words(data: bytes, word_count: int) -> list[str]:
    """Convert exactly word_count little-endian uint32 values to numeric hex."""
    assert len(data) == word_count * 4, (
        f"READBACK_LENGTH_MISMATCH: got={len(data)} expected={word_count * 4}"
    )
    numeric = struct.unpack("<" + "I" * word_count, data)
    return [f"{value:08x}" for value in numeric]


def encode_words(values: list[str]) -> bytes:
    """Encode hexadecimal numeric words as little-endian test-only readback."""
    numeric = [int(value, 16) for value in values]
    return struct.pack("<" + "I" * len(numeric), *numeric)


def verify_run_log(directory: Path, candidate: str) -> None:
    """Require completed OpenCL execution markers and exit zero before parsing."""
    status = (directory / "probe.exit").read_text(encoding="ascii").strip()
    assert status == "0", f"Probe exit must be zero; observed={status!r}"
    lines = (directory / "probe.log").read_text(encoding="utf-8", errors="replace")
    assert "PROBE_BUILD_SUCCESS" in lines, "OpenCL kernel build not confirmed"
    assert f"PROBE_COMPLETE candidate={candidate}" in lines, (
        "Probe did not complete the requested known candidate"
    )
    for stage in STAGES:
        assert f"STAGE_SAVED {stage} {STAGE_BYTES}" in lines, (
            f"Missing complete {stage} readback marker"
        )
    assert f"STAGE_SAVED comp {COMP_BYTES}" in lines, (
        "Missing six-word final ciphertext readback marker"
    )


def convert_capture(directory: Path, candidate: str, reference: dict) -> dict:
    """Parse exactly three device structs and one six-word ciphertext buffer."""
    verify_run_log(directory, candidate)
    result = {
        "device": "RedMagic NX709S",
        "adb_serial": "91809037",
        "mode": reference["mode"],
        "cost": reference["cost"],
        "candidate": candidate,
        "salt_hex": reference["salt_hex"],
        "source_sha256": reference["source_sha256"],
        "kernel_sha256": reference["kernel_sha256"],
        "input_sha256": reference["input_sha256"],
        "probe_source_sha256": hashlib.sha256(PROBE_SOURCE.read_bytes()).hexdigest(),
        "readback_kind": "u32_numeric_hex",
        "stages": {},
    }

    for stage in STAGES:
        binary = (directory / f"m03200-{stage}.bin").read_bytes()
        words = decode_words(binary, STAGE_WORDS)
        offset = 0
        arrays = {}

        for buffer, length in BUFFERS.items():
            arrays[buffer] = words[offset:offset + length]
            offset += length

        assert offset == STAGE_WORDS
        result["stages"][stage] = arrays

    comp = decode_words((directory / "m03200-comp.bin").read_bytes(), 6)
    result["comp_words"] = comp[:4]
    result["comp_words_full"] = comp

    return result


def compare_capture(result: dict, reference: dict, comparator: dict, origin: str) -> None:
    """Compare numeric device-style words and label their actual input origin."""
    candidate = comparator["validate_metadata"](result, reference)
    comparator["compare_stage_arrays"](result, reference, candidate)
    assert comparator["compare_comp_words"](result, reference, candidate)
    print(
        f"PASS {origin} bcrypt readback: {candidate} "
        "init/loop1/loop_final and six ciphertext words"
    )


def self_test(reference: dict, comparator: dict) -> None:
    """Exercise exact binary offsets, little-endian conversion and error checks."""
    for candidate in ("hashcat", "hashcat2"):
        cpu = reference["vectors"][candidate]
        key_words = comparator["expanded_key_words"](candidate, reference)

        with tempfile.TemporaryDirectory(prefix="m03200-readback-cpu-only-") as path:
            directory = Path(path)
            lines = [
                "PROBE_BUILD_BEGIN",
                "PROBE_BUILD_SUCCESS",
            ]

            for stage in STAGES:
                source = cpu["stages"][stage]
                values = {
                    "E": key_words,
                    "P": source["p_words"],
                    "S0": source["sbox_words"]["S0"],
                    "S1": source["sbox_words"]["S1"],
                    "S2": source["sbox_words"]["S2"],
                    "S3": source["sbox_words"]["S3"],
                }
                words = [word for name in BUFFERS for word in values[name]]
                assert len(words) == STAGE_WORDS
                (directory / f"m03200-{stage}.bin").write_bytes(encode_words(words))
                lines.append(f"STAGE_SAVED {stage} {STAGE_BYTES}")

            ciphertext = cpu["final_ciphertext_hex"]
            comp_words = [ciphertext[index:index + 8] for index in range(0, 48, 8)]
            (directory / "m03200-comp.bin").write_bytes(encode_words(comp_words))
            lines.append(f"STAGE_SAVED comp {COMP_BYTES}")
            lines.append(f"PROBE_COMPLETE candidate={candidate}")
            (directory / "probe.log").write_text("\n".join(lines) + "\n", encoding="utf-8")
            (directory / "probe.exit").write_text("0\n", encoding="ascii")

            result = convert_capture(directory, candidate, reference)
            compare_capture(result, reference, comparator, "CPU-only synthetic")

            assert result["stages"]["init"]["E"] == key_words
            assert result["stages"]["loop_final"]["S3"] == (
                cpu["stages"]["loop_final"]["sbox_words"]["S3"]
            )

            source = directory / "m03200-init.bin"
            complete = source.read_bytes()
            source.write_bytes(complete[:-1])
            try:
                convert_capture(directory, candidate, reference)
            except AssertionError as error:
                assert str(error).startswith("READBACK_LENGTH_MISMATCH"), str(error)
            else:
                raise AssertionError("Truncated readback was accepted")

            source.write_bytes(complete)
            (directory / "probe.exit").write_text("2\n", encoding="ascii")
            try:
                convert_capture(directory, candidate, reference)
            except AssertionError as error:
                assert "Probe exit must be zero" in str(error), str(error)
            else:
                raise AssertionError("Failed OpenCL probe was accepted")

    print("PASS CPU-only readback converter: exact offsets, endian and failure rejection")
    print("NO DEVICE READBACK CREATED")


def main() -> None:
    """Convert existing GPU bytes or run CPU-only converter verification."""
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument(
        "--binary-dir", type=Path,
        help="Existing device output directory containing log, exit and four binary files",
    )
    group.add_argument(
        "--self-test", action="store_true", help="Verify parsing using CPU-only binaries",
    )
    parser.add_argument("--candidate", choices=("hashcat", "hashcat2"))
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()

    reference = load_reference()
    comparator = runpy.run_path(str(COMPARATOR))

    if args.self_test:
        assert args.candidate is None and args.output is None, (
            "CPU self-test does not accept a candidate or persistent output"
        )
        self_test(reference, comparator)
        return

    assert args.candidate is not None, "The GPU candidate must be explicit"
    assert args.output is not None, "Output JSON path must be explicit"
    assert not args.output.exists(), f"Refusing to overwrite existing trace: {args.output}"

    result = convert_capture(args.binary_dir, args.candidate, reference)
    args.output.write_text(
        json.dumps(result, indent=2) + "\n", encoding="utf-8", newline="\n"
    )
    print(f"READBACK_JSON_SAVED {args.output}")

    compare_capture(result, reference, comparator, "provided device files")


if __name__ == "__main__":
    main()
