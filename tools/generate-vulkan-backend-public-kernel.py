#!/usr/bin/env python3
"""Generate deterministic embedded Vulkan 1.0 SPIR-V public Compute modules.

Reads one complete compiled SPIR-V 1.0 little-endian module and writes
a C array with an explicit symbol name, include guard and GLSL source
path. The defaults produce the original public-integer shader header
byte-for-byte. Named options also support the second independent
public-data stage. No shader transformation, GPU operation, password
processing or cryptographic implementation is performed.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import struct


def write_public_kernel_header(
    spirv_path: Path,
    header_path: Path,
    symbol: str = "backend_vulkan_public_spirv",
    guard: str = "HC_BACKEND_VULKAN_PUBLIC_SPIRV_H",
    source: str = "tools/vulkan-backend-public-kernel.comp",
) -> None:
    """Write all original SPIR-V 1.0 words to a deterministic C header.

    Rejects invalid magic, non-1.0 modules, incomplete 32-bit words,
    invalid schema and non-C-identifier symbols or guards. The shader
    bytes remain unchanged. The named GLSL source becomes provenance
    documentation in the generated header and is never executed.
    """
    if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", symbol) is None:
        raise ValueError("C array symbol must be an exact C identifier")

    if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", guard) is None:
        raise ValueError("C header guard must be an exact C identifier")

    if not source.startswith("tools/") or re.fullmatch(
        r"tools/[a-zA-Z0-9_.-]+\.comp", source
    ) is None:
        raise ValueError("Shader source must identify one tools/*.comp file")

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
        " * Embed the exact public-integer Compute shader as Vulkan SPIR-V 1.0.",
        " *",
        f" * Generated from {source} via",
        " * tools/generate-vulkan-backend-public-kernel.py after GLSL",
        " * compilation with --target-env vulkan1.0. This immutable shader",
        " * reads and writes only caller-owned unsigned 32-bit test values.",
        " * No cryptographic state or hashcat password input is present.",
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
        line = "  " + ", ".join(f"UINT32_C (0x{word:08x})" for word in group)

        if offset + len(group) < len(words):
            line += ","

        lines.append(line)

    lines += [
        "};",
        "",
        f"#endif // {guard}",
        "",
    ]

    output = ("\n".join(lines)).encode("utf-8")
    header_path.parent.mkdir(parents=True, exist_ok=True)
    header_path.write_bytes(output)

    print(f"SPIRV_EMBEDDED bytes={len(data)} words={len(words)}")
    print(f"HEADER_WRITTEN {header_path.as_posix()}")


def main() -> None:
    """Parse one compiled SPIR-V module and its named C header target."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--spirv", type=Path, required=True)
    parser.add_argument("--header", type=Path, required=True)
    parser.add_argument(
        "--symbol", default="backend_vulkan_public_spirv"
    )
    parser.add_argument(
        "--guard", default="HC_BACKEND_VULKAN_PUBLIC_SPIRV_H"
    )
    parser.add_argument(
        "--source", default="tools/vulkan-backend-public-kernel.comp"
    )
    args = parser.parse_args()

    write_public_kernel_header(
        args.spirv, args.header, args.symbol, args.guard, args.source
    )


if __name__ == "__main__":
    main()
