# Mode 22000 OpenCL compilation on Mali-G715 (Pixel 9 Pro XL)

Status: **resolved in HASHCAT-APP 1.1.20+** through the LD_PRELOAD heap shim
described under "Resolution" below. The measurements in the first half of this
document are the evidence that led there.

Device: Pixel 9 Pro XL (`komodo`), Android 16, ARM OpenCL 3.0 on `Mali-G715 r0p0`,
15356 MB device memory, 7 MCU. Runtime: the published `v7.1.2-fork.1` assets that
`HASHCAT-APP` installs into `files/hashcat/runtime`, executed through
`libhashcat_exec.so` from the application native library directory with
`LD_LIBRARY_PATH` containing `/vendor/lib64/egl:/vendor/lib64`.

## Symptom

Any mode 22000 job aborts during kernel compilation:

```
Initializing bridge salts... Initializing device kernels and memory...
Initializing backend runtime for device #1... mmap failed: Out of memory
SIGABRT (134), Abort message: 'Scudo ERROR: internal map failure (error desc=Out of memory)'
```

The native stack ends inside the Mali compiler:
`clCompileProgram` -> `clcc::ProgramContext::compile_program_from_source` ->
`clcc::run_early_inliner` -> `llvm::InlineFunction` -> `PruningFunctionCloner::CloneBlock`
-> `llvm::ValueMapper` -> `operator new`.

## Measurements

| Property | Value |
| --- | --- |
| `MemAvailable` during the crash | ~10 GB (16 GB total, 7.9 GB swap) |
| Mappings before the compile phase | ~1200 (`/proc/<pid>/maps` lines) |
| Mappings at abort | **65 601** |
| `VmRSS` at abort | ~8.0-8.4 GB |
| `VmSize` | ~12-12.6 GB |
| Mapping growth rate | 1213 -> 37727 -> 65601 within ~0.6 s |

`65 601` is just above the Linux default `vm.max_map_count` (65 530), which is not
readable on this build without root, so this is an inference: the process runs out of
**address-space mappings**, not of RAM. Mode 0 (MD5) and mode 100 (SHA-1) compile with
1 305 and 1 486 mappings respectively on the same device.

## Compiled options (dumped from inside the kernel with `_Static_assert`)

PMKID attack, `-a 3`, application runtime:

```
VECT_SIZE=1 KERN_TYPE=22000 ATTACK_EXEC=10 ATTACK_KERN=3 ATTACK_MODE=3
MAX_THREADS=1024 INC=/data/data/fuck.system.hashcat/files/hashcat/runtime/OpenCL
```

All runtime `OpenCL/*.cl` and `OpenCL/inc_*.h` files in the application match the
repository sources byte for byte (`sha256` checked for `m22000-pure.cl`,
`inc_common.cl`, `inc_hash_sha1.cl`, `inc_vendor.h`, `inc_types.h`,
`inc_comp_multi.cl`).

## Bisection results

All rows: one PMKID vector (`WPA*01*4d4fe7aa...`, passphrase `hashcat!`), mask
`hashcat!`, `--self-test-disable`, application runtime. Kernel bodies were replaced by
empty stubs; "stub" means the whole kernel body was removed but the signature kept.

| Variant | Mappings | Result |
| --- | --- | --- |
| stock kernel | 65 601 | abort 134 |
| all kernels stubbed | 65 601 | abort 134 |
| all kernels stubbed, `hmac_sha1_run_V` body emptied | 564 | compiles ("kernel not found" error only) |
| all kernels stubbed, `hmac_sha1_run_V` with a **single** `sha1_transform_vector` call | 65 601 | abort 134 |
| `sha1_transform_vector` marked `__attribute__ ((noinline))` | 65 601 | abort 134 |
| `check()` replaced by `1` in `inc_comp_multi.cl` (`-b` benchmark path) | 536 | compiles |
| minimal TU: hashcat includes + one `sha1_transform_vector` call, no kernel logic | ~600 | compiles in ~70 s |
| truly minimal TU: `__kernel void m22000_*` with no includes | 615 | compiles instantly |

Conclusions supported by these rows:

1. The blow-up is produced by inlining `sha1_transform_vector` inside the mode 22000
   translation unit; an unused helper is enough, kernel bodies are irrelevant.
