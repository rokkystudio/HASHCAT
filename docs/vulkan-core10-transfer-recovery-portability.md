# Vulkan 1.0 fault-to-working-buffer recovery: real Android GPUs

Date: 2026-11-01

## Implemented scope

An independent Android API 24 diagnostic under `tools/` verifies
that a Vulkan buffer session can be reopened **without closing the
owning VkInstance** after a controlled partial-acquisition failure.

For each test-defined acquisition point 1 through 10, the diagnostic:

1. Creates a real Vulkan 1.0 or Vulkan 1.1 `VkInstance`, enumerates
   physical devices and selects device index zero against an explicit
   1×1×1 Compute requirement with two 4 KiB storage buffers.
2. Starts a `vulkan_core10_transfer_session_t` and requests a
   deterministic interruption immediately after acquiring one
   specified real Vulkan object.
3. Verifies that the interrupted session returns
   `VK_ERROR_INITIALIZATION_FAILED`, destroys each acquired
   `VkDevice` resource and clears its complete owner while
   **VkInstance remains live**.
4. Opens a new `VkDevice` using the **same selected physical device
   and same VkInstance**. Executes five fence-completed
   `vkCmdCopyBuffer` transfers of 1, 7, 64, 257 and 1024 public
   32-bit integers, verifies all values and out-of-range sentinels,
   then destroys the complete session.
5. Opens a **second new VkDevice**, still under the same VkInstance,
   performs another 1024-word copy and full verification, and closes
   the logical device again.
6. Destroys the parent catalogue and VkInstance **only after both
   recovered VkDevices are gone**.

Every test invocation runs these steps twice using two fresh
VkInstances. Unlike a test that only checks a successful return from
`vkDestroyDevice`, this proves that real GPU allocations, buffer
mapping, queue submission, fence wait and host-memory synchronization
are still usable immediately after the controlled failure.

This diagnostic **does not integrate into the production HASHCAT
backend_ctx**, execute a hashing kernel, process password candidates,
create a Compute shader, or schedule any cryptographic work.

## Explicit acquisition fault points

The failed acquisition creates an actual driver object before the
application injects a test-defined `VK_ERROR_INITIALIZATION_FAILED`.

| Point | Vulkan object acquired before the interruption |
| ---: | --- |
| 1 | `VkDevice` |
| 2 | `VkCommandPool` |
| 3 | `VkCommandBuffer` |
| 4 | `VkFence` |
| 5 | Source `VkBuffer` |
| 6 | Source `VkDeviceMemory` |
| 7 | Source `vkMapMemory` |
| 8 | Destination `VkBuffer` |
| 9 | Destination `VkDeviceMemory` |
| 10 | Destination `vkMapMemory` |

The existing ownership implementation reports exact release counts
for mapped allocations, buffers, allocated device memory, fences,
command buffers, command pools and the logical device.

The recovery diagnostic requires a zeroed owner following every
injected failure. Both subsequent successful sessions independently
acquire ten real Vulkan resources and execute their own GPU copies.
These methods use a real `VkDevice` and `VkQueue`, never mock Vulkan
driver handles or infer memory compatibility from a GPU vendor name.

## Real Android results

All tests below completed on physically connected Android GPUs.

| GPU / process ABI | Requested Vulkan API | HOST_VISIBLE memory | Fault points | Successful reopened VkDevices | Completed GPU copies |
| --- | --- | --- | ---: | ---: | ---: |
| RedMagic Adreno 730 / ARM64 | 1.0 and 1.1 | Preferred coherent | 20/20 PASS | 80 | 240 |
| RedMagic Adreno 730 / ARMv7 | 1.0 and 1.1 | Preferred coherent | 20/20 PASS | 80 | 240 |
| Pixel 9 Pro XL Mali-G715 / ARM64 | 1.0 and 1.1 | Preferred coherent | 20/20 PASS | 80 | 240 |
| Pixel 9 Pro XL Mali-G715 / ARM64 | 1.0 and 1.1 | **Required noncoherent** | 20/20 PASS | 80 | 240 |
| **Total** | | | **80/80** | **320** | **960** |

