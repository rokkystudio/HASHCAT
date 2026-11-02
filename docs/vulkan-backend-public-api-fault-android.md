# Production four-stage Vulkan API-fault cleanup on Android

Date: 2026-11-02

## Scope

The production information-mode Vulkan executor already owns buffers, memory,
descriptor objects, shader modules, Compute pipelines, command objects and one
temporary VkDevice dynamically from `backend_vulkan_program_t`.

This diagnostic verifies the next error boundary: negative `VkResult` values
returned by the actual linked call sites used by
`source/src/backend_vulkan_kernel.c`.

The production executor source is not given a diagnostic branch. Dedicated
Android executables link selected Vulkan symbols through `ld.lld --wrap`.
Only the selected call returns a test-generated error; earlier calls and all
cleanup calls use the real Android Vulkan loader.

No physical GPU loss, reset, hang or driver-memory exhaustion is induced.
The reported failures are test-linker results, not failures observed from the
device driver. HASHCAT password candidates, salts, target digests and the
ordinary hashing scheduler remain disconnected.

## Fixed API-fault scenarios

| Site | Injected call | Earlier real calls | Synthetic result |
| --- | ---: | ---: | ---: |
| `vkCreateBuffer` | 5th buffer | 4 buffer creations | `VK_ERROR_OUT_OF_DEVICE_MEMORY` (-2) |
| `vkCreateShaderModule` | 4th shader | 3 shader creations | `VK_ERROR_OUT_OF_DEVICE_MEMORY` (-2) |
| `vkCreateComputePipelines` | 4th pipeline | 3 pipeline creations | `VK_ERROR_OUT_OF_DEVICE_MEMORY` (-2) |
| `vkQueueSubmit` | 1st submit | no real submit | `VK_ERROR_DEVICE_LOST` (-4) |
| `vkWaitForFences` | 1st wait | 1 real submit | `VK_ERROR_DEVICE_LOST` (-4) |

The resource-creation positions are deliberate. They leave an already populated
dynamic job for the production cleanup path:

- buffer failure occurs after four complete storage-buffer allocations;
- shader failure occurs after three complete stage pipelines and allocation of
  the fourth descriptor set;
- pipeline failure occurs after four shader modules and three pipelines;
- submit failure occurs after all buffers, descriptors, shaders, pipelines,
  command pool, command buffer and fence exist;
- wait failure occurs after the real command buffer has entered the GPU queue.

Every failed job executes the production `backend_vulkan_public_release()`
path. While the test interceptor remains armed, its
`vkDeviceWaitIdle` wrapper only observes and forwards the real loader call.
The wrapper never injects a wait-idle failure.

After cleanup the interceptor is disabled. A fresh VkDevice created under the
same backend-owned VkInstance executes the ordinary four-stage public program:
four `vkCmdDispatch` calls, three Compute write-to-read barriers and exact
CPU comparison of all 256 output words.

## Test-only driver contracts

### `backend_vulkan_public_api_fault_arm(site, fail_on_attempt, result, diagnostics)`

Arms exactly one wrapped Vulkan site and a one-based call number. Resource
creation sites accept `VK_ERROR_OUT_OF_DEVICE_MEMORY`; submit/wait sites
accept `VK_ERROR_DEVICE_LOST`. The method stores process-local test state and
performs no Vulkan operation.

### `backend_vulkan_public_api_fault_snapshot(counts)`

Copies the current selected-site attempts, real selected calls, real queue
submissions, real fence waits, cleanup wait-idle calls, injected count and
latched error. It performs no Vulkan operation.

### `backend_vulkan_public_api_fault_disarm()`

Reports the final counters and clears the process-local interceptor state.
The production executor has already released its failed VkDevice before this
method is called. Subsequent wrapped calls pass directly to libvulkan.

### Wrapped Vulkan entry points

The dedicated executable defines test-only wrappers for:

```text
vkCreateBuffer
vkCreateShaderModule
vkCreateComputePipelines
vkQueueSubmit
vkWaitForFences
vkDeviceWaitIdle
```

Unarmed and non-selected calls invoke their `__real_*` Vulkan symbol.
At the selected attempt the first five sites return the configured synthetic
error without invoking the real driver at that call. Creation outputs are set
to `VK_NULL_HANDLE` when their call is intercepted.

`vkDeviceWaitIdle` is different: it is never faulted. While a scenario is
armed it counts the call, executes the real Vulkan function and records the
actual result. This proves that the wait-failure case drains its already
submitted command before resource destruction.

