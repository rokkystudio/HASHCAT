# Android GPU hashcat multi-mode diagnostics (2026-10-30)

## Scope and method

These are **short isolated hashcat v7.1.2 `-b` benchmarks**, not proof that a recovered password is correct. Test frontend from `build/arm64-v8a/hashcat` (before experimental Mali-split rebuild) had SHA-256 `116db362931bbd25d5aff0fe09b929934258d62dea6af885508b3c16f08373bf`. RedMagic NX709S runs Android 13 with Adreno/Qualcomm OpenCL 3.0. Pixel 9 Pro XL runs Android 16 with Mali-G715/ARM OpenCL 3.0. The Pixel has no `fuck.system.hashcat` package installed, so its tests run from an isolated `/data/local/tmp` tree instead of using `run-as` or modifying an application.

All numbers below come from preserved `build/diagnostic-multimode/` logs, except the previously recorded isolated RedMagic 22000 measurement (see `docs/adreno-m22000-diagnostic.md`). The Pixel `-w 1` tests use a lower workload profile; direct inter-device performance rankings from these numbers are **not** valid. SHA-512crypt mode **1800** is the salted and iterated `$6$` algorithm, not raw SHA-512 (mode 1700).

## Observed results

| Hashcat mode | Algorithm | RedMagic NX709S / Adreno | Pixel 9 Pro XL / Mali-G715 | Qualification |
| --- | --- | --- | --- | --- |
| 0 | MD5 | **2131.2 MH/s**, default profile | **1171.8 MH/s**, default profile | RedMagic hashcat log says exit 0 but one host ADB invocation reported 255; Pixel log has finished speed, no explicit process-exit marker |
| 100 | SHA-1 | **199.8 MH/s**, default | **92.731 MH/s**, `-w 1` | Both finished; profiles differ |
| 1000 | NTLM | **3005.6 MH/s**, default | **136.3 MH/s**, `-w 1` | Both completed: RedMagic on-device+ADB exit 0, Pixel on-device+ADB exit 0; profiles differ |
| 1400 | SHA-256 | **128.3 MH/s**, default | **40.5914 MH/s**, `-w 1` | Both finished; profiles differ |
| 1800 | SHA-512crypt | **910 H/s**, default | Not established | RedMagic `ISOLATED_BENCHMARK_EXIT:0`; no finished Pixel result |
| 22000 | WPA-PBKDF2-PMKID+EAPOL | **761 H/s**, previous isolated test | **No speed**, original and split Mali builds | Original log emitted `mmap failed: Out of memory`; separate split-program trial also emitted it and returned 134. Neither is a working GPU result |

## Pixel reliability and GPU compilation issues

The Pixel OpenCL platform identifies itself as ARM OpenCL 3.0 on Mali-G715 r0p0, with approximately 7,678 MiB device global memory and a 3,839 MiB single-allocation limit. Initial default-workload SHA-1/SHA-256 Pixel logs had no final speed; re-running with `-w 1` yielded the completed measurements above. This does **not** prove the default workload is inherently broken, nor that the GPU lacks memory. The original non-split Pixel 22000 benchmark emitted an `mmap failed: Out of memory` message and no speed. It may reflect the driver/compiler or process limits; additional controlled diagnostics are needed.

Read-only `adb logcat -b crash` review of the original (non-split) 22000 failure showed Android `SIGABRT` in the `hashcat` process after `mmap failed: Out of memory`. The recorded native stack contains Bionic linker `__dl_BionicSmallObjectAllocator::alloc_page`, Mali compiler `llvm::InlineFunction`, `clcc::run_early_inliner`, `clcc::ProgramContext::build_program`, and ultimately `clCompileProgram` in `/vendor/lib64/egl/libGLES_mali.so`. **This localizes the original failure to compiler/optimizer activity**; the error string alone does not prove device-global-memory exhaustion. The separate two-program trial also failed with `mmap failed`, but a corresponding detailed native stack for that trial was not captured.

