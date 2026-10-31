#!/usr/bin/env python3
"""Check exact synthetic Vulkan command-buffer replay results on a scalar CPU.

The reference calculates the public 32-bit inputs and three independent
workgroup stages for four sequential submissions. It checks distinct
per-submission intermediate and final FNV-1a checksums. No GPU execution,
Android file access, or cryptographic workload is performed.
"""

from __future__ import annotations

import struct


ELEMENTS = 2048
LANES = 32
MASK32 = 0xFFFFFFFF
MASK64 = 0xFFFFFFFFFFFFFFFF
FNV_OFFSET = 14695981039346656037
FNV_PRIME = 1099511628211

EXPECTED = (
    {
        "stage_a": "de56d6d0aa66c6ec",
        "stage_b": "c1259ad4f024499a",
        "output": "05de70c62f8d7cfd",
    },
    {
        "stage_a": "66ee1b4b1385e0e3",
        "stage_b": "27fb4f34e64d89a7",
        "output": "7509bfa6ed0455d8",
    },
    {
        "stage_a": "69b8f65ae2e8be72",
        "stage_b": "d8038be5522a0b58",
        "output": "e50a539d4459e45b",
    },
    {
        "stage_a": "8fb63465a93a5631",
        "stage_b": "ac37a5c3b35fc366",
        "output": "07d63cfd9773324d",
    },
)


def rotate(word: int, bits: int) -> int:
    """Return the 32-bit rotation used by the three original GLSL stages."""
    assert 0 < bits < 32

    return ((word << bits) | (word >> (32 - bits))) & MASK32


def input_word(index: int, submission: int) -> int:
    """Compute the exact changing input stored before a Vulkan queue submission."""
    original = (
        (index * 0x9E3779B9)
        ^ (index << 15)
        ^ (index >> 3)
        ^ 0xA5A5A5A5
    ) & MASK32

    return original ^ ((submission * 0x9E3779B9) & MASK32)


def stage_a_words(submission: int) -> list[int]:
    """Calculate all first-stage values from submission-specific public words."""
    result = []

    for index in range(ELEMENTS):
        value = input_word(index, submission)
        affine = (value * 1664525 + 1013904223) & MASK32
        result.append(
            ((affine ^ rotate(value, 7)) + index * 2654435761) & MASK32
        )

    return result


def stage_b_words(stage_a: list[int]) -> list[int]:
    """Read the seven-lanes-forward A buffer and calculate stage B."""
    assert len(stage_a) == ELEMENTS
    result = []

    for index, current in enumerate(stage_a):
        lane = index % LANES
        group_start = index - lane
        neighbor = stage_a[group_start + ((lane + 7) % LANES)]

        result.append(
            ((current + neighbor) ^ rotate(current, 11) ^ 0x6C8E9CF5)
            & MASK32
        )

    return result


def stage_c_words(stage_a: list[int], stage_b: list[int]) -> list[int]:
    """Produce all four stage-C values per item from both earlier stage buffers."""
    assert len(stage_a) == len(stage_b) == ELEMENTS
    result = []

    for a, b in zip(stage_a, stage_b):
        result.extend((
            a,
            b,
            a ^ b,
            ((a * 2246822519 + b * 3266489917) ^ rotate(b, 3))
            & MASK32,
        ))

    return result


def checksum(words: list[int]) -> str:
    """Calculate FNV-1a over little-endian 32-bit GPU-visible words."""
    result = FNV_OFFSET

    for word in words:
        assert 0 <= word <= MASK32

        for byte in struct.pack("<I", word):
            result = ((result ^ byte) * FNV_PRIME) & MASK64

    return f"{result:016x}"


def submission_checksums(submission: int) -> dict[str, str]:
    """Return three CPU checksums for one fresh set of synthetic input values."""
    assert 0 <= submission < len(EXPECTED)

    a = stage_a_words(submission)
    b = stage_b_words(a)
    c = stage_c_words(a, b)

    assert len(a) == ELEMENTS
    assert len(b) == ELEMENTS
    assert len(c) == ELEMENTS * 4

    return {
        "stage_a": checksum(a),
        "stage_b": checksum(b),
        "output": checksum(c),
    }


def main() -> None:
    """Validate four distinct submissions and their exact recorded GPU digests."""
    observations = tuple(
        submission_checksums(submission)
        for submission in range(len(EXPECTED))
    )

    assert observations == EXPECTED, (
        f"GPU replay and independent CPU reference disagree: {observations}"
    )
    assert len({entry["output"] for entry in observations}) == len(EXPECTED)

    for submission, values in enumerate(observations):
        print(
            f"PASS CPU replay submission={submission}: "
            f"stageA={values['stage_a']} "
            f"stageB={values['stage_b']} output={values['output']}"
        )

    print(
        "PASS four distinct synthetic inputs and 49152 independently "
        "verified 32-bit values; NO GPU EXECUTION"
    )


if __name__ == "__main__":
    main()
