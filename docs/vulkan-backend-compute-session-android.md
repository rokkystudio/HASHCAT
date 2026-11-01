# Vulkan 1.0 Compute queue session owned by HASHCAT backend_ctx

Date: 2026-11-01

## Implementation and ownership

The opt-in Android HASHCAT backend owns one independent Vulkan 1.0
Compute session in addition to its indexed physical-device catalogue.
The session creates a real VkDevice, obtains one Compute queue, creates
one command pool, one primary command buffer and one fence. It submits
an **empty command buffer** and waits for the fence to complete.

No GPU shader, descriptor set, hashing buffer, password candidate,
cryptographic kernel or hashing scheduler entry is created.

```text
backend_ctx_t
  |
  +-- vulkan_catalog                 VkInstance
  |      +-- VkPhysicalDevice[]       borrowed physical-device handles
  |
  +-- vulkan_session                 VkDevice
         +-- VkQueue                 borrowed from VkDevice
         +-- VkCommandPool
         |      +-- VkCommandBuffer   primary, empty
         +-- VkFence                 fence-completed empty submission

backend_vulkan_session_close:
    vkDeviceWaitIdle
    vkDestroyFence
    vkFreeCommandBuffers
    vkDestroyCommandPool
    vkDestroyDevice
    free session allocation

backend_vulkan_catalog_close:
    requires vulkan_session == NULL
    free physical device handles
    vkDestroyInstance
```

The logical device enables **zero optional features and extensions**.
The physical device and Compute queue are checked against explicitly
declared public workload limits, not the GPU vendor/model name.

## Production lifecycle

With `ENABLE_VULKAN_DISCOVERY=1` the production `source/src/backend.c`
uses these owner transitions:

1. `backend_ctx_init` opens the Vulkan 1.0 physical-device catalogue
   alongside normal CUDA/HIP/Metal/OpenCL initialization.
2. `backend_ctx_devices_init` opens the isolated logical-device session
   **only in backend-information mode (`hashcat -I`)** using an explicit
   1×1×1 public Compute workload, one storage descriptor and a 4-byte
   storage-range requirement. It submits a single empty command buffer,
   waits on its fence and logs the real Vulkan result.
3. `backend_ctx_devices_destroy` closes the logical-device session
   before the ordinary device contexts are destroyed.
4. `backend_ctx_destroy` attempts session closure as a backstop and
   closes the Vulkan catalogue afterward. The catalogue refuses to
   destroy VkInstance while a VkDevice session is still active.

All Vulkan errors are reported with their real VkResult values.
Ordinary CUDA, HIP, Metal and OpenCL devices remain independent.
Vulkan devices are not inserted into HASHCAT's active hashing-device
list, and the `backend_session_begin` attack/kernel path is not used.

## Public C API

### `backend_vulkan_session_open`

```c
VkResult backend_vulkan_session_open (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  const vulkan_auto_compute_requirements_t *workload,
  FILE *diagnostics);
```

Requires an opened parent catalogue, explicit in-range device index,
non-NULL workload and diagnostics stream, and an empty session owner.
Checks the driver's Vulkan API, queue availability and Compute limits.
Creates VkDevice, queue, pool, primary buffer and fence, then records,
submits and waits for one empty command buffer. On success the session
belongs to `backend_ctx->vulkan_session`.

Any failure releases all partially acquired Vulkan objects and leaves
the session pointer NULL. Malformed host calls return
`VK_ERROR_INITIALIZATION_FAILED`. A workload exceeding the actual
driver limits returns `VK_ERROR_FEATURE_NOT_PRESENT`.

### `backend_vulkan_session_close`

```c
VkResult backend_vulkan_session_close (
  backend_ctx_t *backend_ctx);
```

Waits for the device to idle, destroys its subordinate resources and
logical device, frees the allocation and clears the session pointer.
An empty session closes successfully. NULL context returns
`VK_ERROR_INITIALIZATION_FAILED`. If vkDeviceWaitIdle reports an error,
that error is returned after attempting resource teardown.

### `backend_vulkan_catalog_close`

```c
VkResult backend_vulkan_catalog_close (
  backend_ctx_t *backend_ctx);
```

Requires the logical-device session to be closed before VkInstance.
Returns `VK_ERROR_INITIALIZATION_FAILED` without releasing the
catalogue if an active session still owns VkDevice.

## Real Android hardware verification

Every Vulkan resource below was provided by a real mobile GPU driver.

| Android process | Vulkan 1.0 standalone | Vulkan 1.1 standalone | Full `hashcat -I` |
| --- | ---: | ---: | ---: |
| RedMagic / Adreno 730 / ARM64 | 2 | 2 | 1 |
| RedMagic / Adreno 730 / ARMv7 | 2 | 2 | 1 |
| Pixel 9 Pro XL / Mali-G715 / ARM64 | 2 | 2 | 1 |
| **Total** | **6** | **6** | **3** |

The standalone diagnostic completed **12 Vulkan sessions and 12
fence-verified empty queue submissions** across both requested API
versions. Those runs also observed:

- 48 rejected invalid host-session opens;
- 24 rejected active-session ownership operations, including attempts
  to destroy VkInstance before VkDevice;
- 12 real driver refusals for an explicitly unsupported local invocation
  count of 4,294,967,295;
