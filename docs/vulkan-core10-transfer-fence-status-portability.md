# Vulkan 1.0 fence status and bounded-wait portability on Android

Date: 2026-11-01

## Scope

This milestone verifies **real Vulkan driver-returned completion
statuses** on Android without deliberately losing or hanging a GPU.

The standalone `vulkan-core10-transfer-fence-status-diagnostic.c`
creates VkInstance, selects a real Compute-capable physical device,
opens a normal Vulkan 1.0 `vulkan_core10_transfer_session_t` and
exercises its genuinely **unsignaled** and **signaled** fence states.

No status codes are injected, no Vulkan function is mocked and no
vendor-name heuristics determine whether to accept a response.
Unexpected results cause a diagnostic failure with no automatic
recreation of a potentially lost device.

The diagnostic remains **separate from the production HASHCAT
backend_ctx**. It does not compile cryptographic kernels, process
password candidates, submit hashcat scheduler work, create shaders
or alter any Android APK.

## Exact Vulkan synchronization contract

All the methods below are available in Vulkan core 1.0.

### Unsignaled fence

At the start of the Vulkan session, `vkCreateFence` has produced an
unsignaled VkFence and **no work has been submitted** with that fence.

- `vkGetFenceStatus` must return `VK_NOT_READY` (integer **1**).
- `vkWaitForFences` with timeout **0 ns** must return `VK_TIMEOUT`
  (integer **2**).
- `vkWaitForFences` with timeout **1,000,000 ns (1 ms)** must also
  return `VK_TIMEOUT`. The fence has no queued submission that can
  make it signal.
- All 1,024 sentinel words in the host output array must remain
  untouched, and the session must have completed zero copies.

These are **real nonfatal Vulkan status results**, not
`VK_ERROR_DEVICE_LOST` or an allocation error.

### Completed GPU transfer

The test uses the existing session's public-data copy method:

`vulkan_core10_transfer_session_copy(session, input, output, bytes)`

The method copies public 32-bit integers to mapped source memory,
flushes source CPU writes if its driver memory is noncoherent,
records `vkCmdCopyBuffer`, submits to the real Compute queue,
waits for fence completion and invalidates noncoherent destination
memory before returning the resulting bytes.

After each successful transfer:

- `vkGetFenceStatus` must return **`VK_SUCCESS` (0)**.
- A second `vkWaitForFences` with timeout **0 ns** must return
  **`VK_SUCCESS` (0)**, proving that the signal state persists.
- Each transferred word must match its input.
- All host output elements outside the requested copy range must
  retain their unchanged sentinel.
- `vkResetFences` must return `VK_SUCCESS`.
- The reset fence must once again return `VK_NOT_READY` from
  `vkGetFenceStatus` and `VK_TIMEOUT` from a zero-timeout wait.

The five exact copy lengths are 1, 7, 64, 257 and 1024 words,
equivalent to 4, 28, 256, 1,028 and 4,096 bytes.
The same command pool, primary command buffer, VkQueue, VkFence,
source buffer and destination buffer are reused within the session.

When the memory mode is explicitly noncoherent, every transfer
requires a real `vkFlushMappedMemoryRanges` and
`vkInvalidateMappedMemoryRanges` using the driver's
`nonCoherentAtomSize`. The strict policy does not silently select
coherent memory instead.

### Resource lifecycle

One native program executes two fully independent trials. Each trial
opens its own VkInstance and VkDevice, performs the full status/copy/
reset sequence, waits for device idle, destroys mapped buffers,
fence, command buffer/pool and VkDevice, and destroys VkInstance
**last**.

Any unexpected status, including a genuine `VK_ERROR_DEVICE_LOST`,
causes a diagnostic failure; no additional GPU work is submitted.
The code **does not deliberately trigger or simulate device loss**.
It does not claim to reconstruct a VkDevice after a lost-device event.

## Public diagnostic methods

`fence_verify_unsignaled(session, trial, step)`

Queries `vkGetFenceStatus` and zero-timeout `vkWaitForFences` on
an unsignaled real VkFence and requires `VK_NOT_READY` and
`VK_TIMEOUT`. At step zero, it also performs one bounded 1 ms wait,
requiring `VK_TIMEOUT`. Reports all observed VkResult codes.

`fence_verify_signaled(session, trial, round)`

Requires a real fence already completed by a GPU queue submission.
Verifies that `vkGetFenceStatus` and a zero-timeout
`vkWaitForFences` both return `VK_SUCCESS`.

`fence_status_trial(trial, minimum_api, memory_mode)`

Opens one Vulkan catalogue/device, verifies its unsignaled fence,
performs five buffer copies and full public-integer comparisons,
verifies signal/reset transitions after every copy and releases
VkDevice before VkInstance. Fails on any mismatch, Vulkan error or
incomplete cleanup.

`main(argc, argv)`

Accepts exactly a requested Vulkan core API `1.0` or `1.1` and
memory mode `prefer` or `noncoherent`. Runs two independent
trials and returns process status zero only when both complete.
Malformed CLI returns **2**; an unexpected driver result or test
failure returns **1**. No implicit capability fallback is applied.

These complete method implementations and their matching
documentation live in
`tools/vulkan-core10-transfer-fence-status-diagnostic.c`.

## Real Android GPU matrix

