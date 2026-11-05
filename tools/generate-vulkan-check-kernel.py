#!/usr/bin/env python3
"""Compile the M2 data-path check shader and emit its SPIR-V header.

The generated header is committed so an Android build needs no shader
compiler. Run this script only when vulkan-hashcat-check.comp changes.

Usage:
    python tools/generate-vulkan-check-kernel.py \
        --glslc D:/Android/Sdk/ndk/<ver>/shader-tools/windows-x86_64/glslc.exe
"""

import argparse
import pathlib
import subprocess
import tempfile


ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCE = ROOT / "tools" / "vulkan-hashcat-check.comp"
HEADER = ROOT / "source" / "include" / "backend_vulkan_check_spirv.h"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--glslc", required=True)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory() as temporary:
        output = pathlib.Path(temporary) / "vulkan-hashcat-check.spv"
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

        words = list(int.from_bytes(output.read_bytes()[i : i + 4], "little")
                     for i in range(0, output.stat().st_size, 4))

        if words[0] != 0x07230203:
            raise SystemExit("glslc did not produce a little-endian SPIR-V module")

        lines = [
            "/**",
            " * Generated from tools/vulkan-hashcat-check.comp by",
            " * tools/generate-vulkan-check-kernel.py. Do not edit by hand.",
            " */",
            "",
            "#ifndef HC_BACKEND_VULKAN_CHECK_SPIRV_H",
            "#define HC_BACKEND_VULKAN_CHECK_SPIRV_H",
            "",
            "#include <stdint.h>",
            "",
            "static const uint32_t backend_vulkan_check_spirv[] =",
            "{",
        ]

        for start in range(0, len(words), 6):
            chunk = words[start : start + 6]
            lines.append("  " + ", ".join(f"0x{word:08x}u" for word in chunk) + ",")

        lines += [
            "};",
            "",
            f"#define BACKEND_VULKAN_CHECK_SPIRV_BYTES ({len(words)}u * 4u)",
            "",
            "#endif // HC_BACKEND_VULKAN_CHECK_SPIRV_H",
            "",
        ]

        HEADER.write_text("\n".join(lines), encoding="ascii", newline="\n")

        print(f"wrote {HEADER} ({len(words)} words)")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
