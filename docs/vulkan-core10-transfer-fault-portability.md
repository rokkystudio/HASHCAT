# Vulkan 1.0 buffer acquisition-fault ownership on Android

Date: 2026-11-01

## Scope

The standalone `vulkan_core10_transfer_session_t` tests deterministic
resource-acquisition failures after genuine Vulkan object creation.
This milestone is independent of the production HASHCAT backend and
executes no hashing algorithm, shader, descriptor set or GPU copy while
a fault is active.

`vulkan_core10_transfer_session_open` accepts an explicit `fail_after`
value:

- `0`: normal Vulkan resource acquisition and public integer copying;
- `1..10`: return `VK_ERROR_INITIALIZATION_FAILED` immediately after
  the specified acquisition succeeds, then release all acquired
  resources in dependency-safe order;
- greater than `10`: return `VK_ERROR_INITIALIZATION_FAILED` before
  allocating any Vulkan object.

The fault is a **controlled test-defined interruption**. It is not a
driver failure and does not simulate `VK_ERROR_DEVICE_LOST`.

## Acquisition and destruction contracts

The point number corresponds to the exact Vulkan handle obtained
successfully from the actual Android driver.

| Point | Successful acquisition immediately before test failure | Mapped allocations released | Buffers destroyed | Device memory freed |
| ---: | --- | ---: | ---: | ---: |
| 1 | `VkDevice` | 0 | 0 | 0 |
| 2 | `VkCommandPool` | 0 | 0 | 0 |
| 3 | `VkCommandBuffer` | 0 | 0 | 0 |
| 4 | `VkFence` | 0 | 0 | 0 |
| 5 | Source `VkBuffer` | 0 | 1 | 0 |
| 6 | Source `VkDeviceMemory` | 0 | 1 | 1 |
| 7 | Source `vkMapMemory` | 1 | 1 | 1 |
| 8 | Destination `VkBuffer` | 1 | 2 | 1 |
| 9 | Destination `VkDeviceMemory` | 1 | 2 | 2 |
| 10 | Destination `vkMapMemory` | 2 | 2 | 2 |

All points destroy **exactly one** `VkDevice`. Point 2 onward releases
a command pool, point 3 onward frees a command buffer and point 4
onward destroys a fence.

The `vulkan_core10_transfer_acquired` method counts **genuinely
acquired** Vulkan resources and emits an explicit fault record only
when its acquisition count equals the caller's `fail_after` input.

On each controlled failure, `vulkan_core10_transfer_session_open`
releases partial resources through
`vulkan_core10_transfer_session_close`. The close method invokes
`vkDeviceWaitIdle`, unmaps each successful `vkMapMemory` mapping,
destroys each created `VkBuffer`, frees each allocated
`VkDeviceMemory`, releases fence/command/pool and destroys
`VkDevice`. It then clears the entire owning session.

The test runner verifies the exact reported release counts, the
expected injected `VkResult`, zero GPU queue copies and closure of
the independent physical-device `VkInstance` **after VkDevice**.

For example, point 10 requires:

```text
TRANSFER_INJECT_FAULT point=10 acquisition=10 resource=destination_mapping result=-3
TRANSFER_OPEN_FAILED index=0 mode=0 result=-3
TRANSFER_CLOSE index=0 bytes=0 copies=0 copied_bytes=0 flushes=0 invalidates=0 wait_result=0
TRANSFER_RELEASE point=10 acquired=10 mapped=2 buffers=2 memory=2 fence=1 command=1 pool=1 device=1
TRANSFER_FAULT_TRIAL trial=0 point=10 injected=1 owned=0
DEVICE_CATALOG_CLOSE devices=1 selections=1
```

The driver can return a real allocation failure at an earlier step.
Such an unexpected result does not count as the requested controlled
fault and causes the native diagnostic to report failure rather than
pretending that an injection occurred.

## Actual Android hardware

Each of the ten acquisition points runs **twice** in each device/API
combination with a fresh `VkInstance`, real physical-device
enumeration and partial logical-device ownership.

| Device / native process | Vulkan 1.0 points | Vulkan 1.1 points | Partial VkDevice sessions |
| --- | ---: | ---: | ---: |
| RedMagic / Adreno 730 / ARM64 | 10/10 PASS | 10/10 PASS | 40 |
| RedMagic / Adreno 730 / ARMv7 | 10/10 PASS | 10/10 PASS | 40 |
| Pixel 9 Pro XL / Mali-G715 / ARM64 | 10/10 PASS | 10/10 PASS | 40 |
| **Total** | **30/30** | **30/30** | **120** |

