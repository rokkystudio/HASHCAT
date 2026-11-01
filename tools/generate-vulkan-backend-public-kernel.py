#!/usr/bin/env python3
"""Generate an embedded Vulkan 1.0 public Compute shader from SPIR-V.

Reads a complete compiled SPIR-V 1.0 little-endian module and writes
one deterministic C header for the Android HASHCAT information-mode
GPU diagnostic. This generator preserves every 32-bit SPIR-V word and
performs no shader transformation, cryptographic work or GPU access.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import struct


def write_public_kernel_header(spirv_path: Path, header_path: Path) -> None:
    """Write one exact SPIR-V 1.0 word array with a stable C declaration.

    Rejects incomplete or non-1.0 modules, invalid magic, unsupported
    byte alignment and zero-length instruction sections. Header bytes
    are deterministic for a given compiled module. The shader is a
    public unsigned-integer transform, not a password-processing kernel.
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
        " * Embed the exact public-integer Compute shader as Vulkan SPIR-V 1.0.",
        " *",
        " * Generated from tools/vulkan-backend-public-kernel.comp via",
        " * tools/generate-vulkan-backend-public-kernel.py after GLSL",
        " * compilation with --target-env vulkan1.0. This immutable shader",
        " * reads and writes only caller-owned unsigned 32-bit test values.",
        " * No cryptographic state or hashcat password input is present.",
        " */",
        "",
        "#ifndef HC_BACKEND_VULKAN_PUBLIC_SPIRV_H",
        "#define HC_BACKEND_VULKAN_PUBLIC_SPIRV_H",
        "",
        "#include <stdint.h>",
        "",
        "static const uint32_t backend_vulkan_public_spirv[] =",
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
        "#endif // HC_BACKEND_VULKAN_PUBLIC_SPIRV_H",
        "",
    ]

    output = ("\n".join(lines)).encode("utf-8")
    header_path.parent.mkdir(parents=True, exist_ok=True)
    header_path.write_bytes(output)

    print(f"SPIRV_EMBEDDED bytes={len(data)} words={len(words)}")
    print(f"HEADER_WRITTEN {header_path.as_posix()}")


def main() -> None:
    """Parse exact compiled SPIR-V and output-header paths from the CLI."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--spirv", type=Path, required=True)
    parser.add_argument("--header", type=Path, required=True)
    args = parser.parse_args()

    write_public_kernel_header(args.spirv, args.header)


if __name__ == "__main__":
    main()