- 9 rejected malformed native command lines.

Both complete Android API 24 HASHCAT executables were linked and
executed on the three device/process configurations. All three
`hashcat -I` processes returned exit **0** and completed one additional
Vulkan session, one empty fence submission and ordered cleanup each.
The ordinary OpenCL GPU was listed during the same native process.

Across the two test families this is **15 actual logical-device
lifecycles and 15 fence-completed empty submits**. No GPU shader was
dispatched and Vulkan hashing explicitly remained disabled.

## Android builds and reproducibility

Compile standalone API-24 Compute session diagnostics:

```powershell
.\tools\build-vulkan-backend-session.ps1 -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865 -AndroidApi 24
```

Run the diagnostic on an explicitly connected Android device:

```powershell
.\tools\run-vulkan-backend-session.ps1 -Serial $DeviceAdbSerial -Abi auto -ReportPrefix device-arm -TestCore11
```

For the RedMagic 32-bit process specify `-Abi armeabi-v7a`.

Build the complete Android HASHCAT frontends into an isolated directory:

```powershell
.\scripts\BUILD.ps1 -Target android-arm64-v8a -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865 -AndroidApi 24 -EnableVulkanDiscovery -VulkanOutputName vulkan-hashcat-session-20261101 -Package:$false

.\scripts\BUILD.ps1 -Target android-armeabi-v7a -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865 -AndroidApi 24 -EnableVulkanDiscovery -VulkanOutputName vulkan-hashcat-session-20261101 -Package:$false
```

The experimental build uses an explicit `COMPTIME=1793491200`
(2026-11-01 00:00 UTC) by default, instead of the Makefile's
time-of-build epoch, so repeated diagnostic builds have stable input.
The value can be supplied explicitly with `-VulkanComptime`.
Ordinary release builds keep their existing time-of-build policy.

Each full ABI build contains the HASHCAT v7.1.2 executable, 581
native modules, four bridges and 1,595 OpenCL asset files. Android
API 24 experimental builds exclude bundled UnRAR because it requires
the unavailable `lutimes` API. Runtime assets resolve relative to
the installed executable directory.

Static ELF validation checks Android API 24, exact ARM64/ARMv7 type,
`libvulkan.so`, Vulkan 1.0 `vkCreateDevice`, `vkQueueSubmit` and
`vkWaitForFences` imports, absence of a required Vulkan 1.1
`vkEnumerateInstanceVersion` symbol, and no unnecessary Vulkan loader
dependency in the standard module `module_00000.so`.

All native binaries are stored in ignored diagnostic directories.
No release ZIP, HASHCAT-APP repository or APK was modified.

## Archived evidence and independent verification

The 12 public transcripts and SHA-256 manifest are in:

```text
tests/fixtures/android-gpu/vulkan-backend-session-20261101/
  manifest.json
  redmagic-arm64-1.0.log
  redmagic-arm64-1.1.log
  redmagic-arm64-negative-controls.log
  redmagic-arm64-hashcat-info.log
  redmagic-armv7-1.0.log
  redmagic-armv7-1.1.log
  redmagic-armv7-negative-controls.log
  redmagic-armv7-hashcat-info.log
  pixel-arm64-1.0.log
  pixel-arm64-1.1.log
  pixel-arm64-negative-controls.log
  pixel-arm64-hashcat-info.log
```

The standalone diagnostic logs preserve the LF-only text emitted by
the Android test runner, which normalizes ADB output line endings.
The full `hashcat -I` transcripts likewise contain native
stdout/stderr plus the exit marker with LF separators. Neither
capture is **represented as byte-for-byte original ADB streams**.

The independent verifier audits 18 canonical source identities,
four diagnostic binaries/objects, two complete Android HASHCAT
executables, all 15 GPU session lifecycles, driver refusals,
ordinary OpenCL coexistence and deliberately corrupted evidence.

```powershell
python -B tools/test-vulkan-backend-session-evidence.py --artifacts build/diagnostic-multimode/vulkan-backend-session-20261101 --full build/diagnostic-multimode/vulkan-hashcat-session-20261101 --readelf D:\ANDROID\SDK\ndk\29.0.14206865\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe
```

Compatibility limitations: tested phones expose newer Vulkan loaders
even though only Vulkan 1.0 is requested. A physical Vulkan-1.0-only
GPU and actual Android 7.0/API-24 OS were unavailable. Concurrent
Vulkan device sessions, device-loss recovery, validation-layer
tracking, Vulkan hashing kernels and production hashing-scheduler
integration remain outside this milestone.

The follow-on [production-owned public SPIR-V Compute kernel](vulkan-backend-public-kernel-android.md)
executes an actual shader and verifies 64 GPU outputs from `hashcat -I`
on Android ARM64 and ARMv7. The earlier session log remains an exact
historical snapshot of the empty-submission-only milestone; its source
hashes are not reinterpreted as evidence of this later shader.

An independent public-integer Vulkan 1.0 buffer-transfer diagnostic is
available in `docs/vulkan-core10-transfer-buffer-portability.md`.
It verifies actual VkBuffer allocations, queue copying and noncoherent
memory synchronization without linking these resources to HASHCAT's
hashing-device list or production kernel scheduler.
