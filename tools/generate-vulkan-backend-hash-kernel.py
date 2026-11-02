#!/usr/bin/env python3
"""Embed Vulkan SHA, BLAKE2s and Keccak-family Compute modules in C headers.

The input is canonical SPIR-V 1.0 generated with glslangValidator for
Vulkan 1.0. The output contains the exact original 32-bit shader words
and algorithm-specific provenance in reproducible UTF-8 formatting.
SHA3-224/256/384/512 and Keccak-256 use distinct configured variants
of tools/vulkan-backend-keccak.comp with no optional shaderInt64 feature.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import struct


ALGORITHMS = {
    "sha1": ("SHA-1", None),
    "sha224": ("SHA-224", None),
    "blake2s": ("BLAKE2s-256", None),
    "sha3_224": ("SHA3-224", (144, 6, 28)),
    "sha3_256": ("SHA3-256", (136, 6, 32)),
    "sha3_384": ("SHA3-384", (104, 6, 48)),
    "sha3_512": ("SHA3-512", (72, 6, 64)),
    "keccak_256": ("Keccak-256", (136, 1, 32)),
}


def embed_hash_spirv(
    spirv_path: Path,
    header_path: Path,
    algorithm: str,
) -> None:
    """Generate a deterministic C uint32 array for one declared GPU shader.

    Validates complete SPIR-V word alignment, canonical little-endian magic,
    SPIR-V 1.0 version and reserved schema. Writes the exact binary words
    with source provenance and Keccak-family compile-time parameters.
    A missing algorithm or malformed module raises ValueError.
    """
    if algorithm not in ALGORITHMS:
        raise ValueError(f"Unsupported Vulkan hash algorithm: {algorithm}")

    data = spirv_path.read_bytes()

    if len(data) < 24 or len(data) % 4 != 0:
        raise ValueError("SPIR-V module must contain complete 32-bit words")

    words = struct.unpack("<" + "I" * (len(data) // 4), data)

    if words[0] != 0x07230203:
        raise ValueError("Expected canonical SPIR-V little-endian magic")

    if words[1] != 0x00010000:
        raise ValueError("Only SPIR-V 1.0 is supported")

    if words[4] != 0:
        raise ValueError("Reserved SPIR-V schema must be zero")

    symbol = f"backend_vulkan_{algorithm}_spirv"
    guard = f"HC_BACKEND_VULKAN_{algorithm.upper()}_SPIRV_H"
    display, variant = ALGORITHMS[algorithm]
    source = (
        "tools/vulkan-backend-keccak.comp"
        if variant is not None
        else f"tools/vulkan-backend-{algorithm}.comp"
    )

    lines = [
        "/**",
        f" * Embed the Vulkan 1.0 {display} message-batch Compute shader.",
        " *",
        f" * Compiled from {source} with",
        " * glslangValidator -V --target-env vulkan1.0 -S comp.",
    ]

    if variant is not None:
        rate, suffix, digest = variant
        lines.extend([
            f" * Build parameters: RATE_BYTES={rate}, SUFFIX={suffix},",
            f" * DIGEST_BYTES={digest}. The 1600-bit state uses 32-bit halves.",
        ])

    lines.extend([
        " * Each invocation hashes caller-supplied byte data independently;",
        " * candidate generation and target comparison are not performed.",
        " */",
        "",
        f"#ifndef {guard}",
        f"#define {guard}",
        "",
        "#include <stdint.h>",
        "",
        f"static const uint32_t {symbol}[] =",
        "{",
    ])

    for offset in range(0, len(words), 8):
        group = words[offset:offset + 8]
        line = "  " + ", ".join(
            f"UINT32_C (0x{word:08x})" for word in group
        )

        if offset + len(group) < len(words):
            line += ","

        lines.append(line)

    lines.extend([
        "};",
        "",
        f"#endif // {guard}",
        "",
    ])

    header_path.parent.mkdir(parents=True, exist_ok=True)
    header_path.write_bytes("\n".join(lines).encode("utf-8"))

    print(f"SPIRV_{algorithm.upper()} bytes={len(data)} words={len(words)}")


def main() -> None:
    """Read one declared algorithm, SPIR-V module and C header destination."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--algorithm",
        required=True,
        choices=tuple(ALGORITHMS),
    )
    parser.add_argument("--spirv", required=True, type=Path)
    parser.add_argument("--header", required=True, type=Path)
    args = parser.parse_args()

    embed_hash_spirv(args.spirv, args.header, args.algorithm)


if __name__ == "__main__":
    main()
