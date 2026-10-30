#!/usr/bin/env python3
"""Compare captured bcrypt 3200 GPU stage words with the native CPU reference.

The input is a device readback of one candidate's m03200 intermediate buffer
after init, the first loop iteration, and all cost rounds. Every 32-bit word
is compared exactly in the same numeric representation as the OpenCL kernel.
Optional final four comparison words are also checked.

This tool does not execute a GPU kernel. The --self-test option tests only
the comparator with CPU-derived input and deliberate synthetic mismatches.
"""

from __future__ import annotations

import argparse
import copy
import json
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
REFERENCE = (
    ROOT / "tests" / "fixtures" / "android-gpu"
    / "redmagic-selected-modes-20261030" / "m03200-cpu-stage-reference.json"
)
STAGES = ("init", "loop1", "loop_final")
BUFFERS = {"P": 18, "S0": 256, "S1": 256, "S2": 256, "S3": 256}


def read_json(path: Path) -> dict:
    """Load a required JSON object without substituting missing trace data."""
    if not path.is_file():
        raise FileNotFoundError(f"Required bcrypt stage data is absent: {path}")
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise TypeError(f"Expected JSON object in {path}")
    return value


def validate_metadata(trace: dict, reference: dict) -> str:
    """Require source identity, known candidate, device model and test parameters."""
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


def compare_stage_arrays(trace: dict, expected: dict, candidate: str) -> None:
    """Identify the earliest differing GPU stage, buffer and 32-bit word index."""
    stages = trace.get("stages")
    assert isinstance(stages, dict) and set(stages) == set(STAGES), (
        "Device trace requires exact init, loop1 and loop_final stages"
    )

    for name in STAGES:
        observed = stages[name]
        assert isinstance(observed, dict) and set(observed) == set(BUFFERS), (
            f"{name}: expected exactly P, S0, S1, S2, S3 arrays"
        )
        cpu = expected["vectors"][candidate]["stages"][name]
        for buffer, count in BUFFERS.items():
            words = observed[buffer]
            assert isinstance(words, list) and len(words) == count, (
                f"{name}.{buffer}: expected {count} uint32 words"
            )
            reference_words = (
                cpu["p_words"] if buffer == "P" else cpu["sbox_words"][buffer]
            )
            for index, word in enumerate(words):
                assert isinstance(word, str) and re.fullmatch("[0-9a-f]{8}", word), (
                    f"{name}.{buffer}[{index}]: expected lowercase 8-digit hex"
                )
                assert word == reference_words[index], (
                    f"FIRST_DIVERGENCE {name}.{buffer}[{index}]: "
                    f"GPU={word}, CPU={reference_words[index]}"
                )


def compare_comp_words(trace: dict, expected: dict, candidate: str) -> bool:
    """Compare the four 32-bit bcrypt result words if a device dump contains them."""
    observed = trace.get("comp_words")
    if observed is None:
        return False

    assert isinstance(observed, list) and len(observed) == 4, (
        "comp_words must contain exactly the four words from m03200_comp"
    )
    full = expected["vectors"][candidate]["final_ciphertext_hex"]
    cpu = [full[i:i + 8] for i in range(0, 32, 8)]

    for index, word in enumerate(observed):
        assert isinstance(word, str) and re.fullmatch("[0-9a-f]{8}", word), (
            f"comp_words[{index}]: expected lowercase 8-digit hex"
        )
        assert word == cpu[index], (
            f"FIRST_DIVERGENCE comp_words[{index}]: GPU={word}, CPU={cpu[index]}"
        )
    return True


def self_test(reference: dict) -> None:
    """Exercise exact stage comparison and first-divergence reporting using CPU data."""
    source = reference["vectors"]["hashcat"]
    values = {
        name: {
            buffer: (
                list(source["stages"][name]["p_words"]) if buffer == "P"
                else list(source["stages"][name]["sbox_words"][buffer])
            )
            for buffer in BUFFERS
        }
        for name in STAGES
    }
    sample = {"stages": values, "comp_words": [
        source["final_ciphertext_hex"][i:i + 8] for i in range(0, 32, 8)
    ]}
    compare_stage_arrays(sample, reference, "hashcat")
    assert compare_comp_words(sample, reference, "hashcat")

    for stage, buffer, index in (
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
            compare_stage_arrays(mutated, reference, "hashcat")
        except AssertionError as error:
            assert str(error).startswith(
                f"FIRST_DIVERGENCE {stage}.{buffer}[{index}]"
            ), str(error)
        else:
            raise AssertionError(f"Undetected synthetic difference: {stage}.{buffer}[{index}]")

    mutated = copy.deepcopy(sample)
    mutated["comp_words"][0] = "00000000"
    try:
        compare_comp_words(mutated, reference, "hashcat")
    except AssertionError as error:
        assert str(error).startswith("FIRST_DIVERGENCE comp_words[0]"), str(error)
    else:
        raise AssertionError("Undetected synthetic comparison word difference")

    print("PASS CPU-only bcrypt checkpoint comparator and four synthetic divergences")
    print("NO GPU DATA WAS CAPTURED OR VERIFIED")


def main() -> None:
    """Compare a real device trace or exercise only the CPU comparator self-test."""
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--trace", type=Path, help="JSON containing captured GPU uint32 arrays")
    group.add_argument(
        "--self-test", action="store_true", help="Check comparator logic without GPU data",
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
    print(f"PASS GPU bcrypt {candidate}: init/loop1/loop_final words match CPU reference")

    if compare_comp_words(trace, reference, candidate):
        print("PASS GPU bcrypt final four comparison words match CPU reference")
    else:
        print("GPU COMP_R NOT CAPTURED: final kernel comparison remains unverified")


if __name__ == "__main__":
    main()
