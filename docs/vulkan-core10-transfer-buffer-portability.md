# Vulkan 1.0 public GPU buffer transfers on Android

Date: 2026-11-01

## Scope

An independent Vulkan 1.0 transfer session under `tools/` creates two
host-visible storage/transfer buffers using actual driver memory-type
compatibility, submits `vkCmdCopyBuffer` on a real Compute-capable queue,
waits for a Vulkan fence and returns the observed public integer data.

This module is deliberately **not connected to HASHCAT backend_ctx**.
The production HASHCAT Vulkan information-mode session retains its
shader-free empty queue submission. No hashing kernel, password
candidate, SPIR-V shader or cryptographic input is processed.

## Vulkan resources and synchronization

```text
Caller-owned VkInstance
  +-- VkPhysicalDevice[explicit index]
      +-- vulkan_core10_transfer_session_t
          +-- VkDevice, VkQueue
          +-- VkCommandPool, primary VkCommandBuffer, VkFence
          +-- source VkBuffer, VkDeviceMemory, mapped pointer
          +-- destination VkBuffer, VkDeviceMemory, mapped pointer
```

The two buffers are created with STORAGE_BUFFER, TRANSFER_SRC and
TRANSFER_DST usage and receive **independent driver memory
requirements**, allocations and mappings.

The `PREFER_COHERENT` memory mode first selects compatible
HOST_VISIBLE|HOST_COHERENT, then HOST_VISIBLE. The explicitly requested
`REQUIRE_NONCOHERENT` mode accepts **only** compatible HOST_VISIBLE
memory without HOST_COHERENT and returns
`VK_ERROR_FEATURE_NOT_PRESENT` if that type is unavailable. Vendor
names never determine the chosen memory type.

A public input copy follows exact Vulkan 1.0 synchronization:

1. Copy the supplied host bytes to the mapped source allocation.
2. Flush the full mapped allocation when source memory is noncoherent.
3. Reset the command buffer; record `vkCmdCopyBuffer` and a
   TRANSFER_WRITE → HOST_READ pipeline memory dependency.
4. Reset the fence, submit to the actual GPU queue and wait for fence
   completion.
5. Invalidate noncoherent destination memory and return the requested
   bytes to the caller's output array.

Mapped memory uses offset zero and `VK_WHOLE_SIZE` for noncoherent
synchronization, satisfying the driver's `nonCoherentAtomSize` with no
inferred subrange alignment.

Closing the session waits for device idle, unmaps allocations, destroys
both buffers and their memory, releases fence/command/pool resources
and destroys `VkDevice`. The caller then closes its VkInstance.
Failed partial opens use the same release ordering.

## Public C API

`vulkan_core10_transfer_session_open(session, selected, buffer_bytes, memory_mode, diagnostics)`

Requires a zero-initialized session, explicitly capability-checked
physical device, exact in-range 4-byte-aligned buffer size and a
supported memory mode. Creates VkDevice, queue, fence, command pool,
buffer objects and driver-compatible mapped allocations. On any error
the session is empty and the driver result is returned.

`vulkan_core10_transfer_session_copy(session, input, output, bytes)`

Requires non-NULL input/output and a positive, four-byte-aligned byte
count no larger than the allocated buffers. Performs the GPU copy,
fence wait, host-visible cache synchronization and output transfer.
No output is written until GPU completion and synchronization succeed.

`vulkan_core10_transfer_session_close(session)`

Waits for GPU idle, releases all mapped memory and Vulkan resources,
and clears the complete session. Closing an empty session succeeds;
device-idle failures are returned after resource teardown attempts.

All complete parameter contracts and resource fields are documented
in `tools/vulkan-core10-transfer-session.h`.

## Real Android hardware evidence

Each successful case comprises **two independent VkDevice sessions
and ten fence-completed copies**. Each session verifies five public
unsigned-integer arrays of lengths 1, 7, 64, 257 and 1024 words.
All in-range values must match, and the caller's out-of-range output
sentinels must remain untouched.

