#!/usr/bin/env python3
"""Compare the original bcrypt 3200 OpenCL stages with the standalone probe.

The native GCC regression compiles BOTH OpenCL implementations verbatim
using hashcat's CPU emulation definitions:

- tools/m03200-stage-probe.cl through tools/m03200-stage-probe-cpu.c
- source/OpenCL/m03200-pure.cl through tools/m03200-stock-kernel-cpu.c

It compares all three 4240-byte bcrypt intermediate states byte-for-byte
and with the independently archived CPU reference. The standalone probe's
six final ciphertext words are also verified. Both public candidates run
under -O0 and -O2, including deliberate comparison-error detection.

This is CPU execution only: no Android device, GPU, OpenCL ICD or search
workload is involved.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
PROBE_SOURCE = ROOT / "tools" / "m03200-stage-probe-cpu.c"
STOCK_SOURCE = ROOT / "tools" / "m03200-stock-kernel-cpu.c"
PROBE_KERNEL = ROOT / "tools" / "m03200-stage-probe.cl"
STOCK_KERNEL = ROOT / "source" / "OpenCL" / "m03200-pure.cl"
REFERENCE = (
    ROOT / "tests" / "fixtures" / "android-gpu"
    / "redmagic-selected-modes-20261030" / "m03200-cpu-stage-reference.json"
)
MANIFEST = REFERENCE.with_name("m03200-standalone-probe-manifest.json")
COMPARATOR = ROOT / "tools" / "compare-m03200-gpu-stages.py"
CONVERTER = ROOT / "tools" / "convert-m03200-stage-readback.py"
STAGES = ("init", "loop1", "loop_final")
COUNTS = {"E": 18, "P": 18, "S0": 256, "S1": 256, "S2": 256, "S3": 256}
INCLUDES = (
    "source/include",
    "source/OpenCL",
    "tools",
    "source/deps/LZMA-SDK/C",
    "source/deps/zlib",
    "source/deps/zlib/contrib",
    "source/deps/xxHash",
    "source/deps/OpenCL-Headers",
    "source/deps/unrar",
)


def checked_run(args: list[str], *, timeout: int = 90) -> str:
    """Run GCC or one native CPU kernel process and expose complete errors."""
    process = subprocess.run(
        args, cwd=ROOT, capture_output=True, text=True, timeout=timeout,
    )
    if process.returncode:
        raise RuntimeError(
            f"Command failed ({process.returncode}): {args}\n"
            f"stdout:\n{process.stdout[-3000:]}\nstderr:\n{process.stderr[-9000:]}"
        )
    return process.stdout


def compile_emulation(
    cc: str, optimization: str, directory: Path, source: Path,
) -> Path:
    """Compile the specified unchanged OpenCL kernel through native emulation."""
    binary = directory / (
        f"{source.stem}-{optimization[1:]}" + (".exe" if os.name == "nt" else "")
    )
    flags = [
        "-std=gnu11",
        "-fgnu89-inline",
        optimization,
        "-ffunction-sections",
        "-fdata-sections",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wno-unused-function",
        "-Wno-unused-parameter",
        "-Wno-unused-variable",
        "-Wno-unused-but-set-variable",
        *["-I" + str(ROOT / item) for item in INCLUDES],
    ]
    checked_run([
        cc, *flags, str(source),
        str(ROOT / "source/src/bitops.c"),
        "-Wl,--gc-sections",
        "-o", str(binary),
    ], timeout=120)
    return binary


def read_native_stages(
    directory: Path,
    candidate: str,
    output: str,
    converter: dict,
    *,
    stock: bool,
) -> dict:
    """Decode all stage fields and require the exact CPU-only completion record."""
    complete = (
        "CPU_EMULATED_STOCK_BCRYPT_COMPLETE"
        if stock else "CPU_EMULATED_OPENCL_PROBE_COMPLETE"
    )
    assert complete in output, f"CPU executable did not complete: {complete}"
    assert "PROBE_BUILD_SUCCESS" not in output, (
        "Native CPU emulation cannot claim driver compilation"
    )

    expected_names = {f"m03200-{stage}.bin" for stage in STAGES}
    if not stock:
        expected_names.add("m03200-comp.bin")
    assert {path.name for path in directory.iterdir()} == expected_names, (
        "CPU emulation produced unexpected output files"
    )

    prefix = "CPU_STOCK_STAGE_SAVED" if stock else "CPU_STAGE_SAVED"
    result = {"candidate": candidate, "stages": {}}

    for stage in STAGES:
        assert f"{prefix} {stage} 4240" in output, stage
        words = converter["decode_words"](
            (directory / f"m03200-{stage}.bin").read_bytes(), 1060,
        )
        arrays = {}
        offset = 0

        for name, count in COUNTS.items():
            arrays[name] = words[offset:offset + count]
            offset += count

        assert offset == 1060
        result["stages"][stage] = arrays

    if not stock:
        assert "CPU_STAGE_SAVED comp 24" in output
        result["comp_words_full"] = converter["decode_words"](
            (directory / "m03200-comp.bin").read_bytes(), 6,
        )
        result["comp_words"] = result["comp_words_full"][:4]

    return result


def compare_native_stages(
    result: dict, reference: dict, comparator: dict, *, stock: bool,
) -> None:
    """Validate every intermediate word and the probe's final ciphertext."""
    candidate = result["candidate"]
    comparator["compare_stage_arrays"](result, reference, candidate)
    if not stock:
        assert comparator["compare_comp_words"](result, reference, candidate)
        expected = reference["vectors"][candidate]["final_ciphertext_hex"]
        assert result["comp_words_full"] == [
            expected[index:index + 8] for index in range(0, 48, 8)
        ]