2. `noinline` on the transform does not change the outcome on Mali.
3. The same TU with a single transform call still needs ~70 s and lands near the
   mapping limit, so small reductions are not sufficient; the compile cost has to drop
   by an order of magnitude.
4. `-O` (optimized kernels) does not exist for mode 22000; `-w 1` and
   `HASHCAT_MEMORY_LIMIT_MB` do not change the compile path.

## Options that were already tried earlier (see `android-gpu-multimode-benchmarks.md`)

`-cl-opt-disable`, `noinline` on pure helpers, two-program split of mode 22000,
`M22000_ADRENO_NOINLINE` for Mali: all still abort with 134.

## Candidate directions (not yet verified)

1. Replace the unrolled `sha1_transform_vector` inlining with a smaller, looped
   implementation for Mali so the front end has an order of magnitude less IR.
2. Serve the compiler's heap from a preloaded arena allocator (`LD_PRELOAD` from
   `HashcatRunner`) so the mapping count stays low; the compile then needs ~8 GB RSS
   once, after which hashcat's kernel cache removes the cost.
3. Split the mode 22000 program so that `hmac_sha1_run_V` is compiled in a program that
   contains almost nothing else (the existing `M22000_SPLIT_*` guards are a starting
   point).

## Arena allocator experiment (2026-11-05, second round)

A minimal size-class arena allocator (`build/mali-22000/arena.c`, built with the NDK as
`libarena.so`) was preloaded into the hashcat process. Result for the same PMKID vector,
`-a 3 hashcat!`, application runtime:

| Metric | stock allocator | arena preload |
| --- | --- | --- |
| mappings | 65 601 | 452-465 |
| `VmRSS` | 8.4 GB | 6.0 GB |
| compile result | `SIGABRT` (134) | **builds successfully** |
| job result | crash | `Status: Error` (see below) |

Control: mode 0 (MD5) with the same preload still returns `Status: Cracked`,
`Recovered: 1/1`, exit 0, so the shim itself does not break working modes.

With the compile unblocked, the next failure is `clSetKernelArg(): CL_INVALID_ARG_INDEX`
reported about 107 times right after `Starting autotune`, and the job ends with
`Status: Error`, 0 H/s. It is not yet localized.

## Kernel argument behaviour on Mali

`build/mali-22000/clprobe*.c` are standalone NDK-built probes (dlopen `libOpenCL.so`)
that build a source file and inspect kernels. Findings on Mali-G715, ARM OpenCL 3.0:

- `clGetKernelInfo(CL_KERNEL_NUM_ARGS)` returns **0** for every kernel, including a
  kernel that reads all of its 25 parameters. This is a driver reporting quirk and is
  not by itself the reason hashcat fails.
- `clSetKernelArg()` on the same kernels succeeds for indices 0..24 and fails only at
  index 25, so kernel parameters are **not** dropped even when the body never reads
  them (`probe-used.cl` and `probe-unused.cl` behave identically).

Consequently the `CL_INVALID_ARG_INDEX` seen by hashcat is not explained by Mali
dropping unused parameters, and the next step is to log, per kernel, the argument
index that fails (a `dlsym` interposer or an instrumented hashcat build) rather than
guessing from the kernel source.

## Resolution

Two independent defects produced the mode 22000 failure.

### A. Address-space mappings, not RAM

The Mali LLVM front end allocates tens of thousands of small blocks while
inlining `sha1_transform_vector`; Bionic/Scudo backs enough of them with separate
mappings that the process crosses `vm.max_map_count` (65 530) and the compiler
dies with `mmap failed: Out of memory` / SIGABRT 134 while ~10 GB of RAM is free.

Fix shipped in `HASHCAT-APP`: `scripts/native/hc-arena/arena.c` builds a small
`libhc_arena.so` (prebuilt for all four ABIs through
`scripts/BUILD-HC-ARENA.ps1`, packaged in `app/src/main/jniLibs/<abi>/`). It
reserves one large virtual range, commits it in 64 MB steps and serves every
allocation from bump pointers plus per-size-class free lists, so the mapping
count stays flat. `HashcatRunner` sets `LD_PRELOAD` to it for modes
22000/22001/22002 only; every other mode keeps the stock allocator.

Measured on the same PMKID vector, `-a 3 hashcat!`:

| Metric | stock allocator | `libhc_arena.so` |
| --- | --- | --- |
| mappings | 65 601 | **460** |
| peak `VmRSS` | 8.4 GB before abort | 10.7 GB, compile completes |
| job result | SIGABRT 134 | `Status: Cracked`, 1/1 |

