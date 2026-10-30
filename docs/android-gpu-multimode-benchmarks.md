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
| 1700 | Raw SHA-512 | **8696.9 kH/s**, default | **No speed**, `-w 1` compiler SIGABRT / exit 134 | Pixel `-cl-opt-disable` and correctly targeted pure-helper `noinline` trials also failed with exit 134 |
| 1800 | SHA-512crypt | **910 H/s**, default | **No speed**, `-w 1` exit 134 | RedMagic benchmark passed and subsequently recovered the OpenSSL-reference `$6$` hash; Pixel compiler crash |
| 22000 | WPA-PBKDF2-PMKID+EAPOL | **755 H/s** on rebuilt Adreno-only baseline; **761 H/s** in earlier isolated test | **No speed**, original and split Mali builds | Original log emitted `mmap failed: Out of memory`; separate split-program trial also emitted it and returned 134. Neither is a working GPU result |

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

### Rebuilt Adreno-only WPA benchmark confirmed (2026-10-30)

After source rollback and a successful Android ARM64 rebuild, tested mode `22000` on RedMagic NX709S again using isolated app-private staging. `build/diagnostic-multimode/NX709S-m22000-device.log` reported **755 H/s**, `ISOLATED_BENCHMARK_EXIT:0 mode=22000 workload=0`; the host recorded **`DEVICE_EXIT:0`, `ADB_EXIT:0`, `HASHCAT_BENCHMARK_SUCCESS:True`** and the device exit-marker file contains `0`. After exit the temporary app-private stage was absent, and stock `module_22000.so` and `m22000-pure.cl` SHA-256 values were unchanged. The 755 and prior 761 H/s measurements are small single benchmark samples; no statistical performance conclusion follows. This remains a **benchmark-only** success, not proof of correct EAPOL/PMKID password verification.

For the unresolved Pixel Mali driver crash, consider controlled isolated compile-only experiments that vary optimization (for example a supported OpenCL optimization-disable flag) and record the Mali compiler build log/crash stack. Only integrate a change after the actual kernel runs correctly on independent WPA-PBKDF2/PMKID/EAPOL test vectors; do not infer correctness from successful kernel creation or a benchmark alone.

## GPU known-vector verification across both phones (2026-10-30)

Unlike `-b` speed tests, these are **actual hashcat GPU candidate checks** against independently generated synthetic hashes. Hashcat ran using a bounded `-a 3` candidate and a shell-controlled temporary runtime, with no application installation on Pixel and no replacement of app files on RedMagic. The committed raw evidence is in `tests/fixtures/android-gpu/`; re-check with `python tools/test-android-gpu-known-vectors.py` (requires PyCryptodome for the independently computed NTLM/MD4 digest). The archived files contain only deliberately synthetic passwords/hashes and GPU status logs, not captured Wi-Fi traffic.

On **Pixel 9 Pro XL, Mali-G715**, one explicitly constructed candidate `benchdemo2026` matched each independently computed digest:

- Mode `0`, MD5: `ab9ce53a8dece3a0440197f719263ac5`.
- Mode `100`, SHA-1: `5c8bc3c3244442a3c5fbaf2ec021656ea2678b61`.
- Mode `1000`, NTLM/MD4(UTF-16LE): `82d20ab2cac864cf5c32eed420d32340`.
- Mode `1400`, SHA-256: `546a49cc74208a398ff9229f7935234cd052e9471cfaa45dd490700cd1848e06`.

All four produced `Status: Cracked`, `Recovered: 1/1`, `Candidate.Engine: Device Generator`, the exact `digest:benchdemo2026` recovered record, and on-device exit marker `0`. Both the recovered lines and the detailed logs were copied to the PC **before** the temporary device test tree was removed. This confirms correct positive matches for these *specific synthetic vectors* and pure GPU kernel path, not comprehensive algorithm correctness across all lengths/attack modes.

On **RedMagic NX709S, Adreno**, additional positive known-vector tests found:

