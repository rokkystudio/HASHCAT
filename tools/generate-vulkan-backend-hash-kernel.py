#!/usr/bin/env python3
"""Embed a standard byte-message Vulkan SHA Compute module in a C header.

The input is canonical SPIR-V 1.0 generated with glslangValidator for
Vulkan 1.0. The output contains the exact original 32-bit shader words
and algorithm-specific provenance in reproducible UTF-8 formatting.
Supported modules are SHA-1, SHA-224 and BLAKE2s-256.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import struct


def embed_hash_spirv(
    spirv_path: Path,
    header_path: Path,
    algorithm: str,
) -> None:
    """Generate a C uint32 module for the selected SHA or BLAKE2s shader.

    Validates the complete SPIR-V word alignment, canonical signature,
    SPIR-V 1.0 version and reserved schema before emitting a fixed symbol
    and include guard. The payload is preserved without transformations.
    """
    if algorithm not in ("sha1", "sha224", "blake2s"):
        raise ValueError("Expected sha1, sha224 or blake2s")

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
    display = {
        "sha1": "SHA-1",
        "sha224": "SHA-224",
        "blake2s": "BLAKE2s-256",
    }[algorithm]

    lines = [
        "/**",
        f" * Embed the Vulkan 1.0 {display} message-batch Compute shader.",
        " *",
        f" * Compiled from tools/vulkan-backend-{algorithm}.comp with",
        " * glslangValidator -V --target-env vulkan1.0 -S comp.",
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
    ]

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
    """Read the target algorithm, source SPIR-V and output header paths."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--algorithm",
        required=True,
        choices=("sha1", "sha224", "blake2s"),
    )
    parser.add_argument("--spirv", required=True, type=Path)
    parser.add_argument("--header", required=True, type=Path)
    args = parser.parse_args()

    embed_hash_spirv(args.spirv, args.header, args.algorithm)


if __name__ == "__main__":
    main()
