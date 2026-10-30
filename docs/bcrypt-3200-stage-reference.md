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

The CPU reference covers P and the four S-box arrays (1042 words). E is a separate 18-word expanded-key field that remains available for debugging password preprocessing. The source is the authority for offsets and field sizes; a future capture must confirm the compiled device buffer layout and gid before comparing.

A diagnostic build can obtain snapshots through a read-only clEnqueueReadBuffer of the temporary buffer after the m03200_init dispatch, after the first iteration, and after the 32nd iteration. Since a normal self-test may submit several iterations together, obtaining loop1 requires a deliberately segmented 1 + 31 loop dispatch whose cumulative work equals the ordinary 32 iterations. Collect results only from the selected synthetic candidate. Keep hashcat's built-in self-test enabled and do not modify cryptographic comparison semantics.

If the m03200_comp diagnostic writes its four result words to a separate observation buffer, the comparator can also validate these words. The ordinary compare kernel does not store them into bcrypt_tmp_t.

## GPU dump format and strict comparison

tools/compare-m03200-gpu-stages.py --trace DEVICE_READBACK.json requires device model, ADB serial, mode, cost, salt, all relevant source/input SHA-256 values and the candidate string. The readback_kind field is u32_numeric_hex. Each stages entry (init, loop1, loop_final) contains arrays named P, S0, S1, S2, S3 in the sizes above, with each word as eight lowercase hexadecimal digits. Optional comp_words contains four hexadecimal words from m03200_comp.

On little-endian Android, convert captured bytes to numeric uint32 values before formatting each word; do not hex-dump raw little-endian memory bytes as though they were big-endian numeric values. The script validates exact metadata and then compares arrays in execution order. The first divergence is reported as FIRST_DIVERGENCE <stage>.<array>[<index>] with expected and observed numeric words.

If init diverges, investigate password/salt decoding and initial Blowfish state. If init matches but loop1 diverges, investigate the first expansion iteration. If both match but loop_final diverges, inspect repeated loop dispatch and temporary-buffer reuse. If all stages match but the built-in hashcat self-test fails, investigate final comp or digest comparison. These branches are conditional diagnostic interpretations, not an assertion about which GPU stage currently fails.

## Verification commands

Run the native CPU reference check (requires GCC):

    python tools/test-m03200-stage-emu.py

Run the comparator's CPU-only mutation checks, which do not claim GPU success:

    python tools/compare-m03200-gpu-stages.py --self-test

Compare an actual device trace only when one has been captured:

    python tools/compare-m03200-gpu-stages.py --trace DEVICE_READBACK.json

No device readback is archived as part of this report. The RedMagic bcrypt mode 3200 self-test result remains failed (245); the noinline helper experiment has no device result. Application runtime and production binaries are outside the scope of these CPU-only diagnostics.
