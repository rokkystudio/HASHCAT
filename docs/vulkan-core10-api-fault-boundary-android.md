# Vulkan 1.0 test-only API boundary errors and strict submission ownership

Date: 2026-11-01

## Scope and separation from the real Vulkan driver

This milestone verifies the **actual application-side Vulkan call
boundary** for `vkQueueSubmit` and `vkWaitForFences` in a standalone
Android API24 public-integer buffer diagnostic.

Unlike a test that directly passes `VK_ERROR_DEVICE_LOST` into a
policy, this executable links the existing
`vulkan_core10_transfer_session_copy` implementation with explicit
**test-only** linker interception:

```text
-Wl,--wrap=vkQueueSubmit
-Wl,--wrap=vkWaitForFences
```

Only this dedicated diagnostic executable uses these linker flags.
The session implementation and production HASHCAT binary are
**not** modified or linked with the interceptor.

When unarmed, both linked entry points call the genuine
`libvulkan.so` functions. When the caller explicitly arms one
site for one existing `VkQueue` and `VkDevice`, the selected
function returns a test-generated `VkResult` **instead of calling
the driver at that site**. The unchanged transfer-session method
receives this return value directly and propagates it to its caller.

This verifies the error path **at the real call site in the native
application**. It is *not* an actual Vulkan-driver failure.
The GPU is never deliberately lost, reset, stalled or corrupted.

The dedicated executable does not enter the production HASHCAT
`backend_ctx`, execute a Compute shader, process hash candidates
or change the installed Android APK.

## Exact test boundary and VkResult behavior

The caller selects only one of these explicit combinations:

| Test site | Test-returned result | Actual Vulkan driver calls at the selected point |
| --- | --- | --- |
| `submit` | `VK_ERROR_DEVICE_LOST` (-4) | No real `vkQueueSubmit`; nothing queued for that attempt |
| `submit` | `VK_ERROR_OUT_OF_DEVICE_MEMORY` (-2) | No real `vkQueueSubmit`; nothing queued for that attempt |
| `wait` | `VK_ERROR_DEVICE_LOST` (-4) | Real `vkQueueSubmit` succeeds; the selected `vkWaitForFences` is intercepted |
| `wait` | `VK_ERROR_OUT_OF_DEVICE_MEMORY` (-2) | Real `vkQueueSubmit` succeeds; the selected `vkWaitForFences` is intercepted |

The test-generated result is passed **from the wrapped Vulkan
function** back into `vulkan_core10_transfer_session_copy`.
The copy method returns it without producing caller-visible
output, incrementing `completed_copies`, or claiming successful
fence completion.

For WAIT-site injection, a real command has already entered the
physical GPU queue. The owning session calls the unwrapped,
real `vkDeviceWaitIdle` during cleanup, waiting for that work
before releasing the actual mapped Vulkan resources. The test
does not pretend the submitted command disappeared.

### Fail-closed policy and API boundary guard

Immediately after a terminal return:

1. The existing `vulkan_core10_result_gate_observe` records the
   negative result at `VULKAN_CORE10_RESULT_SITE_TRANSFER` and
   transitions the policy to `FAILED`.
2. Three subsequent calls to
   `vulkan_core10_result_gate_before_submit` refuse to authorize
   more GPU work. The original error remains sticky.
3. Two direct zero-command `vkQueueSubmit` attempts are rejected
   by the test-linked interception **before reaching the driver**.
4. One direct zero-timeout `vkWaitForFences` attempt is also
   intercepted and rejected without calling the Vulkan loader.
5. The output sentinel array stays intact, and the old session
   remains at exactly two successful public-data copies.
6. The test disarms interception and invokes real
   `vkDeviceWaitIdle` through the session close path.
   Mapped memory, VkBuffer, VkDeviceMemory, command buffer, fence,
   command pool and VkDevice are destroyed while VkInstance is live.
7. A **fresh VkDevice**, constructed using the same VkInstance
   and selected physical device, completes an independent real
   64-word `vkCmdCopyBuffer` transfer with fence synchronization
   and exact output verification.
8. The fresh VkDevice closes, and VkInstance is destroyed last.

A fresh-device copy confirms that the *physically healthy*
driver can continue working once the intentionally interrupted
test device is closed. It does **not** demonstrate recovery from
a genuine lost VkDevice.

The test only intercepts the selected existing queue and device.
Unarmed or unrelated calls use the original Vulkan loader
entry points. The standalone policy requires callers to
consult it; the linker guard is an additional test-only
assurance, **not** global production interception of every
possible Vulkan call.

## Complete method and struct contracts