- Mode `1700`, raw SHA-512: `hashcat` recovered against Python `hashlib.sha512(b'hashcat')`; log shows `Cracked`, `Recovered 1/1`, `Device Generator`. The on-device exit `0` was observed at execution time, but an `*.exit` marker was **not archived** for this case.
- Mode `1800`, SHA-512crypt: `hashcat` recovered against OpenSSL reference `$6$72820166$U4DVzpcYxgw7MVVDGGvB2/H5lRistD5.Ah4upwENR5UtffLR4X4SxSzfREv8z6wVl0jRFX40/KnYVvK4829kD1`. Saved exit marker `0`, full recovered output and `Cracked` log were independently checked. A runtime-path warning about `hashcat.outfiles` appeared, but the output, exit and digest all matched.
- Mode `22000`, **PMKID (`WPA*01`)**: the module's synthetic `hashcat!` / `hashcat-essid` record was independently checked with Python PBKDF2-HMAC-SHA1 (4096 rounds, 32-byte PMK) and HMAC-SHA1 over `PMK Name || AP_MAC || STA_MAC`. The recomputed PMKID was `4d4fe7aac3a2cecab195321ceb99a7d0`. The Adreno GPU recovered `hashcat!`; the exact canonical `PMKID:AP:STA:SSID:password` output matched, and the **device exit marker was 0**. The host's original ADB shell returned `255` after a transport disconnect while GPU compilation was underway; the on-device process continued and completed successfully, so the 255 is not a kernel failure.

The last two RedMagic tests left temporary application-private staging after the earlier ADB interruption; their logs and recovered outputs were archived, then their **exact temporary directories, input files and markers** were removed after checking no hashcat process was running. The stock mode-22000 files still have SHA-256 `abff75bd2ad3dc4c4818419b500bf07e19340a28e4c506f024c11cc43bf0aaf0` (module) and `8ab35e16fa2055ba36939507ccac0bbcd5e52d9205d3d635417921f12582b428` (OpenCL source).

Pixel Mali diagnostics: small OpenCL smoke kernels compiled successfully with default options, `-cl-std=CL1.2` and `-cl-std=CL3.0`. Both raw SHA-512 (`1700`, `-w 1`) and SHA-512crypt (`1800`, `-w 1`) failed with exit `134`; the raw SHA-512 run emitted `mmap failed: Out of memory`. A module-only `-cl-opt-disable` experiment also returned 134. Annotating the *optimized* SHA-512 kernel was not a valid noinline test under `-w 1` (that profile uses pure kernels). A later isolated test annotating the actual `sha512_transform` helper in the **pure** kernel still returned exit 134 with Mali native crash stack in `llvm::InlineFunction` / `clcc::run_early_inliner`. Thus no SHA-512 or WPA 22000 GPU success on Pixel is established.

### PMKID negative control on RedMagic (2026-10-30)

Re-ran the **same** independently constructed `WPA*01` PMKID with one deliberately incorrect but valid-length candidate `hashcat2` instead of the actual `hashcat!`. The Adreno computation completed with `Status: Exhausted`, `Recovered: 0/1 (0.00%)`, `Guess.Mask: hashcat2 [8]`, `Candidate.Engine: Device Generator` and **device exit `1`** (expected for an exhausted candidate). The on-device runner additionally required the recovered-output file to be empty. It printed `KNOWN_VECTOR_NEGATIVE_PASS mode=22000 exit=1`, and the **host ADB exit was `0`**. The log and marker are committed as `m22000-pmkid-negative-device.log` and `.exit`. The runner stage was removed automatically; the exact temporary inputs and on-device log/marker were cleared after archiving. Stock module and OpenCL source SHA-256 values remained unchanged.

**Historical scope note:** the earlier PMKID-only stage did not verify EAPOL. Subsequent GPU tests below now cover all three EAPOL key versions and a mixed batch. Pixel negative/distractor checks are independently verified by `tools/test-pixel-gpu-candidates.py`. This broader coverage still does not establish every WPA edge case or release readiness.

## Adreno WPA 22000 EAPOL AUX1–AUX3 and mixed-batch verification (2026-10-30)

