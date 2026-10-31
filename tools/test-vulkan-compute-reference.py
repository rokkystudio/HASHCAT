#!/usr/bin/env python3
"""Verify synthetic Vulkan Compute GPU checksums using an independent CPU oracle.

This reference implements the exact 32-bit wraparound arithmetic, 32-thread
neighbor exchange, and 32 KiB shared-array access patterns of the two
noncryptographic GLSL compute shaders. It does not load Vulkan or hashcat.
"""

from __future__ import annotations

import struct


MASK = 0xFFFFFFFF
ELEMENT_COUNT = 2048
LOCAL_SIZE = 32
SHARED_WORDS = 8192
FNV_OFFSET = 14695981039346656037
FNV_PRIME = 1099511628211

EXPECTED_CHECKSUMS = {
    "baseline": "967d964855b53334",
    "shared32k": "0c099d93a929bc82",
}


def input_word(index: int) -> int:
    """Return the deterministic synthetic word at the requested global index."""
    return (
        (index * 0x9E3779B9) ^ (index << 15)
        ^ (index >> 3) ^ 0xA5A5A5A5
    ) & MASK


def rotated(value: int, amount: int) -> int:
    """Rotate one unsigned 32-bit value without relying on Python overflow."""
    return ((value << amount) | (value >> (32 - amount))) & MASK


def fold_shared(scratch: list[int], lane: int) -> int:
    """Fold 256 cross-lane reads using the pre-update accumulator for rotation."""
    accumulated = 0x12345678

    for offset in range(lane, SHARED_WORDS, LOCAL_SIZE):
        next_word = scratch[(offset + 1) & (SHARED_WORDS - 1)]
        previous = accumulated
        accumulated = (
            (previous + next_word) ^ rotated(previous, 5)
        ) & MASK

    return accumulated


def output_words(shared32k: bool):
    """Yield all four expected output words per lane in exact dispatch order."""
    for group_start in range(0, ELEMENT_COUNT, LOCAL_SIZE):
        values = [
            input_word(group_start + lane)
            for lane in range(LOCAL_SIZE)
        ]

        scratch = None

        if shared32k:
            scratch = [
                (
                    ((values[offset & (LOCAL_SIZE - 1)] * 1664525
                      + 1013904223) & MASK)
                    ^ ((offset * 2654435761) & MASK)
                ) & MASK
                for offset in range(SHARED_WORDS)
            ]

        for lane, value in enumerate(values):
            multiply_add = (value * 1664525 + 1013904223) & MASK
            shift7 = rotated(value, 7)

            if shared32k:
                assert scratch is not None
                folded = fold_shared(scratch, lane)
                last_index = 8160 + ((lane + 1) & (LOCAL_SIZE - 1))
                yield multiply_add, shift7, folded, scratch[last_index]
            else:
                neighbor = values[(lane + 1) & (LOCAL_SIZE - 1)]
                neighbor_a = (neighbor * 1664525 + 1013904223) & MASK
                neighbor_b = rotated(neighbor, 7)
                mixed = ((multiply_add + shift7) & MASK) ^ ((value * 3) & MASK)
                swapped = rotated(value, 16)

                yield (
                    multiply_add,
                    shift7,
                    multiply_add ^ neighbor_a ^ neighbor_b,
                    mixed ^ swapped,
                )


def reference_checksum(shared32k: bool) -> str:
    """Compute the bytewise little-endian FNV-1a checksum of all CPU outputs."""
    checksum = FNV_OFFSET
    count = 0

    for words in output_words(shared32k):
        assert len(words) == 4
        count += 1

        for byte in struct.pack("<4I", *words):
            checksum = ((checksum ^ byte) * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF

    assert count == ELEMENT_COUNT

    return f"{checksum:016x}"


def main() -> None:
    """Require both reference digests to match independently observed devices."""
    for mode, expected in EXPECTED_CHECKSUMS.items():
        actual = reference_checksum(mode == "shared32k")

        assert actual == expected, (
            f"{mode} CPU oracle differs from device output: "
            f"expected={expected} computed={actual}"
        )

        print(
            f"PASS Vulkan {mode}: {ELEMENT_COUNT} public synthetic outputs, "
            f"{ELEMENT_COUNT * 4} words, FNV1A64={actual}"
        )

    print("NO VULKAN DEVICE, CREDENTIAL, OR HASHCAT EXECUTION PERFORMED")


if __name__ == "__main__":
    main()
