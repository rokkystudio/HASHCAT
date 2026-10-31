# Vulkan 1.0 memory heap portability and four-buffer allocation planning (2026-10-31)

## Purpose

A portable Vulkan allocation path must validate more than `memoryTypeBits` and `HOST_VISIBLE`: each returned Vulkan memory type belongs to one **driver-declared memory heap** through `VkMemoryType.heapIndex`, each allocation requests its entire `VkMemoryRequirements.size`, and `VkPhysicalDeviceLimits.maxMemoryAllocationCount` limits the number of simultaneous `VkDeviceMemory` objects.

A one-buffer-at-a-time choice may be individually compatible yet prevent another buffer from fitting into any heap. The reusable module `tools/vulkan-memory-heap-policy.h` and `tools/vulkan-memory-heap-policy.c` searches for a feasible **joint assignment** for the four independent synthetic Vulkan allocations used by the staging diagnostic. It uses no GPU vendor ID, driver version, Android model or estimated amount of free memory.

This module is **standalone Vulkan infrastructure**, not a production hashcat backend or an implementation of any password-processing shader.

The model follows Vulkan's official [memory requirements](https://docs.vulkan.org/refpages/latest/refpages/source/VkMemoryRequirements.html), [memory-type heap indices](https://docs.vulkan.org/refpages/latest/refpages/source/VkMemoryType.html) and [memory allocation valid-usage requirements](https://docs.vulkan.org/refpages/latest/refpages/source/vkAllocateMemory.html).

## Exact selection rules

The caller provides an array of one to four `vulkan_heap_allocation_request_t` entries. Each contains the actual `VkMemoryRequirements`, a diagnostic label, one exact role and any property flags forbidden by features not enabled in the Vulkan logical device.

- **`VULKAN_HEAP_BUFFER_GPU_STORAGE`:** all allowed types in `memoryTypeBits` are eligible. Types with `DEVICE_LOCAL` are tried first, then remaining types.
- **`VULKAN_HEAP_BUFFER_HOST_TRANSFER`:** types must have `HOST_VISIBLE`; compatible `HOST_COHERENT` types are tried first, followed by noncoherent types.
- **Both roles:** candidates are ordered by declared role, then ascending Vulkan memory-type index. Every type's `heapIndex` must reference a valid heap. Every allocation's full `VkMemoryRequirements.size` is counted against that heap's declared size.
- **Allocation handles:** `live_allocation_count + requested_buffer_count` must fit within the actual device's `maxMemoryAllocationCount`; this is checked without integer overflow.
- **Placement:** if an earlier chosen type leaves insufficient heap capacity for a later buffer, recursive backtracking examines other compatible types for that earlier buffer. No request is silently resized or skipped.
- **Failure:** structurally invalid input reports `VK_ERROR_INITIALIZATION_FAILED`; no compatible type reports `VK_ERROR_FEATURE_NOT_PRESENT`; no heap-capacity-valid combination reports `VK_ERROR_OUT_OF_DEVICE_MEMORY`; exceeding the specified allocation count reports `VK_ERROR_TOO_MANY_OBJECTS`. Failed searches leave the result structure untouched.

For the Vulkan **1.0-only** probe the caller explicitly forbids protected and AMD device-coherent memory types, because those optional features have not been enabled in the logical device.

The planner accepts up to **four allocations** because this standalone experimental path contains two compute buffers and two staging buffers. Larger workloads need a separately designed allocator, not an implicit or arbitrary change in this module's bound.

### Important distinction: declared size versus actual free memory

`VkMemoryHeap.size` expresses the heap's declared capacity; it is **not** its real-time free memory. A feasible plan must still be followed by actual `vkAllocateMemory` calls with checked results. Memory pressure from other processes can cause allocation failures even when a plan fits. The optional `VK_EXT_memory_budget` interface could supply additional context on supporting devices, but is neither required by Vulkan 1.0 nor used to guess success here.

## Independent native C and Python feasibility checks

The native test `tools/test-vulkan-memory-heap-policy-host.c` checks six groups: backtracking, exact four-buffer staging, noncoherent host staging fallback, invalid inputs, high memory-type indices/64-bit heap sizes, and an exhaustive two-buffer matrix.

The exhaustive matrix uses **three synthetic memory types**, four valid memory-property classes (`DEVICE_LOCAL`, noncoherent `HOST_VISIBLE|HOST_CACHED`, coherent `HOST_VISIBLE|HOST_COHERENT`, combined `DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT`), two synthetic heaps, all type-to-heap assignments, all seven nonzero compatible-bit masks per buffer, both GPU/CPU memory roles per buffer and two exact request-size patterns.

| Outcome verified by independent brute-force oracle | Count |
| --- | ---: |
| Joint memory placement exists | 78,138 |
| One or both requests have no type satisfying their role | 24,816 |
| Compatible types exist but no declared-heap placement fits | 97,750 |
| **Total exact cases** | **200,704** |

Native MSYS2 GCC 16.2.0 `-O0` and `-O2` produced identical decisions. An independent **Python** enumerator in `tools/test-vulkan-memory-heap-evidence.py` reproduced all three counts without using the C search function.

Additional native C controls verify type index **31**, `UINT64_MAX` heap arithmetic, invalid alignment, invalid `heapIndex`, insufficient `maxMemoryAllocationCount` and the unchanged caller result after failure.

The host binaries import only `KERNEL32.dll` and `msvcrt.dll`, not a Vulkan driver. Native policy tests do not submit work to the GPU.

