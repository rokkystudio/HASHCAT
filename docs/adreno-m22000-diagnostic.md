# Adreno OpenCL / mode 22000 diagnostic (2026-10-28)

## Correction: standalone SHA1 failure was caused by an incomplete probe (2026-10-28)

The earlier `realsha1` `CL_OUT_OF_HOST_MEMORY (-6)` result must **NOT** be interpreted as proof that Qualcomm cannot compile `sha1_transform()`: that harness included function declarations from `inc_common.h` but omitted their implementation (`inc_common.cl` and `inc_platform.cl`). The Qualcomm driver misleadingly returned -6 with build log `Pass` for this incomplete program.

Reproduced on RedMagic NX709S / ADB `91809037` with the same GPU and OpenCL runtime:

| Standalone test | `clBuildProgram` | Interpretation |
| --- | --- | --- |
| `sha1unused` | 0 | Included SHA1 source; unused |
| `realsha1` | -6 | Missing helper definitions; invalid test of SHA1 support |
| `realsha1defs` | 0 | Actual SHA1 + definitions for missing helpers |
| `realsha1common` | 0 | Actual SHA1 + real platform/common implementations |
| `realallincludes` | 0 | Actual SHA1 and SHA256/MD5/AES include implementations |
| `sha1r1`, `sha1r16`, `sha1r40`, `sha1r80` | -6 | Diagnostic probe still has incomplete support; these failures do not demonstrate an SHA1 round threshold |

The *real* mode 22000 failure remains reproducible in hashcat, but the isolated SHA1 failure was a false lead. Further work should concentrate on compiled whole-kernel semantics and linker dependencies, not rewrite SHA1 based on this flawed probe.

Device: RedMagic NX709S; ADB serial `91809037`. Do not target other connected devices.

## Confirmed baseline
- OpenCL 3.0 QUALCOMM, Adreno; mode 0 benchmark works (~2126.8 MH/s).
- Mode 22000 fails kernel creation for `m22000-pure.cl` with `CL_OUT_OF_HOST_MEMORY`.
- Two-stage (`clCompileProgram` + `clLinkProgram`) and single-stage (`clBuildProgram`) both failed.
- GPU `512 MB allocatable` is an allocation limit, not proof of a RAM shortage.

## Kernel map
- `m22000_init`: PBKDF2/HMAC-SHA1 setup (conditional noinline implementation exists)
- `m22000_loop`: repeated PBKDF2/HMAC-SHA1 vector work
- `m22000_comp`: intentionally empty
- `m22000_aux1`: EAPOL keyver 1 validation
- `m22000_aux2`: EAPOL keyver 2 validation
- `m22000_aux3`: EAPOL keyver 3 validation (AES helper)
- `m22000_aux4`: PMKID validation

Includes: inc_vendor, inc_types, inc_platform, inc_common, inc_simd, inc_hash_md5, inc_hash_sha1, inc_hash_sha256, inc_cipher_aes.

## Audit required before any production binary
- There are uncommitted experiments in `source/OpenCL/m22000-pure.cl`, `source/src/backend.c`, and `source/src/modules/module_22000.c`; preserve them until evaluated.
- In `module_jit_build_options`, the aux-pruning decisions iterate `hashes->digests_cnt` over `hashes->esalts_buf`. Confirm that this count and order correspond to all applicable `wpa_t` records. Otherwise disabling an aux kernel may silently omit legitimate checks.
- The benchmark path enables only AUX4, whereas other test modes may use a different auxiliary set; a successful benchmark alone would not validate every key version.
- `stub_opencl_kernel_function` rewrites OpenCL source by searching braces. This may be unsafe for preprocessor branches, nested braces, or changes to upstream sources; test against known fixtures before shipping.
- Never ship stubs that might bypass required key verification. If a backend cannot compile a required verification kernel, fail visibly rather than report a false result.

## Next engineering task
Build isolated OpenCL compile-only fixtures for the vendor compiler. Keep inputs synthetic and avoid hash cracking. Compare minimal empty kernel, SHA1 transform, loop helper, and AES helper separately. Record compile/link status, elapsed time, driver and memory info, and exact source hash.

