# Vulkan 1.0 fail-closed VkResult submission policy on Android

Date: 2026-11-01

## Scope

`tools/vulkan-core10-result-gate.c` and `.h` provide a strict
standalone host-side policy for deciding whether a caller may start a
new Vulkan GPU submission after observing one explicit `VkResult`.

The policy owns **no VkInstance, VkDevice, queue, fence, buffer or
command pool**. It does not wrap the Vulkan driver or guess a GPU's
state. The application explicitly supplies:

- the exact Vulkan operation site;
- the actual result returned by that operation;
- a permission check before any subsequent GPU submission.

The new demonstration uses an actual Android Vulkan device to obtain
**real `VK_NOT_READY`, `VK_TIMEOUT` and `VK_SUCCESS`**, executes
public-integer GPU buffer transfers, and then tests terminal
classification by passing a **clearly labeled application-generated
negative VkResult** to the policy.

The application-generated negative result is **not returned by the
driver**. This milestone does not simulate a GPU reset, hang,
allocation failure or real `VK_ERROR_DEVICE_LOST` and does not claim
recovery after physical device loss.

The standalone policy is **not connected to the production HASHCAT
backend_ctx, scheduler or cryptographic kernels**. No passwords,
candidate processing, Compute shaders or Android APK releases are
involved.

## Result classification and lifecycle

The policy uses explicit operation types with no heuristic fallback.

| Operation site | Accepted `VkResult` | Meaning |
| --- | --- | --- |
| `FENCE_STATUS` | `VK_SUCCESS` (0) | Fence is signaled |
| `FENCE_STATUS` | `VK_NOT_READY` (1) | Fence is not signaled |
| `FENCE_WAIT` | `VK_SUCCESS` (0) | Fence has completed |
| `FENCE_WAIT` | `VK_TIMEOUT` (2) | Specified wait has elapsed |
| `TRANSFER` | `VK_SUCCESS` (0) | The entire Vulkan buffer transfer has completed |
| Any site | Negative `VkResult`, including `VK_ERROR_DEVICE_LOST` (-4) | Terminal error for this logical-device lifetime |
| Unexpected positive result at any site | `VK_ERROR_INITIALIZATION_FAILED` (-3) | Invalid completion result for that operation |
| Unknown operation site | `VK_ERROR_INITIALIZATION_FAILED` (-3) | Undeclared source of `VkResult` |

The policy never classifies `VK_TIMEOUT` as a successful buffer
transfer or `VK_NOT_READY` as a successful fence wait.

An accepted nonfatal status keeps the policy in `ACTIVE` and
does not block a later submission.

A negative result moves the policy to `FAILED`. Its **first terminal
error remains unchanged**, even if a caller subsequently reports
`VK_SUCCESS`. Calls to `before_submit` return that first error
and cannot invoke Vulkan, alter an output buffer or submit GPU work.
A new policy object is required for a separately created logical
device.

```text
ZERO
  |
  +-- open --> ACTIVE
  |              |
  |              +-- VK_SUCCESS / valid NOT_READY / valid TIMEOUT --> ACTIVE
  |              |
  |              +-- negative / invalid result --> FAILED
  |                          |
  |                          +-- before_submit --> rejected, remains FAILED
  |                          +-- later VK_SUCCESS --> ignored, remains FAILED
  |
  +-- close ACTIVE or FAILED --> CLOSED (never reopened)
```

The demonstration's real GPU copy calls consult `before_submit`
first. A denied result causes the caller to **skip the copy
entirely**. The program verifies that the completed GPU copy
counter remains at two and that every element of a separate
1,024-word sentinel output array remains unchanged.

After rejecting three submission attempts, the program closes
mapped allocations and all real VkDevice resources, then closes
the policy and finally destroys VkInstance.

## Public C interface

### `vulkan_core10_result_gate_open(gate, diagnostics)`

Requires a fully zero-initialized policy object and valid
diagnostics stream. Starts the policy in `ACTIVE`. Rejects any
attempt to reopen an active, failed or closed object. No Vulkan
driver call or allocation is performed.

### `vulkan_core10_result_gate_observe(gate, site, observed)`