The 80 native programs each create two fresh VkInstances and perform
two controlled acquisition failures per instance recovery sequence:

- **160 real VkInstance lifecycles** retained while the interrupted
  VkDevice is released and two fully working VkDevices are created.
- **160 controlled resource-acquisition failures** with complete
  partial ownership cleanup.
- **320 fresh fully owned and closed VkDevice recovery sessions**.
- **960 real fence-completed `vkCmdCopyBuffer` GPU submissions**.
- **380,320 verified unsigned 32-bit integers**, totaling
  **1,521,280 bytes**.
- **240 required noncoherent GPU copies on Mali-G715**, each with
  a real `vkFlushMappedMemoryRanges` and
  `vkInvalidateMappedMemoryRanges` using the driver-reported
  64-byte memory atom.
- **18 malformed Android recovery CLI requests rejected** with
  native exit 2.

The preferred memory mode selected a real driver-compatible
HOST_VISIBLE memory type; the explicit noncoherent mode required
HOST_VISIBLE memory **without HOST_COHERENT**, and was tested on
Mali-G715 where that memory type is supported.

The Adreno drivers refused that noncoherent memory request in prior
dedicated capability tests; strict noncoherent recovery was therefore
**not asserted for Adreno**. All Adreno recovery runs used its actual
supported coherent memory.

The experimental executables request exactly Vulkan core 1.0 or 1.1,
but all tested GPU loaders are newer. The binaries target Android
API 24; the phones do not run Android 7.0 and no physically
Vulkan-1.0-only device was available.

## Native recovery events

The diagnostic records this exact ordered resource chain for each
trial:

```text
DEVICE_CATALOG_OPEN ... instance_api=1.0 ...
DEVICE_CATALOG_SELECT ... effective_api=1.0 ...
TRANSFER_INJECT_FAULT ... point=10 ... resource=destination_mapping result=-3
TRANSFER_OPEN_FAILED ... result=-3
TRANSFER_CLOSE ... wait_result=0
TRANSFER_RELEASE ... mapped=2 buffers=2 memory=2 fence=1 command=1 pool=1 device=1
RECOVERY_FAULT_CONFIRMED trial=0 point=10 result=-3 owner=0 instance=1 copies=0
TRANSFER_OPEN ... bytes=4096 ... shaders=0
RECOVERY_REOPEN trial=0 phase=0 ... acquisitions=10 copied=0
TRANSFER_COPY ... fence=signaled shaders=0
RECOVERY_VERIFIED ... valid=1 shaders=0
RECOVERY_PHASE_PASS ... copies=5 verified_words=1353 copied_bytes=5412 ...
TRANSFER_CLOSE ... copies=5 ... wait_result=0
RECOVERY_PHASE_CLOSED trial=0 phase=0 owner=0 instance=1
TRANSFER_OPEN ... bytes=4096 ... shaders=0
RECOVERY_REOPEN trial=0 phase=1 ... acquisitions=10 copied=0
TRANSFER_COPY ... bytes=4096 ... fence=signaled shaders=0
RECOVERY_VERIFIED ... valid=1 shaders=0
RECOVERY_PHASE_PASS ... copies=1 verified_words=1024 copied_bytes=4096 ...
TRANSFER_CLOSE ... copies=1 ... wait_result=0
RECOVERY_PHASE_CLOSED trial=0 phase=1 owner=0 instance=1
DEVICE_CATALOG_CLOSE ... selections=1
RECOVERY_TRIAL_PASS trial=0 ... failed=1 reopened=2 copies=6 ...
```

The event excerpt condenses the five phase-zero copy records; the
archived logs contain every individual actual GPU submission, fence
completion, source flush and destination invalidate count.

Successful native exit zero requires **both complete trials**, four
recovered devices, twelve GPU copies and 4,754 verified 32-bit words
per invocation. Any real Vulkan driver error or output mismatch
returns a nonzero test failure rather than a recovery success.