## Independent OpenCL smoke test (2026-10-28)

`tools/opencl-smoke.c` was cross-compiled with Android NDK r29 for arm64 and run on RedMagic NX709S (serial `91809037`). It uses `dlopen("libOpenCL.so")`, selects `CL_DEVICE_TYPE_GPU`, creates a context, and compiles a synthetic kernel that performs integer multiply/add on an array. It does **not** execute a hashcat attack.

Observed output:

```
platforms err=0 count=1
devices err=0 count=1
context err=0
program err=0
build err=0
build log: Pass
exit=0
```

This proves compilation of a trivial OpenCL program succeeds; it does not prove any particular hashcat mode works. Continue testing isolated compiler features rather than changing production verification semantics.

## Synthetic OpenCL feature compilation (2026-10-28)

Expanded `tools/opencl-smoke.c` into four selectable compile-only variants. Ran on RedMagic NX709S via `adb -s 91809037`:

| Variant | Compiler result | Notes |
| --- | --- | --- |
| basic | `build err=0`, `Pass` | Integer multiply/add |
| loop | `build err=0`, `Pass` | 4096 iterations of bit arithmetic |
| vector | `build err=0`, `Pass` | `uint4` rotations and XOR |
| sha1like | `build err=0`, `Pass` | SHA1-like bitwise logic; **not actual SHA1** |

These are compiler acceptance checks only. No kernel execution or result verification was performed. The full m22000 compile failure remains unresolved.

## Multi-entry and nested helper checks (2026-10-28)

- `multi`: seven distinct entry points named `init`, `loop`, `comp`, `aux1` through `aux4` in one OpenCL program, including a loop. `clBuildProgram=0`, log `Pass` on NX709S.
- `helper`: shared helper function with 160 rounds, called 32 times from the kernel. `clBuildProgram=0`, log `Pass` on NX709S.

These probes use intentionally synthetic arithmetic. They do not compile the actual `inc_hash_sha1.cl`, AES, or hashcat `KERN_ATTR_TMPS_ESALT` declarations. Consequently the real kernel's macro expansion, register pressure, and include dependencies remain suspects. No correctness of WPA verification was claimed.

## Actual hashcat SHA1 include reproduction (2026-10-28)

An isolated OpenCL program includes the installed, unchanged `inc_vendor.h`, `inc_types.h`, `inc_platform.h`, `inc_common.h`, and `inc_hash_sha1.cl` and calls `sha1_transform` once from a synthetic kernel. The compiler is invoked through `clBuildProgram` with `-I /data/user/0/fuck.system.hashcat/files/hashcat/runtime/OpenCL -D KERNEL_STATIC -D VECT_SIZE=1 -D DEVICE_TYPE=4 -D VENDOR_ID=1 -D DGST_ELEM=4`. Earlier failed probes without `KERNEL_STATIC`/`DGST_ELEM` were invalid due to missing hashcat compile-time definitions.

The first correctly configured probe returned `build err=-6` (`CL_OUT_OF_HOST_MEMORY`) with build log `Pass`, running on RedMagic NX709S, ADB `91809037`. This reproduces the failure class outside hashcat, but the smoke-test options are not necessarily identical to the application's JIT options. Needs a repeat and reduction to confirm stable triggering conditions before attributing it solely to SHA1 transform or a compiler bug.

## Repeated SHA1 isolation (2026-10-28)

Using the same independent OpenCL probe and options, compared:
- `headers`: vendor/types/platform/common includes with an integer arithmetic kernel — build=0, `Pass`.
- `sha1unused`: adds real `inc_hash_sha1.cl` without calling its functions — build=0, `Pass` (repeated).
- `realsha1`: same includes and compile options but calls `sha1_transform()` on a synthetic block — build=-6 (`CL_OUT_OF_HOST_MEMORY`), log `Pass` (repeated three times total).

This establishes a reproducible compile-time failure triggered by a reachable real SHA1 transform, not merely by file size or including the module. A minimal reproduction still needs reduction of the SHA1 implementation; these results do not establish why Qualcomm's compiler returns -6.

## Optimization-disabled probe (2026-10-28)

