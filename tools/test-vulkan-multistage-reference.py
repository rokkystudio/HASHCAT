#!/usr/bin/env python3
"""Verify public synthetic three-stage Vulkan results with independent Python.

This file computes all intermediate 32-bit words and the four-word output
per element, models 32-lane workgroup wrapping, and compares FNV-1a checksums.
No Vulkan API, password input or hashcat process is involved.
"""

from __future__ import annotations

import struct


ELEMENTS = 2048
LANES = 32
MASK = 0xFFFFFFFF
FNV_OFFSET = 14695981039346656037
FNV_PRIME = 1099511628211

EXPECTED = {
    "stage_a": "de56d6d0aa66c6ec",
    "stage_b": "c1259ad4f024499a",
    "output": "05de70c62f8d7cfd",
}


def synthetic_input(index: int) -> int:
    """Generate a deterministic public 32-bit test word from its index."""
    return (
        (index * 0x9E3779B9) ^ (index << 15)
        ^ (index >> 3) ^ 0xA5A5A5A5
    ) & MASK


def rotate(value: int, amount: int) -> int:
    """Compute a 32-bit left rotation while retaining exact unsigned wrap."""
    assert 0 < amount < 32

    return ((value << amount) | (value >> (32 - amount))) & MASK


def calculate_stage_a() -> list[int]:
    """Produce all stage-A intermediate words from synthetic inputs."""
    result = []

    for index in range(ELEMENTS):
        value = synthetic_input(index)
        affine = (value * 1664525 + 1013904223) & MASK
        transformed = (affine ^ rotate(value, 7))
        result.append((transformed + index * 2654435761) & MASK)

    return result


def calculate_stage_b(stage_a: list[int]) -> list[int]:
    """Read stage-A neighbors and produce one stage-B word per work item."""
    assert len(stage_a) == ELEMENTS
    result = []

    for index, value in enumerate(stage_a):
        lane = index % LANES
        start = index - lane
        neighbor = stage_a[start + ((lane + 7) % LANES)]
        result.append(
            ((value + neighbor) ^ rotate(value, 11) ^ 0x6C8E9CF5)
            & MASK
        )

    return result


def calculate_output(stage_a: list[int], stage_b: list[int]) -> list[int]:
    """Merge both intermediate arrays into interleaved four-word results."""
    assert len(stage_a) == ELEMENTS
    assert len(stage_b) == ELEMENTS
    result = []

    for a, b in zip(stage_a, stage_b):
        result.extend((
            a,
            b,
            a ^ b,
            ((a * 2246822519 + b * 3266489917)
             ^ rotate(b, 3)) & MASK,
        ))

    return result


def checksum(words: list[int]) -> str:
    """Return the little-endian bytewise FNV-1a checksum of unsigned words."""
    result = FNV_OFFSET

    for word in words:
        assert 0 <= word <= MASK

        for byte in struct.pack("<I", word):
            result = ((result ^ byte) * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF

    return f"{result:016x}"


def reference() -> dict[str, str]:
    """Calculate independent checksums for both intermediate and final buffers."""
    stage_a = calculate_stage_a()
    stage_b = calculate_stage_b(stage_a)
    output = calculate_output(stage_a, stage_b)

    assert len(output) == ELEMENTS * 4

    return {
        "stage_a": checksum(stage_a),
        "stage_b": checksum(stage_b),
        "output": checksum(output),
    }


def main() -> None:
    """Require all CPU stage checksums to match archived real GPU output."""
    observed = reference()

    assert observed == EXPECTED, (
        f"Vulkan pipeline CPU reference differs: {observed} vs {EXPECTED}"
    )

    for stage, digest in observed.items():
        words = ELEMENTS * (4 if stage == "output" else 1)
        print(f"PASS Vulkan synthetic {stage}: {words} words FNV1A64={digest}")

    print("PASS 12288 deterministic synthetic words; no Vulkan or hashcat execution")


if __name__ == "__main__":
    main()
