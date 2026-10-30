# bcrypt 3200 CPU stage reference and GPU readback contract (2026-10-30)

## Purpose and result

This report defines reproducible intermediate-state reference data for the public hashcat bcrypt $2a$05$ self-test. The selected candidate is hashcat and the negative candidate is hashcat2; the input is tests/fixtures/android-gpu/redmagic-selected-modes-20261030/m03200.hash.

tools/test-m03200-stage-emu.py compiles the complete Blowfish helper implementation from source/OpenCL/inc_cipher_blowfish.cl with a minimal native CPU harness in tools/m03200-stage-emu.c.in. The harness performs the same key expansion, initial salted Blowfish setup, 32 key-schedule iterations, and final ciphertext encryption as the stages in source/OpenCL/m03200-pure.cl. The native CPU harness runs with -O0 and -O2; their intermediate states must match exactly.

The positive candidate generates all 23 digest bytes from the published bcrypt self-test. The negative candidate generates a different digest. These are CPU outcomes only; neither success says that the Adreno GPU implementation passes hashcat's built-in self-test.

## Current CPU checkpoints

Each checkpoint covers the 18-word Blowfish P-array and four 256-word S-box arrays, using numeric 32-bit words in canonical eight-digit hexadecimal format.

| Checkpoint | Boundary | Positive candidate combined-state SHA-256 |
| --- | --- | --- |
| init | Immediately after m03200_init stores P and S-box values in tmps[gid] | bb3e3a7020f2541c4f5a5220a941124f9d61eb9212dbc4e226240d7366314253 |
| loop1 | After the first expansion iteration in m03200_loop | b9f05112fbb289bb1f04e51ddf16bd0f5bca321971722b63d85c2c8a507399a6 |
| loop_final | After all 32 expansion iterations | 206da24f322245de0cd2cca2e27759c764b964d98f296d8fa5bef99f40078bbe |

The combined state digest is SHA-256 of the big-endian byte representations of P, S0, S1, S2 and S3, concatenated in that order. The recorded JSON additionally contains every word of each array, P-words, four sampled S-box words, individual S-box SHA-256 values, expanded key digest, and the six final bcrypt ciphertext words. The last ciphertext byte is not compared as part of bcrypt's 23-byte digest.

The final positive ciphertext starts with cb7e2934bbeb274f0529c5802c2a6c2d and the full 23-byte value equals cb7e2934bbeb274f0529c5802c2a6c2d615abab244010c. The negative candidate is rejected.

## Exact memory layout for GPU observation

The bcrypt_tmp_t structure in m03200-pure.cl has the following unsigned 32-bit fields, with 1060 words (4240 bytes) in total:

| Field | Word offset | Count |
| --- | ---: | ---: |
| E | 0 | 18 |
| P | 18 | 18 |
| S0 | 36 | 256 |
| S1 | 292 | 256 |
| S2 | 548 | 256 |
| S3 | 804 | 256 |

The CPU reference records the P-array and all four S-box arrays (1042 words), plus the SHA-256 digest of the 18-word expanded key E. The comparator derives all 18 expected E words exactly from the synthetic candidate and verifies the recorded CPU digest. A complete device stage therefore compares all 1060 words (4240 bytes). The source is the authority for field offsets and sizes.

A diagnostic build can obtain snapshots through a read-only clEnqueueReadBuffer of the temporary buffer after the m03200_init dispatch, after the first iteration, and after the 32nd iteration. Since a normal self-test may submit several iterations together, obtaining loop1 requires a deliberately segmented 1 + 31 loop dispatch whose cumulative work equals the ordinary 32 iterations. Collect results only from the selected synthetic candidate. Keep hashcat's built-in self-test enabled and do not modify cryptographic comparison semantics.

The standalone probe records all six final ciphertext words in a separate 24-byte buffer. The comparator checks the first four words used by m03200_comp and all six if supplied. The ordinary compare kernel does not store these words into bcrypt_tmp_t.

## GPU dump format and strict comparison

tools/compare-m03200-gpu-stages.py --trace DEVICE_READBACK.json requires device model, ADB serial, mode, cost, salt, all relevant source/input SHA-256 values and the candidate string. The readback_kind field is u32_numeric_hex. Each stages entry (init, loop1, loop_final) contains arrays E, P, S0, S1, S2, S3 with exactly 1060 numeric words. Optional comp_words contains four comparison words; comp_words_full can also contain the complete six-word ciphertext. The comparator tests both when present.

On little-endian Android, convert captured bytes to numeric uint32 values before formatting each word; do not hex-dump raw little-endian memory bytes as though they were big-endian numeric values. The script validates exact metadata and then compares arrays in execution order. The first divergence is reported as FIRST_DIVERGENCE <stage>.<array>[<index>] with expected and observed numeric words.

