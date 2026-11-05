# Vulkan backend: mode 0 straight (M3)

Date: 2026-11-05

M3 makes Vulkan execute one real hashcat workload end to end:

```text
hashcat -m 0 -a 0 hash.txt words.txt --backend-ignore-opencl
```

Scope is exactly mode 0 (MD5), attack `-a 0` (straight wordlist), no rules,
candidates of 0..55 bytes (one 512-bit MD5 block). Everything else is refused
before any GPU work; the refusal table is in `docs/backend-cli-contract.md`.

## Kernel set for mode 0 / -a 0

`choose_kernel()` in `source/src/backend.c` handles a fast hash
(`ATTACK_EXEC_INSIDE_KERNEL`) with a pure kernel by launching **one** kernel,
`KERN_RUN_4`. The `kernel1`, `kernel2p`, `kernel2`, `kernel3` and `kernel4`
stages belong to the `ATTACK_EXEC_OUTSIDE_KERNEL` graph of slow hashes, and the
optimized-kernel branch of `choose_kernel()` selects kernel 1/2/3 by password
length only for `OPTI_TYPE_OPTIMIZED_KERNEL` (`-O`), which Vulkan refuses.

`source/OpenCL/m00000_a0-pure.cl` confirms the same picture: `m00000_mxx`
copies the password, runs `md5_init` / `md5_update` / `md5_final` and calls
`COMPARE_M_SCALAR`; `m00000_sxx` is the single-hash variant. For `-a 0` without
rules the inner loop (`IL_CNT`) is 1, so no rule stage exists either.

The Vulkan registry therefore contains one mode program:

| program | stages | local size | push constants | flags |
| --- | --- | --- | --- | --- |
| `m0-straight` | 1 (init + 64 MD5 rounds + compare + mark_hash) | 64x1x1 | 76 bytes | MODE0_MD5, STRAIGHT, NO_RULES, NO_BRAIN, ONE_BLOCK |

The M2 `m2-check-sum` program stays registered for the diagnostic chain.

Device-side `memset`/`bzero` stages are not needed: the host pre-fills the
4-byte result counter before every dispatch, and the candidate buffers are
written through the normal upload path.

## Descriptor ABI

The mode program uses ABI version 2 from
`source/include/backend_vulkan_bindings.h`.

| binding | content |
| --- | --- |
| set 0, b0 (push constant) | `kernel_param` (72 bytes) + `base_workgroup` (4 bytes) |
| set 0, b1 | `pws_buf` |
| set 0, b2 | `digests_buf` |
| set 0, b3 | `salt_bufs` |
| set 0, b4 | `esalt_bufs` |
| set 0, b5 | `tmps` |
| set 0, b6 | `hooks` |
| set 0, b7 | `result` (u32 hit counter) |
| set 0, b8..b13 | `bitmap_s1_a..d`, `bitmap_s2_a..b` |
| set 0, b14 | `plains_buf` (added in version 2) |

Version 2 exists because `mark_hash()` needs a `plain_t` output buffer; the
version-1 table ended at b13. Entries b0..b13 keep their version-1 meaning.

Push constants are declared in the GLSL module as nineteen `uint` words, not as
`uint64_t`, so the program does not depend on the optional `shaderInt64`
device feature. The layout is byte-identical to `kernel_param_t` followed by
the split-dispatch base workgroup:

```text
offset  0  bitmap_mask, bitmap_shift1, bitmap_shift2, salt_pos_host
offset 16  loop_pos (lo, hi), loop_cnt (lo, hi), il_cnt (lo, hi)
offset 40  digests_cnt, digests_offset_host, combs_mode, salt_repeat
offset 56  pws_pos (lo, hi), gid_max (lo, hi)
offset 72  base_workgroup
```

`sizeof(kernel_param_t) == 72`, `sizeof(pw_t) == 260` (65 words),
`sizeof(plain_t) == 40` (8-byte aligned struct) and
`VK_MD5_STRAIGHT_PUSH_BYTES == 76` are checked by `_Static_assert`.

## Buffer layout

With a loaded hashconfig the Vulkan runtime reuses the exact sizes hashcat
computes for the OpenCL path: `size_pws`, `size_pws_amp`, `size_pws_comp`,
`size_pws_idx`, `size_digests`, `size_salts`, `size_esalts`, `size_tmps`,
`size_hooks`, `size_results`, `size_plains`, `size_extra*`, bitmap size and
the short-term buffers. The diagnostic `-I` path has no hashconfig and uses a
fixed layout instead (131072 `pw_t`, 1 MiB bitmaps, 1024 `plain_t`, 1 MiB
result); that layout is used only by the self-check and the M2 runtime check.