| Physical device and process ABI | Requested API | Memory mode | Completed trials | GPU copies |
| --- | --- | --- | ---: | ---: |
| RedMagic, Adreno 730, ARM64 | 1.0 and 1.1 | Preferred | 4 | 20 |
| RedMagic, Adreno 730, ARMv7 | 1.0 and 1.1 | Preferred | 4 | 20 |
| Pixel 9 Pro XL, Mali-G715, ARM64 | 1.0 and 1.1 | Preferred | 4 | 20 |
| Pixel 9 Pro XL, Mali-G715, ARM64 | 1.0 and 1.1 | **Required noncoherent** | 4 | 20 |
| **Total** | | | **16** | **80** |

On actual Vulkan GPU drivers, across eight native test invocations:

- **96** genuine `VK_NOT_READY` results from `vkGetFenceStatus`.
- **96** genuine `VK_TIMEOUT` results from zero-timeout
  `vkWaitForFences` on unsignaled fences.
- **16** additional genuine `VK_TIMEOUT` results from bounded 1 ms
  waits, for **112 actual timeouts** in total.
- **80** genuine `VK_SUCCESS` results from signaled
  `vkGetFenceStatus`, and 80 from zero-timeout waits on those
  signaled fences.
- **80** successful `vkResetFences` calls and subsequent
  unsignaled-state checks.
- **80 real fence-completed `vkCmdCopyBuffer` commands**.
- **21,648 verified unsigned 32-bit words**, or **86,592 bytes**.
- **20 real noncoherent memory flushes and 20 invalidates**
  in the strict Mali-G715 mode.
- **15 rejected malformed native CLI controls** across three
  physical device/process configurations.

The tested Adreno memory types do not support the explicit
noncoherent-only policy, as established by the existing hardware
capability tests. Adreno tests therefore use real supported
coherent memory. No vendor-based selection logic is implemented.

The executables target Android API **24** and explicitly request
Vulkan core API 1.0 or 1.1. The phones run newer OS versions and
Vulkan loaders; a physical Vulkan-1.0-only driver and Android 7.0
system were unavailable.

## Build and run the complete hardware test

Compile standalone ARM64 and ARMv7 API24 executables with the NDK:

```powershell
.\tools\build-vulkan-core10-transfer-fence-status.ps1 -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865 -AndroidApi 24
```

Run both requested Vulkan API modes on a supported Android device:

```powershell
.\tools\run-vulkan-core10-transfer-fence-status.ps1 -Serial $DeviceAdbSerial -Abi arm64-v8a -ReportPrefix device-arm64 -TestCore11
```

For Mali-G715 with known compatible noncoherent HOST_VISIBLE
memory, also specify `-TestNoncoherent`. On a device reporting
ARMv7, `-Abi armeabi-v7a` selects the real 32-bit native executable.

The native program can also run one API/memory combination:

```text
vulkan-core10-transfer-fence-status-arm64 --minimum 1.0 --memory prefer
```

The build script validates Android API 24 ELF notes, exact ARM64/
ARMv7 ELF machine types, `libvulkan.so`, all required core 1.0
memory/fence/copy imports and the absence of any mandatory Vulkan
1.1 `vkEnumerateInstanceVersion` import. Source code does not
reference Vulkan 1.1 device/queue entry points.

## Archived evidence and independent verifier

Run the archive process after native testing:

```powershell
python -B tools/archive-vulkan-core10-transfer-fence-status-evidence.py
```

Then validate the retained hardware observations, source SHA-256,
native executable identity and ELF symbols entirely offline:

```powershell
python -B tools/test-vulkan-core10-transfer-fence-status-evidence.py --artifacts build/diagnostic-multimode/vulkan-core10-transfer-fence-status-20261101 --readelf D:\ANDROID\SDK\ndk\29.0.14206865\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe
```

The permanent public evidence directory is:

```text
tests/fixtures/android-gpu/vulkan-core10-transfer-fence-status-20261101/
  manifest.json
  redmagic-arm64-{1.0,1.1}-prefer.log
  redmagic-arm64-negative-controls.log
  redmagic-armv7-{1.0,1.1}-prefer.log
  redmagic-armv7-negative-controls.log
  pixel-arm64-{1.0,1.1}-{prefer,noncoherent}.log
  pixel-arm64-negative-controls.log
```

The **11 archived LF-only logs** are the exact outputs saved by the
native ADB/PowerShell runner; the runner normalized line endings.
They are not presented as raw unprocessed ADB byte streams.

The independent verifier checks the exact API results and their
relative order in **16 complete Vulkan device/instance lifecycles**,
the normal memory copy/cache behavior, complete source and ELF
provenance, and rejects **32 intentionally corrupted records**
that falsify fence states, GPU progress, cache synchronization,
output values, ownership or the native process exit code.

A separate host-only terminal-result classification policy and real
GPU submission-gating diagnostic are documented in
`docs/vulkan-core10-result-gate-android.md`. They confirm that a
synthetic `VK_ERROR_DEVICE_LOST` makes a compliant caller reject
subsequent submissions, without claiming an actual driver-side loss.

## Limitations and next verification boundary

`VK_NOT_READY` and `VK_TIMEOUT` are **nonfatal statuses**, not
evidence of a lost GPU. The bounded-wait test guarantees that it
does not need to cause a real GPU stall. It validates a useful part
of driver-returned VkResult handling but does **not** test
`VK_ERROR_DEVICE_LOST`, an Android GPU reset, a driver allocator
failure, a kernel hang, concurrent queues or validation-layer
memory leak reporting.

No production HASHCAT GPU scheduler, cryptographic kernel or
candidate processing is connected to this standalone diagnostic.
The production `hashcat -I` Vulkan session continues to use its
existing independently verified empty queue submission.