If init diverges, investigate password/salt decoding and initial Blowfish state. If init matches but loop1 diverges, investigate the first expansion iteration. If both match but loop_final diverges, inspect repeated loop dispatch and temporary-buffer reuse. If all stages match but the built-in hashcat self-test fails, investigate final comp or digest comparison. These branches are conditional diagnostic interpretations, not an assertion about which GPU stage currently fails.

## Verification commands

Run the native CPU reference check (requires GCC):

    python tools/test-m03200-stage-emu.py

Run the comparator's CPU-only mutation checks, which do not claim GPU success:

    python tools/compare-m03200-gpu-stages.py --self-test

Compare an actual device trace only when one has been captured:

    python tools/compare-m03200-gpu-stages.py --trace DEVICE_READBACK.json

No successful device readback is archived as part of this report. The RedMagic bcrypt mode 3200 self-test result remains failed (245); the noinline helper experiment has no device result. Application runtime and production binaries are outside the scope of the CPU reference and standalone diagnostics.


## Standalone OpenCL probe status

An experimental diagnostic has been authored in tools/m03200-stage-probe.cl, using the current inc_cipher_blowfish.cl arithmetic in three isolated kernels. The Android C host in tools/m03200-stage-probe-host.c builds the OpenCL source, dispatches one public known candidate, and requests exact 4240-byte readbacks after initialization, one expansion iteration and all 32 iterations. Its final ciphertext readback is 24 bytes. The runner tools/run-redmagic-bcrypt-stage-probe.sh uses a separate app-cache directory and never overwrites the normal hashcat runtime or disables any hashcat self-test. The standalone probe is not a replacement for the built-in hashcat self-test.

Local Android NDK r29 ARM64 host compilation passed with strict C warnings enabled. The separate OpenCL source passed host Clang syntax validation for CL 1.2 and CL 3.0 using -Werror. These are host compilation results, not confirmation from the Qualcomm OpenCL compiler.

The first launch on RedMagic NX709S terminated before clBuildProgram, with exit 2 and the recorded message OpenCL dlopen: dlopen failed: library "libOpenCL.so" not found. Although /vendor/lib64/libOpenCL.so is visible on the device, the app diagnostic process did not find it by library name in that execution environment. The attempted preparation of another device run was blocked by tool safety controls. No subsequent GPU run occurred, and no bcrypt stage bytes were collected. Evidence and exact staged-source hashes are retained in m03200-standalone-probe-loader-failure.log, its exit file, and m03200-standalone-probe-manifest.json. The executable and source were staged separately from the stock application files.

## Binary readback contract

If valid native readbacks become available, tools/convert-m03200-stage-readback.py accepts exactly three 4240-byte stage binaries and one 24-byte final ciphertext binary, plus a process log containing successful build and completion markers and a zero exit code. It converts little-endian uint32 memory into the numeric hexadecimal arrays required by tools/compare-m03200-gpu-stages.py. It refuses truncated input, nonzero execution status, and overwriting an existing trace. For an arithmetically incorrect GPU output, the output JSON is retained and the comparator identifies the first differing word rather than silently accepting the result. Device provenance must be independently established from the archived runtime evidence; JSON metadata alone cannot prove that a device executed the work.

The binary parser and comparator each have CPU-only tests using deterministic synthetic buffers. The test does not write synthetic files to the archived GPU fixture directory and does not claim GPU execution.

Additional CPU-only validation commands:

    python tools/convert-m03200-stage-readback.py --self-test
    python tools/compare-m03200-gpu-stages.py --self-test
    python tools/test-m03200-stage-probe-evidence.py

A real captured trace is only valid when original binary readbacks and log/exit artifacts exist. The standalone OpenCL build and GPU calculations remain unverified on Adreno, and bcrypt mode 3200 must not be reported as working until hashcat's unchanged built-in self-test and recovery succeed.

## Native CPU equivalence: stock OpenCL kernel versus standalone probe

The native test executes the **actual source files** through hashcat's CPU emulation headers:

- tools/m03200-stock-kernel-cpu.c includes source/OpenCL/m03200-pure.cl verbatim. It passes hashcat's pw_t, salt_t, and bcrypt_tmp_t structures to m03200_init and m03200_loop. The CPU harness adapts only the kernel argument list, the single work-item identity, and the loop-count parameter. The stock m03200_comp comparison body is excluded because it does not expose final ciphertext words without the full comparison runtime.
- tools/m03200-stage-probe-cpu.c includes tools/m03200-stage-probe.cl verbatim. It executes bcrypt_stage_init, bcrypt_stage_loop with 1 then 31 rounds, and bcrypt_stage_comp. Both harnesses use the original source/OpenCL/inc_cipher_blowfish.cl helpers and native, functional atomic helpers for the CPU emulation environment.

