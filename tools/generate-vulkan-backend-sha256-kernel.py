#!/usr/bin/env python3
"""Embed the production SHA-256 Vulkan Compute module into a C header.

The GLSL shader is compiled for Vulkan 1.0 and SPIR-V 1.0. This generator
preserves every little-endian 32-bit SPIR-V word without rewriting the module.
The resulting header is included by the native SHA-256 Vulkan batch executor.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import struct


def embed_sha256_spirv(spirv_path: Path, header_path: Path) -> None:
    """Write the complete Vulkan 1.0 SHA-256 module as a C uint32 array.

    Validates the SPIR-V signature, version, reserved schema word and
    word alignment. Emits backend_vulkan_sha256_spirv with complete shader
    provenance and stable formatting. Any invalid module raises ValueError.
    """
    data = spirv_path.read_bytes()

    if len(data) < 24 or len(data) % 4 != 0:
        raise ValueError("SPIR-V must contain complete 32-bit words")

    words = struct.unpack("<" + "I" * (len(data) // 4), data)

    if words[0] != 0x07230203:
        raise ValueError("Invalid SPIR-V little-endian magic")

    if words[1] != 0x00010000:
        raise ValueError("Only SPIR-V 1.0 is supported")

    if words[4] != 0:
        raise ValueError("SPIR-V schema word must be zero")

    lines = [
        "/**",
        " * Embed a Vulkan 1.0 SHA-256 byte-message batch Compute shader.",
        " *",
        " * Compiled from tools/vulkan-backend-sha256.comp with",
        " * glslangValidator -V --target-env vulkan1.0 -S comp.",
        " * The native API accepts raw messages and returns canonical SHA-256",
        " * digests without target comparisons or attack scheduling.",
        " */",
        "",
        "#ifndef HC_BACKEND_VULKAN_SHA256_SPIRV_H",
        "#define HC_BACKEND_VULKAN_SHA256_SPIRV_H",
        "",
        "#include <stdint.h>",
        "",
        "static const uint32_t backend_vulkan_sha256_spirv[] =",
        "{",
    ]

    for offset in range(0, len(words), 8):
        group = words[offset:offset + 8]
        rendered = ", ".join(
            f"UINT32_C (0x{word:08x})" for word in group
        )
        if offset + len(group) < len(words):
            rendered += ","
        lines.append("  " + rendered)

    lines.extend([
        "};",
        "",
        "#endif // HC_BACKEND_VULKAN_SHA256_SPIRV_H",
        "",
    ])

    header_path.parent.mkdir(parents=True, exist_ok=True)
    header_path.write_bytes("\n".join(lines).encode("utf-8"))

    print(f"SPIRV_SHA256 bytes={len(data)} words={len(words)}")


def main() -> None:
    """Read the compiled SPIR-V path and write its deterministic C header."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--spirv", required=True, type=Path)
    parser.add_argument("--header", required=True, type=Path)
    args = parser.parse_args()

    embed_sha256_spirv(args.spirv, args.header)


if __name__ == "__main__":
    main()