Synthesized deterministic `WPA*02` EAPOL-Key frames entirely in Python (`tools/generate-m22000-eapol-vectors.py`). Each frame is 99 bytes with its MIC field zeroed; the PMK uses PBKDF2-HMAC-SHA1 with 4,096 iterations. PTK KCK construction uses the WPA pairwise-key expansion over explicitly sorted invented MAC addresses and nonces. The reference MIC primitives for `keyver=1`, `2` and `3` are **HMAC-MD5**, **HMAC-SHA1 truncated to 16 bytes**, and **AES-CMAC** respectively. Synthetic frames include no captured network traffic. Generator re-parses and checks each record independently before it is used as GPU input.

On **RedMagic NX709S / Qualcomm Adreno**, each `WPA*02` verification branch passed separate positive and negative GPU tests under the split two-program OpenCL backend:

| WPA EAPOL version | OpenCL verification kernel | Correct synthetic passphrase | Incorrect `hashcat2` |
| --- | --- | --- | --- |
| `keyver=1`, HMAC-MD5 | `m22000_aux1` | `Cracked`, 1/1, device exit `0` | `Exhausted`, 0/1, device exit `1` |
| `keyver=2`, HMAC-SHA1-128 | `m22000_aux2` | `Cracked`, 1/1, device exit `0` | `Exhausted`, 0/1, device exit `1` |
| `keyver=3`, AES-CMAC | `m22000_aux3` | `Cracked`, 1/1, device exit `0` | `Exhausted`, 0/1, device exit `1` |

All GPU logs report `Candidate.Engine.: Device Generator` and the expected candidate mask. The stored logs, device exit markers, positive recovered-word markers (or AUX2's host runner output) are in `tests/fixtures/android-gpu/redmagic-eapol-vectors/`. `python tools/test-android-gpu-known-vectors.py` verifies the MICs, records and GPU outcomes independently. Its **14 cases** include the four Pixel positive vectors, three RedMagic non-EAPOL positive vectors, PMKID negative control and six EAPOL positive/negative checks.

**Mixed-batch regression:** independently composed one `WPA*01` PMKID and all three `WPA*02` EAPOL records with a common synthetic SSID and `hashcat!` passphrase (`tests/fixtures/android-gpu/redmagic-mixed-wpa/mixed.hash`). Hashcat parsed **4 distinct digests, 1 distinct salt**. A single Adreno GPU run reported `Status: Cracked`, **`Recovered: 4/4 (100%)`**, produced exactly four `hashcat!` plaintext records (`--outfile-format 2`) and device/host exit `0`. With the same four records and deliberately wrong `hashcat2`, a separate GPU run reported `Status: Exhausted`, **`Recovered: 0/4 (0%)`**, device exit `1` and successful host wrapper exit `0` (expected rejection). The test `python tools/test-redmagic-mixed-wpa.py` checks exact input composition against individually independently verified PMKID/EAPOL records, both complete GPU runs, recovered output and host runner transcripts. The reference PMKID/MIC values are independently recomputed by `python tools/test-android-gpu-known-vectors.py`. Logs, input, markers and host wrapper transcripts are preserved in `tests/fixtures/android-gpu/redmagic-mixed-wpa/`.

Both mixed runs used `tools/run-redmagic-mixed-wpa.sh` and an isolated app-private runtime. Their exact temporary application-cache logs, markers, executable and `/data/local/tmp` staging were removed after archiving; the device was not running hashcat during cleanup. The original app `module_22000.so` SHA-256 remains `abff75bd2ad3dc4c4818419b500bf07e19340a28e4c506f024c11cc43bf0aaf0`; `m22000-pure.cl` remains `8ab35e16fa2055ba36939507ccac0bbcd5e52d9205d3d635417921f12582b428`.

**Qualification:** these are specific positive/negative synthetic WPA vectors, not exhaustive validation of real captures, malformed/multifield frames, alternate nonces, multiple SSIDs, long passwords or sustained multi-device workloads. The **Pixel 9 Pro XL / Mali-G715** still fails native OpenCL compilation for heavy SHA-512 and mode 22000 kernels; no EAPOL GPU success is claimed on Pixel. A universal fix or release remains unverified.
