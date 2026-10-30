# Android pure OpenCL compiler and upstream source audit (2026-10-30)

This report compares the source of three unresolved Android GPU kernel variants with the current official hashcat master revision and runs source-only compilation preflights. All historical GPU outcomes cited below are archived results from independent, public known-vector checks. Host compiler checks are **not** proof of device execution.

## Exact upstream source comparison

The upstream Git blob SHA-1 values were obtained from the official hashcat/hashcat repository on 2026-10-30. Local values were computed with git hash-object on the HASHCAT source files.

| Mode / path | Local Git blob SHA-1 | Official upstream Git blob SHA-1 | Result |
| --- | --- | --- | --- |
| bcrypt 3200 / m03200-pure.cl | fcade9f0cbee1259d56019b7b5cb200b89fc3ef9 | fcade9f0cbee1259d56019b7b5cb200b89fc3ef9 | Identical bytes |
| NetNTLMv2 5600 / m05600_a3-pure.cl | d4e0937fec2ddb6efe527b555eff9486dd56fdd2 | d4e0937fec2ddb6efe527b555eff9486dd56fdd2 | Identical bytes |
| md5crypt 500 / m00500-pure.cl | bb1605d981aba35e548e97fd9676c4dacae8b917 | 02d13806688e44c6876c95208a1d187b40d78d7e | Six renamed MD5 update calls |

Official references:
- https://github.com/hashcat/hashcat/blob/master/OpenCL/m03200-pure.cl
- https://github.com/hashcat/hashcat/blob/master/OpenCL/m05600_a3-pure.cl
- https://github.com/hashcat/hashcat/blob/master/OpenCL/m00500-pure.cl
- https://github.com/hashcat/hashcat/blob/master/OpenCL/inc_hash_md5.cl

The six differences in mode 500 occur in the m00500_loop function: official upstream calls md5_update_padded where this working tree calls md5_update. Inspection of the two helper definitions shows that the entire current local md5_update function body matches the upstream md5_update_padded body exactly. The old helper performs unconditional reads of the 64-byte input window; m00500_loop supplies zero-initialized 64-byte-or-larger backing buffers for its password, salt and digest. Therefore, the six renamed call sites do **not** establish a correction for Mali's pure-mode compiler failure.

Upstream separately provides a bounded-read md5_update implementation and a fast md5_update_padded implementation. This working tree does not declare or define md5_update_padded or hc_bounded_word_le_S. Copying only the six renamed calls would introduce an undefined function instead of solving the device issue. A complete shared-library alignment would affect other MD5 consumers and requires its own compatibility and functional test coverage; it was not attempted in this audit.

## Independently observed device status

| Device | Mode and kernel | Last archived observation | Result |
| --- | --- | --- | --- |
| RedMagic NX709S / Adreno | bcrypt 3200 pure | Normal built-in self-test produces no match; workgroup size 1 also fails | Exit 245 |
| RedMagic NX709S / Adreno | bcrypt 3200, no OpenCL optimizations | Qualcomm LLVM QGPUSplitCallGraphPass crashes before self-test | Exit 139 |
| RedMagic NX709S / Adreno | NetNTLMv2 5600 a3 pure | Qualcomm clLinkProgram returns CL_OUT_OF_HOST_MEMORY | Exit 251 |
| RedMagic NX709S / Adreno | NetNTLMv2 5600 optimized | Public known vector recovered with unchanged self-test | PASS |
| Pixel 9 Pro XL / Mali | md5crypt 500 pure | Valhall compiler aborts during program linking; Scudo memory error reported | Exit 134 |
| Pixel 9 Pro XL / Mali | md5crypt 500 optimized | Public known vector recovered with unchanged self-test | PASS |
| Pixel 9 Pro XL / Mali | bcrypt 3200 pure | Public known vector recovered with unchanged self-test | PASS |

Archived evidence:
- docs/redmagic-gpu-selected-modes.md
- docs/adreno-bcrypt-diagnostic-variants.md
- docs/pixel-gpu-selected-modes.md
- tests/fixtures/android-gpu/redmagic-selected-modes-20261030

For bcrypt mode 3200, the CPU emulation also executes the unmodified stock init, loop, and comparison code, including hashcat's bitmap and digest lookup. The full final comparison passes known positive and negative checks with GCC -O0 and -O2. No CPU arithmetic divergence was detected; this does not prove the Adreno driver is defective because its intermediate states have not been read back.

## Reproducible host OpenCL preflight

tools/test-android-opencl-pure-preflight.py uses explicitly supplied host Clang to compile each current pure kernel with -Werror, the same source-level macro expansion used by hashcat, a Qualcomm-like GPU vendor/device type, OpenCL C 1.2 and 3.0, and local group sizes 1 and 8. Mode 5600 uses its a3 attack-mode definition; the other two cases use the dictionary attack-mode definition.

Example command on the current Windows host:

    python -B tools/test-android-opencl-pure-preflight.py --clang D:\ANDROID\SDK\ndk\29.0.14206865\toolchains\llvm\prebuilt\windows-x86_64\bin\clang.exe

2026-10-30 result: all 12 source-level compilation cases PASS. This test does not load libOpenCL.so, execute a kernel, inspect compiled Qualcomm/Arm binaries or validate application recovery. Host Clang acceptance is compatible with the archived device compiler failures.

## Remaining blocker and verification rule

There is no demonstrated source-level bcrypt algorithm bug to patch: the stock kernel is byte-for-byte identical to current upstream, the normal CPU arithmetic and comparison path passes, and neither the one-thread nor the full global-S-box diagnostic changed the RedMagic self-test failure. Driver-related compilation and execution differences remain plausible but unproven.

The next conclusive bcrypt diagnostic requires a **real** recorded Adreno init/loop/comp readback, with its provenance and exact kernel/source hashes, followed by comparison against tools/compare-m03200-gpu-stages.py. The previously archived standalone-probe loader error occurs before OpenCL program compilation; that archived run generated no GPU stage data. A genuine positive status requires the unchanged built-in hashcat self-test and recovery to pass on both devices. Do not disable the self-test or relabel a CPU-only result as a GPU success.