Records the exact caller-supplied `VkResult` at one declared site.
Returns the actual accepted nonfatal status when the site allows it.
Sets `first_error` and switches to `FAILED` on any negative result.
An invalid site or unexpected positive completion uses terminal
`VK_ERROR_INITIALIZATION_FAILED`. A failed policy retains and
returns its original error without reactivation.

### `vulkan_core10_result_gate_before_submit(gate)`

Returns `VK_SUCCESS` and increments `allowed` while the policy is
`ACTIVE`. After failure, increments `blocked` and returns the
exact first terminal error. It performs no Vulkan operation. The
caller must check this result **before** attempting submission.

### `vulkan_core10_result_gate_close(gate)`

Reports the first error and all classification/permission counters,
then transitions to `CLOSED`. Does not call `vkDeviceWaitIdle`,
`vkDestroyDevice` or other Vulkan functions. The caller releases
its VkDevice before closing the policy and the owning VkInstance.

All four methods have complete declarations and matching behavior
documentation in `tools/vulkan-core10-result-gate.h`.

## Native test coverage

Each native hardware invocation performs **two independently opened
VkInstance and VkDevice lifecycles**:

1. Creates two real mapped storage/transfer Vulkan buffers.
2. Queries a genuinely unsignaled fence and records driver-returned
   `VK_NOT_READY` and `VK_TIMEOUT`.
3. Uses a permitted real GPU copy of 7 public unsigned 32-bit
   integers and verifies the completed bytes.
4. Uses a second permitted real GPU copy of 1,024 integers and
   verifies the completed bytes.
5. Supplies an explicitly labeled **application-only** result to
   the policy:
   - `device-lost`: `VK_ERROR_DEVICE_LOST` (-4);
   - `device-memory`: `VK_ERROR_OUT_OF_DEVICE_MEMORY` (-2);
   - `unexpected-positive`: `VK_INCOMPLETE` (5), which is
     invalid as a completed transfer and becomes terminal -3.
6. Verifies **three rejected later submission permissions**, no
   further GPU copy, unchanged output sentinels and the sticky
   terminal error.
7. Closes the real logical-device resources and only then closes
   VkInstance.

Each invocation also runs five **pure host-only** negative
classification cases: timeout reported as fence status, not-ready
reported as completed wait, incomplete transfer, host-memory error
and unknown result site.

### Real GPU matrix

At the time of this milestone, only **Pixel 9 Pro XL / Mali-G715
ARM64** remained connected to ADB. The RedMagic Adreno device
disconnected before its matrix started. No results are invented
for the disconnected device or ARMv7 execution.

| Device | Requested Vulkan | Memory | Terminal modes | Native invocations |
| --- | --- | --- | --- | ---: |
| Pixel 9 Pro XL ARM64 | 1.0 | Preferred coherent | Lost / device memory / positive | 3 PASS |
| Pixel 9 Pro XL ARM64 | 1.0 | Required noncoherent | Lost / device memory / positive | 3 PASS |
| Pixel 9 Pro XL ARM64 | 1.1 | Preferred coherent | Lost / device memory / positive | 3 PASS |
| Pixel 9 Pro XL ARM64 | 1.1 | Required noncoherent | Lost / device memory / positive | 3 PASS |
| **Total** | | | | **12 PASS** |

Measured across these **real Android processes**:

- **24 fully opened and destroyed VkDevice sessions**.
- **48 genuine, fence-completed `vkCmdCopyBuffer` operations**.
- **24,744 compared unsigned 32-bit words**, or **98,976 bytes**.
- **24 real `VK_NOT_READY` results** from Vulkan fence status.
- **24 real `VK_TIMEOUT` results** from Vulkan fence wait.
- **24 application-generated terminal-result classifications**,
  each labeled `origin=application_only` and `driver_error=0`.
- **72 blocked host-side submission requests**, with no further
  GPU copy recorded after a terminal classification.
- **24 real noncoherent `vkFlushMappedMemoryRanges` and 24
  `vkInvalidateMappedMemoryRanges` operations** on Mali.
- **60 host-only invalid-site/status tests** and six malformed
  CLI requests rejected with exit code 2.

The policy validates **caller compliance** with the submission
gate. Its API does not intercept `vkQueueSubmit` invoked directly
by an unrelated caller. This does **not** guarantee that every
application component is unable to submit outside the policy.