tools/test-m03200-stage-probe-cpu.py compiles both entrypoints using MinGW GCC with -O0 and -O2. It compares all three 4240-byte E/P/S-box snapshots byte-for-byte between the two independent entrypoints, and compares all 1060 32-bit words per checkpoint against the archived CPU reference for both known public candidates. All six final ciphertext words from the standalone probe also match the archived reference. The test exercises detection of a deliberately incorrect S-box word.

Results on 2026-10-30:

    PASS stock/probe OpenCL CPU -O0: hashcat
    PASS stock/probe OpenCL CPU -O0: hashcat2
    PASS stock/probe OpenCL CPU -O2: hashcat
    PASS stock/probe OpenCL CPU -O2: hashcat2
    PASS original m03200_init/loop, standalone init/loop/comp and CPU reference

Run the same complete regression:

    python tools/test-m03200-stage-probe-cpu.py

This confirms that the independently staged diagnostic and the original mode-3200 code produce identical intermediate data under the native CPU representation. It does **not** identify the Adreno GPU failure, demonstrate an OpenCL driver build, or establish a successful hashcat recovery on RedMagic.

SHA-256 source identity remains byte stable on both platforms: source/.gitattributes disables text line-ending rewriting for the original hashcat source tree, while the root .gitattributes explicitly retains LF for tools/m03200-stage-probe.cl and the public m03200.hash fixture.

## Stock bcrypt final comparison on native CPU

A separate native CPU harness executes the stock m03200_comp code from source/OpenCL/m03200-pure.cl together with its original inc_comp_multi.cl, check, find_hash and mark_hash functions. The executable source is tools/m03200-stock-comp-cpu.c. Its input is the stock m03200_loop 4240-byte state produced by tools/m03200-stock-kernel-cpu.c, plus four expected numeric digest words from the public reference. It constructs the exact eight one-digest bitmap entries for hashcat's usual shifts 5 and 13, without replacing the comparison functions or disabling bitmap/digest conditions.

tools/test-m03200-stock-comp-cpu.py compiles both native harnesses with GCC -O0 and -O2. For each optimization level, it compares both synthetic CPU-computed candidate states against both synthetic expected digests. The original kernel's result counter, duplicate reporting counter and recorded candidate coordinates are inspected. The same regression requires failure on truncated or oversized 4240-byte states and malformed hexadecimal digest arguments.

Observed 2026-10-30 results:

    -O0 hashcat  vs hashcat   = match
    -O0 hashcat  vs hashcat2  = no match
    -O0 hashcat2 vs hashcat   = no match
    -O0 hashcat2 vs hashcat2  = match
    -O2 hashcat  vs hashcat   = match
    -O2 hashcat  vs hashcat2  = no match
    -O2 hashcat2 vs hashcat   = no match
    -O2 hashcat2 vs hashcat2  = match

Run the complete comparison regression:

    python -B tools/test-m03200-stock-comp-cpu.py

This further confirms the CPU correctness of hashcat's complete mode-3200 path, including final comparison and result marking, under both native optimization settings. It does not establish that Qualcomm's OpenCL compilation produces the same machine behavior. The RedMagic stock GPU self-test still has archived exit code 245, and no complete GPU stage readback exists. Keep the normal self-test enabled and require a positive real GPU result before declaring bcrypt 3200 supported on Adreno.

## Native GCC undefined-behavior trap regression

The CPU-only script tools/test-m03200-ub-trap-cpu.py compiles the unchanged stock mode-3200 init/loop source, the isolated diagnostic probe source, and the unmodified stock comparison path as three separate native executables. Each uses GCC -O1 -fsanitize=undefined -fsanitize-undefined-trap-on-error, which inserts direct process traps instead of requiring the libubsan runtime library.

Before executing bcrypt, the test compiles a deliberately signed-overflowing standalone program using the same sanitizer flags. Its nonzero process exit demonstrates that the selected toolchain can detect and stop on a basic undefined operation; this is a control for the test environment, not a real issue in bcrypt.

The positive and negative synthetic candidates hashcat and hashcat2 are then executed. All 1060 32-bit words in each of the three 4240-byte states are compared with the independent CPU reference, and the stock/probe native buffers must match exactly. The six output ciphertext words are checked, followed by the original mode-3200 inc_comp_multi, check, find_hash, and mark_hash path against both expected digests. All four positive/negative comparisons are required to agree with the public reference.

Run the regression with a native GCC that supports undefined-behavior trap instrumentation:

    python -B tools/test-m03200-ub-trap-cpu.py

The 2026-10-30 Windows MinGW GCC test passes: one deliberate overflow is trapped and neither candidate triggers a sanitizer trap in the instrumented native bcrypt calculations. This result does not prove freedom from undefined behavior for every input, does not detect GPU-only races or compiler miscompilation, and does not change the Adreno GPU status. The last archived RedMagic bcrypt self-test remains exit 245, with no verified intermediate device readback.