## Actual Vulkan 1.0 buffer-allocation trials

The separate `tools/vulkan-memory-heap-probe.c` executable uses Vulkan 1.0 entry points on Android. It:

1. Selects a compute-capable physical device through the existing capability checker.
2. Queries `VkPhysicalDeviceMemoryProperties`, every memory type and heap, and `maxMemoryAllocationCount`.
3. Creates four **8192-byte** Vulkan buffers with their actual storage/transfer usage flags.
4. Fetches each buffer's genuine `vkGetBufferMemoryRequirements` and obtains one jointly feasible heap plan.
5. Calls `vkAllocateMemory` and `vkBindBufferMemory` separately for all four buffers.
6. Maps the two `HOST_VISIBLE` staging allocations and writes a deterministic public byte pattern; flushes the whole allocation when noncoherent; unmaps and destroys all Vulkan resources.

The probe creates **no shader module, pipeline or command buffer and submits zero GPU commands**. It does not open hashcat or process credentials.

| Driver-reported property | RedMagic / Adreno 730 | Pixel 9 Pro XL / Mali-G715 |
| --- | ---: | ---: |
| Memory type count | 9 | 4 |
| Heap count | 2 | 2 |
| `maxMemoryAllocationCount` | 4,096 | 16,384 |
| Heap 0 declared size | 11,584,221,184 B | 16,101,965,824 B |
| Heap 1 declared size | 268,435,456 B | 104,857,600 B |
| Compute buffer selected memory type | 0 | 0 |
| CPU staging selected memory type | 6 | 0 |
| All four allocations | **PASS** | **PASS** |
| Both staging mappings | **PASS** | **PASS** |
| Total allocation requested | 32,768 B | 32,768 B |
| GPU dispatches | 0 | 0 |

Both physical devices placed all four buffer allocations in declared heap zero. The actual requirements were 8192 bytes at 64-byte alignment for each buffer, but the compatible type masks differed:

| Actual role | Adreno `memoryTypeBits` | Mali `memoryTypeBits` |
| --- | --- | --- |
| GPU input | `0x00000049` | `0x00000003` |
| GPU output | `0x00000041` | `0x00000003` |
| Staging upload | `0x00000041` | `0x00000003` |
| Staging download | `0x00000049` | `0x00000003` |

The different choices illustrate why neither `memoryTypeIndex = 0` nor `memoryTypeIndex = 6` should be prescribed globally. The code derives them only from the real per-buffer compatible masks and actual memory properties.

In **this original allocation-only probe**, host staging memory was coherent on both devices and no Compute command was submitted. A separate Vulkan 1.0 Compute diagnostic subsequently performed a real noncoherent staging upload/download on Pixel using compatible memory type 1, including successful driver `vkFlushMappedMemoryRanges` and `vkInvalidateMappedMemoryRanges` calls and 2,048 correct GPU outputs. RedMagic explicitly rejected noncoherent staging upload because its `memoryTypeBits` contained no compatible HOST_VISIBLE noncoherent type. See `docs/vulkan-core10-portability.md` and the four ARM64 explicit-mode logs in `tests/fixtures/android-gpu/vulkan-core-baseline-20261031/`. A subsequent real ARMv7 Vulkan 1.0 memory survey on RedMagic reported **4 GiB** for heap 0, whereas the ARM64 survey reported **11,584,221,184 bytes** for the same GPU. The Vulkan planner therefore always uses the heap properties of the *current process ABI*. The standalone API24 diagnostic has successful ARM64 and ARMv7 compute runs, with an independently archived ARMv7 heap survey.

## Build and test from Windows

The build tests native C in both optimization modes, checks that the Windows executables do not import Vulkan/OpenCL, and produces the Android ARM64 buffer-only executable:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/build-vulkan-memory-heap-policy.ps1 -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865 -HostGcc C:\msys64\mingw64\bin\gcc.exe

python -B tools/test-vulkan-memory-heap-evidence.py --artifacts build/diagnostic-multimode/vulkan-heap-policy-20261031
```

To repeat the Android allocation trial on an explicitly selected connected test phone:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/run-vulkan-memory-heap-probe.ps1 -Serial $DeviceAdbSerial -ReportPrefix device
```

The runner verifies the actual compatibility bits, all returned type indices and heap indices, heap-size arithmetic, four successful allocations, two host mappings and zero GPU dispatches. It rejects unexpected CLI input, writes local evidence and deletes its temporary Android binary in `finally`.

The archive `tests/fixtures/android-gpu/vulkan-heap-20261031/` contains two native test logs (`-O0`/`-O2`), two real Android allocation logs, two invalid-input logs and an exact SHA-256 manifest. The independent Python validator detects corrupted heap totals and incompatible staging-type assignments.

The manifest hashes UTF-8 source with Windows CRLF normalized to LF; archived real-device logs and executable artifacts use their exact original bytes. The existing `tools/vulkan-auto-config.c` and `tools/vulkan-auto-config.h` retain their earlier archived SHA-256.

## Compatibility boundaries

This verifies memory-type, heap and allocation-count **planning** plus four real small allocation/binding operations. It does not estimate live free memory, guarantee successful allocations under changing memory pressure, perform GPU compute, validate a third phone's Vulkan driver or integrate a Vulkan hashcat backend.

The original OpenCL bcrypt mode-3200 self-test failure **245** on RedMagic remains separate and unresolved.