### `vulkan_core10_api_fault_counts_t`

Holds the actual attempted and real `vkQueueSubmit`/
`vkWaitForFences` call counts during one armed test:
`submit_attempts`, `real_submissions`, `wait_attempts`,
`real_waits`, `injections`, `blocked`, and `first_error`.

`real_submissions` counts calls to `__real_vkQueueSubmit`.
`blocked` counts calls rejected at the test-linked Vulkan
boundary without entering the real driver.

### `vulkan_core10_api_fault_arm(queue, device, site, result, diagnostics)`

Requires a live queue and VkDevice, a declared SUBMIT/WAIT site,
one explicit negative test result, and valid diagnostics. Stores
the existing handles as one process-local test target and
performs no Vulkan operation. Rejects second-arm requests
while a target remains armed.

### `__wrap_vkQueueSubmit(queue, count, submits, fence)`

Passes unarmed and unrelated calls through to
`__real_vkQueueSubmit`. At a SUBMIT fault, returns the selected
negative result without a real submission. At a WAIT fault,
calls the real Vulkan function, then permits the subsequent
wait to be interrupted separately. All later matching calls
return the sticky error without invoking Vulkan.

### `__wrap_vkWaitForFences(device, count, fences, waitAll, timeout)`

Passes unarmed and unrelated waits to the genuine driver.
At a WAIT fault, the function returns the test error without
calling the real fence wait; a prior command has already
entered the GPU queue. Once fault state is terminal, all
matching waits are rejected without accessing the real driver.

### `vulkan_core10_api_fault_snapshot(counts)`

Copies the exact attempted, real, injected and blocked Vulkan
function counters from the currently armed test to a
caller-owned output struct. Makes no Vulkan call.

### `vulkan_core10_api_fault_disarm()`

Reports all counts and clears the interception target. The
caller must invoke this method before destroying the live
target VkDevice; the next Vulkan call once again reaches
the real loader. Does not free VkDevice or VkInstance.

### `api_fault_verified_copy(session, gate, trial, round, words, phase)`

Queries the strict permission gate, executes one real Vulkan
buffer copy, checks the returned VkResult and signaled fence,
then verifies every element of the public 1,024-word host
output array, including unchanged out-of-range sentinels.

### `api_fault_trial(trial, minimum_api, memory_mode, site, result)`

Owns one physical-device catalogue and two successive real
logical devices. The first completes two buffer copies and
then receives a controlled negative Vulkan call result. After
strict submission refusal and cleanup, the second VkDevice
completes a real public buffer copy under the same VkInstance.
Returns nonzero if any driver call, result classification,
output comparison, submission denial or teardown is invalid.

### `main(argc, argv)`

Accepts exactly Vulkan API 1.0/1.1, memory policy
`prefer`/`noncoherent`, test interception site
`submit`/`wait`, and test result
`device-lost`/`device-memory`. Runs two separate complete
VkInstance lifecycles with real GPUs. Returns native exit 0
only if every check succeeds, 2 for malformed options,
and 1 for an unexpected native driver or test failure.

Full implementations are in
`tools/vulkan-core10-api-fault-driver.h`,
`tools/vulkan-core10-api-fault-driver.c`, and
`tools/vulkan-core10-api-fault-boundary-diagnostic.c`.

## Real Android hardware matrix

| Physical GPU and process ABI | Requested Vulkan | Memory policy | Test scenarios | Real completed GPU copies |
| --- | --- | --- | ---: | ---: |
| RedMagic Adreno 730 ARM64 | 1.0 and 1.1 | Preferred coherent | 8 PASS | 48 |
| RedMagic Adreno 730 ARMv7 | 1.0 and 1.1 | Preferred coherent | 8 PASS | 48 |
| Pixel 9 Pro XL Mali-G715 ARM64 | 1.0 and 1.1 | Preferred coherent and required noncoherent | 16 PASS | 96 |
| **Total** | | | **32 PASS** | **192** |

Every native scenario runs two complete trials:

- **64 expressly test-generated Vulkan API returns** (no
  driver-generated error claimed).
- **128 real VkDevice lifecycles** (interrupted and freshly
  created devices) under **64 real VkInstance lifecycles**.
- **192 real fence-completed `vkCmdCopyBuffer` copies** before
  fault or on freshly created devices.
- **70,080 compared unsigned 32-bit public integers**, equal
  to **280,320 bytes**.
- **192 host policy denials** and **192 test-linker Vulkan
  boundary denials** of subsequent submissions and waits.
- At WAIT injection, **32 real queue submissions** reach
  the GPU before the test intercepts the matching fence wait.
  These commands complete during real `vkDeviceWaitIdle`
  teardown and are not counted as successful host-output copies.
