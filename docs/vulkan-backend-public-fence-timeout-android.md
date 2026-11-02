# Real Vulkan fence timeout and same-instance recovery on Android

Date: 2026-11-02

## Scope

The production four-stage public Vulkan executor now has a diagnostic for one
real non-success Vulkan synchronization result: `VK_TIMEOUT` returned by the
device driver's ordinary `vkWaitForFences` entry point.

The diagnostic uses the same validated `backend_vulkan_program_t`, dynamic
buffer and stage ownership, descriptor sets, shader modules, Compute pipelines,
command recording and cleanup path as the existing information-mode executor.
No Vulkan function is linked through `--wrap`, replaced or made to return a
synthetic value.

The four-stage command buffer is submitted with `VK_NULL_HANDLE` instead of
the separately owned fresh fence. That fence therefore remains unsignaled.
A zero-duration `vkWaitForFences` call on the real VkDevice and that real
unsignaled fence returns the driver's `VK_TIMEOUT` result. The production
cleanup then waits for actual device idle before destroying the submitted
command buffer and all dynamically owned resources.

This is a synchronization-boundary diagnostic on public integers. It does not
run HASHCAT password candidates, salts, digests, cryptographic kernels or the
ordinary hashing scheduler.

## Production executor contract

### `backend_vulkan_public_job_t::submitted`

Records whether the current temporary Vulkan job has successfully entered the
queue. The value is set only after `vkQueueSubmit` returns `VK_SUCCESS` and
is retained until the standard job cleanup clears the structure.

The flag lets the timeout diagnostic distinguish a real post-submit wait from
an error that occurred before any work reached the queue.

### `backend_vulkan_public_dispatch(job, diagnostics, probe_unsignaled_fence)`

Records the complete declared Compute program exactly as the ordinary executor
does. In normal mode it submits with the owned fence and performs the ordinary
infinite fence wait before CPU verification.

When `probe_unsignaled_fence == VK_TRUE`, the method:

```text
records all four Compute stages and barriers
-> calls real vkQueueSubmit with VK_NULL_HANDLE as submission fence
-> records submitted=1 after VK_SUCCESS
-> calls real vkWaitForFences on the separately owned unsignaled fence
   with timeout=0
-> reports the returned VkResult
-> returns VK_TIMEOUT without reading incomplete GPU output
```

An unexpected `VK_SUCCESS` from that unsignaled zero-timeout wait is treated as
`VK_ERROR_INITIALIZATION_FAILED`. Any other real Vulkan result propagates.

### `backend_vulkan_kernel_verify_program(..., probe_unsignaled_fence)`

Accepts the timeout probe only for the four-stage public program with no
controlled resource-fault checkpoint active. Ordinary one-, two- and
four-stage information-mode calls pass `VK_FALSE` and retain their existing
execution path.

For the timeout probe, cleanup requests an exact owned-resource inventory and
preserves `VK_TIMEOUT` when `backend_vulkan_public_release()` succeeds.
A cleanup error takes precedence over the observed timeout.

### `backend_vulkan_kernel_verify_chain4_fence_timeout(...)`

Requires a live backend Vulkan catalogue, valid physical-device index and
non-NULL diagnostics. It executes the production four-stage program in the
unsignaled-fence mode described above.

Successful diagnostic behavior is represented by `VK_TIMEOUT`, not
`VK_SUCCESS`: the real driver wait must time out and the full resource cleanup
must complete successfully. Invalid context, device index or diagnostics are
rejected with `VK_ERROR_INITIALIZATION_FAILED`.

## Observable native events

The real wait event is:

```text
BACKEND_PUBLIC_FENCE_WAIT submitted=1 fence_submitted=0 timeout_ns=0 driver_call=1 result=2 scheduled=0
```

Vulkan defines `VK_TIMEOUT` as result value `2`. The event is emitted after a
successful real queue submission and contains no synthetic-result marker.

The production cleanup event is:

```text
BACKEND_PUBLIC_FENCE_RELEASE submitted=1 device=1 fence=1 command_buffer=1 pipelines=4 buffers=5 wait_idle=0 completed=1 cleanup_result=0 scheduled=0
```

Here `wait_idle=0` and `cleanup_result=0` are `VK_SUCCESS`. The job still
owns all five storage buffers, four pipelines, its command buffer and its
unsignaled fence when cleanup begins.