| Android GPU / ABI | Vulkan 1.0 preferred | Vulkan 1.1 preferred | Vulkan 1.0 noncoherent | Vulkan 1.1 noncoherent |
| --- | --- | --- | --- | --- |
| RedMagic Adreno 730 / ARM64 | 2 PASS | 2 PASS | Driver refused | Driver refused |
| RedMagic Adreno 730 / ARMv7 | 2 PASS | 2 PASS | Driver refused | Driver refused |
| Pixel 9 Pro XL Mali-G715 / ARM64 | 2 PASS | 2 PASS | 2 PASS | 2 PASS |
| **Total** | **6** | **6** | **2** | **2** |

Across the matrix there were **16 successful Vulkan session lifecycles,
80 completed GPU copies, 21,648 verified 32-bit words and 86,592
transferred bytes**. The Mali-G715 strict noncoherent cases performed
20 real `vkFlushMappedMemoryRanges` calls and 20 real
`vkInvalidateMappedMemoryRanges` calls with the driver's reported
64-byte noncoherent memory atom.

On the Adreno GPU the driver refused the explicitly required
HOST_VISIBLE noncoherent type in eight trials with
`VK_ERROR_FEATURE_NOT_PRESENT`. These cases use native exit **3**,
not a fabricated successful transfer. The supported coherent memory
case used a real HOST_COHERENT type, without unnecessary flush or
invalidate operations. Nine malformed command-line cases returned
exit **2**.

The executables target Android API **24** and request Vulkan core
1.0 or optionally 1.1. The tested physical phones have newer Vulkan
loaders; no Vulkan-1.0-only hardware or actual Android 7 OS was tested.

## Build and reproduce

The standalone diagnostics do not compile, modify or link the
production HASHCAT backend.

```powershell
.\tools\build-vulkan-core10-transfer.ps1 -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865 -AndroidApi 24
.\tools\run-vulkan-core10-transfer.ps1 -Serial $DeviceAdbSerial -Abi auto -ReportPrefix device-arm64 -TestCore11 -TestNoncoherent
```

For a 32-bit Android process, explicitly select `-Abi armeabi-v7a` on
a device reporting that ABI. The build script independently checks
Android API24 ELF notes, architecture, `libvulkan.so` and Vulkan 1.0
queue/memory imports, with no mandatory Vulkan 1.1 loader entry point.

The runner saves 15 LF-only logs in the ignored diagnostic directory.
Archive them and verify the retained copies:

```powershell
python -B tools/archive-vulkan-core10-transfer-evidence.py
python -B tools/test-vulkan-core10-transfer-evidence.py --artifacts build/diagnostic-multimode/vulkan-core10-transfer-20261101 --readelf D:\ANDROID\SDK\ndk\29.0.14206865\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe
```

Permanent evidence:

```text
tests/fixtures/android-gpu/vulkan-core10-transfer-20261101/
  manifest.json
  redmagic-arm64-{1.0,1.1}-{prefer,noncoherent}.log
  redmagic-arm64-negative-controls.log
  redmagic-armv7-{1.0,1.1}-{prefer,noncoherent}.log
  redmagic-armv7-negative-controls.log
  pixel-arm64-{1.0,1.1}-{prefer,noncoherent}.log
  pixel-arm64-negative-controls.log
```

The 15 UTF-8 LF logs preserve the exact runner outputs. Android ADB
stdout/stderr line endings were normalized by the runner, so the
archive is **not presented as the original raw ADB byte stream**.

The independent verifier checks 13 source hashes, two native ELF
hashes, exact queue/fence/transfer lifecycles, noncoherent cache sync,
driver capability refusals and **20 deliberate evidence corruptions**.

## Deliberate boundaries

There is no Vulkan Compute shader execution, hashing-device
registration, candidate generation, crypto workload or APK
deployment in this implementation. Transfer ownership remains
standalone and separate from the production HASHCAT backend.
Concurrent-device work, device-loss recovery, validation-layer
leak detection and actual API24 OS hardware are not covered.