def check_corrupted_stage(
    result: dict, reference: dict, comparator: dict,
) -> None:
    """Require detection of an injected source-kernel stage-word discrepancy."""
    corrupted = copy.deepcopy(result)
    previous = corrupted["stages"]["loop1"]["S1"][89]
    corrupted["stages"]["loop1"]["S1"][89] = (
        "00000000" if previous != "00000000" else "ffffffff"
    )

    try:
        comparator["compare_stage_arrays"](
            corrupted, reference, result["candidate"],
        )
    except AssertionError as error:
        assert str(error).startswith("FIRST_DIVERGENCE loop1.S1[89]"), str(error)
    else:
        raise AssertionError("Incorrect S-box word was accepted")


def main() -> None:
    """Verify stock and standalone OpenCL kernels on CPU for both optimizers."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc", help="Native GCC executable")
    args = parser.parse_args()

    assert sys.byteorder == "little", (
        "Native CPU readback requires little-endian uint32 storage"
    )

    reference = json.loads(REFERENCE.read_text(encoding="utf-8"))
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    assert hashlib.sha256(PROBE_KERNEL.read_bytes()).hexdigest() == (
        manifest["standalone_probe_source_sha256"]
    ), "Standalone probe OpenCL source differs from the archived test"
    assert hashlib.sha256(STOCK_KERNEL.read_bytes()).hexdigest() == (
        manifest["stock_kernel_sha256"]
    ), "Production bcrypt OpenCL source differs from the known tested version"
    assert hashlib.sha256(
        (ROOT / "source/OpenCL/inc_cipher_blowfish.cl").read_bytes()
    ).hexdigest() == reference["source_sha256"]
    assert '#include "m03200-stage-probe.cl"' in PROBE_SOURCE.read_text(
        encoding="utf-8",
    )
    assert '#include "m03200-pure.cl"' in STOCK_SOURCE.read_text(
        encoding="utf-8",
    )

    comparator = runpy.run_path(str(COMPARATOR))
    converter = runpy.run_path(str(CONVERTER))
    recorded: dict[str, dict] = {}

    with tempfile.TemporaryDirectory(prefix="m03200-source-cpu-") as path:
        directory = Path(path)

        for optimization in ("-O0", "-O2"):
            stock_exe = compile_emulation(
                args.cc, optimization, directory, STOCK_SOURCE,
            )
            probe_exe = compile_emulation(
                args.cc, optimization, directory, PROBE_SOURCE,
            )

            for candidate in ("hashcat", "hashcat2"):
                with tempfile.TemporaryDirectory(
                    prefix=f"stock-{candidate}-", dir=directory,
                ) as stock_path, tempfile.TemporaryDirectory(
                    prefix=f"probe-{candidate}-", dir=directory,
                ) as probe_path:
                    stock_dir = Path(stock_path)
                    probe_dir = Path(probe_path)
                    stock_output = checked_run([
                        str(stock_exe), candidate, str(stock_dir),
                    ])
                    probe_output = checked_run([
                        str(probe_exe), candidate, str(probe_dir),
                    ])

                    stock = read_native_stages(
                        stock_dir, candidate, stock_output, converter, stock=True,
                    )
                    probe = read_native_stages(
                        probe_dir, candidate, probe_output, converter, stock=False,
                    )
                    compare_native_stages(
                        stock, reference, comparator, stock=True,
                    )
                    compare_native_stages(
                        probe, reference, comparator, stock=False,
                    )
                    check_corrupted_stage(stock, reference, comparator)
                    check_corrupted_stage(probe, reference, comparator)

                    for stage in STAGES:
                        assert (
                            (stock_dir / f"m03200-{stage}.bin").read_bytes()
                            == (probe_dir / f"m03200-{stage}.bin").read_bytes()
                        ), f"STOCK_PROBE_STAGE_DIFFERENCE {candidate} {stage}"

                    if optimization == "-O0":
                        recorded[candidate] = probe
                    else:
                        assert recorded[candidate] == probe, (
                            "CPU output differs between -O0 and -O2", candidate,
                        )

                    print(
                        f"PASS stock/probe OpenCL CPU {optimization}: "
                        f"{candidate}, three identical 4240-byte stages "
                        "and six correct ciphertext words"
                    )

    assert recorded["hashcat"]["comp_words_full"] != (
        recorded["hashcat2"]["comp_words_full"]
    )
    print("PASS original m03200_init/loop, standalone init/loop/comp and CPU reference")
    print("NO GPU OR OPENCL DRIVER EXECUTION WAS PERFORMED")


if __name__ == "__main__":
    main()
