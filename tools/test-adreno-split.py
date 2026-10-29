#!/usr/bin/env python3
"""Check m22000 entrypoint partitioning without requiring a GPU or WPA inputs.

This is a preprocessor/structural test, not a cryptographic correctness test.
"""
import argparse
import re
import subprocess
from pathlib import Path

NAMES = {"init", "loop", "comp", "aux1", "aux2", "aux3", "aux4"}
BASE = {"init", "loop", "comp"}
AUX = NAMES - BASE
ENTRY = re.compile(r"\bvoid\s+m22000_(init|loop|comp|aux[1-4])\s*\(")


def kernels(preprocessed):
    result = {}
    for match in ENTRY.finditer(preprocessed):
        name = match.group(1)
        assert name not in result, f"duplicate entrypoint: {name}"
        start = preprocessed.find("{", match.end())
        assert start != -1, f"body not found: {name}"
        depth = 0
        for i in range(start, len(preprocessed)):
            if preprocessed[i] == "{":
                depth += 1
            elif preprocessed[i] == "}":
                depth -= 1
                if depth == 0:
                    result[name] = re.sub(r"\s+", " ", preprocessed[start:i + 1]).strip()
                    break
        else:
            raise AssertionError(f"unclosed body: {name}")
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cpp", default="clang", help="C preprocessor executable (clang or gcc)")
    args = parser.parse_args()
    src = (Path(__file__).resolve().parents[1] / "source/OpenCL/m22000-pure.cl").read_text(encoding="utf-8")
    start = src.index("#ifdef KERNEL_STATIC")
    stop = src.index("#endif", start) + len("#endif")
    src = src[:start] + "/* test: omit common includes */" + src[stop:]
    src = re.sub(r"(?m)^\s*#include\s+COMPARE_[SM]\s*$", "", src)

    cases = {
        "normal": ([], NAMES),
        "full-noinline": (["M22000_ADRENO_NOINLINE"], NAMES),
        "base": (["M22000_ADRENO_NOINLINE", "M22000_SPLIT_BASE"], BASE),
        "aux": (["M22000_ADRENO_NOINLINE", "M22000_SPLIT_AUX"], AUX),
        "both": (["M22000_ADRENO_NOINLINE", "M22000_SPLIT_BASE", "M22000_SPLIT_AUX"], set()),
    }
    results = {}
    for name, (defines, expected) in cases.items():
        cmd = [args.cpp, "-E", "-P", "-x", "c", *["-D" + v for v in defines], "-"]
        proc = subprocess.run(cmd, input=src, capture_output=True, text=True, timeout=30)
        assert proc.returncode == 0, f"{name}: preprocessor failed: {proc.stderr[:500]}"
        actual = kernels(proc.stdout)
        assert set(actual) == expected, f"{name}: wanted {expected}, got {set(actual)}"
        results[name] = actual
        print(f"PASS {name}: {', '.join(sorted(actual)) or '(none)'}")
    for name, fragment in [("base", BASE), ("aux", AUX)]:
        for item in fragment:
            assert results[name][item] == results["full-noinline"][item], f"{item} body changed in split build"
    print("PASS kernel bodies preserved across split programs")


if __name__ == "__main__":
    main()
