#!/usr/bin/env python3
"""CPU reference KAT for the Vulkan mode 0 / -a 0 straight stage.

The Vulkan program is a single stage: build the 512-bit MD5 block from one
pw_t record, run the 64 rounds, compare the four digest words and emit a
plain_t on a match. This script pins:

  * the exact block construction the GLSL kernel performs (word copy, tail
    mask, 0x80 pad position, bit length) against Python's hashlib for fixed
    vectors, including the one-block boundary at 55 bytes;
  * the digest constants used by the device self-check;
  * the static contract between the kernel source, the generated SPIR-V
    header and backend_vulkan_bindings.h (ABI version, binding numbers,
    local size, one-block limit).

Run from the repository root:
    python tools/test-vulkan-md5-straight-kat.py
"""

from __future__ import annotations

import hashlib
import pathlib
import re
import struct
import sys


ROOT = pathlib.Path(__file__).resolve().parent.parent

KERNEL = ROOT / "tools" / "vulkan-md5-straight.comp"
SPIRV = ROOT / "source" / "include" / "backend_vulkan_md5_straight_spirv.h"
BINDINGS = ROOT / "source" / "include" / "backend_vulkan_bindings.h"

MASK32 = 0xFFFFFFFF


def md5_block_from_pw(password: bytes) -> list[int]:
    """Build the 16-word MD5 block exactly like the GLSL compute stage."""
    length = len(password)
    assert length <= 55, "one-block candidates are 0..55 bytes"

    block = [0] * 16

    full_words = length // 4
    for i in range(full_words):
        block[i] = int.from_bytes(password[i * 4:i * 4 + 4], "little")

    tail = length & 3
    if tail:
        piece = password[full_words * 4:]
        block[full_words] = int.from_bytes(piece, "little") & ((1 << (8 * tail)) - 1)

    block[length // 4] |= 0x80 << (8 * (length & 3))
    block[14] = length << 3
    block[15] = 0

    return block


def compress(block: list[int]) -> bytes:
    """The same 64 rounds as the shader; returns the digest bytes."""
    shifts = [
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
        5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
        6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
    ]
    constants = [
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee,
        0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
        0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
        0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed,
        0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
        0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05,
        0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
        0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
    ]

    a, b, c, d = 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476
    ia, ib, ic, id = a, b, c, d

    def rotl(value: int, amount: int) -> int:
        value &= MASK32
        return ((value << amount) | (value >> (32 - amount))) & MASK32

    for i in range(64):
        if i < 16:
            f = (b & c) | ((~b) & d)
            g = i
        elif i < 32:
            f = (d & b) | ((~d) & c)
            g = (5 * i + 1) & 15
        elif i < 48:
            f = b ^ c ^ d
            g = (3 * i + 5) & 15
        else:
            f = c ^ (b | (~d))
            g = (7 * i) & 15

        previous_d = d
        d = c
        c = b
        b = (b + rotl((a + f + constants[i] + block[g]) & MASK32, shifts[i])) & MASK32
        a = previous_d

    state = [(ia + a) & MASK32, (ib + b) & MASK32, (ic + c) & MASK32, (id + d) & MASK32]
    return b"".join(word.to_bytes(4, "little") for word in state)


VECTORS = [
    b"",
    b"a",
    b"abc",
    b"hashcat",
    b"a" * 54,
    b"a" * 55,
]

# CPU reference constants used by backend_vulkan_device_md5_straight_check().
SELF_CHECK_DIGESTS = {
    b"hashcat": "8743b52063cd84097a65d1633f5c74f5",
    b"a" * 55: "ef1772b6dff9a122358552954ad0df65",
}


def parse_spirv_header(path: pathlib.Path) -> list[int]:
    text = path.read_text(encoding="ascii")
    match = re.search(r"backend_vulkan_md5_straight_spirv\[\]\s*=\s*\{(.*?)\};", text, re.S)
    if not match:
        raise AssertionError("SPIR-V header does not define the expected array")

    words = [int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{8})u", match.group(1))]
    return words


def main() -> int:
    failures = 0

    for password in VECTORS:
        block = md5_block_from_pw(password)
        digest = compress(block)
        expected = hashlib.md5(password).digest()

        status = "PASS" if digest == expected else "FAIL"
        if digest != expected:
            failures += 1

        print(f"{status} vector len={len(password):2d} digest={digest.hex()}")

    for password, expected_hex in SELF_CHECK_DIGESTS.items():
        got = compress(md5_block_from_pw(password)).hex()
        status = "PASS" if got == expected_hex else "FAIL"
        if got != expected_hex:
            failures += 1
        print(f"{status} self-check constant len={len(password):2d} {got}")

    # 56 bytes is outside the one-block boundary and must not be usable.
    try:
        md5_block_from_pw(b"a" * 56)
        failures += 1
        print("FAIL one-block boundary accepted 56 bytes")
    except AssertionError:
        print("PASS one-block boundary rejects 56 bytes")

    # Static contract: ABI v2, plains binding, local size 64, one-block limit.

    bindings = BINDINGS.read_text(encoding="utf-8")
    kernel = KERNEL.read_text(encoding="utf-8")

    checks = [
        ("bindings version 2", "#define VK_BINDINGS_VERSION 2u" in bindings),
        ("binding count 15", "#define VK_BINDING_COUNT 15u" in bindings),
        ("one-block limit 55", "#define VK_MD5_STRAIGHT_MAX_LEN 55u" in bindings),
        ("plains binding 14", "HC_VK_BUFFER_PLAIN" in bindings),
        ("kernel local size 64", "local_size_x = 64" in kernel),
        ("kernel pws binding 1", "binding = 1" in kernel),
        ("kernel digests binding 2", "binding = 2" in kernel),
        ("kernel result binding 7", "binding = 7" in kernel),
        ("kernel plains binding 14", "binding = 14" in kernel),
        ("kernel one-block guard", "pw_len > 55u" in kernel),
        ("kernel plain_t stride 10 words", "idx * 10u" in kernel),
    ]

    for name, ok in checks:
        print(("PASS " if ok else "FAIL ") + name)
        if not ok:
            failures += 1

    words = parse_spirv_header(SPIRV)

    spirv_ok = (
        len(words) >= 5
        and words[0] == 0x07230203
        and words[1] == 0x00010000
        and words[4] == 0
    )

    print(("PASS " if spirv_ok else "FAIL ") + f"SPIR-V header magic/version words={len(words)}")
    if not spirv_ok:
        failures += 1

    # Verify the committed byte count macro matches the embedded words.

    header_text = SPIRV.read_text(encoding="ascii")
    macro = re.search(r"#define BACKEND_VULKAN_MD5_STRAIGHT_SPIRV_BYTES \((\d+)u \* 4u\)", header_text)
    if macro and int(macro.group(1)) == len(words):
        print("PASS SPIR-V byte-count macro")
    else:
        print("FAIL SPIR-V byte-count macro")
        failures += 1

    print(f"RESULT: {'PASS' if failures == 0 else 'FAIL'} failures={failures}")

    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
