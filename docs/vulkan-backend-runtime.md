# Vulkan execution through the hashcat device table (M2)

Date: 2026-11-05

> Historical note (M3): this document describes the M2 state, where Vulkan
> was verified but intentionally unschedulable. Since M3 the backend executes
> `-m 0 -a 0` and `vulkan.schedulable` can be `true`; see
> `docs/vulkan-backend-md5-straight.md`. Statements below about "hashing
> remains disabled" and `schedulable: false` are M2 history, not current
> behaviour.

Milestone M2 turns the Vulkan entry that M1 registered in the hashcat device
table into a complete diagnostic compute device: it owns a logical device,
allocates the whole hashcat buffer set, compiles a SPIR-V program from a
versioned registry, dispatches it, reads the result back and verifies the
data against a CPU reference.

Vulkan hashing is still disabled. No hash mode is scheduled, no mode kernel
exists, `vulkan` stays `schedulable: false`, and `-m 0 -a 0` keeps running
through OpenCL exactly as before.

## Commit map

| commit | closes |
| --- | --- |
| `4e1aaea` | T1 — Vulkan metadata moves into `hc_vulkan_device` |
| `f29609e` | T2 — full hashcat buffer set, upload/download, teardown |
| `a7b5caf` | T3 — binding ABI v1, registry, descriptor/pipeline creation |
| `76adf9c` | T4 dispatch + T6 check program (both landed here) and the direct-buffer teardown fix |
| `6c6f0d7` | T5 — `-I` chain through the new owner, old diagnostic chain removed |
| `7087a17` | T6 — runner, verifier, this document, remaining header surface |
| `395af28` | build artifact sizes |

T4 and T6 share `76adf9c` because of commit staging order; history is kept
as-is instead of being rewritten.

## Pipeline

```text
catalogue VkPhysicalDevice
        |
        v
hc_vulkan_device metadata -> VkDevice + Compute queue + command pool/fence
        |
        v
37 hashcat storage buffers (host-visible or device-local + staging)
        |
        v
descriptor set layout v1 + pipeline layout + SPIR-V pipeline
        |
        v
vkCmdDispatch (split at maxComputeWorkGroupCount) -> fence -> invalidate
        |
        v
result readback -> CPU reference comparison
```

## Ownership

`hc_vulkan_device` (in `source/include/backend_vulkan_device.h`) is the only
owner of Vulkan objects. `hc_device_param_t` keeps just `is_vulkan` and the
pointer; no Vulkan metadata is stored in the reused `opencl_device_*` slots
any more. `terminal.c` and `backend_info_json.c` dereference
`device_param->vulkan` only after an `is_vulkan` check.

The structure also owns the buffer slots, the cached pipelines, the reusable
command buffer/fence, the descriptor pool/set and the pipeline layout. The
catalogue still owns `VkInstance` and `VkPhysicalDevice` and is closed after
every `VkDevice` is destroyed.

## Metadata (T1)

`backend_vulkan_devices_init()` reads `VkPhysicalDeviceProperties` and stores
the device name, `VkPhysicalDeviceType`, vendor id, vendor name and
`Vulkan MAJOR.MINOR` string in `hc_vulkan_device`. `-I` and
`--backend-info-json` print the same content as M1; only the storage moved.

## Memory (T2)

`backend_vulkan_devices_memory_init()` opens the logical device and allocates
the 37 slots used by the OpenCL backend:

```text
pws, pws_amp, pws_comp, pws_idx, rules, rules_c, combs, combs_c,
bfs, bfs_c, tm_c, bitmap_s1_a..d, bitmap_s2_a..d, plain, digests,
digests_shown, salts, esalts, tmps, hooks, result, extra0..3,
root_css, markov_css, st_digests, st_salts, st_esalts, kernel_param
```

When a hashconfig is loaded and hashcat has already computed `size_*` for the
device, those exact values are reused. The `-I` path has no hashconfig, so it
uses a fixed diagnostic layout: 131072 `pw_t` elements, a 1 MiB bitmap, a
1 MiB result buffer and otherwise the same derivations as the OpenCL path.
This is the only layout difference and it exists solely for the verification
path.