`kernel_accel` and `kernel_loops` are not autotuned: M3 takes `-n`/`-u`
verbatim and otherwise uses 64 invocations per workgroup and accel 64
(`kernel_power = 16 * 64 * 64 = 65536` candidates per batch). The autotune
thread marks Vulkan as passed without measuring anything.

## Execution sequence

For every batch (`run_cracker` for a Vulkan device):

1. expand the batch from the host `pws_comp`/`pws_idx` arrays into `pw_t`
   records (the OpenCL path would run its decompress kernel here);
2. upload `pws_buf` through the normal Vulkan upload path;
3. pre-fill the result counter with zero;
4. push `kernel_param` and dispatch `m0-straight`; the dispatch splits at
   `maxComputeWorkGroupCount[0]` and rewrites `base_workgroup` per chunk;
5. wait on the single fence;
6. `check_cracked()` downloads the hit counter, then the `plain_t` records,
   and resets the counter to zero.

A candidate longer than one MD5 block stops the session with an explicit
message; it is never silently skipped.

## Self-check

Before a Vulkan device becomes schedulable it runs a fixed-vector check through
the real program. Three candidates (`"hashcat"`, `"hashcat!"`, 55 x `'a'`) and
two target digests are uploaded; the program must report exactly two hits and
the `plain_t` records must carry `gidvid`, `digest_pos` and `hash_pos`
(0/0/0 and 2/1/1). The CPU reference values are the standard MD5 digests:

```text
"hashcat"  -> 8743b52063cd84097a65d1633f5c74f5
'a' * 55   -> ef1772b6dff9a122358552954ad0df65
```

`tools/test-vulkan-md5-straight-kat.py` repeats the shader's block
construction in Python and compares it with `hashlib.md5` for lengths 0, 1, 3,
7, 54 and 55, checks that 56 bytes is rejected, and pins the static contract
(ABI version, binding numbers, local size, one-block limit, SPIR-V header).

## Scheduling rules

A registered Vulkan device participates in a run when

* it was selected with `-d` / `--backend-device`, or
* it is the only active backend (for example `--backend-ignore-opencl`).

Without an explicit selection and with an active OpenCL device, Vulkan is
detected and self-checked for `--backend-info-json` but receives no work in that
run. `schedulable` in JSON is true only after the self-check passed.

## Reproduce

```powershell
# build (all Android ABIs include Vulkan)
.\scripts\BUILD-ANDROID.ps1 -Abi all

# one ABI only
.\scripts\BUILD-ANDROID.ps1 -Abi arm64-v8a

# diagnostic M1/M2 chain (unchanged)
.\scripts\BUILD.ps1 -Target android-arm64-v8a -AndroidApi 26 `
    -EnableVulkanDiscovery -VulkanOutputName vulkan-runtime-demo -Package:$false

# static CPU reference
python tools\test-vulkan-md5-straight-kat.py
```

On device:

```text
adb push build\arm64-v8a /data/local/tmp/m3
cd /data/local/tmp/m3
LD_LIBRARY_PATH=. ./hashcat --backend-info-json --backend-ignore-opencl
LD_LIBRARY_PATH=. ./hashcat -m 0 -a 0 hash.txt words.txt --backend-ignore-opencl -o out.txt
```

## Verification

### RedMagic 7S Pro (Adreno 730, Android 13, arm64-v8a, production build)

Verified on the arm64-v8a production build (`ENABLE_VULKAN=1`, archive
`release/hashcat-v7.1.2-android-arm64-v8a.zip`):

```text
$ LD_LIBRARY_PATH=. ./hashcat -m 0 -a 0 m3-hash.txt m3-words.txt \
    --backend-ignore-opencl -o out.txt --potfile-disable