## Building the standalone native diagnostics

Compile the two Android API24 executables using Android NDK
Clang and read-only ELF symbol inspection:

```powershell
.\tools\build-vulkan-core10-result-gate.ps1 -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865 -AndroidApi 24
```

The executables are created in an ignored local build directory:

```text
build/diagnostic-multimode/vulkan-core10-result-gate-20261101/
  vulkan-core10-result-gate-arm64
  vulkan-core10-result-gate-armv7
```

Both ABIs compile at `-Wall -Wextra -Werror` and link core
`libvulkan.so`. The build validates ELF32/ELF64 architecture,
Android API24 notes, the Vulkan core-1.0 fence and buffer symbols,
and absence of a required Vulkan 1.1 loader entry point.

Run all terminal test cases for an available physical Android GPU:

```powershell
.\tools\run-vulkan-core10-result-gate.ps1 -Serial $DeviceAdbSerial -Abi arm64-v8a -ReportPrefix pixel-arm64 -TestCore11 -TestNoncoherent
```

The `-TestNoncoherent` switch requires actual compatible
HOST_VISIBLE memory without HOST_COHERENT and does not select
fallback memory. Native command syntax:

```text
vulkan-core10-result-gate-arm64 --minimum 1.0 --memory prefer --terminal device-lost
```

This command **does not cause physical GPU device loss**.
The `device-lost` argument chooses only which synthetic
`VkResult` value the test passes to the host policy after two
successful real buffer copies.

A full native success exits zero. Actual Vulkan failures,
inconsistent status classification or incomplete cleanup exit
nonzero; malformed native arguments exit 2.

## Permanent evidence and independent verification

Archive real ADB runner logs:

```powershell
python -B tools/archive-vulkan-core10-result-gate-evidence.py
```

Verify exact source hashes, two Android ELF identities and every
saved GPU/host-policy result without launching Android:

```powershell
python -B tools/test-vulkan-core10-result-gate-evidence.py --artifacts build/diagnostic-multimode/vulkan-core10-result-gate-20261101 --readelf D:\ANDROID\SDK\ndk\29.0.14206865\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe
```

Permanent hardware and host-policy records:

```text
tests/fixtures/android-gpu/vulkan-core10-result-gate-20261101/
  manifest.json
  pixel-arm64-{1.0,1.1}-{prefer,noncoherent}-{device-lost,device-memory,unexpected-positive}.log
  pixel-arm64-negative-controls.log
```

The archive contains **12 Android GPU process logs, one malformed
CLI record, 15 source identities and two exact ELF hashes**.
The ARMv7 executable was compiled and inspected but **not run
on physical ARMv7 hardware in this milestone**.

The independent verifier requires exact event ordering, genuine
nonfatal driver statuses, completed public GPU copies before the
synthetic terminal event, no later submission, unchanged host
output, accurate memory-cache operations and safe release ordering.
It rejects **35 deliberately falsified records**, including
attempted claims that an application-supplied terminal code came
from the Vulkan driver.

ADB output was normalized to LF by the PowerShell runner and the
resulting saved UTF-8 files were archived unchanged. These are not
claimed to be raw, unprocessed ADB byte streams.

The follow-on
[test-linked Vulkan API boundary report](vulkan-core10-api-fault-boundary-android.md)
connects the same strict VkResult policy to actual intercepted
`vkQueueSubmit` and `vkWaitForFences` call returns in the existing
public buffer transfer method. Unlike this original policy-only
milestone, its evidence includes real ARM64 and ARMv7 Android native
runs; the injected negative values still do not originate from the
genuine Vulkan driver.

## Verification boundaries

This milestone establishes a strict, non-retrying **host policy**
under test and its integration into the dedicated standalone
diagnostic. It does not prove a genuine `VK_ERROR_DEVICE_LOST`
can be recovered, that a physically lost VkDevice's pending
resources can be safely reused, or that driver-level memory
leaks are absent. No GPU reset, forced GPU fault or device-loss
injection into a real driver is performed.

The production HASHCAT Vulkan information-mode session remains
independent, and no HASHCAT cryptographic Compute kernel or
password candidate processing is enabled.