Tried `realsha1_noopt` with `-cl-opt-disable`. The Qualcomm compiler returned `-11` with `inc_vendor.h file not found` despite an `-I` option. This is an invalid comparison against the prior `realsha1` `-6` result. Investigate compiler option handling before interpreting optimization effects.

## Safety change: disable automatic AUX pruning (2026-10-28)

Removed Adreno `module_jit_build_options()` logic that inferred which AUX verification kernels were needed from `hashes->esalts_buf` indexed by `hashes->digests_cnt`, including benchmark-specific pruning. The experimental module now returns only `-D M22000_ADRENO_NOINLINE`. The backend's pruning helper remains in source but is not requested by these module options; it should be removed or isolated before shipping. No full end-to-end correctness test or publish has occurred.

## Removed backend kernel stubbing; Android build attempt (2026-10-28)

Removed `stub_opencl_kernel_function`, `remove_opencl_build_option`, `prune_m22000_adreno_source` and the associated OpenCL source rewriting/build-option substitution from `source/src/backend.c`. Retained only the Adreno m22000 JIT diagnostic log. Combined with the module JIT safety change, no automatic AUX stubbing remains in these two files.

Ran `scripts/BUILD-ANDROID-ARM64.ps1 -Package:$false -MakeJobs 8`: NDK r29 clang frontend crashed compiling `emu_inc_common.NATIVE.o` (compiler crash, not mode 22000 runtime result). Retried with `-MakeJobs 1`; build progressed into many Android modules without that immediate crash, but was stopped by the runner's 180-second timeout (exit 1460). **No successful complete build or device correctness test** was established. Do not publish the resulting partial build.

## Cross-kernel compilation isolation (2026-10-28)

Standalone `tools/opencl-smoke.c` compiles source variants with normal hashcat OpenCL includes, using RedMagic NX709S, ADB serial `91809037`. The variants only test whether **compilation** is accepted; stubs are NOT valid cracking kernels and were never shipped.

| Synthetic full-kernel configuration | `clBuildProgram` |
| --- | --- |
| `part-none`: real init+loop, all aux stubs | 0 |
| `part-aux1`, `part-aux2`, `part-aux4`: real init+loop and one aux | -6 |
| `part-aux3`: real init+loop and aux3 | 0 |
| `part-aux4-no-hmac`: HMAC stubbed, original compare retained | 0 |
| `part-aux4-no-compare`: HMAC real, compare simplified | -6 |
| `part-aux4-init-only`, `part-aux4-init-update`, `part-aux4-init-final` | -6 |
| `realhmacinit`, `realhmacfull`: real HMAC compiled separately with all dependencies | 0 |
| `part-aux4-init-nl-init`, `part-aux4-init-nl-chain`, `part-aux4-nl-chain` | -6 |
| `part-aux4-only` (no init/loop, full aux4) | 0 |
| `part-aux4-no-init` (loop+aux4) | 0 |
| `part-aux4-no-loop` (init+aux4) | -6 |
| `part-aux1-no-init`, `part-aux2-no-init` (loop+one aux) | 0 |
| `part-aux-all-only` (all real aux1..4, no init/loop) | 0 |
| `part-aux-all-no-init` (all real aux1..4 and real loop, init stubbed) | 0 |

**Strongest finding:** Qualcomm's compiler rejects the simultaneous presence of `m22000_init` with several HMAC-based auxiliary kernels, while **both complete kernel groups compile independently**. A promising production workaround is two `cl_program` objects: one for `init/loop`, the second for `loop/aux1..aux4`. The production implementation must create the correct kernel objects from each program, retain all cryptographic computations, validate the self-test / known vectors, and handle cleanup and cache separation. Do not ship stubs or a compile-only success as a working fix.

## Clean AUX path: successful ARM64 build and real Adreno benchmark failure (2026-10-28)

Rebuilt using `scripts/BUILD-ANDROID-ARM64.ps1 -Package:$false -MakeJobs 1` with a 600-second tool limit: build exit 0, frontend 1,514,912 bytes, 582 modules, 4 bridges. No kernel function stubbing in `backend.c`; module JIT adds only `-D M22000_ADRENO_NOINLINE` on Adreno.