Memory policy:

1. Prefer a host-visible **coherent** memory type, preferring
   `DEVICE_LOCAL` when the driver offers it. The CPU maps the storage buffer
   directly.
2. If only host-visible **non-coherent** memory exists, use it and call
   `vkFlushMappedMemoryRanges` before GPU reads and
   `vkInvalidateMappedMemoryRanges` after GPU writes.
3. If no compatible host-visible type exists, allocate device-local memory
   and a separate host-visible staging buffer. Upload/download copy through
   staging with `vkCmdCopyBuffer`, a transfer barrier and the shared fence.

On Adreno 730 and Mali-G715 the first path is available for all 37 slots, so
the normal runs below never allocate staging memory. `HASHCAT_VULKAN_FORCE_STAGING=1`
forces path 3 for testing; the same selftest and check pass through staging.

A slot larger than `maxStorageBufferRange` is refused before allocation with
an explicit message. Every failure path closes the partially built runtime.

Teardown distinguishes direct from staged slots: in direct mode
`host_buffer`/`host_memory` alias `buffer`/`memory`, so only the staging mode
releases a second pair. Mali-G715 aborted with a Scudo invalid-chunk-state
error inside `vkDestroyBuffer` while this aliasing was ignored; Adreno
tolerated the duplicate destroy. Both devices now run ten clean cycles.

## Descriptor ABI v1 (T3)

`source/include/backend_vulkan_bindings.h` fixes the contract:

| binding | content |
| --- | --- |
| set 0, b0 | `kernel_param` in a 128-byte Compute push-constant range |
| set 0, b1 | `pws_buf` |
| set 0, b2 | `digests_buf` |
| set 0, b3 | `salt_bufs` |
| set 0, b4 | `esalt_bufs` |
| set 0, b5 | `tmps` |
| set 0, b6 | `hooks` |
| set 0, b7 | `result` |
| set 0, b8..b11 | `bitmap_s1_a..d` |
| set 0, b12..b13 | `bitmap_s2_a..b` |

`VK_BINDINGS_VERSION` is `1`. Static asserts check the table count, the
version constant and that `kernel_param_t` fits the 128-byte push-constant
range. A program whose table, version or SPIR-V header differs is rejected
before a pipeline object is created. The same build also rejects a local
size above `maxComputeWorkGroupInvocations`/`maxComputeWorkGroupSize`, and a
push-constant block above the limit.

The registry currently contains one program, `m2-check-sum`. Per-mode
programs arrive in M3 and must keep this table or bump the version.

## Dispatch (T4)

`backend_vulkan_device_dispatch()` binds the pipeline and descriptor set,
pushes the program constants, and splits the requested workgroup count into
chunks of at most `maxComputeWorkGroupCount[0]`. Adreno 730 reports 65535, so
131072 workgroups become three `vkCmdDispatch` calls in one command buffer,
separated by a Compute-to-Compute memory barrier. A Compute-to-Host barrier
precedes the fence for the result buffer.

One fence covers the submission; it is reset before every submission and
waited on with a five-second timeout. On timeout the device is idled and the
call fails instead of silently reading stale data.

A split dispatch restarts `gl_GlobalInvocationID` at zero, so the last four
bytes of the push-constant block carry the base workgroup of the current
chunk. The host rewrites it before every `vkCmdDispatch`; a program that can
exceed the limit therefore must declare at least one `u32` of push constants.

## Backend wiring (T5)

`backend_ctx_devices_init_vulkan_info()` now only drives the new owner:

```text
memory_init -> buffer selftest -> registered check dispatch
            -> CPU comparison -> result readback -> memory_term
```

The old public session/kernel/MD5/SHA-256 diagnostic chain and its includes
were removed from the frontend. The standalone tools under `tools/` keep
compiling their own copies of those sources; the frontend binary no longer
links them.

`backend_ctx_devices_destroy()` releases every `VkDevice` before
`backend_ctx_destroy()` closes the catalogue/`VkInstance`.

Vulkan device entries remain `skipped`, `backend_devices_active` does not
count them, `-d` still refuses a Vulkan-only selection, and
`--backend-info-json` keeps `"schedulable": false`.

