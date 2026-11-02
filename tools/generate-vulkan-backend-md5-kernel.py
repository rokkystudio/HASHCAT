#!/usr/bin/env python3
"""Generate the embedded Vulkan 1.0 SPIR-V module for the MD5 diagnostic.

The input is one complete little-endian SPIR-V 1.0 module compiled from
tools/vulkan-backend-md5.comp. The output is a deterministic C header containing
the exact original uint32 words and provenance documentation. No SPIR-V
rewriting, cryptographic transformation or GPU execution is performed.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import struct


def write_md5_kernel_header(spirv_path: Path, header_path: Path) -> None:
    """Validate and embed the complete MD5 SPIR-V 1.0 module unchanged.

    Requires canonical little-endian SPIR-V magic, version exactly 1.0,
    complete 32-bit words and reserved schema zero. The generated header
    exposes backend_vulkan_md5_spirv and records the GLSL source used by the
    Android build. The binary words are emitted without normalization.
    """
    data = spirv_path.read_bytes()

    if len(data) < 24 or len(data) % 4 != 0:
        raise ValueError("SPIR-V module must contain complete 32-bit words")

    words = struct.unpack("<" + "I" * (len(data) // 4), data)

    if words[0] != 0x07230203:
        raise ValueError("Expected canonical little-endian SPIR-V magic")

    if words[1] != 0x00010000:
        raise ValueError("Only Vulkan 1.0 SPIR-V 1.0 modules are supported")

    if words[4] != 0:
        raise ValueError("SPIR-V reserved schema word must be zero")

    lines = [
        "/**",
        " * Embed the fixed-vector MD5 Compute shader as Vulkan SPIR-V 1.0.",
        " *",
        " * Generated from tools/vulkan-backend-md5.comp via",
        " * tools/generate-vulkan-backend-md5-kernel.py after GLSL compilation",
        " * with --target-env vulkan1.0. The shader hashes only four fixed",
        " * public one-block messages supplied by the diagnostic host path.",
        " * HASHCAT candidates, salts, targets and scheduler state are absent.",
        " */",
        "",
        "#ifndef HC_BACKEND_VULKAN_MD5_SPIRV_H",
        "#define HC_BACKEND_VULKAN_MD5_SPIRV_H",
        "",
        "#include <stdint.h>",
        "",
        "static const uint32_t backend_vulkan_md5_spirv[] =",
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

    lines += [
        "};",
        "",
        "#endif // HC_BACKEND_VULKAN_MD5_SPIRV_H",
        "",
    ]

    output = ("\n".join(lines)).encode("utf-8")

    header_path.parent.mkdir(parents=True, exist_ok=True)
    header_path.write_bytes(output)

    print(f"SPIRV_EMBEDDED bytes={len(data)} words={len(words)}")
    print(f"HEADER_WRITTEN {header_path.as_posix()}")


def main() -> None:
    """Parse the compiled MD5 SPIR-V path and deterministic header target."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--spirv", type=Path, required=True)
    parser.add_argument("--header", type=Path, required=True)
    args = parser.parse_args()

    write_md5_kernel_header(args.spirv, args.header)


if __name__ == "__main__":
    main()