Staged freshly built frontend, `module_22000.so`, and `m22000-pure.cl` temporarily on RedMagic NX709S (`91809037`). The test script `build/hc-safe-22000-test.sh` restores the app's existing module/source with a trap and removes the temporary frontend. Ran `-b -m 22000 --runtime=3` with CUDA/HIP ignored. Device identified `QUALCOMM Adreno(TM)`, OpenCL 3.0. Full logged build options included `-D M22000_ADRENO_NOINLINE` and **no** `M22000_DISABLE_AUX*`. After about 50 seconds: `clLinkProgram(): CL_OUT_OF_HOST_MEMORY`, `Kernel .../m22000-pure.cl build failed`, `TEST_EXIT:251`. The external remote process exited 1 because of the test failure; the build itself had exited 0. This is a genuine failed device test, **not** a working release.

Verified phone's restored `module_22000.so` (1,061,768 bytes) and `m22000-pure.cl` (11,524 bytes) were present, temporary executable/backups absent, and cleaned staged `/data/local/tmp` files. `git diff --check` clean; no commit, push, or release.

## Full mode 22000 isolated kernel combinations (2026-10-28, verified on NX709S)

All results below are `clBuildProgram` checks on Qualcomm Adreno OpenCL 3.0 (ADB **91809037**) with real `m22000-pure.cl` from the current uncommitted worktree and the same KERNEL_STATIC/CL3.0 device options. Variants use `-D M22000_ADRENO_NOINLINE`. The test source SHA-256 was `7c13fd261221c9654e1dcecc65892ff97cc01aebbc4865c4196de87c847c45dd`. Generated diagnostic files are in ignored `build/diagnostic-20261028-allaux-isolation/`; they are **not production kernels**. A body stub means `return;` and is *only* used for source minimization.

| Active real functions in a single OpenCL program | Build result |
| --- | --- |
| All seven entrypoints, without noinline | -6, log Pass |
| All seven entrypoints, with noinline | -6, log Pass |
| All seven entrypoints stubbed | 0, seven `clCreateKernel` successes |
| `init` only | 0, seven `clCreateKernel` successes |
| `loop` only | 0, seven `clCreateKernel` successes |
| `init + loop + comp` | 0, seven `clCreateKernel` successes |
| `aux1 + aux2 + aux3 + aux4` | 0, seven `clCreateKernel` successes |
| `init + loop + comp + aux1` | -6, log Pass |
| `init + loop + comp + aux2` | -6, log Pass |
| `init + loop + comp + aux3` | 0, seven `clCreateKernel` successes |
| `init + loop + comp + aux4` | -6, log Pass |
| `init + aux2` | -6, log Pass |
| `loop + aux2` | 0, seven `clCreateKernel` successes |
| `init + aux4` | -6, log Pass |
| `loop + aux4` | 0, seven `clCreateKernel` successes |

The strong distinction is simultaneous compilation of the **real `init` kernel with aux1/2/4 code**, not the physical number of entrypoints or the size of aux3. No proof of physical host-memory exhaustion, or of incorrect SHA1 calculations, follows from code -6.

### Two OpenCL programs in one context: successful proof of concept

Built *two separate `cl_program` objects in the same OpenCL context* with identical compile defines:

1. `baseonly`: real init/loop/comp, AUX stubs (stubs never selected for use).
2. `auxonly`: real aux1/aux2/aux3/aux4, init/loop stubs (stubs never selected for use).

Both `clBuildProgram` calls returned 0. `clCreateKernel` returned 0 for init/loop/comp obtained from program 1 and aux1/aux2/aux3/aux4 obtained from program 2. The probe exited 0. This establishes that splitting programs is viable at **compile and kernel-creation time** on this device. It has NOT tested `clSetKernelArg`, buffers, dispatch, results, WPA correctness, self-tests, or full hashcat integration. Never publish these diagnostic stub programs as a production replacement. Integrate a correct multi-program selection in hashcat backend first, then test PMKID and EAPOL keyver 1/2/3 with known vectors and self-test before release.

