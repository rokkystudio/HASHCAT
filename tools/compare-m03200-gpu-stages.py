#!/usr/bin/env python3
"""Compare complete bcrypt 3200 GPU readbacks with the native CPU reference.

The input contains one synthetic candidate's E/P/S-box temporary buffer
after initialization, one expansion round and all 32 rounds. Numeric uint32
words are checked in memory order; the first differing stage, array and word
are reported with their exact hexadecimal values.

A final comparison capture can contain the first four and all six ciphertext
words. The --self-test mode exercises the comparator exclusively using CPU
reference data and deliberately corrupted words; it does not run a GPU.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
REFERENCE = (
    ROOT / "tests" / "fixtures" / "android-gpu"
    / "redmagic-selected-modes-20261030" / "m03200-cpu-stage-reference.json"
)
STAGES = ("init", "loop1", "loop_final")
BUFFERS = {"E": 18, "P": 18, "S0": 256, "S1": 256, "S2": 256, "S3": 256}


def read_json(path: Path) -> dict:
    """Load one existing JSON object without supplying missing trace fields."""
    if not path.is_file():
        raise FileNotFoundError(f"Required bcrypt stage data is absent: {path}")
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise TypeError(f"Expected JSON object in {path}")
    return value


def expanded_key_words(candidate: str, reference: dict) -> list[str]:
    """Derive the exact 72-byte bcrypt key expansion used by m03200_init."""
    password = candidate.encode("ascii")
    assert 1 <= len(password) <= 71
    repeated = password + b"\0"
    expanded = (repeated * ((72 + len(repeated) - 1) // len(repeated)))[:72]
    assert len(expanded) == 72
    expected_hash = reference["vectors"][candidate]["expanded_key_sha256"]
    assert hashlib.sha256(expanded).hexdigest() == expected_hash, (
        "Expanded password bytes differ from the CPU stage reference"
    )
    return [expanded[index:index + 4].hex() for index in range(0, 72, 4)]


def validate_metadata(trace: dict, reference: dict) -> str:
    """Require the exact RedMagic vector, original sources and numeric format."""
    expected = {
        "device": "RedMagic NX709S",
        "adb_serial": "91809037",
        "mode": reference["mode"],
        "cost": reference["cost"],
        "salt_hex": reference["salt_hex"],
        "source_sha256": reference["source_sha256"],
        "kernel_sha256": reference["kernel_sha256"],
        "input_sha256": reference["input_sha256"],
    }
    for key, value in expected.items():
        assert trace.get(key) == value, (
            f"GPU trace {key}: observed={trace.get(key)!r}, expected={value!r}"
        )

    candidate = trace.get("candidate")
    assert candidate in reference["vectors"], f"Unknown bcrypt candidate: {candidate!r}"
    assert trace.get("readback_kind") == "u32_numeric_hex", (
        "Device trace must contain numeric uint32 words formatted as 8-digit lowercase hex"
    )
    return candidate


def compare_stage_arrays(trace: dict, reference: dict, candidate: str) -> None:
    """Check the complete 1060-word GPU buffer at every recorded stage."""
    stages = trace.get("stages")
    assert isinstance(stages, dict) and set(stages) == set(STAGES), (
        "Device trace requires exact init, loop1 and loop_final stages"
    )

    key_words = expanded_key_words(candidate, reference)

    for name in STAGES:
        observed = stages[name]
        assert isinstance(observed, dict) and set(observed) == set(BUFFERS), (
            f"{name}: expected exactly E, P, S0, S1, S2, S3 arrays"
        )
        cpu = reference["vectors"][candidate]["stages"][name]

        for buffer, count in BUFFERS.items():
            words = observed[buffer]
            assert isinstance(words, list) and len(words) == count, (
                f"{name}.{buffer}: expected {count} uint32 words"
            )
            if buffer == "E":
                reference_words = key_words
            elif buffer == "P":
                reference_words = cpu["p_words"]
            else:
                reference_words = cpu["sbox_words"][buffer]

            for index, word in enumerate(words):
                assert isinstance(word, str) and re.fullmatch("[0-9a-f]{8}", word), (
                    f"{name}.{buffer}[{index}]: expected lowercase 8-digit hex"
                )
                assert word == reference_words[index], (
                    f"FIRST_DIVERGENCE {name}.{buffer}[{index}]: "
                    f"GPU={word}, CPU={reference_words[index]}"
                )


def compare_comp_words(trace: dict, reference: dict, candidate: str) -> bool:
    """Check the four comparison words and optional full six-word ciphertext."""
    observed = trace.get("comp_words")
    full_observed = trace.get("comp_words_full")

    if observed is None:
        assert full_observed is None, "comp_words_full requires comp_words"
        return False

    assert isinstance(observed, list) and len(observed) == 4, (
        "comp_words must contain exactly four numeric uint32 words"
    )
    full_reference = reference["vectors"][candidate]["final_ciphertext_hex"]
    cpu = [full_reference[index:index + 8] for index in range(0, 48, 8)]

    for index, word in enumerate(observed):
        assert isinstance(word, str) and re.fullmatch("[0-9a-f]{8}", word), (
            f"comp_words[{index}]: expected lowercase 8-digit hex"
        )
        assert word == cpu[index], (
            f"FIRST_DIVERGENCE comp_words[{index}]: GPU={word}, CPU={cpu[index]}"
        )

    if full_observed is not None:
        assert isinstance(full_observed, list) and len(full_observed) == 6, (
            "comp_words_full must contain exactly six numeric uint32 words"
        )
        assert full_observed[:4] == observed, (
            "comp_words_full must contain the same first four comparison words"
        )
        for index, word in enumerate(full_observed):
            assert isinstance(word, str) and re.fullmatch("[0-9a-f]{8}", word), (
                f"comp_words_full[{index}]: expected lowercase 8-digit hex"
            )
            assert word == cpu[index], (
                f"FIRST_DIVERGENCE comp_words_full[{index}]: GPU={word}, CPU={cpu[index]}"
            )

    return True


def self_test(reference: dict) -> None:
    """Test success and first-error reporting on CPU-only synthetic readbacks."""
    for candidate in ("hashcat", "hashcat2"):
        source = reference["vectors"][candidate]
        key_words = expanded_key_words(candidate, reference)
        values = {
            name: {
                buffer: (
                    list(key_words) if buffer == "E" else
                    list(source["stages"][name]["p_words"]) if buffer == "P" else
                    list(source["stages"][name]["sbox_words"][buffer])
                )
                for buffer in BUFFERS
            }
            for name in STAGES
        }
        final_words = [
            source["final_ciphertext_hex"][index:index + 8]
            for index in range(0, 48, 8)
        ]
        sample = {
            "stages": values,
            "comp_words": final_words[:4],
            "comp_words_full": final_words,
        }
        compare_stage_arrays(sample, reference, candidate)
        assert compare_comp_words(sample, reference, candidate)

        for stage, buffer, index in (
            ("init", "E", 0),
            ("init", "P", 0),
            ("loop1", "S2", 127),
            ("loop_final", "S3", 255),
        ):
            mutated = copy.deepcopy(sample)
            previous = mutated["stages"][stage][buffer][index]
            mutated["stages"][stage][buffer][index] = (
                "00000000" if previous != "00000000" else "ffffffff"
            )
            try:
                compare_stage_arrays(mutated, reference, candidate)
            except AssertionError as error:
                assert str(error).startswith(
                    f"FIRST_DIVERGENCE {stage}.{buffer}[{index}]"
                ), str(error)
            else:
                raise AssertionError(
                    f"Undetected synthetic difference: {stage}.{buffer}[{index}]"
                )

        for field, index in (("comp_words", 0), ("comp_words_full", 5)):
            mutated = copy.deepcopy(sample)
            previous = mutated[field][index]
            mutated[field][index] = "00000000" if previous != "00000000" else "ffffffff"
            try:
                compare_comp_words(mutated, reference, candidate)
            except AssertionError as error:
                assert str(error).startswith(
                    f"FIRST_DIVERGENCE {field}[{index}]"
                ), str(error)
            else:
                raise AssertionError(
                    f"Undetected synthetic difference: {field}[{index}]"
                )

    print("PASS CPU-only bcrypt comparator: two candidates and twelve synthetic divergences")
    print("NO GPU DATA WAS CAPTURED OR VERIFIED")


def main() -> None:
    """Compare one captured device trace or exercise CPU-only comparator checks."""
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--trace", type=Path, help="JSON with captured GPU uint32 arrays")
    group.add_argument(
        "--self-test", action="store_true", help="Test comparison without device data",
    )
    args = parser.parse_args()

    reference = read_json(REFERENCE)
    assert reference["mode"] == 3200

    if args.self_test:
        self_test(reference)
        return

    trace = read_json(args.trace)
    candidate = validate_metadata(trace, reference)
    compare_stage_arrays(trace, reference, candidate)
    print(f"PASS GPU bcrypt {candidate}: all init/loop1/loop_final words match CPU")

    if compare_comp_words(trace, reference, candidate):
        print("PASS GPU bcrypt final comparison ciphertext matches CPU reference")
    else:
        print("GPU COMP_R NOT CAPTURED: final kernel comparison remains unverified")


if __name__ == "__main__":
    main()
