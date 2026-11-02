# Four-stage public Vulkan resource cleanup and same-instance recovery on Android

Date: 2026-11-02

## Scope

The production Android HASHCAT information-mode Vulkan executor owns
dynamically allocated host and device resources for one-, two- and
four-stage public SPIR-V 1.0 Compute programs. It uses the same explicit
program declarations, storage dependencies and CPU arithmetic reference
as the existing four-stage hardware tests.

A dedicated native diagnostic exercises **37 explicitly numbered
resource-acquisition and submission checkpoints** in the exact four-stage
executor source. Every controlled fault is generated on the host after
successful acquisition of actual Vulkan resources, not by replacing a
driver call or forging a returned `VkResult`.

A successful recovery opens a fresh `VkDevice` under the *same*
backend-owned `VkInstance`, records four real `vkCmdDispatch` commands,
waits for GPU completion and checks all 256 public `uint32` outputs
against independent CPU arithmetic. No password candidate, target hash,
salt, attack mode or ordinary HASHCAT hashing scheduler is used.

**Driver errors and simulated failures are distinct.** These tests do
not reproduce `VK_ERROR_DEVICE_LOST`, GPU resets or actual
`vkQueueSubmit` failures. The injected result is
`VK_ERROR_INITIALIZATION_FAILED` and each trigger declares
`simulated=1`. The corresponding resource acquisition calls were real.

## Exact checkpoint sequence

| Point | Acquired object / boundary |
| --- | --- |
| 1 | Four dynamic host ownership arrays |
| 2 | Real `VkDevice` |
| 3–17 | Five groups of `VkBuffer`, `VkDeviceMemory` and mapped memory |
| 18 | Descriptor-set layout |
| 19 | Pipeline layout |
| 20 | Descriptor pool |
| 21–32 | Four groups of descriptor set, shader module and Compute pipeline |
| 33 | Command pool |
| 34 | Command buffer |
| 35 | Fence |
| 36 | After `vkEndCommandBuffer`, **before** `vkQueueSubmit` |
| 37 | After successful real `vkQueueSubmit`, **before** `vkWaitForFences` |

For checkpoints 3–17 the order is:

```text
buffer 0: VkBuffer / VkDeviceMemory / VkMapMemory
buffer 1: VkBuffer / VkDeviceMemory / VkMapMemory
buffer 2: VkBuffer / VkDeviceMemory / VkMapMemory
buffer 3: VkBuffer / VkDeviceMemory / VkMapMemory
buffer 4: VkBuffer / VkDeviceMemory / VkMapMemory
```

The four descriptor/pipeline groups are created in the exact order:

```text
stage 0: descriptor set / VkShaderModule / VkComputePipeline
stage 1: descriptor set / VkShaderModule / VkComputePipeline
stage 2: descriptor set / VkShaderModule / VkComputePipeline
stage 3: descriptor set / VkShaderModule / VkComputePipeline
```

The diagnostic never infers a failure point from a device name or
discovered resource count. The requested point is explicit and
one-based. A missing expected checkpoint is a hard failure.

## Lifecycle methods and ownership

### `backend_vulkan_public_checkpoint(job, resource)`

When a test-controlled fault is active, counts each successfully
reached checkpoint. At the exact requested point it emits:

```text
BACKEND_PUBLIC_FAULT_TRIGGER point=N resource=... simulated=1 submitted=0 result=-3
```

For point 37, `submitted=1` records that the actual
`vkQueueSubmit` completed successfully before the synthetic abort.

The checkpoint function never calls or substitutes a Vulkan driver
function. Ordinary `hashcat -I` has no active fault control, so the
checkpoints immediately return `VK_SUCCESS` and print nothing.

### `backend_vulkan_public_release(job, counts)`

Releases the existing temporary job using its declared stage and buffer
counts, regardless of which acquisition was the last completed
operation. It waits for an existing logical device to become idle,
destroys acquired pipelines, shaders, descriptor objects, command
objects, buffers and memory, frees all host ownership arrays and clears
the job.

When a cleanup counter is requested, the method reports the exact
non-null resources present at the start of cleanup, the actual
`vkDeviceWaitIdle` result and whether every cleanup operation completed.

It operates on partially initialized Vulkan handles and also handles
a host-only allocation with no `VkDevice`. The parent `VkInstance` is
never destroyed by the temporary job.

### `backend_vulkan_kernel_verify_chain4_fault(...)`

Accepts an open Vulkan backend catalogue, physical-device index,
non-null diagnostics and an explicit point in 1..37. It runs the exact
production four-stage resource creation and dispatch path until the
selected checkpoint, prints a resource inventory, releases all
acquired objects and returns the expected simulated initialization
error.

Invalid point 0, point 38, invalid backend, out-of-range device index
and null diagnostics are rejected before opening a Vulkan logical
device. An unexpected real driver error, missing checkpoint or
unsuccessful cleanup causes the native diagnostic to fail.

### `backend_vulkan_kernel_verify_chain4(...)`

With no injection control, validates the four-stage program against the
real physical-device limits, creates five storage buffers and four
descriptor/shader/pipeline stages, executes four ordered public
Compute dispatches and checks all 256 CPU-referenced words. This
ordinary method is called **after each of the 37 controlled failures**
while the same `VkInstance` remains open.

## Queue-submission boundaries

Point 36 returns a simulated host error after recording the complete
command buffer but **before** calling `vkQueueSubmit`. The cleanup
operates on a command buffer that never entered the queue.

Point 37 calls the *real driver* `vkQueueSubmit` and requires
`VK_SUCCESS` from that call. It then returns the simulated error
before the explicit `vkWaitForFences`. The normal release path invokes
a real `vkDeviceWaitIdle` and requires success before disposing of the
submitted command buffer and its resources.