- **48 completed noncoherent GPU copies** on Mali, using
  real `vkFlushMappedMemoryRanges` and
  `vkInvalidateMappedMemoryRanges`.
- **21 invalid native command-line requests rejected** on the
  three device/process configurations (seven per configuration).

Both requested Vulkan instance API levels were exercised using
Vulkan core 1.0 symbols. The actual phones use newer drivers,
and their Android versions are newer than Android 7.0/API24.

All tested GPU memory types derive from the real Vulkan driver
requirements. The strict noncoherent mode runs on Mali where
a compatible HOST_VISIBLE noncoherent type exists. No driver-
name heuristic substitutes coherent memory.

## Native executable build and physical GPU runs

Compile standalone ARM64 and ARMv7 Android API24 executables:

```powershell
.\tools\build-vulkan-core10-api-fault-boundary.ps1 -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865 -AndroidApi 24
```

The build uses `-Wall -Wextra -Werror` and explicitly links
both test-only `--wrap` symbols. It verifies ELF class, ARM
architecture, Android API24 notes and the native Vulkan
core-1.0 loader dependency. Neither production HASHCAT nor
its Android APK is recompiled by this diagnostic build.

Run all fault-site/result combinations for both requested
Vulkan API levels:

```powershell
.\tools\run-vulkan-core10-api-fault-boundary.ps1 -Serial $DeviceAdbSerial -Abi arm64-v8a -ReportPrefix pixel-arm64 -TestCore11 -TestNoncoherent
```

Use `-Abi armeabi-v7a` only on a phone that actually reports
32-bit ARM support. The Adreno driver in these tests does not
provide the required strict noncoherent type, so omit
`-TestNoncoherent` for RedMagic.

Directly run one native scenario:

```text
vulkan-core10-api-fault-boundary-arm64 --minimum 1.0 --memory prefer --fault-site wait --result device-lost
```

**This command returns a synthetic error from the test linker
only. It does not ask the GPU to lose the device.**

## Independent evidence archive and mutation checks

Archive exact LF-normalized native Android outputs and create
a SHA-256 manifest:

```powershell
python -B tools/archive-vulkan-core10-api-fault-evidence.py
```

Verify all saved Android results, exact source identities,
both native ELF binaries, GNU/LLD `--wrap` symbols and
device/instance ownership entirely offline:

```powershell
python -B tools/test-vulkan-core10-api-fault-evidence.py --artifacts build/diagnostic-multimode/vulkan-core10-api-fault-boundary-20261101 --readelf D:\ANDROID\SDK\ndk\29.0.14206865\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe
```

The permanent hardware records are stored at:

```text
tests/fixtures/android-gpu/vulkan-core10-api-fault-boundary-20261101/
  manifest.json
  redmagic-arm64-{1.0,1.1}-prefer-{submit,wait}-{device-lost,device-memory}.log
  redmagic-arm64-negative-controls.log
  redmagic-armv7-{1.0,1.1}-prefer-{submit,wait}-{device-lost,device-memory}.log
  redmagic-armv7-negative-controls.log
  pixel-arm64-{1.0,1.1}-{prefer,noncoherent}-{submit,wait}-{device-lost,device-memory}.log
  pixel-arm64-negative-controls.log
```

The evidence consists of **32 real native Android process
transcripts and three native negative-control logs**, exact
SHA-256 identities for 17 relevant source files, and both
ARM64/ARMv7 executables.

An independent Python verifier requires the full event order
for every driver response, blocked API call, real GPU copy,
noncoherent memory synchronization and safe release.
It rejects **49 deliberately corrupted records**, including
fabricated driver-side device loss, falsely completed GPU
copies, accidental post-failure submissions, missing
cache invalidation and unsafe VkInstance destruction.

ADB output is normalized to UTF-8 LF by the runner, then
archived without modification. These files are not
represented as raw unprocessed ADB byte streams.

## Limitations

The error is generated by an explicit test-only linked Vulkan
function and reaches the real transfer-session implementation
through its normal VkResult return path. This is a stronger
test of the call boundary than direct policy-only injection,
but **it still does not test a real failing physical Vulkan
driver, actual VK_ERROR_DEVICE_LOST or recovery of a truly
lost VkDevice**.

The test-linked interceptor and strict host policy are not
enabled in HASHCAT's production Vulkan information-mode
or hashing backends. There is no GPU password-cracking
implementation, compute-shader dispatch, driver-level memory
leak tracing, physically Vulkan-1.0-only hardware or
Android API24 OS device in this evidence.
