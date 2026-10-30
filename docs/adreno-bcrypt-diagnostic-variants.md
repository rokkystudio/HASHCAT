# Adreno bcrypt (3200): compiler and S-box storage diagnostics (2026-10-30)

## Scope

This report records two isolated RedMagic NX709S / Qualcomm Adreno OpenCL 3.0 diagnostics for the existing public bcrypt self-test vector (`m03200.hash`). Neither case produced a recovered candidate, and neither case establishes bcrypt correctness on Adreno.

The stock hashcat runtime, ordinary modules, and application OpenCL files were read through symlinks rather than overwritten. Experimental modules and OpenCL sources lived in test-specific app-cache stages and temporary PC directories. The original baseline and one-thread self-test failures are covered by `docs/redmagic-gpu-selected-modes.md`.

## Results

| Variant | GPU observation | Device exit |
| --- | --- | --- |
| Stock mode 3200, pure kernel | Kernel self-test returned no match | 245 |
| Stock mode 3200, `--kernel-threads 1` | Kernel self-test returned no match | 245 |
| `-cl-opt-disable` added to the mode-3200 JIT options | Qualcomm LLVM compiler crashed with SIGSEGV during `llvm::QGPUSplitCallGraphPass`; the self-test never began | 139 |
| S-boxes moved from `__local` to the existing per-candidate `__global` temporary buffer, with helper definitions and declarations using matching pointer address spaces | OpenCL initialization completed; the normal hashcat self-test then failed | 245 |

The `-cl-opt-disable` diagnostic used `-D FIXED_LOCAL_SIZE=8 -cl-opt-disable`. The crash buffer identifies `/vendor/lib64/libllvm-qcom.so`, `llvm::CloneFunctionInto`, and `llvm::QGPUSplitCallGraphPass::runOnModule`. Exit 139 corresponds to termination by signal 11. It is a compiler crash rather than a bcrypt comparison result.

An earlier global-S-box attempt mixed `__global` pointers in the kernel with `__local` helper declarations and therefore received OpenCL address-space errors. That attempt cannot test global S-box correctness. The retained global-S-box result above uses aligned `__global` pointer types in `m03200-pure.cl`, `inc_cipher_blowfish.cl`, and `inc_cipher_blowfish.h`, and reaches `Starting self-test` before exit 245.

The global-S-box failure means a straightforward transfer of the S-box working arrays to global memory is not sufficient to produce a correct result. It does not isolate the cause between Blowfish intermediate calculations, host buffer handling, GPU execution, and digest comparison. Normal application kernels and self-test checks are not modified.

## Reproducibility and evidence

The following archived inputs and device evidence appear under `tests/fixtures/android-gpu/redmagic-selected-modes-20261030/`:

- `m03200.hash`: the same public hashcat bcrypt vector used on Pixel;
- `m03200-cl-opt-disable-failure.exit`, `.log`, and `.crash.txt`: exit 139, compiler diagnostic message, and Android crash-buffer stack;
- `m03200-global-scratch-failure.exit` and `.log`: exit 245 after OpenCL kernel initialization and the standard self-test;
- `m03200-diagnostic-variants.json`: device identity, source/module/frontend SHA-256 values, selected flags, and expected statuses.

The global-memory sources were experimental scratch files rather than production code. Their individual SHA-256 values were calculated on the PC and compared to the staged OpenCL files on the device. The original kernel and module SHA-256 values are retained in the selected-mode manifest. The `-cl-opt-disable` module was a diagnostic build, not a production artifact.

Run the archive-only regression:

```text
python tools/test-redmagic-bcrypt-diagnostics.py
```

This test validates the exit codes, raw compiler crash signature, presence or absence of the normal GPU self-test phase, and reported source identity. It does not execute hashcat or infer that any bcrypt kernel is correct.

A separate `noinline` Blowfish-helper variant was prepared but **not executed**, so there is no outcome to report for it.

## Next technical steps

To locate the first incorrect bcrypt computation, compare reference and GPU intermediate state after the initialization, loop, and compare phases using read-only diagnostic capture. Distinguish an incorrect Blowfish result from a correct result rejected by the comparison path, and require the unchanged built-in self-test to pass before accepting any proposed kernel implementation.

Do not disable the self-test to claim success. The published application binary remains out of scope for these diagnostics.