The full hardware matrix therefore comprises:

- **60 independent real Android native test invocations**, each
  returning expected exit **42** after two controlled faults;
- **120 actual VkDevice partial allocation and cleanup trials**;
- **120 parent VkInstance creation/destruction lifecycles**;
- **15 rejected malformed native fault options** (five per ABI/device
  configuration, all with exit 2);
- **zero** queue-copy submissions or shaders in the fault sessions.

The faults use the preferred host-visible memory policy to avoid
conflating a missing noncoherent driver memory type with an intentional
resource-acquisition interruption.

### Normal-copy regression

After adding explicit fault points, the unchanged nonfault mode
(`fail_after=0`) was rerun on the same three Android configurations.
Both Vulkan API versions and both exact memory-mode policies were
retested.

The normal hardware evidence retains **16 successful complete device
sessions**, **80 fence-completed `vkCmdCopyBuffer` submissions**,
**21,648 verified 32-bit integers**, **86,592 transferred bytes**
and **eight explicit Adreno noncoherent-memory compatibility refusals**.
The Mali strict noncoherent mode still performs real
`vkFlushMappedMemoryRanges` and `vkInvalidateMappedMemoryRanges`.

The normal-copy archive has been regenerated from these new real
device runs and the fault-aware executables. The earlier Git
revision `ba6dcc1` independently retains the original binary/source
identities and capture history.

## Reproducing the tests

Build two standalone Android API 24 ELF executables with the original
public Vulkan 1.0 loader contract:

```powershell
.\tools\build-vulkan-core10-transfer.ps1 -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865 -AndroidApi 24
```

Run all ten exact fault points for both supported API versions:

```powershell
.\tools\run-vulkan-core10-transfer-faults.ps1 -Serial $DeviceAdbSerial -Abi arm64-v8a -ReportPrefix device-arm64 -TestCore11
```

For a RedMagic ARMv7 process, use `-Abi armeabi-v7a`. The runner
validates the ABI reported by the device before executing any
native test.

The diagnostic program's direct interface accepts:

```text
vulkan-core10-transfer-arm64 --minimum 1.0 --memory prefer --fault-after 10
```

Its exit codes are `42` for a complete controlled-fault test,
`2` for malformed input, and `1` for any genuine driver/test failure.

All 63 LF-only hardware logs are archived at:

```text
tests/fixtures/android-gpu/vulkan-core10-transfer-faults-20261101/
  manifest.json
  redmagic-arm64-{1.0,1.1}-fault-{1..10}.log
  redmagic-arm64-fault-negative-controls.log
  redmagic-armv7-{1.0,1.1}-fault-{1..10}.log
  redmagic-armv7-fault-negative-controls.log
  pixel-arm64-{1.0,1.1}-fault-{1..10}.log
  pixel-arm64-fault-negative-controls.log
```

The logs preserve the native Android runner's LF-normalized output
and are not presented as original unprocessed ADB bytes. The manifest
records exact binaries, source SHA-256 and the test scope.

Recreate and validate the fixture inventory:

```powershell
python -B tools/archive-vulkan-core10-transfer-fault-evidence.py
python -B tools/test-vulkan-core10-transfer-fault-evidence.py --artifacts build/diagnostic-multimode/vulkan-core10-transfer-20261101 --readelf D:\ANDROID\SDK\ndk\29.0.14206865\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe
```

The independent offline validator audits every acquisition and
release event in all 120 sessions, process return codes, the complete
source/ELF identities, absence of submitted GPU copies, and **25
deliberately corrupted fault-evidence records**.

## Coverage boundaries

The evidence demonstrates that the Vulkan driver can create each
resource and that the application calls its dependency-safe release
sequence after each **controlled interruption**. It does not prove
absence of all GPU/driver memory leaks through validation layers or
driver instrumentation.

No actual driver allocation error, `VK_ERROR_DEVICE_LOST`, device
reset, multithreaded contention, Android API-24 OS hardware or
physically Vulkan-1.0-only GPU was part of these tests.

The production HASHCAT `backend_ctx` session is unchanged and
remains limited to empty queue validation during `hashcat -I`.
These standalone tests cannot execute HASHCAT hashing kernels or
password-candidate workloads.