All probe staging was in `files/hashcat/runtime/probe-allaux-20261028-v2`, never overwrote the app runtime `OpenCL` or `modules`. The stage was removed after tests: `run-as fuck.system.hashcat ls files/hashcat/runtime/probe-allaux-20261028-v2` returned exit 1 (`No such file or directory`). Test binaries placed in this new stage and its `/data/local/tmp/` input files were also removed; pre-existing runtime probe directories were not touched. The production JIT options currently retain all AUX paths and only enable noinline on Adreno; backend source rewriting/pruning is absent.

## Major isolation result: split OpenCL programs both compile (2026-10-28)

Created compile-only fixtures from the current unmodified-on-disk `source/OpenCL/m22000-pure.cl` snapshot (SHA-256 `7c13fd261221c9654e1dcecc65892ff97cc01aebbc4865c4196de87c847c45dd`) under `build/adreno-split-safe`. Each fixture preserves the original bodies of the kernels it retains. The first wraps the AUX1-4 entrypoints in `#if 0` so it defines `m22000_init`, `m22000_loop`, `m22000_comp`. The second wraps the PBKDF2 init/loop/comp block in `#if 0` so it defines `m22000_aux1` through `m22000_aux4`. Both keep the same common includes/helpers and are built independently, with the production-style OpenCL options supplied by `probe22000dual`.

On RedMagic NX709S (`91809037`), independent `clBuildProgram` reported `build err=0`, `Pass` and `second build err=0`; all 7 `clCreateKernel` calls returned 0, each using the appropriate program object. Test duration about 47 seconds, external process exit 0. This sharply suggests the Qualcomm whole-program codegen/link stage is sensitive to the combined translation unit. **No kernel execution, output comparison, WPA self-test, or integration into the hashcat backend was performed.** Building two OpenCL programs and binding kernels to them in the real backend is the next engineering step; do not ship based on compile-only checks.

## Integrated two-program backend benchmark (2026-10-29)

Implemented an Adreno-only split in `source/OpenCL/m22000-pure.cl`, `source/include/types.h`, and `source/src/backend.c`. The primary OpenCL program contains init/loop/comp; a separate `cl_program` stores aux1-4, and the four AUX kernel objects are instantiated from it. Separate split programs are built without reuse of the previous binary cache, and the extra program handle is released during backend cleanup. The experiment preserves the verification bodies rather than stubbing them.

`BUILD-ANDROID-ARM64.ps1 -Package:$false -MakeJobs 1` completed with `BUILD_EXIT:0`, producing ARM64 frontend 1,516,016 bytes and 582 modules. On RedMagic NX709S (ADB `91809037`), temporary staging and the restore-on-exit test script ran `hashcat -b -m 22000 --runtime=3 ...` successfully: **755 H/s**, benchmark `TEST_EXIT:0` (remote job exit 0, ~108 seconds). The historic `clLinkProgram(): CL_OUT_OF_HOST_MEMORY` error was absent. This proves real benchmark execution with a split backend, **not** correctness against independently checked WPA-PBKDF2/PMKID/EAPOL vectors.

An attempted known-vector test was blocked by the execution environment and was not run. Treat self-test and verification correctness as unverified; do not publish the binary based only on the benchmark. After the benchmark, confirmed existing application `module_22000.so` (1,061,768 bytes) and `m22000-pure.cl` (11,524 bytes) were restored and temporary frontend/backups were absent. `git diff --check` passed. No commit or push performed.

## Split program error-path resource cleanup (2026-10-29)

Reviewed `source/src/backend.c` and found that failed auxiliary program compilation could leave the first program allocated when skipping the device. Added explicit release of `opencl_program_m22000_aux` and `opencl_program` and free of `build_options_module_buf` on second-program failure; also release base program and free the buffer on the unexpected missing split-marker path. Confirmed `cache_disable` prevents both binary cache reading and writing in `load_kernel` for both split programs. Full Android arm64-v8a build (NDK r29, one job, no packaging) completed `BUILD_EXIT:0`, frontend 1,516,128 bytes, 582 modules and four bridges. This updated binary has NOT yet been benchmarked or correctness-tested on the device. No commits or pushes.
