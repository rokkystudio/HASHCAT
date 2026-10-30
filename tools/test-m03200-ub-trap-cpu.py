#!/usr/bin/env python3
"""Check the stock bcrypt 3200 CPU path with runtime undefined-behavior traps.

Builds three hashcat native CPU harnesses from their actual OpenCL sources
with GCC -fsanitize=undefined -fsanitize-undefined-trap-on-error. The test
first demonstrates that its compiler flags trap a deliberate signed-overflow
control. It then executes only the public hashcat and hashcat2 test vectors,
verifies every 32-bit stage word, checks all six ciphertext words, and
exercises four stock digest comparison outcomes.

The test does not load an OpenCL runtime, run Android or GPU code, or bypass
any hashcat self-test. A passing result applies only to the tested native CPU
configurations, not to Qualcomm's compiled OpenCL device code.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
FIXTURES = (
    ROOT / "tests" / "fixtures" / "android-gpu"
    / "redmagic-selected-modes-20261030"
)
STAGES = ("init", "loop1", "loop_final")
FIELDS = (("E", 18), ("P", 18), ("S0", 256), ("S1", 256), ("S2", 256), ("S3", 256))
SOURCES = {
    "stock": ROOT / "tools" / "m03200-stock-kernel-cpu.c",
    "probe": ROOT / "tools" / "m03200-stage-probe-cpu.c",
    "comp": ROOT / "tools" / "m03200-stock-comp-cpu.c",
}
INCLUDES = (
    ".",
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
SANITIZER_FLAGS = (
    "-std=gnu11",
    "-fgnu89-inline",
    "-O1",
    "-fsanitize=undefined",
    "-fsanitize-undefined-trap-on-error",
)


def run(args: list[str], timeout: int = 90) -> subprocess.CompletedProcess[str]:
    """Execute a bounded local compiler or native CPU test process."""
    return subprocess.run(
        args, cwd=ROOT, capture_output=True, text=True, timeout=timeout,
    )


def require_success(args: list[str], timeout: int = 90) -> str:
    """Return stdout when the process succeeds, preserving errors on failure."""
    process = run(args, timeout)

    if process.returncode != 0:
        raise AssertionError(
            f"Native CPU command exited {process.returncode}: {args}\n"
            f"stdout:\n{process.stdout[-3000:]}\n"
            f"stderr:\n{process.stderr[-6000:]}"
        )

    return process.stdout


def verify_trap_control(cc: str, directory: Path) -> None:
    """Prove that the selected toolchain traps a deliberately overflowing int."""
    source = directory / "ub-trap-positive-control.c"
    binary = directory / (
        "ub-trap-positive-control" + (".exe" if os.name == "nt" else "")
    )

    source.write_text(
        "#include <limits.h>\n"
        "int main (void)\n"
        "{\n"
        "  volatile int a = INT_MAX;\n"
        "  volatile int b = a + 1;\n"
        "  return b == 0;\n"
        "}\n",
        encoding="utf-8",
    )

    require_success([
        cc, *SANITIZER_FLAGS, str(source), "-o", str(binary),
    ], timeout=60)
    process = run([str(binary)], timeout=20)

    assert process.returncode != 0, (
        "UBSan trap positive control did not reject signed integer overflow"
    )
    print("PASS UBSan positive control: signed overflow terminates the process")


def build_harness(cc: str, source: Path, directory: Path) -> Path:
    """Compile an original hashcat source harness with native UBSan traps."""
    assert source in SOURCES.values()
    binary = directory / (
        f"ubsan-{source.stem}" + (".exe" if os.name == "nt" else "")
    )

    flags = [
        *SANITIZER_FLAGS,
        "-ffunction-sections",
        "-fdata-sections",
        "-Wall",
        "-Wextra",
        "-Werror",
        "-Wno-unused-function",
        "-Wno-unused-parameter",
        "-Wno-unused-variable",
        "-Wno-unused-but-set-variable",
        *["-I" + str(ROOT / path) for path in INCLUDES],
    ]
    require_success([
        cc,
        *flags,
        str(source),
        str(ROOT / "source" / "src" / "bitops.c"),
        "-Wl,--gc-sections",
        "-o",
        str(binary),
    ], timeout=150)

    print(f"PASS UBSan instrumented compilation: {source.name}")
    return binary


def decode_stage(raw: bytes, converter: dict) -> dict:
    """Decode all 1060 native little-endian words in one bcrypt stage."""
    words = converter["decode_words"](raw, 1060)
    result = {}
    offset = 0

    for field, count in FIELDS:
        result[field] = words[offset:offset + count]
        offset += count

    assert offset == 1060
    return result


def verify_candidate(
    candidate: str,
    executables: dict[str, Path],
    directory: Path,
    reference: dict,
    converter: dict,
    comparator: dict,
) -> None:
    """Verify the instrumented stock/probe stages and all stock comparisons."""
    stock = directory / "stock"
    probe = directory / "probe"

    stock.mkdir()
    probe.mkdir()

    stock_log = require_success([
        str(executables["stock"]), candidate, str(stock),
    ])
    probe_log = require_success([
        str(executables["probe"]), candidate, str(probe),
    ])

    assert "CPU_EMULATED_STOCK_BCRYPT_COMPLETE" in stock_log
    assert "CPU_EMULATED_OPENCL_PROBE_COMPLETE" in probe_log

    records = {"stages": {}}

    for stage in STAGES:
        filename = f"m03200-{stage}.bin"
        stock_bytes = (stock / filename).read_bytes()
        probe_bytes = (probe / filename).read_bytes()

        assert len(stock_bytes) == 4240
        assert stock_bytes == probe_bytes, (
            f"UBSan stock/probe state divergence: {candidate}.{stage}"
        )
        records["stages"][stage] = decode_stage(stock_bytes, converter)

    comparator["compare_stage_arrays"](records, reference, candidate)

    ciphertext = converter["decode_words"](
        (probe / "m03200-comp.bin").read_bytes(), 6,
    )
    expected_hex = reference["vectors"][candidate]["final_ciphertext_hex"]
    assert ciphertext == [
        expected_hex[index:index + 8] for index in range(0, 48, 8)
    ], f"UBSan ciphertext differs from public reference: {candidate}"

    for target in ("hashcat", "hashcat2"):
        target_digest = reference["vectors"][target]["final_ciphertext_hex"][:32]
        output = require_success([
            str(executables["comp"]),
            str(stock / "m03200-loop_final.bin"),
            target_digest,
        ])

        pairs = [line.split(" ") for line in output.strip().splitlines()]

        assert all(len(pair) == 2 for pair in pairs)
        values = {key: int(value, 10) for key, value in pairs}
        assert len(values) == len(pairs) == 6

        expected_match = int(candidate == target)

        assert values["MATCH_COUNT"] == expected_match
        assert values["HASHES_SHOWN"] == expected_match

        for field in (
            "PLAIN_GID",
            "PLAIN_SALT_POS",
            "PLAIN_DIGEST_POS",
            "PLAIN_HASH_POS",
        ):
            assert values[field] == 0

        print(
            f"PASS UBSan stock comparison: {candidate} vs {target}, "
            f"matches={expected_match}"
        )

    print(
        f"PASS UBSan bcrypt {candidate}: stock/probe stages and "
        "all six ciphertext words"
    )


def main() -> None:
    """Prove trap instrumentation and validate both public bcrypt candidates."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--cc", default="gcc", help="Native GCC compiler with UBSan trap support",
    )
    args = parser.parse_args()

    assert sys.byteorder == "little", (
        "Exact native uint32 stage representations require little-endian CPU"
    )

    manifest = json.loads(
        (FIXTURES / "m03200-standalone-probe-manifest.json").read_text(
            encoding="utf-8",
        )
    )
    reference = json.loads(
        (FIXTURES / "m03200-cpu-stage-reference.json").read_text(
            encoding="utf-8",
        )
    )

    source_hashes = {
        ROOT / "source" / "OpenCL" / "m03200-pure.cl":
            manifest["stock_kernel_sha256"],
        ROOT / "source" / "OpenCL" / "inc_cipher_blowfish.cl":
            manifest["helper_source_sha256"],
        ROOT / "tools" / "m03200-stage-probe.cl":
            manifest["standalone_probe_source_sha256"],
    }
    for path, expected in source_hashes.items():
        assert hashlib.sha256(path.read_bytes()).hexdigest() == expected, (
            f"Unexpected production or diagnostic source: {path}"
        )

    comparator = runpy.run_path(
        str(ROOT / "tools" / "compare-m03200-gpu-stages.py"),
    )
    converter = runpy.run_path(
        str(ROOT / "tools" / "convert-m03200-stage-readback.py"),
    )

    with tempfile.TemporaryDirectory(prefix="m03200-ubsan-trap-") as path:
        directory = Path(path)
        verify_trap_control(args.cc, directory)

        executables = {
            name: build_harness(args.cc, source, directory)
            for name, source in SOURCES.items()
        }
        for candidate in ("hashcat", "hashcat2"):
            candidate_path = directory / candidate
            candidate_path.mkdir()

            verify_candidate(
                candidate,
                executables,
                candidate_path,
                reference,
                converter,
                comparator,
            )

    print("PASS CPU bcrypt 3200 UBSan trap, full positive/negative reference set")
    print("NO GPU EXECUTION OR QUALCOMM COMPILATION WAS PERFORMED")


if __name__ == "__main__":
    main()