## Build and run

Compile the isolated standalone executable for Android API 24
ARM64 and ARMv7 with the installed NDK:

```powershell
.\tools\build-vulkan-core10-transfer-recovery.ps1 -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865 -AndroidApi 24
```

The two native ELF files are written under the ignored build tree:

```text
build/diagnostic-multimode/vulkan-core10-transfer-recovery-20261101/
  vulkan-core10-transfer-recovery-arm64
  vulkan-core10-transfer-recovery-armv7
```

Run all ten recovery fault points with both Vulkan versions for one
physically attached Android device:

```powershell
.\tools\run-vulkan-core10-transfer-recovery.ps1 -Serial $DeviceAdbSerial -Abi arm64-v8a -ReportPrefix pixel-arm64 -TestCore11 -TestNoncoherent
```

Select `-Abi armeabi-v7a` on a device that actually reports ARMv7
support. The `-TestNoncoherent` switch requires compatible
driver memory; it is not a fallback policy.

Direct native execution of one point:

```text
vulkan-core10-transfer-recovery-arm64 --minimum 1.0 --memory prefer --fault-after 10
```

Only points 1..10 and the two declared API/memory modes are
accepted. An invalid CLI exits 2; an unexpected Vulkan driver or
memory error exits 1.

The build script checks the actual Android 24 ELF ABI note, ARM32/
ARM64 process type, Vulkan 1.0 loader dependency and native
`vkCreateDevice`, `vkCmdCopyBuffer`, `vkQueueSubmit`,
`vkWaitForFences`, `vkFlushMappedMemoryRanges` and
`vkInvalidateMappedMemoryRanges` imports. No mandatory Vulkan 1.1
`vkEnumerateInstanceVersion` import is permitted.

## Source provenance and independent offline verifier

Archive all real hardware stdout/stderr after running the device
matrix:

```powershell
python -B tools/archive-vulkan-core10-transfer-recovery-evidence.py
```

Verify the exact archived source/binary provenance, complete GPU
events, correct partial/final cleanup and rejected malformed native
commands:

```powershell
python -B tools/test-vulkan-core10-transfer-recovery-evidence.py --artifacts build/diagnostic-multimode/vulkan-core10-transfer-recovery-20261101 --readelf D:\ANDROID\SDK\ndk\29.0.14206865\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe
```

The independent regression reads only saved text/ELF inputs and
never accesses Android or executes any GPU command. It inspects
**80 actual native recovery transcripts, 3 negative-control logs,
13 source hashes and both native ELF identities**. It also
deliberately corrupts **31** resource, fence, memory, output and
instance-teardown events, requiring every altered record to fail
validation.

Archived public logs:

```text
tests/fixtures/android-gpu/vulkan-core10-transfer-recovery-20261101/
  manifest.json
  redmagic-arm64-{1.0,1.1}-prefer-recover-{1..10}.log
  redmagic-arm64-recovery-negative-controls.log
  redmagic-armv7-{1.0,1.1}-prefer-recover-{1..10}.log
  redmagic-armv7-recovery-negative-controls.log
  pixel-arm64-{1.0,1.1}-{prefer,noncoherent}-recover-{1..10}.log
  pixel-arm64-recovery-negative-controls.log
```

All logs are generated from actual ADB driver output and normalized
to UTF-8 LF by the PowerShell test runner. Archived files preserve
the runner's output bytes; **they are not represented as original
unprocessed ADB byte streams**.

## Verification limits

The failure is *injected by the application immediately after a real
successful Vulkan object acquisition*. This verifies the application
recovery path, not an authentic driver allocation failure, GPU
reset or `VK_ERROR_DEVICE_LOST`. Validation-layer instrumentation,
in-process multithreading and driver-level memory-leak measurement
remain outside this evidence.

No production HASHCAT kernel/session integration, password-cracking
dispatcher, cryptographic shader or Android APK is part of this
milestone. Production HASHCAT's existing `hashcat -I` empty-submit
Vulkan information-mode behaviour remains separate and unchanged.
