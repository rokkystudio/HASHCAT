#!/usr/bin/env python3
"""Verify the stock bcrypt OpenCL kernel ABI and local-memory declaration.

This offline regression compiles the unmodified m03200-pure.cl with Clang's
SPIR64 OpenCL target and inspects the resulting LLVM IR. It checks all three
entrypoints, the 25 memory-pointer kernel arguments, address spaces, type
metadata, workgroup-size requirements and four local S-box arrays per stage.

It also checks host OpenCL argument bindings, self-test/restore assignments,
and the original shared kernel_param_t, pw_t, and salt_t declarations using
native C compilation on Windows and cross-compilation for Android ARM64.
Optimized SPIR64 compilation is audited for the same kernel interface,
per-stage local S-box storage and compiler-added address qualifiers.

The regression uses no Android driver, OpenCL ICD, GPU, hashcat runtime,
credential material or device execution. SPIR64 metadata is not evidence
that Qualcomm's compiler produces identical device behavior.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[1]
STAGES = ("m03200_init", "m03200_loop", "m03200_comp")
SBOXES = ("S0", "S1", "S2", "S3")
LOCAL_SIZES = (1, 8)
OPENCL_STANDARDS = ("CL1.2", "CL3.0")
OPTIMIZATIONS = ("O0", "O2")
ARG_TYPES = (
    "pw_t*",
    "kernel_rule_t*",
    "pw_t*",
    "bf_t*",
    "bcrypt_tmp_t*",
    "void*",
    "u32*", "u32*", "u32*", "u32*",
    "u32*", "u32*", "u32*", "u32*",
    "plain_t*",
    "digest_t*",
    "u32*",
    "salt_t*",
    "void*",
    "u32*",
    "void*", "void*", "void*", "void*",
    "kernel_param_t*",
)
ARG_ADDRESSES = (1, 1, 1, 2, *([1] * 21))
HOST_BINDINGS = (
    "opencl_d_pws_buf",
    "opencl_d_rules_c",
    "opencl_d_combs_c",
    "opencl_d_bfs_c",
    "opencl_d_tmps",
    "opencl_d_hooks",
    "opencl_d_bitmap_s1_a",
    "opencl_d_bitmap_s1_b",
    "opencl_d_bitmap_s1_c",
    "opencl_d_bitmap_s1_d",
    "opencl_d_bitmap_s2_a",
    "opencl_d_bitmap_s2_b",
    "opencl_d_bitmap_s2_c",
    "opencl_d_bitmap_s2_d",
    "opencl_d_plain_bufs",
    "opencl_d_digests_buf",
    "opencl_d_digests_shown",
    "opencl_d_salt_bufs",
    "opencl_d_esalt_bufs",
    "opencl_d_result",
    "opencl_d_extra0_buf",
    "opencl_d_extra1_buf",
    "opencl_d_extra2_buf",
    "opencl_d_extra3_buf",
    "opencl_d_kernel_param",
)
FIXTURES = (
    ROOT / "tests" / "fixtures" / "android-gpu"
    / "redmagic-selected-modes-20261030"
)


def verify_source_identity() -> None:
    """Require the original archived stock kernel and Blowfish helper bytes."""
    manifest = json.loads(
        (FIXTURES / "m03200-standalone-probe-manifest.json").read_text(
            encoding="utf-8",
        ),
    )
    sources = (
        (
            ROOT / "source" / "OpenCL" / "m03200-pure.cl",
            manifest["stock_kernel_sha256"],
        ),
        (
            ROOT / "source" / "OpenCL" / "inc_cipher_blowfish.cl",
            manifest["helper_source_sha256"],
        ),
    )

    for source, digest in sources:
        assert hashlib.sha256(source.read_bytes()).hexdigest() == digest, (
            f"Unexpected archived OpenCL source identity: {source}"
        )


def verify_host_bindings() -> None:
    """Check the indexed OpenCL bindings, later allocations and self-test swaps."""
    assert len(ARG_TYPES) == len(ARG_ADDRESSES) == len(HOST_BINDINGS) == 25

    backend = (ROOT / "source" / "src" / "backend.c").read_text(
        encoding="utf-8",
    )
    selftest = (ROOT / "source" / "src" / "selftest.c").read_text(
        encoding="utf-8",
    )

    first = "device_param->kernel_params[ 0] = NULL; // &device_param->opencl_d_pws_buf;"
    last = "device_param->kernel_params[24] = &device_param->opencl_d_kernel_param;"

    assert backend.count(first) == 1
    assert backend.count(last) == 1

    start = backend.index(first)
    end = backend.index(last, start) + len(last)
    init_bindings = re.findall(
        r"device_param->kernel_params\[\s*(\d+)\]\s*=\s*"
        r"(NULL|&device_param->opencl_d_\w+);",
        backend[start:end],
    )
    assert len(init_bindings) == 25, (
        f"Backend initializes {len(init_bindings)} OpenCL kernel arguments"
    )

    for index, (raw_index, pointer) in enumerate(init_bindings):
        assert int(raw_index) == index, (
            f"Out-of-order OpenCL kernel binding at index {index}"
        )
        if index in (0, 4, 5):
            assert pointer == "NULL", (index, pointer)
        else:
            assert pointer == f"&device_param->{HOST_BINDINGS[index]}", (
                index, pointer,
            )

    for index in (0, 4, 5):
        later = (
            f"device_param->kernel_params[{index:2d}] = "
            f"&device_param->{HOST_BINDINGS[index]};"
        )
        assert later in backend[end:], f"Dynamic OpenCL binding missing: {later}"

    dispatch = (
        "for (u32 i = 0; i <= 24; i++)\n"
        "    {\n"
        "      if (hc_clSetKernelArg (hashcat_ctx, opencl_kernel, i, "
        "sizeof (cl_mem), device_param->kernel_params[i]) == -1) return -1;"
    )
    assert dispatch in backend, "Host OpenCL kernel argument count/type differs"
    assert (
        "device_param->size_kernel_params, &device_param->kernel_param"
    ) in backend, "The host does not copy the shared kernel_param_t buffer"

    for index, (selftest_name, normal_name) in {
        15: ("opencl_d_st_digests_buf", "opencl_d_digests_buf"),
        17: ("opencl_d_st_salts_buf", "opencl_d_salt_bufs"),
        18: ("opencl_d_st_esalts_buf", "opencl_d_esalt_bufs"),
    }.items():
        selftest_pointer = (
            f"device_param->kernel_params[{index}] = "
            f"&device_param->{selftest_name};"
        )
        restore_pointer = (
            f"device_param->kernel_params[{index}] = "
            f"&device_param->{normal_name};"
        )
        assert selftest_pointer in selftest
        assert restore_pointer in selftest
        assert selftest.index(selftest_pointer) < selftest.index(restore_pointer)

    print("PASS host OpenCL 25-argument bindings and self-test buffer switching")


def verify_native_host_layout(
    cc: str, clang: Path, android_sysroot: Path, directory: Path,
) -> None:
    """Check original shared structures on Windows x86-64 and Android ARM64."""
    fields = {
        "kernel_param_t": (
            72,
            (
                ("bitmap_mask", 0),
                ("bitmap_shift1", 4),
                ("bitmap_shift2", 8),
                ("salt_pos_host", 12),
                ("loop_pos", 16),
                ("loop_cnt", 24),
                ("il_cnt", 32),
                ("digests_cnt", 40),
                ("digests_offset_host", 44),
                ("combs_mode", 48),
                ("salt_repeat", 52),
                ("pws_pos", 56),
                ("gid_max", 64),
            ),
        ),
        "pw_t": (
            260,
            (
                ("i", 0),
                ("pw_len", 256),
            ),
        ),
        "salt_t": (
            572,
            (
                ("salt_buf", 0),
                ("salt_buf_pc", 256),
                ("salt_len", 512),
                ("salt_len_pc", 516),
                ("salt_iter", 520),
                ("salt_iter2", 524),
                ("salt_dimy", 528),
                ("salt_sign", 532),
                ("salt_repeats", 540),
                ("orig_pos", 544),
                ("digests_cnt", 548),
                ("digests_done", 552),
                ("digests_offset", 556),
                ("scrypt_N", 560),
                ("scrypt_r", 564),
                ("scrypt_p", 568),
            ),
        ),
    }
    source = directory / "native-host-layout.c"
    lines = [
        '#include "common.h"',
        '#include "types.h"',
        "#include <stddef.h>",
        "#define CHECK_FIELD(type, field, offset) "
        '_Static_assert (offsetof (type, field) == (offset), #type "." #field)',
    ]

    for name, (size, field_offsets) in fields.items():
        lines.append(
            f'_Static_assert (sizeof ({name}) == {size}, "{name} size");'
        )

        for field, offset in field_offsets:
            lines.append(f"CHECK_FIELD ({name}, {field}, {offset});")

    source.write_text("\n".join(lines) + "\n", encoding="utf-8")

    includes = [
        "-I" + str(ROOT / path)
        for path in (
            "source/include",
            "source/OpenCL",
            "source/deps/LZMA-SDK/C",
            "source/deps/zlib",
            "source/deps/zlib/contrib",
            "source/deps/xxHash",
            "source/deps/OpenCL-Headers",
            "source/deps/unrar",
        )
    ]
    common_args = [
        "-std=gnu11",
        "-fsyntax-only",
        "-Werror",
        *includes,
        str(source),
    ]
    configurations = (
        ("native host", [cc, *common_args]),
        (
            "Android AArch64 API 28",
            [
                str(clang),
                "--target=aarch64-linux-android28",
                f"--sysroot={android_sysroot}",
                *common_args,
            ],
        ),
    )

    for platform, command in configurations:
        process = subprocess.run(
            command, cwd=ROOT, capture_output=True, text=True, timeout=75,
        )
        assert process.returncode == 0, (
            f"{platform} host ABI differs from the stock OpenCL layout:\n"
            f"{process.stderr[-5000:]}"
        )
        assert not process.stderr.strip(), process.stderr

        print(
            f"PASS {platform}: kernel_param_t 72 bytes, pw_t 260 bytes, "
            f"salt_t 572 bytes, all 31 verified field offsets"
        )


def metadata(ir: str, number: int) -> str:
    """Read one precise numeric LLVM metadata record from emitted SPIR64 IR."""
    match = re.search(rf"^!{number} = (!\{{[^\n]*\}})$", ir, re.MULTILINE)
    assert match is not None, f"Missing LLVM metadata node !{number}"

    return match.group(1)


def require_metadata(metadata_record: str, values: tuple, kind: str) -> None:
    """Require the exact ordered sequence of typed LLVM metadata values."""
    if kind == "integer":
        expected = "!{" + ", ".join(f"i32 {value}" for value in values) + "}"
    elif kind == "string":
        expected = "!{" + ", ".join(f'!"{value}"' for value in values) + "}"
    else:
        raise ValueError(f"Unknown metadata kind: {kind}")

    assert metadata_record == expected, (
        f"LLVM {kind} metadata differs from stock ABI:\n"
        f"actual:   {metadata_record}\n"
        f"expected: {expected}"
    )


def verify_spir_ir(ir: str, local_size: int, optimization: str) -> dict:
    """Check unoptimized or optimized SPIR64 kernel metadata and S-box storage."""
    assert optimization in OPTIMIZATIONS
    assert 'target triple = "spir64"' in ir

    expected_types = {
        "%struct.kernel_param": (
            "%struct.kernel_param = type { "
            "i32, i32, i32, i32, i64, i64, i64, "
            "i32, i32, i32, i32, i64, i64 }"
        ),
        "%struct.bcrypt_tmp": (
            "%struct.bcrypt_tmp = type { [18 x i32], [18 x i32], "
            "[256 x i32], [256 x i32], [256 x i32], [256 x i32] }"
        ),
        "%struct.pw": "%struct.pw = type { [64 x i32], i32 }",
        "%struct.salt": (
            "%struct.salt = type { [64 x i32], [64 x i32], "
            "i32, i32, i32, i32, i32, [2 x i32], "
            "i32, i32, i32, i32, i32, i32, i32, i32 }"
        ),
    }

    for type_name, definition in expected_types.items():
        if optimization == "O0" or f"{type_name} = type " in ir:
            assert definition in ir, (
                f"SPIR64 {type_name} layout differs under -{optimization}"
            )

    expected_type_quals = tuple(
        "const" if index in (1, 2, 3, *range(6, 14), 15, 17, 18, 24) else ""
        for index in range(25)
    )
    stages = {}

    for stage in STAGES:
        entry = re.search(
            rf"^define [^\n]*\bspir_kernel void @{stage}"
            r"\((.*)\) (?:local_unnamed_addr )?#\d+ "
            r"!kernel_arg_addr_space !(\d+) "
            r"!kernel_arg_access_qual !\d+ "
            r"!kernel_arg_type !(\d+) "
            r"!kernel_arg_base_type !\d+ "
            r"!kernel_arg_type_qual !(\d+) "
            r"!reqd_work_group_size !(\d+) \{$",
            ir,
            re.MULTILINE,
        )
        assert entry is not None, (
            f"Missing SPIR64 kernel signature under -{optimization}: {stage}"
        )
        signature, address_id, type_id, qual_id, group_id = entry.groups()

        actual_pointer_spaces = tuple(
            int(item)
            for item in re.findall(r"\bptr addrspace\((\d+)\)", signature)
        )
        assert actual_pointer_spaces == ARG_ADDRESSES, (
            f"SPIR64 pointer arguments differ in {stage} under -{optimization}"
        )
        require_metadata(metadata(ir, int(address_id)), ARG_ADDRESSES, "integer")
        require_metadata(metadata(ir, int(type_id)), ARG_TYPES, "string")
        require_metadata(
            metadata(ir, int(qual_id)), expected_type_quals, "string",
        )
        require_metadata(
            metadata(ir, int(group_id)), (local_size, 1, 1), "integer",
        )

        sbox_address_attributes = {}

        for box in SBOXES:
            pattern = (
                rf"^@{stage}\.{box}_all = internal "
                r"(?P<unnamed>unnamed_addr )?"
                rf"addrspace\(3\) global "
                rf"\[{local_size} x \[256 x i32\]\] undef, align 4$"
            )
            storage = re.search(pattern, ir, re.MULTILINE)
            assert storage is not None, (
                f"Expected 256-word per-work-item local {box} array "
                f"in stage {stage} with -{optimization}, group {local_size}"
            )

            symbol = f"@{stage}.{box}_all"
            assert ir.count(symbol) >= 2, (
                f"Local {box} array is not referenced in {stage} "
                f"under -{optimization}"
            )
            sbox_address_attributes[box] = (
                "unnamed_addr" if storage.group("unnamed") else "default"
            )

        stages[stage] = {
            "argument_count": 25,
            "argument_address_spaces": list(ARG_ADDRESSES),
            "required_work_group_size": [local_size, 1, 1],
            "local_sbox_bytes": local_size * 4096,
            "sbox_address_attributes": sbox_address_attributes,
        }

    unnamed = {
        stage: [
            box for box, attr in details["sbox_address_attributes"].items()
            if attr == "unnamed_addr"
        ]
        for stage, details in stages.items()
    }
    print(
        f"PASS -{optimization} SPIR64 ABI: 25 arguments per stage, "
        f"12 referenced local S-box arrays, {local_size * 4096} bytes "
        f"per stage, unnamed_addr={unnamed}"
    )

    return stages


def verify_negative_control(ir: str, local_size: int, optimization: str) -> None:
    """Reject incorrect local-memory address spaces in two kernel stages."""
    for stage in ("m03200_init", "m03200_comp"):
        pattern = (
            rf"^@{stage}\.S0_all = internal "
            r"(?:unnamed_addr )?addrspace\(3\) global "
            rf"\[{local_size} x \[256 x i32\]\] undef, align 4$"
        )
        match = re.search(pattern, ir, re.MULTILINE)
        assert match is not None, (
            f"Missing local S-box negative-control target in {stage}"
        )

        actual = match.group(0)
        incorrect = actual.replace("addrspace(3)", "addrspace(1)")
        mutated = ir.replace(actual, incorrect, 1)

        assert actual != incorrect
        assert mutated != ir

        try:
            verify_spir_ir(mutated, local_size, optimization)
        except AssertionError:
            print(
                f"PASS -{optimization} SPIR64 negative control: "
                f"{stage} global-memory S-box rejected"
            )
        else:
            raise AssertionError(
                f"SPIR64 ABI audit accepted a global-memory S-box in {stage}"
            )


def compile_spir(
    clang: Path, wrapper: Path, standard: str, local_size: int,
    optimization: str, output: Path,
) -> str:
    """Compile stock OpenCL into SPIR64 LLVM IR with an explicit optimization."""
    assert standard in OPENCL_STANDARDS
    assert local_size in LOCAL_SIZES
    assert optimization in OPTIMIZATIONS

    command = [
        str(clang),
        "-target", "spir64",
        "-x", "cl",
        f"-cl-std={standard}",
        f"-{optimization}",
        "-Werror",
        "-S",
        "-emit-llvm",
        "-o", str(output),
        "-I", str(ROOT),
        "-I", str(ROOT / "source" / "OpenCL"),
        "-D", "VECT_SIZE=1",
        "-D", "DEVICE_TYPE=4",
        "-D", "VENDOR_ID=2147483648",
        "-D", "LOCAL_MEM_TYPE=1",
        "-D", "ATTACK_MODE=0",
        "-D", "ATTACK_EXEC=11",
        "-D", "ATTACK_KERN=3",
        "-D", "DGST_ELEM=6",
        "-D", "DGST_R0=0",
        "-D", "DGST_R1=1",
        "-D", "DGST_R2=2",
        "-D", "DGST_R3=3",
        f"-DFIXED_LOCAL_SIZE={local_size}",
        str(wrapper),
    ]
    process = subprocess.run(
        command, cwd=ROOT, capture_output=True, text=True, timeout=120,
    )
    assert process.returncode == 0, (
        f"SPIR64 compilation failed: {standard} / {local_size} "
        f"/ -{optimization}\n"
        f"{process.stderr[-8000:]}"
    )
    assert not process.stderr.strip(), process.stderr

    return output.read_text(encoding="utf-8")


def main() -> None:
    """Audit native host ABI and both unoptimized and optimized SPIR64 kernels."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--clang", type=Path, required=True,
        help="Explicit path to a host Clang executable with SPIR64 support",
    )
    parser.add_argument(
        "--cc", default="gcc",
        help="Native C compiler for the shared host ABI structures",
    )
    parser.add_argument(
        "--android-sysroot", type=Path, required=True,
        help="Explicit Android NDK sysroot directory for AArch64 compilation",
    )
    parser.add_argument(
        "--report", type=Path,
        help="Optional path for a machine-readable, offline-only JSON IR report",
    )
    options = parser.parse_args()

    clang = options.clang.resolve(strict=True)
    android_sysroot = options.android_sysroot.resolve(strict=True)

    assert clang.is_file()
    assert android_sysroot.is_dir()

    compiler_version = subprocess.run(
        [str(clang), "--version"],
        cwd=ROOT, capture_output=True, text=True, timeout=20,
    )
    assert compiler_version.returncode == 0, compiler_version.stderr
    assert compiler_version.stdout.strip()

    verify_source_identity()
    verify_host_bindings()

    source = ROOT / "source" / "OpenCL" / "m03200-pure.cl"
    helper = ROOT / "source" / "OpenCL" / "inc_cipher_blowfish.cl"
    configurations = []

    with tempfile.TemporaryDirectory(prefix="bcrypt-spir64-contract-") as tmp:
        directory = Path(tmp)

        verify_native_host_layout(
            options.cc, clang, android_sysroot, directory,
        )

        wrapper = directory / "bcrypt-source.cl"

        wrapper.write_text(
            "/* Stock OpenCL bcrypt ABI audit; no device calls. */\n"
            "#define KERNEL_STATIC\n"
            "#define INCLUDE_PATH source/OpenCL\n"
            "#define STRINGIFY(x) #x\n"
            "#define M2S(x) STRINGIFY(x)\n"
            '#include "source/OpenCL/m03200-pure.cl"\n',
            encoding="utf-8",
        )

        for standard in OPENCL_STANDARDS:
            for local_size in LOCAL_SIZES:
                for optimization in OPTIMIZATIONS:
                    spir = compile_spir(
                        clang, wrapper, standard, local_size, optimization,
                        directory / "m03200.ll",
                    )
                    stages = verify_spir_ir(
                        spir, local_size, optimization,
                    )

                    if standard == "CL1.2" and local_size == 1:
                        verify_negative_control(
                            spir, local_size, optimization,
                        )

                    configurations.append({
                        "opencl_standard": standard,
                        "optimization": optimization,
                        "local_size": local_size,
                        "llvm_ir_sha256": hashlib.sha256(
                            spir.encode("utf-8"),
                        ).hexdigest(),
                        "llvm_ir_bytes": len(spir.encode("utf-8")),
                        "stages": stages,
                    })

                    print(
                        f"PASS OpenCL {standard} SPIR64 -{optimization} "
                        f"ABI, group size {local_size}"
                    )

    if options.report is not None:
        report = options.report.resolve()
        report.parent.mkdir(parents=True, exist_ok=True)
        report.write_text(
            json.dumps(
                {
                    "scope": "offline Clang SPIR64 LLVM IR; no GPU execution",
                    "clang_version": compiler_version.stdout.splitlines()[0],
                    "stock_kernel_sha256": hashlib.sha256(
                        source.read_bytes(),
                    ).hexdigest(),
                    "blowfish_helper_sha256": hashlib.sha256(
                        helper.read_bytes(),
                    ).hexdigest(),
                    "configurations": configurations,
                },
                indent=2,
            ) + "\n",
            encoding="utf-8",
        )
        print(f"PASS offline optimizer diagnostic report: {report}")

    assert len(configurations) == (
        len(OPENCL_STANDARDS) * len(LOCAL_SIZES) * len(OPTIMIZATIONS)
    )

    print(
        f"PASS {len(configurations)} SPIR64 configurations and "
        "unchanged Windows/Android host binding contract"
    )
    print("NO GPU OR QUALCOMM DRIVER EXECUTION PERFORMED")

if __name__ == "__main__":
    main()
