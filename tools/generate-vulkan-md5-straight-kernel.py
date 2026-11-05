#!/usr/bin/env python3
"""Compile tools/vulkan-md5-straight.comp and embed it as a C SPIR-V header.

The generated header is committed so an Android build needs no shader
compiler. Run this script only when the GLSL source changes.

The module is compiled for Vulkan 1.0 with the same glslc invocation used by
the M2 generator (glslangValidator -V --target-env vulkan1.0 is equivalent).

Usage:
    python tools/generate-vulkan-md5-straight-kernel.py \
        --glslc D:/Android/Sdk/ndk/<ver>/shader-tools/windows-x86_64/glslc.exe
"""

import argparse
import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCE = ROOT / "tools" / "vulkan-md5-straight.comp"
HEADER = ROOT / "source" / "include" / "backend_vulkan_md5_straight_spirv.h"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--glslc", required=True)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as temporary:
        output = pathlib.Path(temporary) / "vulkan-md5-straight.spv"

        subprocess.run(
            [
                args.glslc,
                "--target-env=vulkan1.0",
                "-O",
                "-o",
                str(output),
                str(SOURCE),
            ],
            check=True,
        )

        data = output.read_bytes()

        if len(data) < 20 or len(data) % 4 != 0:
            raise SystemExit("glslc did not produce complete SPIR-V words")

        words = list(int.from_bytes(data[i:i + 4], "little")
                     for i in range(0, len(data), 4))

        if words[0] != 0x07230203:
            raise SystemExit("glslc did not produce a little-endian SPIR-V module")

        if words[1] != 0x00010000:
            raise SystemExit("expected a Vulkan 1.0 SPIR-V 1.0 module")

        if words[4] != 0:
            raise SystemExit("SPIR-V reserved schema word must be zero")

        lines = [
            "/**",
            " * Generated from tools/vulkan-md5-straight.comp by",
            " * tools/generate-vulkan-md5-straight-kernel.py. Do not edit by hand.",
            " *",
            " * mode 0 (MD5) / -a 0 / ATTACK_EXEC_INSIDE_KERNEL, candidates of",
            " * 0..55 bytes. Declares the Vulkan ABI v2 descriptor table.",
            " */",
            "",
            "#ifndef HC_BACKEND_VULKAN_MD5_STRAIGHT_SPIRV_H",
            "#define HC_BACKEND_VULKAN_MD5_STRAIGHT_SPIRV_H",
            "",
            "#include <stdint.h>",
            "",
            "static const uint32_t backend_vulkan_md5_straight_spirv[] =",
            "{",
        ]

        for start in range(0, len(words), 6):
            chunk = words[start:start + 6]
            lines.append("  " + ", ".join(f"0x{word:08x}u" for word in chunk) + ",")

        lines += [
            "};",
            "",
            f"#define BACKEND_VULKAN_MD5_STRAIGHT_SPIRV_BYTES ({len(words)}u * 4u)",
            "",
            "#endif // HC_BACKEND_VULKAN_MD5_STRAIGHT_SPIRV_H",
            "",
        ]

        HEADER.write_text("\n".join(lines), encoding="ascii", newline="\n")

        print(f"wrote {HEADER} ({len(words)} words, {len(data)} bytes)")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