The Pixel NTLM test used an on-device `timeout` wrapper and saved both `m01000-w1.log` and `m01000-w1.exit` under the isolated staging area; the files were also copied to `build/diagnostic-multimode/Pixel_9_Pro_XL-m01000-w1-device.log` and `.exit` (code `0`). Other host-side logs created by Windows PowerShell `Tee-Object` are **UTF-16LE** (BOM `FF FE`), not UTF-8, which simple log parsers must account for. The on-device NTLM log is UTF-8.

An experimental build extended mode-22000's **two-program split** from Adreno to Mali-G715 and compiled for Android arm64 (`BUILD_EXIT:0`; experimental frontend SHA-256 `9272d7e73454372e6879c4cb8adf27aabd531c84aee7f51dd8389861371673e5`). The on-device trial was staged separately and returned **exit 134** with `mmap failed: Out of memory` despite splitting. Its full log was pulled to `build/diagnostic-multimode/Pixel_9_Pro_XL-m22000-split-G715.log`. The stage was removed after execution. The two uncommitted source changes enabling split/noinline on Mali were reverted to the last known Adreno-only state after this failure; no Mali fix is claimed. GPU checks against known WPA-PBKDF2/PMKID/EAPOL inputs have not established end-to-end correctness.

## Preservation and release status

RedMagic benchmark inputs were staged in app-private cache with the existing module and OpenCL source preserved; after the tests the original 22000 files still had SHA-256 `abff75bd2ad3dc4c4818419b500bf07e19340a28e4c506f024c11cc43bf0aaf0` (module) and `8ab35e16fa2055ba36939507ccac0bbcd5e52d9205d3d635417921f12582b428` (source). Pixel binaries live only in shell-owned `/data/local/tmp` staging. No Android application installation or OS replacement was required. Do **not** publish or install these experimental hashcat builds as releases on the strength of benchmark measurements alone.

## Local source rollback and executable comparison

The failed Mali-G715 `m22000_split`/`M22000_ADRENO_NOINLINE` experiment was reverted only in `source/src/backend.c` and `source/src/modules/module_22000.c`, leaving the previously tested Adreno implementation intact. A clean ARM64 build with the restored code finished `RESTORED_ADRENO_BUILD_EXIT:0` (582 modules, 4 bridges), module/source SHA-256 unchanged (`9406352cebbe3de28c0e4f5e46dfb7c2be1ab10e2c50379b85494df087532753` / `e87cad4e0f36fb635223f29cc0e51b0e02b95e321c4a3b934c0407412745114f`).

The recompiled frontend SHA-256 is `49c1619f608b1ab8e9e15deef90ac5269adf21425fb6da7873a3398e30937507`, versus the previously benchmarked `116db362931bbd25d5aff0fe09b929934258d62dea6af885508b3c16f08373bf`. A **whole-file binary comparison** of the two ELF executables found exactly **3 differing bytes**, all in one AArch64 `mov w5, immediate` instruction at file offsets 223944-223946 (section `.text` offset 584). Together with the unchanged following `movk w5,#0x6ae3,lsl#16`, the two embedded 32-bit constants decode to UTC `2026-10-29 18:24:28` and `2026-10-29 21:52:34`, matching successive build times. All other bytes, including `.rodata`, match; this is strong evidence of a build-timestamp-only difference, **not** a new GPU correctness test.

### Additional RedMagic NTLM verification (2026-10-30)

The serial-specific app-private isolated runner was re-tested with mode `1000` after resolving its earlier staging conflict. Device log `build/diagnostic-multimode/NX709S-m01000-device.log` shows **3005.6 MH/s**, `ISOLATED_BENCHMARK_EXIT:0 mode=1000 workload=0`; host ADB also returned 0 and saved `.exit`=0. Isolated app cache and temporary staging were removed; rechecked the stock mode-22000 module/source SHA-256 values and found both unchanged. This measurement supersedes the previously inconclusive RedMagic NTLM attempt.