Mixed reference vector (`tests/fixtures/android-gpu/redmagic-mixed-wpa/mixed.hash`,
wordlist `hashcat!`): `Status: Cracked`, **4/4 digests recovered**, 457 mappings,
peak RSS 12.3 GB, exit 0.

### B. Stale kernel cache masquerading as a Mali argument bug

After the compile started succeeding, hashcat reported
`clSetKernelArg(): CL_INVALID_ARG_INDEX` about 107 times and ended in
`Status: Error`. The cause was not Mali: hashcat caches compiled programs in
`<runtime>/kernels/<source>.<checksum>.kernel`, and the checksum covers the
device, the module name and the build options -- **not the contents of the
`.cl` file**. An earlier bisection run had compiled a stub `m22000-pure.cl`
into that cache, so every later run reused a kernel with a different argument
list. `rm -f runtime/kernels/m22000-pure.*.kernel` made the argument errors
disappear; the real kernel accepts all 25 `KERNEL_ATTR` arguments.

Fix (fork, commit `d653556`): the kernel cache checksum now also covers the
contents of the `.cl` file, so editing a kernel invalidates its cached binary.
Verified on the same device: appending a comment to
`OpenCL/m00000_a3-optimized.cl` changed the cached name from
`m00000_a3-optimized.83bfec5f.kernel` to `m00000_a3-optimized.488a41ca.kernel`
and hashcat recompiled for 39 s instead of reusing the stale program. Benchmark
`-m 0 -b -O` stayed at 1223 MH/s, matching the unpatched build (1208 MH/s).

Until that fix lands in a published runtime, deleting `runtime/kernels/` after an
`OpenCL/*.cl` change is still required.

## Remaining limitation

The one-time compile needs ~10.7 GB RSS (12.3 GB for the mixed vector) on the
Mali driver, which only fits comfortably on 12-16 GB devices. hashcat's kernel
cache turns every later run into a no-op compile, and mode 0 (MD5) keeps the
stock allocator and its 1 300 mappings. Reducing the compile cost itself still
requires a fork-side change (see directions 1 and 3 above); the arena shim is a
runtime workaround, not a compiler fix.

## Real captures on the device (application, OpenCL, Mali-G715)

Captures from `F:\AIRTOOLS\CAPS`, wordlist of the known network passwords,
run through the application UI (mode 22000, OpenCL backend):

| Capture | Result | Time |
| --- | --- | --- |
| `ALEKSEEVKA/YES-baron-01.cap` | `Cracked`, 1/1 | ~1 s after the one-time compile |
| `LENINA/DIR-806A-1f50-01-YES.cap` | `Cracked`, 1/1 | 1 s |
| `ALEKSEEVKA/YES-Huawei-0050-02.cap` | `Cracked`, 2/2 | 0 s |
| `LENINA/MTSRouter-7F6D91-YES.cap` | `Cracked`, 1/1 | 1 s |

Steady-state mask throughput on the same device stayed at 19 036 H/s for 6.5 M
candidates, and the wordlist runs above do not recompile because the kernel
cache key is unchanged for a given attack mode.

Note that the cache is keyed per attack mode, so the first wordlist run after a
mask run pays the ~3 minute Mali compile again (peak RSS ~10.8 GB); later runs
of the same mode start in about a second.

## Verification of the fork build itself

The Android arm64 runtime built from this branch (commit `d653556`) was pushed
to `/data/local/tmp/hcfull` together with its own `OpenCL/`, `modules/` and
`hashcat.hcstat2`, and run on the same device against the mixed fixture:

```
$ ./libhashcat_exec.so -m 22000 -a 0 --self-test-disable \
    --potfile-path pot --outfile out --backend-ignore-vulkan mixed.hash words.txt
Status...........: Cracked
Recovered........: 4/4 (100.00%) Digests
EXIT=0
```

The cached kernel for that build is `m22000-pure.<checksum>.kernel`, where the
checksum now covers the source file, and a subsequent edit of
`OpenCL/m00000_a3-optimized.cl` produced a new cache name and a recompile, as
described above. The same build benchmarks MD5 (`-m 0 -b -O`) at 1223.1 MH/s,
within 1% of the unpatched build, so the cache-key change has no runtime cost.