## Native diagnostic contract

`tools/vulkan-backend-public-api-fault-diagnostic.c` opens one production
Vulkan catalogue and keeps its VkInstance live across all five scenarios.

For each scenario it performs:

```text
arm test-only API boundary
-> run production backend_vulkan_kernel_verify_chain4()
-> require exact injected VkResult
-> require one successful real vkDeviceWaitIdle cleanup
-> disarm test interceptor
-> require HASHCAT scheduler state unchanged
-> run a fresh production backend_vulkan_kernel_verify_chain4()
-> verify 4 dispatches and 256 public uint32 results
```

One requested Vulkan API therefore contains five negative jobs and five healthy
recovery jobs. The final summary is:

```text
BACKEND_API_FAULT_DIAGNOSTIC_PASS ... scenarios=5 injected=5
cleanup_wait_idle=5 recovered_devices=5 recovered_dispatches=20
verified_words=1280 real_submit_before_wait_fault=1
real_driver_failures=0 password_inputs=0 scheduled=0
```

## Build

Build both Android API24 ARM ABIs:

```powershell
.\tools\build-vulkan-backend-public-api-fault.ps1 \
  -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865 \
  -AndroidApi 24
```

The build uses `-Wall -Wextra -Werror`, links the exact production Vulkan
executor sources and enables six test-only linker wrappers only in these
diagnostic binaries.

Current binaries:

| ABI | Bytes | SHA-256 |
| --- | ---: | --- |
| ARM64 | 46,256 | `4bf77d2fa46d9f31b4fdec488af07fe66faec9985309a09d847a071ccf37fc44` |
| ARMv7 | 45,512 | `829a7e7294f94bca093d63590c02c53abb7a4d95821533a513c024ca321e57c6` |

## Hardware results

Physical Android runs completed successfully for both requested Vulkan 1.0 and
1.1 contracts:

| GPU / process ABI | API 1.0 | API 1.1 |
| --- | --- | --- |
| Pixel 9 Pro XL / Mali-G715 ARM64 | PASS | PASS |
| RedMagic 7S Pro / Adreno 730 ARM64 | PASS | PASS |
| RedMagic 7S Pro / Adreno 730 ARMv7 | PASS | PASS |

Totals across six native runs:

| Property | Count |
| --- | ---: |
| Test-generated negative Vulkan results | 30 |
| Real production cleanup `vkDeviceWaitIdle` calls | 30 |
| Healthy recovery VkDevices | 30 |
| Recovery `vkCmdDispatch` calls | 120 |
| CPU-verified public `uint32` outputs | 7,680 |
| Real queue submissions before intercepted fence waits | 6 |
| Physical driver failures induced or claimed | 0 |

All six native processes exited 0. The fence-wait scenario reports one real
queue submission, no real `vkWaitForFences`, then a successful real
`vkDeviceWaitIdle` from the production cleanup path.

## Reproducible device runner

The runner stages only the selected ABI executable and writes LF-normalized
transcripts without the ADB serial:

```powershell
.\tools\run-vulkan-backend-public-api-fault.ps1 \
  -Serial $DeviceAdbSerial \
  -Abi arm64-v8a \
  -ReportPrefix pixel-arm64 \
  -TestCore11
```

Use `-Abi armeabi-v7a` on a device that advertises the 32-bit ARM ABI.

## Boundary established

The dynamic executor now has hardware evidence for both categories of
application-side failure:

1. controlled aborts after each resource acquisition and around submission;
2. synthetic negative Vulkan API returns delivered at the production linked
   call sites for buffer creation, shader creation, pipeline creation, queue
   submission and fence waiting.

This does not establish recovery from a genuinely lost VkDevice. A physical
`VK_ERROR_DEVICE_LOST` may imply driver state that a test linker cannot
reproduce. The diagnostic establishes that the production executor propagates
these negative results, performs its owned-resource cleanup, leaves the parent
VkInstance usable and can create a fresh healthy four-stage device afterward.

The follow-on [real fence-timeout diagnostic](vulkan-backend-public-fence-timeout-android.md)
uses no test-linker wrapper at the synchronization call. It observes an actual
driver `VK_TIMEOUT` after real four-stage submission, performs production
device-idle cleanup and verifies a fresh same-instance VkDevice on Mali ARM64
and Adreno ARM64/ARMv7. That result is a deterministic unsignaled-fence timeout,
not physical device loss or allocation exhaustion.

Cryptographic HASHCAT SPIR-V kernels and the normal password-processing
scheduler are still outside this diagnostic.