## Data-path check (T6)

`tools/vulkan-hashcat-check.comp` is compiled once into
`source/include/backend_vulkan_check_spirv.h`:

```powershell
python tools/generate-vulkan-check-kernel.py `
  --glslc D:\Android\Sdk\ndk\29.0.14206865\shader-tools\windows-x86_64\glslc.exe
```

The program processes every 32-bit word of `pws_buf` exactly once. Element
`i` is handled by workgroup `i mod stride`, where `stride` is the dispatch
workgroup count. Each workgroup writes a wrapping sum and a processed-element
counter into `result`. `pws_buf` is filled through the normal
`backend_vulkan_device_upload()` path, not by writing mapped memory directly.
The result buffer is pre-filled with `0xff`, so an unwritten group cannot
look like a valid zero.

The CPU reference uses the same arithmetic and compares every workgroup, not
just the total. The check fails on the first differing group, on a coverage
counter that differs from the element count, or on any Vulkan error.

## Verification results

Build results:

| target | result |
| --- | --- |
| `BUILD-DESKTOP.ps1` (windows-x64) | ok, 2456 runtime files |
| `BUILD-ANDROID-ARM64.ps1` (release arm64-v8a) | ok, frontend 1,519,224 bytes, 582 modules |
| `BUILD.ps1 -EnableVulkanDiscovery` (diagnostic arm64-v8a) | ok, frontend 1,552,008 bytes, 582 modules |

Builds and devices used:

| item | value |
| --- | --- |
| build | `BUILD.ps1 -Target android-arm64-v8a -AndroidApi 26 -EnableVulkanDiscovery -VulkanOutputName vulkan-runtime-demo -Package:$false` |
| RedMagic 7S Pro (`NX709S`) | Adreno 730, Android 13, arm64-v8a |
| Pixel 9 Pro XL (`komodo`) | Mali-G715, Android 16, arm64-v8a |

### RedMagic 7S Pro / Adreno 730

`HASHCAT_VULKAN_RUNTIME_ITERATIONS=10`:

```text
Vulkan Device #01: Adreno (TM) 730, Qualcomm, Vulkan 1.1
Vulkan device #1: buffer pws_buf             33280 KiB (host-visible, coherent=1)
...
Vulkan program m2-check-sum ready: 1660 bytes SPIR-V, local=1x1x1, push_constants=16.
Vulkan dispatch m2-check-sum complete: groups=131072 splits=3 local=1x1x1.
Vulkan check passed: groups=131072 elements=8519680 sum=728f0000 count=8519680 dispatch_splits=30
Vulkan runtime iteration 10/10: device #1 buffers=37 rss=209444 KiB
Vulkan memory lifecycle: iterations=10 rss_start=196796 rss_peak=209444 rss_end=209444 failures=0
Vulkan data path verified through the hc_vulkan_device owner; hashing remains disabled.
```

`dispatch_splits` in the check line is the cumulative counter of the device
object; 30 means three chunks per iteration. Every iteration passed.

### Pixel 9 Pro XL / Mali-G715

`HASHCAT_VULKAN_RUNTIME_ITERATIONS=10`:

```text
Vulkan Device #01: Mali-G715, ARM, Vulkan 1.4
Vulkan program m2-check-sum ready: 1660 bytes SPIR-V, local=1x1x1, push_constants=16.
Vulkan dispatch m2-check-sum complete: groups=131072 splits=1 local=1x1x1.
Vulkan check passed: groups=131072 elements=8519680 sum=728f0000 count=8519680 dispatch_splits=10
Vulkan runtime iteration 10/10: device #1 buffers=37 rss=221760 KiB
Vulkan memory lifecycle: iterations=10 rss_start=208520 rss_peak=221760 rss_end=221760 failures=0
Vulkan data path verified through the hc_vulkan_device owner; hashing remains disabled.
```

Both devices were also run through the new verifier on the final build:

```text
# RedMagic / Adreno 730
PASS log: groups=131072 elements=8519680 sum=728f0000 count=8519680 splits=30 iterations=10 rss=196796->209444 KiB
PASS json: detected=True schedulable=False device=Adreno (TM) 730 vendor_id=20803