There is no injected `VK_ERROR_DEVICE_LOST`, no forced GPU hang and no
claim that a real submit or wait failure was observed.

## Build and Android validation

Build diagnostics against the same production source used in complete
HASHCAT, for both API24 ARM ABIs:

```powershell
.\tools\build-vulkan-backend-public-fault.ps1 -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865 -AndroidApi 24 -GlslangCompiler D:\ANDROID\SDK\emulator\lib64\vulkan\glslangValidator.exe
```

The script compiles with `-Wall -Wextra -Werror`, links real Vulkan
core1.0 symbols and rebuilds both public SPIR-V 1.0 modules. Every
embedded shader header must remain byte-identical to the committed
generated source.

Run all checkpoints on a connected GPU and supported Android ABI:

```powershell
.\tools\run-vulkan-backend-public-fault.ps1 -Serial $DeviceAdbSerial -Abi arm64-v8a -ReportPrefix pixel-arm64 -TestCore11
```

For each requested Vulkan 1.0/1.1 API, the process opens one real
`VkInstance`, performs 37 controlled partial acquisitions and then
recovers with 37 new fully working `VkDevice` lifetimes. All 37
partial-job inventories and cleanup completions must be present.
After the last recovery, the backend catalogue closes with exactly
74 device selections.

Build the full opt-in Android HASHCAT process:

```powershell
.\scripts\BUILD.ps1 -Target android-arm64-v8a -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865 -AndroidApi 24 -EnableVulkanDiscovery -VulkanOutputName vulkan-hashcat-public-fault-20261102 -Package:$false -MakeJobs 4
.\scripts\BUILD.ps1 -Target android-armeabi-v7a -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865 -AndroidApi 24 -EnableVulkanDiscovery -VulkanOutputName vulkan-hashcat-public-fault-20261102 -Package:$false -MakeJobs 4
```

Run normal information mode without injected failures:

```powershell
.\tools\run-vulkan-hashcat-public-fault.ps1 -Serial $DeviceAdbSerial -Abi arm64-v8a -ReportPrefix pixel-arm64
```

The full HASHCAT process must still validate and execute its one-,
two- and four-stage public programs, verify 448 public uint32 results,
display normal OpenCL device information and exit with code 0.
**Fault trigger/release messages are forbidden in the full process**.

## Physical GPU evidence

The native fault/recovery matrix covers:

| GPU / process ABI | Requested Vulkan | Native checkpoint test |
| --- | --- | --- |
| Adreno 730 ARM64 | 1.0, 1.1 | PASS |
| Adreno 730 ARMv7 | 1.0, 1.1 | PASS |
| Mali-G715 ARM64 | 1.0, 1.1 | PASS |

Totals from **six native hardware invocations**:

| Confirmed property | Count |
| --- | ---: |
| Exact simulated cleanup checkpoints | 222 |
| Fresh working four-stage devices after failure | 222 |
| CPU-verified GPU dispatches after recovery | 888 |
| Independently verified public uint32 results | 56,832 |
| Additional successful queue submissions before simulated abort | 6 |
| Additional dispatched stages at the after-submit boundary | 24 |
| Malformed native CLI requests rejected | 9 |

The 24 post-submit dispatched stages complete when
`vkDeviceWaitIdle` succeeds during cleanup; their numerical results
are not compared with CPU. Accordingly, they are **not included** in
the 888 verified dispatches or 56,832 verified words.

The complete full-HASHCAT `hashcat -I` hardware matrix provides a
separate regression check of ordinary, non-fault execution.

## Evidence archive

The archive preserves UTF-8 LF-normalized ADB/PowerShell output:

```text
tests/fixtures/android-gpu/vulkan-backend-public-fault-20261102/
  manifest.json
  redmagic-arm64-1.0.log
  redmagic-arm64-1.1.log
  redmagic-arm64-hashcat-fault-info.log
  redmagic-arm64-negative-controls.log
  redmagic-armv7-1.0.log
  redmagic-armv7-1.1.log
  redmagic-armv7-hashcat-fault-info.log
  redmagic-armv7-negative-controls.log
  pixel-arm64-1.0.log
  pixel-arm64-1.1.log
  pixel-arm64-hashcat-fault-info.log
  pixel-arm64-negative-controls.log
```

The exact copied transcripts are not raw ADB byte streams: the runner
normalizes carriage-return line endings before saving them.

Archive exact source, SPIR-V and ELF SHA-256 identities:

```powershell
python -B tools/archive-vulkan-backend-public-fault-evidence.py
```

Check the saved data independently from Android hardware:

```powershell
python -B tools/test-vulkan-backend-public-fault-evidence.py --native build/diagnostic-multimode/vulkan-backend-public-fault-20261102 --full build/diagnostic-multimode/vulkan-hashcat-public-fault-20261102 --readelf D:\ANDROID\SDK\ndk\29.0.14206865\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe
```

The evidence verifier reconstructs the **exact** owned-resource inventory
at every point from the declared acquisition sequence. It also checks
ordering (failure → cleanup → new device → GPU output verification)
and rejects deliberately falsified resource, API, GPU, submission,
device-loss and HASHCAT scheduling records.

## Limits

These tests validate cleanup following controlled failures and verify
actual public Compute work on physically connected Android GPUs.
They do not simulate or induce physical GPU device loss, driver-memory
exhaustion, actual Vulkan API return failures, concurrent attack work
or cryptographic SPIR-V kernel execution. The complete production
HASHCAT password-processing scheduler remains separate from Vulkan.