Status...........: Cracked
RC=0
$ cat out.txt
8743b52063cd84097a65d1633f5c74f5:hashcat
```

Self-check on the same binary:

```text
Vulkan program m0-straight ready: 8308 bytes SPIR-V, local=64x1x1, push_constants=76.
Vulkan dispatch m0-straight complete: groups=1 splits=1 local=64x1x1.
Vulkan MD5 straight self-check passed: vectors=3 hits=2 (1-block 0..55 bytes).
Vulkan device #1: self-check passed (mode 0 straight); schedulable=1.
```

`--backend-info-json` with OpenCL present reports
`"id": "vulkan", "detected": true, "schedulable": true`.

Refusals, all `RC=255`, no crash:

| command | message |
| --- | --- |
| `-m 1000` | `Vulkan backend: hash mode 1000 is not implemented; only -m 0 (MD5) is available.` |
| `-m 22000` | `Vulkan backend: hash mode 22000 is not implemented; only -m 0 (MD5) is available.` |
| `-a 3` | `Vulkan backend: attack mode -a 3 is not implemented; only -a 0 (straight wordlist) is available.` |
| `-r` | `Vulkan backend: rules (-r / --generate-rules) are not implemented.` |
| `-O` | `Vulkan backend: -O / --optimized-kernel is not implemented.` |
| 56-byte candidate | `Vulkan mode 0 straight supports candidates of 1..55 bytes; candidate 0 is 56 bytes.` |

The 56-byte refusal comes from `backend_vulkan_validate_workload()` and is a
hard `RC=255`. A Vulkan-only session no longer reaches it for wordlist input:
see the next section.

## Password-length contract

The mode 0 straight program hashes one 512-bit MD5 block, so the candidate
range is 0..`VK_MD5_STRAIGHT_MAX_LEN` (55) bytes. That limit is published
through `hashconfig->pw_max` in `source/src/interface.c`, at the end of
`hashconfig_init()`:

```text
Kernel.Feature...: Pure Kernel (password length 0-55 bytes)
```

Three constraints shaped the placement:

* `hashconfig_init()` writes `pw_max` from `default_pw_max()` and then from
  `module_pw_max()`, so any earlier assignment in the same function is
  overwritten. The clamp has to be the last writer.
* `backend_vulkan_devices_activate()` runs before `hashconfig_init()`, when
  `hashconfig->pw_max` is still `0`; a diagnostic build confirmed
  `before=0 after=0`. Publishing the limit there cannot work.
* `hashconfig` is global. The clamp is therefore applied only when every
  selected, non-skipped device is a Vulkan device. A mixed Vulkan + OpenCL
  session keeps the OpenCL limits, and the Vulkan program still refuses
  oversized candidates through `backend_vulkan_validate_workload()`.

Measured effect on the RedMagic Adreno 730 with
`-m 0 -a 0 --backend-ignore-opencl` and a wordlist holding a 60-byte candidate
plus `hashcat`:

```text
Status...........: Cracked
Kernel.Feature...: Pure Kernel (password length 0-55 bytes)
Recovered........: 1/1 (100.00%) Digests (total), 1/1 (100.00%) Digests (new)
Rejected.........: 1/2 (50.00%)
```

The 60-byte candidate is now counted as `Rejected` by the wordlist reader and
the run continues, instead of aborting the whole session with `RC=255` on the
first oversized candidate. The same binary on an Adreno OpenCL-only session
still reports `Optimized Kernel (password length 0-31 bytes)`, so the clamp
does not leak into OpenCL.

### Pixel 9 Pro XL (Mali-G715, Android 16, arm64-v8a)

The same arm64-v8a production binary was pushed to the Pixel and produced the
same result:

```text
RC=0
Status...........: Cracked
8743b52063cd84097a65d1633f5c74f5:hashcat
```

`--backend-info-json` reports the Vulkan device as detected and schedulable.

Not yet verified at the time this document was written:

* `--increment`, `-b` and the 1 MiB wordlist coverage counter.
* Speed comparison and the OpenCL regression gate.

`-d <vulkan-id>` with OpenCL present is verified: P3 leaves the derived
optimized-kernel bit unset for a Vulkan-only selection, so `-d 2` runs the
Vulkan device and reports `Status: Cracked`. `--backend-ignore-opencl` and
`-d 2` produce the same result.

## OpenCL regression

The gate was run on this tree and is green:

```text
powershell -File tools/check-opencl-regression.ps1 `
    -HashcatExecutable build\windows-x64\hashcat.exe `
    -Baseline docs\opencl-baseline.json

Devices: 1 match the baseline
OK: -m 0 device #1: 13,270,800.0 kH/s vs 14,042,200.0 kH/s baseline (-5.5%)
OK: -m 1000 device #1: 15,582,300.0 kH/s vs 15,367,900.0 kH/s baseline (+1.4%)
OK: -m 22000 device #1: 496.6 kH/s vs 459.2 kH/s baseline (+8.1%)
```

The `windows-x64` build carries every shared-source change of this branch. The
Vulkan objects are absent from it by construction: `source/src/Makefile` builds
`VULKAN_OBJECTS` only for the Android target and rejects `ENABLE_VULKAN=1`
elsewhere (lines 490-516), so the desktop binary is an OpenCL-only probe of the
same `backend.c` / `interface.c` / module tree.

Interpretation: the device list is identical to the baseline and all three
benchmarks stay inside the warn threshold of 10 %. The two OpenCL-only changes
are the Adreno optimized-kernel preference and the removal of the
`M22000_NOINLINE` attribute from `hmac_sha1_run_V`, both of which only apply to
a selected Adreno OpenCL device.