After cleanup, the same parent VkInstance creates a new VkDevice and runs the
ordinary four-stage executor. That recovery performs four real
`vkCmdDispatch` calls and compares all 256 output words with the independent
CPU arithmetic reference.

## Dedicated native diagnostic

`tools/vulkan-backend-public-fence-timeout-diagnostic.c` keeps one production
Vulkan catalogue open across the timeout and recovery jobs.

Its fixed sequence is:

```text
reject unopened/invalid public diagnostic inputs
-> create fault-probe VkDevice
-> submit four real Compute stages
-> receive real VK_TIMEOUT from zero-duration vkWaitForFences
-> release through production vkDeviceWaitIdle cleanup
-> create fresh VkDevice under the same VkInstance
-> run ordinary four-stage program
-> verify 256 public uint32 results
-> close the backend catalogue
```

The final process event is:

```text
BACKEND_PUBLIC_FENCE_DIAGNOSTIC_PASS api=... real_queue_submissions=2 real_fence_timeouts=1 cleanup_wait_idle=1 recovered_devices=1 recovered_dispatches=4 verified_words=256 synthetic_driver_results=0 password_inputs=0 scheduled=0
```

The two queue submissions are the timeout-probe submission and the healthy
recovery submission.

## Build

Build both Android API24 ARM binaries with:

```powershell
.\tools\build-vulkan-backend-public-fence-timeout.ps1 \
  -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865 \
  -AndroidApi 24
```

The build uses `-Wall -Wextra -Werror` and links the production executor
sources directly to `libvulkan.so`. ELF inspection rejects test-linker
`__wrap_vk*` symbols and requires ordinary undefined imports for
`vkWaitForFences` and `vkDeviceWaitIdle`.

Current binaries:

| ABI | Bytes | SHA-256 |
| --- | ---: | --- |
| ARM64 | 42,136 | `baf102702c3c7636839e2b682a67155c85aab7515e1053b9875f1aaed8c8336b` |
| ARMv7 | 41,956 | `eb9fd8b2e85d391af95bbd812f149e5a5ea07fec3040e21be5bb39a891eb757d` |

## Physical Android results

The real fence-timeout diagnostic completed on all current GPU/process
configurations for both requested Vulkan instance API levels:

| GPU / process ABI | Vulkan 1.0 | Vulkan 1.1 |
| --- | --- | --- |
| Pixel 9 Pro XL / Mali-G715 ARM64 | PASS | PASS |
| RedMagic 7S Pro / Adreno 730 ARM64 | PASS | PASS |
| RedMagic 7S Pro / Adreno 730 ARMv7 | PASS | PASS |

Across six native processes:

| Confirmed property | Count |
| --- | ---: |
| Real `VK_TIMEOUT` fence results | 6 |
| Probe queue submissions before timeout | 6 |
| Real production cleanup `vkDeviceWaitIdle` calls | 6 |
| Healthy same-instance recovery VkDevices | 6 |
| Recovery Compute dispatches | 24 |
| CPU-verified public `uint32` outputs | 1,536 |
| Synthetic Vulkan driver results | 0 |

Every native process exited 0. On every run the timeout event reported
`result=2`, the cleanup event reported `wait_idle=0` and
`cleanup_result=0`, and the recovery device verified all 256 public words.

## Reproducible device runner

Run one device/ABI for Vulkan 1.0 and optionally 1.1 with:

```powershell
.\tools\run-vulkan-backend-public-fence-timeout.ps1 \
  -Serial $DeviceAdbSerial \
  -Abi arm64-v8a \
  -ReportPrefix pixel-arm64 \
  -TestCore11
```

Use `-Abi armeabi-v7a` only when the device advertises that ABI. The runner
requires the exact wait, cleanup, recovery and final summary events, writes an
LF-normalized transcript without the ADB serial and removes the staged
executable after the run.

## Boundary established

This test establishes that the production dynamic executor can receive a real
non-success result from `vkWaitForFences` after actual GPU submission, avoid
reading unfinished output, release the submitted job through a successful real
`vkDeviceWaitIdle`, and create a fresh working four-stage VkDevice under the
same VkInstance.

The deliberately unsignaled fence makes the timeout deterministic; it does not
represent a hung GPU. This evidence does not establish physical
`VK_ERROR_DEVICE_LOST`, real Vulkan allocation exhaustion, a driver-returned
queue-submit failure or recovery of a genuinely lost VkDevice.

Cryptographic HASHCAT SPIR-V kernels and the normal password-processing
scheduler remain outside this diagnostic.
