#!/usr/bin/env python3
"""Validate Android-relevant pure OpenCL source with a host Clang compiler.

The preflight compiles the repository's actual pure kernels for md5crypt
(500), bcrypt (3200), and NetNTLMv2 mask attack (5600) using OpenCL C 1.2
and OpenCL C 3.0, with one and eight local work-items.

The compiler is supplied explicitly with --clang. A generated, temporary
wrapper defines the same source include expansion used by hashcat's OpenCL
loader. These tests perform source-level compilation checks only; they do
not use OpenCL device APIs, Android, hashcat, GPUs, or driver JIT compilers.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
CASES = (
    ("500", "m00500-pure.cl", 0, 4),
    ("3200", "m03200-pure.cl", 0, 6),
    ("5600", "m05600_a3-pure.cl", 3, 4),
)
STANDARDS = ("CL1.2", "CL3.0")
LOCAL_SIZES = (1, 8)


def make_wrapper(source: str, destination: Path) -> None:
    """Include one unchanged hashcat pure kernel with its actual macro paths."""
    assert source in {item[1] for item in CASES}, source
    path = ROOT / "source" / "OpenCL" / source
    assert path.is_file(), f"Missing kernel: {path}"

    destination.write_text(
        "/* Offline source preflight; no Android OpenCL runtime is started. */\n"
        "#define KERNEL_STATIC\n"
        "#define INCLUDE_PATH source/OpenCL\n"
        "#define HC_M2S_INNER(x) #x\n"
        "#define M2S(x) HC_M2S_INNER(x)\n"
        f'#include "source/OpenCL/{source}"\n',
        encoding="utf-8",
        newline="\n",
    )


def verify_case(
    clang: Path,
    wrapper: Path,
    mode: str,
    attack_mode: int,
    digest_words: int,
    standard: str,
    local_size: int,
) -> None:
    """Require warning-free parsing with Android Qualcomm-like type defines."""
    args = [
        str(clang),
        "-x", "cl",
        "-fsyntax-only",
        "-Werror",
        "-Wno-unused-function",
        "-I", str(ROOT),
        "-I", str(ROOT / "source" / "OpenCL"),
        f"-cl-std={standard}",
        "-D", "VECT_SIZE=1",
        "-D", "DEVICE_TYPE=4",
        "-D", "VENDOR_ID=2147483648",
        "-D", "LOCAL_MEM_TYPE=1",
        "-D", f"ATTACK_MODE={attack_mode}",
        "-D", "ATTACK_EXEC=11",
        "-D", "ATTACK_KERN=3",
        "-D", f"DGST_ELEM={digest_words}",
        "-D", "DGST_R0=0",
        "-D", "DGST_R1=1",
        "-D", "DGST_R2=2",
        "-D", "DGST_R3=3",
        "-D", f"FIXED_LOCAL_SIZE={local_size}",
        str(wrapper),
    ]
    process = subprocess.run(
        args, cwd=ROOT, capture_output=True, text=True, timeout=45,
    )
    assert process.returncode == 0, (
        f"OpenCL host syntax preflight failed: mode={mode}, "
        f"standard={standard}, local_size={local_size}, "
        f"exit={process.returncode}\n"
        f"stderr:\n{process.stderr[-8000:]}\n"
        f"stdout:\n{process.stdout[-2000:]}"
    )
    assert not process.stderr.strip(), (
        f"Unexpected Clang diagnostics for mode={mode}: {process.stderr}"
    )

    print(
        f"PASS host Clang pure mode={mode} "
        f"standard={standard} local_size={local_size}"
    )


def main() -> None:
    """Compile each selected kernel under the complete 12-case host matrix."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--clang", type=Path, required=True,
        help="Explicit path to host Clang executable",
    )
    args = parser.parse_args()

    clang = args.clang.resolve(strict=True)
    assert clang.is_file(), f"Clang executable missing: {clang}"

    with tempfile.TemporaryDirectory(prefix="android-opencl-pure-preflight-") as path:
        directory = Path(path)

        for mode, source, attack_mode, digest_words in CASES:
            wrapper = directory / f"mode-{mode}.cl"
            make_wrapper(source, wrapper)

            for standard in STANDARDS:
                for local_size in LOCAL_SIZES:
                    verify_case(
                        clang, wrapper, mode, attack_mode,
                        digest_words, standard, local_size,
                    )

    print("PASS 12 host Clang pure OpenCL syntax configurations")
    print("NO GPU KERNEL EXECUTION OR DEVICE COMPILER BUILD WAS ATTEMPTED")


if __name__ == "__main__":
    main()
