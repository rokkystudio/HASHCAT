# Vulkan 1.0 baseline and optional Vulkan 1.1 (2026-10-31)

## Compatibility contract

**Core Vulkan 1.0 is the default minimum for the synthetic Compute path.** A physical device reporting Vulkan 1.0 must not be rejected because a Vulkan 1.1, 1.2, 1.3 or 1.4 feature exists on a newer phone. Core Vulkan 1.1 is an **explicit workload requirement**, not a default optimization or device-model profile. The standalone native builds target **Android API 24**, where Vulkan first became available, for both `arm64-v8a` and `armeabi-v7a`. See the [Android NDK Vulkan API requirements](https://developer.android.com/ndk/guides/stable_apis#vulkan).

The standalone probe in `tools/vulkan-core11-compute-diagnostic.c` requests **`VK_API_VERSION_1_0`** when launched with only the shader filename, even if the host Vulkan loader and physical GPU support later Vulkan versions. `--minimum 1.0` is equivalent; `--minimum 1.1` requests **`VK_API_VERSION_1_1`** after checking the loader and each physical device. It never requests Vulkan 1.2 or newer. The explicit trailing flag `--require-noncoherent-staging` exercises compatible `HOST_VISIBLE` memory **without** `HOST_COHERENT` and reports an error when either staging buffer lacks such a type.

This implementation is an independent Vulkan Compute diagnostic on **public, noncryptographic integer data**. It is not a production Vulkan hashcat backend, a password recovery accelerator or a replacement for the existing OpenCL bcrypt kernel.

The rules follow the [Khronos Vulkan versioning guide](https://docs.vulkan.org/guide/latest/versions.html), [VkApplicationInfo version rules](https://docs.vulkan.org/refpages/latest/refpages/source/VkApplicationInfo.html) and [Vulkan initialization specification](https://docs.vulkan.org/spec/latest/chapters/initialization.html).

## Version selection without model-specific rules

`tools/vulkan-core11-policy.h` and `tools/vulkan-core11-policy.c` provide three documented functions:

| Function | Exact decision |
| --- | --- |
| `vulkan_core11_query_loader_version` | Looks up `vkEnumerateInstanceVersion` dynamically with `vkGetInstanceProcAddr(VK_NULL_HANDLE, ...)`. If absent, the loader implements Vulkan 1.0 and the result is `VK_API_VERSION_1_0`. A returned error or malformed version is propagated as an error. |
| `vulkan_core11_select_instance_version` | The caller declares the precise required API, 1.0 or 1.1. The result is **exactly that required version**, never automatically upgraded to 1.1 on a newer loader. An unsupported explicit minimum returns `VK_ERROR_INCOMPATIBLE_DRIVER`. |
| `vulkan_core11_select_device_version` | Determines the usable physical-device API from `VkPhysicalDeviceProperties.apiVersion` and the actual requested instance API. An explicit 1.1 requirement rejects a 1.0 physical device with `VK_ERROR_FEATURE_NOT_PRESENT`; a 1.0 requirement accepts it. |

The program checks device Compute limits through the existing `vulkan_auto_check_device` method. The actual shader's fixed workgroup size, dispatch count, descriptor count and storage-buffer range are supplied as requirements. Unsupported limits or queue families are explicit failures, not heuristically adjusted values.

No vendor name, device ID, manufacturer-specific memory index or Android phone model is consulted by these selection functions.

### Why core 1.0 is sufficient here

The public Vulkan diagnostic uses only core 1.0 functionality:

- Compute shader pipelines, descriptor sets, storage buffers and `vkCmdDispatch`.
- `vkGetBufferMemoryRequirements`, `vkAllocateMemory`, `vkBindBufferMemory` and CPU-mappable staging memory selected from actual Vulkan flags.
- `vkCmdCopyBuffer` for upload/download, `vkCmdPipelineBarrier` for host/transfer/compute synchronization, a fence wait, and `vkFlushMappedMemoryRanges` / `vkInvalidateMappedMemoryRanges` when the selected memory type is noncoherent.
- Two storage and two staging buffers planned **jointly** by `vulkan_heap_plan_allocations` against their actual compatibility masks, heap assignments and `maxMemoryAllocationCount`.
- An unchanged **SPIR-V 1.0** shader using 32-bit integer arithmetic and local workgroup size `32 × 1 × 1`.

It does **not** require synchronization2, timeline semaphores, descriptor indexing, buffer device addresses, Vulkan 1.1 features, 16-bit storage, 64-bit shader integers or any device extension. `VkDeviceCreateInfo.pEnabledFeatures` is `NULL`; no optional features/extensions are enabled. The program queries `shaderInt64` and `shaderInt16` for context only, without enabling or using them.

Both `-O0` and `-O` SPIR-V 1.0 outputs were compiled with `glslc --target-env=vulkan1.0` and validated with `spirv-val --target-env vulkan1.0` and `--target-env vulkan1.1`. The shaders are byte-identical to the earlier independently archived stage-A modules.

## Real-device evidence

RedMagic and Pixel were tested with the same standalone ARM64 executable and the same two SPIR-V 1.0 files. The device version in the table is reported by `vkGetPhysicalDeviceProperties`; the loader version is separately reported by `vkEnumerateInstanceVersion`.

| Property | RedMagic / Adreno 730 | Pixel / Mali-G715 |
| --- | --- | --- |
| Loader API version | 1.3.0 | 1.4.0 |
| Physical GPU Vulkan API | 1.1.128 | 1.4.305 |
| Instance and effective GPU API, **default invocation** | **1.0** | **1.0** |
| Instance and effective GPU API, explicit `--minimum 1.0` | 1.0 | 1.0 |
| Instance and effective GPU API, explicit `--minimum 1.1` | 1.1 | 1.1 |
| Shader build `-O0` / `-O` | PASS / PASS | PASS / PASS |
| Enabled device features/extensions | 0 / 0 | 0 / 0 |
| GPU input/output memory type | 0 / 0 | 0 / 0 |
| CPU staging upload/download memory type | 6 / 6 | 0 / 0 |
| Total memory allocated and bound | 32,768 bytes | 32,768 bytes |
| GPU dispatches per run | 1 | 1 |
| GPU transfer copies per run | 2 | 2 |
| CPU/GPU comparison | 2,048 exact words | 2,048 exact words |
| Output FNV-1a 64-bit | `00d6d6e4837edebc` | `00d6d6e4837edebc` |

**All 12 ARM64 baseline GPU executions passed**: two devices × three API selection modes (`default10`, `explicit10`, `required11`) × two shader optimization modes (`-O0`, `-O`). The six separate baseline results for each phone are stored verbatim with exact SHA-256 identities. Both phones also rejected four malformed CLI requests each (exit code 2). Two additional noncoherent Vulkan 1.0 GPU runs passed on Pixel, while the two corresponding RedMagic attempts explicitly reported incompatible staging memory.

The memory planner used the actual four `VkMemoryRequirements.memoryTypeBits` masks on every run. The GPU program submitted a real Compute dispatch between a staging-buffer upload and readback, used the proper barriers and checked all output words on the CPU. No Vulkan 1.1-only calls were used in the default path.

### What the evidence does not establish

Neither connected phone has a **physical GPU restricted to Vulkan 1.0**. We have therefore directly validated a Vulkan 1.0 **requested API path** on later-version GPU drivers, not the full behavior of an older Vulkan-1.0-only driver.

The host-native C policy tests cover the important missing API case: an implementation with no `vkEnumerateInstanceVersion` returns Vulkan 1.0 without an error, and the explicit Vulkan 1.1 requirement is rejected. A real Vulkan-1.0-only GPU remains an additional hardware validation target.

The **12 baseline runs** selected coherent CPU staging memory on both devices. A separate explicit trial selected real **noncoherent** staging memory on Pixel and executed the Vulkan 1.0 `vkFlushMappedMemoryRanges` → GPU Compute/transfer → fence wait → `vkInvalidateMappedMemoryRanges` sequence. The actual driver calls returned `VK_SUCCESS` and all 2,048 GPU results matched the CPU. RedMagic does not expose a compatible noncoherent type for this staging upload buffer, and the same explicit trial returned `VK_ERROR_FEATURE_NOT_PRESENT` without silently selecting a coherent type. The original unforced test continues to work on RedMagic.

Successful execution on one shader does not guarantee that another shader meets Vulkan 1.0's SPIR-V capabilities, descriptor limits, workgroup limits, available memory or driver behavior. Every shader is independently validated against its declared requirements.

## Real Vulkan 1.0 noncoherent memory verification

The optional noncoherent trial uses the same four-buffer Compute pipeline and the same unchanged SPIR-V 1.0 shader as the baseline. It sets `VK_MEMORY_PROPERTY_HOST_COHERENT_BIT` in the caller's **forbidden flags** for both staging buffers. The universal heap planner still enforces each buffer's actual `memoryTypeBits` and `HOST_VISIBLE`. This is an explicit diagnostic option, never an automatically selected default.

| Actual Vulkan property or outcome | RedMagic / Adreno 730 | Pixel / Mali-G715 |
| --- | --- | --- |
| Staging upload compatible memory bits | `0x00000041` | `0x00000003` |
| Staging download compatible memory bits | `0x00000049` | `0x00000003` |
| Eligible noncoherent staging type for both | **Unavailable** | **1** |
| Selected staging `HOST_COHERENT` | N/A | **False** |
| Device-reported `nonCoherentAtomSize` | 1 byte | **64 bytes** |
| Real `vkFlushMappedMemoryRanges` | Not called | **VK_SUCCESS** |
| Real `vkInvalidateMappedMemoryRanges` | Not called | **VK_SUCCESS** |
| GPU/CPU exact comparison | No dispatch in forced mode | **2,048/2,048 PASS** |
| Exact forced-mode result | `VK_ERROR_FEATURE_NOT_PRESENT` | **VK_SUCCESS** |

Both SPIR-V builds (`-O0`, `-O`) produced the correct result on Pixel with an actual mapped noncoherent allocation and both driver cache-management calls. RedMagic explicitly rejected the upload buffer at request index 2, because its permitted types cannot satisfy `HOST_VISIBLE` while excluding `HOST_COHERENT`. No fallback was made, and the default coherent mode still passes on both phones.

The GPU pipeline uses Vulkan 1.0 synchronization: CPU writes → [flush](https://docs.vulkan.org/refpages/latest/refpages/source/vkFlushMappedMemoryRanges.html) → transfer/Compute work → transfer-to-host memory barrier → fence completion → [invalidate](https://docs.vulkan.org/refpages/latest/refpages/source/vkInvalidateMappedMemoryRanges.html) → CPU comparison. Each cache operation receives a mapping beginning at offset zero, with `VK_WHOLE_SIZE`, respecting the device's `nonCoherentAtomSize` alignment requirement.

The archive includes two Pixel noncoherent-GPU PASS logs and two RedMagic explicit incompatibility logs. Both outcomes are verified against independent memory-type properties from the earlier real heap survey; the hardware capability is not inferred from a manufacturer name.

## Android API 24 and ARMv7/ARM64 portability

The Android Vulkan loader library is available from **Android 7.0, API level 24**. The build script produces two standalone binaries using the installed Android NDK r29:

| Executable | Android ABI | NDK target | Minimum Android API |
| --- | --- | --- | --- |
| `vulkan-core-baseline-arm64` | `arm64-v8a` | `aarch64-linux-android24` | 24 |
| `vulkan-core-baseline-armv7` | `armeabi-v7a` | `armv7a-linux-androideabi24` | 24 |

The build also creates the `vulkan-heap-probe-armv7` buffer-only survey for independently recording real ARMv7 heap sizes. The ELF dynamic symbol tables import Vulkan 1.0 functions and `vkGetInstanceProcAddr` but do **not** import `vkEnumerateInstanceVersion` as a mandatory loader symbol. That optional function is queried at runtime.

The runner selects the native executable using `ro.product.cpu.abilist`, **not** a phone model or GPU vendor. In automatic mode it chooses ARM64 when declared available, otherwise ARMv7. An explicitly required unsupported ABI fails before staging a binary. Pixel reports only ARM64; RedMagic reports both, and rejected/unavailable combinations never trigger a silent fallback.

On real RedMagic Vulkan hardware, all **six ARMv7 runs** passed: default Vulkan 1.0, explicit 1.0 and explicit 1.1, each with `-O0` and `-O`. Every run verified 2,048 public integer output words with checksum `00d6d6e4837edebc`. The two ARMv7 forced noncoherent trials correctly rejected unavailable HOST_VISIBLE types, while the ordinary coherent staging path succeeded.

A separate ARMv7 Vulkan 1.0 **buffer-only heap survey** allocated/bound four 8-KiB buffers and reported the following actual driver values, independently of the GPU pipeline:

| Same RedMagic GPU, different process ABI | Reported device-local heap 0 |
| --- | ---: |
| ARM64 Vulkan process | **11,584,221,184 bytes** |
| ARMv7 Vulkan process | **4,294,967,296 bytes** |
| Both process ABIs, heap 1 | 268,435,456 bytes |

The memory-type flags and the four tested buffer compatibility masks were the same across these two processes. The declared heap capacity was **not**: the ARMv7 Vulkan implementation reports only 4 GiB for heap zero. The allocation planner uses each current process's **actual** `VkPhysicalDeviceMemoryProperties` and never transfers heap capacities from another ABI.

This demonstrates native 32-bit and 64-bit executable compatibility on a newer Android device. **An Android 7/API 24 phone and a physical Vulkan-1.0-only driver have not yet been tested**, so runtime correctness on those older devices is not claimed.

## Reproducible build and GPU tests

From the Windows checkout in `D:\PROJECTS\HASHCAT`:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/build-vulkan-core-baseline.ps1 -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865 -HostGcc C:\msys64\mingw64\bin\gcc.exe
```

This builds a standalone Android ARM64 Vulkan program, two Vulkan 1.0 SPIR-V modules and two Windows C policy tests (`-O0`/`-O2`). Native Windows tests use synthetic loader/device responses and do **not** link Vulkan/OpenCL; their PE imports contain only `KERNEL32.dll` and `msvcrt.dll`.

For any device with Vulkan 1.0 Compute, run only the **default** baseline. It does not attempt to require Vulkan 1.1:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/run-vulkan-core-baseline.ps1 -Serial $DeviceAdbSerial -ReportPrefix device
```

On a device **already known** to support Vulkan 1.1, add the optional compatibility test:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/run-vulkan-core-baseline.ps1 -Serial $DeviceAdbSerial -ReportPrefix device -TestCore11
```

To additionally test real noncoherent memory without a GPU vendor profile, run the optional memory-type trial. An unsupported staging-memory type is recorded as an explicit incompatibility rather than a successful GPU dispatch:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/run-vulkan-core-baseline.ps1 -Serial $DeviceAdbSerial -ReportPrefix device -TestNoncoherentStaging
```

The runner automatically chooses an ABI from Android's reported `ro.product.cpu.abilist`. To require ARMv7 explicitly on a device supporting it, run `tools/run-vulkan-core-baseline.ps1 -Serial $DeviceAdbSerial -ReportPrefix armv7 -Abi armeabi-v7a`. The same script accepts `-Abi arm64-v8a` or `-Abi auto`, and rejects unsupported explicit ABI requests. The runner always executes the default and explicit Vulkan 1.0 paths at both shader optimization levels. With `-TestCore11` it additionally runs the 1.1-required path. With `-TestNoncoherentStaging`, it explicitly requests a real noncoherent memory type for **both staging buffers** using Vulkan 1.0; the result is either an exact GPU/cached-memory verification or a clearly reported `VK_ERROR_FEATURE_NOT_PRESENT` when no compatible type exists. It does not silently substitute coherent memory. All runs verify API versions, memory compatibility and GPU results. Temporary files under `/data/local/tmp` are removed in `finally`, and only LF-normalized logs are kept locally.

Offline reproducibility and tamper detection:

```powershell
python -B tools/test-vulkan-core-baseline-evidence.py --artifacts build/diagnostic-multimode/vulkan-core-baseline-20261031
```

The evidence validator checks source hashes, **seven exact build artifact hashes** (ARM64/ARMv7 Compute, ARMv7 heap survey, native CPU tests and SPIR-V 1.0 modules), two Windows host-policy logs, **18** actual GPU baseline runs, **six** explicit noncoherent trials, **12** invalid-input outcomes and an independent ARMv7 heap survey. Per-buffer compatibility is checked against the actual Vulkan memory type/heap properties **from the same ABI**, not copied from another process architecture. It recomputes the integer shader output checksum in Python and rejects altered GPU checksums, unrequested Vulkan 1.1 instances, incompatible staging types and forged noncoherent driver-cache calls.

The archive is located in `tests/fixtures/android-gpu/vulkan-core-baseline-20261031/`. Source hashes normalize CRLF to LF; committed device logs and local binaries are checked by exact original bytes.

## Three-stage Vulkan 1.0 compute and six-buffer memory planning

The separate `tools/vulkan-core10-multistage-diagnostic.c` tests all three public SPIR-V 1.0 integer stages using **four GPU storage and two CPU staging buffers**, with exact joint memory-type/heap planning across six allocations. It requests Vulkan 1.0 by default, checks Compute limits and storage/shared-memory requirements, and uses real GPU transfer barriers and optional noncoherent cache synchronization.

On ARM64, RedMagic and Pixel passed 14 actual staged three-shader GPU executions in total (six standard per phone and two extra noncoherent executions on Pixel). The two explicit noncoherent RedMagic trials instead returned the exact incompatibility of the host memory type. The independent CPU oracle checked **12,288 logical words** per successful run and all stage hashes match the historical three-dispatch reference. Both ARM64 and ARMv7 executables target Android API 24 and compile strictly; this specific six-buffer program has not been executed on ARMv7 hardware.

Full reproduction and SHA-256 archive: `docs/vulkan-core10-multistage-portability.md` and `tests/fixtures/android-gpu/vulkan-core-multistage-20261031/`.

## Reusable Vulkan 1.0 resource runtime

The standalone `tools/vulkan-core10-runtime.c` owns the six Vulkan buffer allocations, CPU staging maps, descriptor/pipeline objects, command buffer and GPU fence independently of the public synthetic workload. It records the command buffer once and reuses it for four consecutive input generations, resetting the signaled fence between submissions. The same source compiles for Android API 24 ARM64 and ARMv7; the runtime program has passed **real ARMv7 three-stage GPU execution** on RedMagic, as well as ARM64 on RedMagic and Pixel.

The new archived suite contains **20 successful real GPU runs × four sequential submissions**, genuine noncoherent `flush`/`invalidate` on Pixel, **84 actual Vulkan resource-acquisition fault tests** checking partial cleanup, and **15** invalid-command controls. Physical Vulkan-1.0-only/Android-7.0 devices remain unavailable. See `docs/vulkan-core10-runtime-portability.md` and `tests/fixtures/android-gpu/vulkan-runtime-20261031/`. The earlier monolithic six-buffer implementation and its historical evidence are preserved unchanged.

## Remaining integration scope

The project now has a separate **capability-driven Vulkan 1.0 synthetic Compute path**, including real GPU staging, exact memory-heap planning and an optional Vulkan 1.1 API requirement.

This is not an integrated Vulkan GPU backend for hashcat. It does not implement bcrypt, alter the production `m03200-pure.cl` kernel or resolve the archived Qualcomm OpenCL bcrypt mode-3200 self-test failure **245**.
