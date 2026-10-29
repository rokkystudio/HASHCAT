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
    parser.add_argument("--opencl-clang", help="Optionally syntax-check real OpenCL source and includes with Clang")
    parser.add_argument("--baseline-revision", help="Optionally compare normal kernel bodies with a stock Git revision")
    args = parser.parse_args()
    source_path = Path(__file__).resolve().parents[1] / "source/OpenCL/m22000-pure.cl"
    src = source_path.read_text(encoding="utf-8")
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

    if args.baseline_revision:
        # Confirm that other vendors (where no split flags are set) retain the
        # same seven entrypoint bodies as the unmodified upstream version.
        path_in_git = "source/OpenCL/m22000-pure.cl"
        proc = subprocess.run(
            ["git", "show", f"{args.baseline_revision}:{path_in_git}"],
            cwd=source_path.parents[2], capture_output=True, text=True, timeout=30,
        )
        assert proc.returncode == 0, f"cannot load baseline: {proc.stderr[:500]}"
        baseline = proc.stdout
        begin = baseline.index("#ifdef KERNEL_STATIC")
        end = baseline.index("#endif", begin) + len("#endif")
        baseline = baseline[:begin] + "/* test: omit common includes */" + baseline[end:]
        baseline = re.sub(r"(?m)^\s*#include\s+COMPARE_[SM]\s*$", "", baseline)
        proc = subprocess.run(
            [args.cpp, "-E", "-P", "-x", "c", "-"],
            input=baseline, capture_output=True, text=True, timeout=30,
        )
        assert proc.returncode == 0, f"baseline preprocessor failed: {proc.stderr[:500]}"
        original = kernels(proc.stdout)
        assert set(original) == NAMES, f"baseline entrypoints differ: {set(original)}"
        for name, body in original.items():
            assert results["normal"][name] == body, f"non-Adreno kernel body changed: {name}"
        print("PASS non-Adreno kernel bodies match stock baseline")

    if args.opencl_clang:
        # Unlike the guard-only test above, this checks the actual OpenCL
        # translation unit with all of its hashcat includes and declarations.
        # It cannot validate Qualcomm runtime linking or WPA output values.
        include_dir = source_path.parent.as_posix()
        flags = [
            "-x", "cl", "-fsyntax-only", "-cl-std=CL3.0", "-Wno-unknown-attributes",
            f"-I{include_dir}", "-DKERNEL_STATIC", f"-DINCLUDE_PATH={include_dir}",
            "-DXM2S(x)=#x", "-DM2S(x)=XM2S(x)",
            "-DLOCAL_MEM_TYPE=1", "-DVENDOR_ID=2147483648", "-DVECT_SIZE=1",
            "-DDEVICE_TYPE=4", "-DDGST_R0=0", "-DDGST_R1=1", "-DDGST_R2=2",
            "-DDGST_R3=3", "-DDGST_ELEM=4", "-DKERN_TYPE=22000",
            "-DATTACK_EXEC=10", "-DATTACK_KERN=3", "-DATTACK_MODE=3",
        ]
        for name, (defines, _) in cases.items():
            if name == "both":
                continue  # intentionally no entrypoints when both flags are defined
            cmd = [args.opencl_clang, *flags, *["-D" + v for v in defines], str(source_path)]
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
            assert proc.returncode == 0, f"{name}: OpenCL syntax check failed:\n{proc.stderr[:2500]}"
            print(f"PASS OpenCL syntax ({name})")


if __name__ == "__main__":
    main()