# Pixel / Mali-G715
PASS log: groups=131072 elements=8519680 sum=728f0000 count=8519680 splits=10 iterations=10 rss=208520->221760 KiB
PASS json: detected=True schedulable=False device=Mali-G715 vendor_id=5045
```

The Adreno `splits=30` in the verifier line is the cumulative counter (three
chunks per iteration); the per-dispatch line still reports `splits=3`.

Mali reports a `maxComputeWorkGroupCount[0]` above 131072, so the same
workgroup count needs one chunk there. The splitting path itself is exercised
on Adreno with three chunks per iteration.

### RSS

Both devices reach a stable RSS after the driver warms up: Adreno
196796 -> 209444 KiB and Mali 208520 -> 221760 KiB across ten
init/check/term cycles, with no growth between later iterations. A 50-cycle
run on Adreno was also flat after the first two cycles.

### OpenCL mode 0 noise check

A single `-b -m 0 -w 2` gate sample reported 12,091.2 MH/s against the
14,042.2 MH/s baseline (−13.9 %). One sample cannot separate drift from a
regression, so the master binary (`a2626c8`) and this branch were measured in
alternating order, six pairs, with the order flipped every pair:

| binary | median |
| --- | --- |
| master (`a2626c8`) | 13,340.55 MH/s |
| branch (`feature/vulkan-device-runtime`) | 13,306.25 MH/s |
| delta | **−0.3 %** |

Per-pair deltas: +2.3, +3.3, −1.4, −0.3, +0.3, −1.2 %. Both binaries land
around 13.3 GH/s in this session, so the gap to the recorded 14.0 GH/s
baseline is session drift, not a branch regression; the branch/master ratio
is flat.

```powershell
# base binary from master, current binary from the feature branch
git worktree add D:\PROJECTS\HASHCAT-m2-base master
Push-Location D:\PROJECTS\HASHCAT-m2-base
.\scripts\BUILD-DESKTOP.ps1 -Target windows-x64
Pop-Location

.\tools\benchmark-m0-alternating.ps1 `
  -BaseHashcat D:\PROJECTS\HASHCAT-m2-base\build\windows-x64\hashcat.exe `
  -CurrentHashcat build\windows-x64\hashcat.exe -Pairs 6 -Mode 0 -WorkloadProfile 2

git worktree remove --force D:\PROJECTS\HASHCAT-m2-base
```

### Reproduce

```powershell
.\scripts\BUILD.ps1 -Target android-arm64-v8a -AndroidApi 26 `
    -EnableVulkanDiscovery -VulkanOutputName vulkan-runtime-demo -Package:$false

.\tools\run-vulkan-runtime-check.ps1 -Serial 91809037 -Abi arm64-v8a -Iterations 10
.\tools\run-vulkan-runtime-check.ps1 -Serial 46281FDAS002AL -Abi arm64-v8a -Iterations 10
```

The runner stages the diagnostic build, runs `hashcat -I
--backend-ignore-opencl`, writes the log and the `--backend-info-json`
manifest under `build/`, and calls `tools/test-vulkan-runtime-check.py`.
The verifier requires:

* `groups > 65535`;
* `count == elements`;
* a lifecycle line with `failures=0` and bounded RSS growth;
* no mismatch, abort or old `BACKEND_VULKAN_SESSION`/`BACKEND_VULKAN_KERNEL`
  marker;
* JSON `vulkan.detected = true` and `vulkan.schedulable = false`.

## Boundaries and next step

* Vulkan is still not schedulable; `-m 0 -a 0` runs through OpenCL.
* No hash-mode SPIR-V program exists; M3 adds per-mode programs against the
  v1 binding table and the hashconfig-sized buffer layout.
* Local size is fixed by the registry; there is no autotune yet.
* The check program uses `local_size_x = 1` on purpose so 131072 candidates
  produce 131072 workgroups and cross the 65535 boundary without a
  multi-gigabyte `pws_comp` buffer. Production programs will use their own
  local sizes.
