#!/usr/bin/env python3
"""Test the complete stock bcrypt 3200 OpenCL comparison path on native CPU.

The regression compiles the original source/OpenCL/m03200-pure.cl through
two native C harnesses, one producing its exact init/loop temporary buffer
and the other executing its unmodified m03200_comp, inc_comp_multi.cl,
check(), find_hash(), and mark_hash() functions.

Only the public hashcat/hashcat2 synthetic fixtures are exercised. Each
candidate is compared with both expected digests, under GCC -O0 and -O2.
The regression also checks truncated state buffers and malformed digest
arguments. No GPU, Android driver, hashcat search or other credentials run.
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
STOCK = ROOT / "tools" / "m03200-stock-kernel-cpu.c"
COMP = ROOT / "tools" / "m03200-stock-comp-cpu.c"
KERNEL = ROOT / "source" / "OpenCL" / "m03200-pure.cl"
BLOWFISH = ROOT / "source" / "OpenCL" / "inc_cipher_blowfish.cl"
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


def launch(args: list[str], *, timeout: int = 100) -> subprocess.CompletedProcess:
    """Run one native compilation or bounded CPU self-test with captured logs."""
    return subprocess.run(
        args, cwd=ROOT, capture_output=True, text=True, timeout=timeout,
    )


def succeed(args: list[str], *, timeout: int = 100) -> str:
    """Require a zero exit code and retain the compiler or regression error."""
    process = launch(args, timeout=timeout)

    if process.returncode:
        raise RuntimeError(
            f"CPU process exited {process.returncode}: {args}\n"
            f"stdout:\n{process.stdout[-3500:]}\n"
            f"stderr:\n{process.stderr[-8000:]}"
        )

    return process.stdout


def compile_harness(
    cc: str, optimization: str, source: Path, destination: Path,
) -> Path:
    """Compile the real hashcat kernel and comparison functions into native C."""
    suffix = ".exe" if os.name == "nt" else ""
    binary = destination / f"{source.stem}-{optimization[1:]}{suffix}"
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
        *["-I" + str(ROOT / path) for path in INCLUDES],
    ]
    succeed([
        cc, *flags, str(source), str(ROOT / "source/src/bitops.c"),
        "-Wl,--gc-sections", "-o", str(binary),
    ], timeout=120)
    return binary


def verify_stock_stage(
    directory: Path, candidate: str, reference: dict,
    comparator: dict, converter: dict,
) -> None:
    """Verify all three full 1060-word stock-kernel CPU stage snapshots."""
    records = {"stages": {}}

    for name in STAGES:
        raw = (directory / f"m03200-{name}.bin").read_bytes()
        words = converter["decode_words"](raw, 1060)
        arrays = {}
        offset = 0

        for field, count in COUNTS.items():
            arrays[field] = words[offset:offset + count]
            offset += count

        assert offset == 1060
        records["stages"][name] = arrays

    comparator["compare_stage_arrays"](records, reference, candidate)


def verify_comparison(
    executable: Path, stage: Path, expected_digest: str,
    expected_match: int,
) -> None:
    """Require the stock bitmap, digest lookup and marking path to agree."""
    output = succeed([str(executable), str(stage), expected_digest])
    rows = output.strip().splitlines()

    assert len(rows) == 6, f"Unexpected comparison output: {output!r}"

    values = {}

    for row in rows:
        name, value = row.split(" ")
        assert name not in values, f"Duplicate comparison field: {name}"
        values[name] = int(value, 10)

    assert set(values) == {
        "MATCH_COUNT",
        "HASHES_SHOWN",
        "PLAIN_GID",
        "PLAIN_SALT_POS",
        "PLAIN_DIGEST_POS",
        "PLAIN_HASH_POS",
    }, f"Unexpected comparison fields: {values!r}"

    assert values["MATCH_COUNT"] == expected_match
    assert values["HASHES_SHOWN"] == expected_match

    for field in ("PLAIN_GID", "PLAIN_SALT_POS", "PLAIN_DIGEST_POS", "PLAIN_HASH_POS"):
        assert values[field] == 0, (field, values[field])


def verify_rejections(executable: Path, stage: Path, expected_digest: str, tmp: Path) -> None:
    """Reject malformed digests and short or oversized binary stage files."""
    for malformed in (
        expected_digest[:-1],
        expected_digest + "0",
        expected_digest.upper(),
        "g" + expected_digest[1:],
    ):
        if malformed == expected_digest:
            continue

        process = launch([str(executable), str(stage), malformed])

        assert process.returncode == 5, (
            f"Malformed digest was accepted: {malformed!r}, "
            f"exit={process.returncode}, stderr={process.stderr!r}"
        )

    raw = stage.read_bytes()
    assert len(raw) == 4240

    for malformed in (raw[:-1], raw + b"\0"):
        path = tmp / f"invalid-{len(malformed)}.bin"
        path.write_bytes(malformed)
        process = launch([str(executable), str(path), expected_digest])
        assert process.returncode == 4, (
            f"Invalid stage of {len(malformed)} bytes was accepted"
        )


def main() -> None:
    """Test all positive/negative stock comparisons at both optimization levels."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc", help="Native GCC compiler")
    args = parser.parse_args()

    assert sys.byteorder == "little", (
        "The native stock bcrypt structure uses little-endian uint32 words"
    )

    reference = json.loads(REFERENCE.read_text(encoding="utf-8"))
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))

    assert hashlib.sha256(KERNEL.read_bytes()).hexdigest() == (
        manifest["stock_kernel_sha256"]
    )
    assert hashlib.sha256(BLOWFISH.read_bytes()).hexdigest() == (
        manifest["helper_source_sha256"]
    )
    assert '#include "m03200-pure.cl"' in STOCK.read_text(encoding="utf-8")
    assert '#include "m03200-pure.cl"' in COMP.read_text(encoding="utf-8")
    assert '#define KERNEL_STATIC' in COMP.read_text(encoding="utf-8")
    assert "inc_comp_multi.cl" in KERNEL.read_text(encoding="utf-8")

    digests = {
        candidate: reference["vectors"][candidate]["final_ciphertext_hex"][:32]
        for candidate in ("hashcat", "hashcat2")
    }

    assert digests["hashcat"] != digests["hashcat2"]

    comparator = runpy.run_path(str(COMPARATOR))
    converter = runpy.run_path(str(CONVERTER))

    with tempfile.TemporaryDirectory(prefix="m03200-stock-compare-cpu-") as root:
        directory = Path(root)

        for optimization in ("-O0", "-O2"):
            stock = compile_harness(args.cc, optimization, STOCK, directory)
            compare = compile_harness(args.cc, optimization, COMP, directory)

            for candidate in ("hashcat", "hashcat2"):
                candidate_directory = directory / f"{optimization[1:]}-{candidate}"
                candidate_directory.mkdir()

                log = succeed([
                    str(stock), candidate, str(candidate_directory),
                ])

                assert "CPU_EMULATED_STOCK_BCRYPT_COMPLETE" in log
                verify_stock_stage(
                    candidate_directory, candidate, reference,
                    comparator, converter,
                )

                stage = candidate_directory / "m03200-loop_final.bin"

                for target, expected_digest in digests.items():
                    expected_match = int(candidate == target)
                    verify_comparison(
                        compare, stage, expected_digest, expected_match,
                    )
                    print(
                        f"PASS stock m03200_comp {optimization}: "
                        f"{candidate} vs {target} = {expected_match}"
                    )

                verify_rejections(
                    compare, stage, digests["hashcat"], candidate_directory,
                )

    print("PASS stock bcrypt compare: eight positive/negative comparisons")
    print("PASS complete native stock comp, bitmaps, digest lookup and result marking")
    print("NO GPU OR ANDROID HASHCAT ATTACK EXECUTED")


if __name__ == "__main__":
    main()
