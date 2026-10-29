#!/usr/bin/env python3
"""CPU-only WPA-PBKDF2 regression test using hashcat's own SHA1 emulator.

Extracts the exact m22000_init_block and hmac_sha1_run_V helpers from the
OpenCL source, compiles them with hashcat's portable CPU emulation code,
and compares initial HMAC blocks and complete 4096-round PBKDF2 outputs
with independent Python standard-library references.

Does NOT execute the OpenCL kernels or validate a GPU driver.
Requires a native GCC toolchain (MinGW GCC on Windows).
"""

import argparse
import hashlib
import hmac
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
CL = ROOT / "source/OpenCL/m22000-pure.cl"
TEMPLATE = ROOT / "tools/m22000-pbkdf2-emu.c.in"

SOURCES = [
    "emu_inc_hash_sha1",
    "emu_inc_platform",
    "emu_inc_common",
    "emu_general",
    "bitops",
]
INCLUDES = [
    "source/include",
    "source/OpenCL",
    "source/deps/LZMA-SDK/C",
    "source/deps/zlib",
    "source/deps/zlib/contrib",
    "source/deps/xxHash",
    "source/deps/OpenCL-Headers",
    "source/deps/unrar",
]
VECTORS = [
    ("password", "IEEE"),
    ("hashcat!", "hashcat-essid"),
    ("abcdefgh", "X"),
    ("MySecurePass123", "OpenAI Regression Lab"),
    ("P" * 63, "S" * 32),
]


def extract_helper(source: str, name: str) -> str:
    signature = re.search(
        r"DECLSPEC\s+M22000_NOINLINE\s+void\s+" + re.escape(name) + r"\s*\(",
        source,
    )
    if signature is None:
        raise AssertionError(f"{name}: missing noinline helper in OpenCL source")

    start = source.find("{", signature.end())
    if start < 0:
        raise AssertionError(f"{name}: missing function body")

    depth = 0
    for pos in range(start, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                fragment = source[signature.start():pos + 1]
                fragment = re.sub(
                    r"^DECLSPEC\s+M22000_NOINLINE\s+void\s+",
                    "static void ",
                    fragment,
                    count=1,
                )
                assert fragment.startswith("static void "), name
                return fragment
    raise AssertionError(f"{name}: unbalanced braces")


def checked_run(argv: list[str], *, timeout: int = 120) -> str:
    proc = subprocess.run(
        argv, cwd=ROOT, capture_output=True, text=True, timeout=timeout,
    )
    if proc.returncode:
        raise RuntimeError(
            f"Command failed ({proc.returncode}): {argv[:3]}\n"
            f"{proc.stderr[-2800:]}\n{proc.stdout[-700:]}"
        )
    return proc.stdout


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="gcc", help="Host GCC executable")
    args = parser.parse_args()

    source = CL.read_text(encoding="utf-8")
    helper_code = "\n\n".join([
        extract_helper(source, "hmac_sha1_run_V"),
        extract_helper(source, "m22000_init_block"),
    ])
    template = TEMPLATE.read_text(encoding="utf-8")
    assert template.count("@@HASHCAT_HELPERS@@") == 1
    implementation = template.replace("@@HASHCAT_HELPERS@@", helper_code)

    flags = [
        "-std=gnu99", "-O2", "-ffunction-sections", "-fdata-sections",
        *["-I" + str(ROOT / folder) for folder in INCLUDES],
    ]

    with tempfile.TemporaryDirectory(prefix="m22000-cpu-") as tmp:
        directory = Path(tmp)
        test_source = directory / "test-m22000.c"
        test_source.write_text(implementation, encoding="utf-8", newline="\n")

        objects = []
        for src in SOURCES:
            obj = directory / (src + ".o")
            checked_run([
                args.cc, *flags, "-c",
                str(ROOT / "source/src" / (src + ".c")), "-o", str(obj),
            ])
            objects.append(str(obj))

        test_obj = directory / "test-m22000.o"
        checked_run([args.cc, *flags, "-c", str(test_source), "-o", str(test_obj)])

        binary = directory / ("m22000-cpu.exe" if os.name == "nt" else "m22000-cpu")
        checked_run([
            args.cc, str(test_obj), *objects, "-Wl,--gc-sections", "-o", str(binary),
        ])

        for passphrase, ssid in VECTORS:
            result = checked_run([str(binary), passphrase, ssid], timeout=90)
            found = dict(line.strip().split("=", 1) for line in result.splitlines())
            key = passphrase.encode("ascii")
            salt = ssid.encode("ascii")
            expected = {
                "U1_BLOCK1": hmac.new(
                    key, salt + (1).to_bytes(4, "big"), hashlib.sha1,
                ).hexdigest(),
                "U1_BLOCK2": hmac.new(
                    key, salt + (2).to_bytes(4, "big"), hashlib.sha1,
                ).hexdigest(),
                "PMK": hashlib.pbkdf2_hmac("sha1", key, salt, 4096, 32).hex(),
            }

            assert found == expected, (
                f"CPU emulator mismatch for passphrase/SSID lengths "
                f"{len(key)}/{len(salt)}: {found} != {expected}"
            )
            print(
                f"PASS WPA HMAC U1 blocks + 4096-round PBKDF2 "
                f"(passphrase bytes={len(key)}, SSID bytes={len(salt)})"
            )

    print("PASS all CPU-emulated WPA-PBKDF2 vectors against Python hashlib")


if __name__ == "__main__":
    main()
