# Reusable Vulkan 1.0 Compute runtime: resources and sequential commands

Date: 2026-10-31

## Purpose and boundaries

This is an isolated, **noncryptographic** Vulkan 1.0 Compute runtime for a public synthetic workload. It consolidates Vulkan device, memory, descriptor, pipeline, command and fence lifetimes into reusable source files. It does not integrate into hashcat, load a password or implement bcrypt.

The physical GPU is chosen by the existing capability checker, not a device model. The runtime receives exact caller-declared buffer and SPIR-V requirements; no memory type index, queue index, heap size or API upgrade is inferred from Adreno/Mali/PowerVR identity. Vulkan 1.0 is sufficient for every command used. Vulkan 1.1 is optional and enabled only when explicitly requested and supported by both loader and physical device.

Both executables target Android **API 24**, but actual GPU testing used newer installed Android devices. A physical GPU supporting only Vulkan 1.0 and a physical Android API 24 handset were unavailable; neither is claimed as tested.

## Source ownership and interfaces

The public owner is declared in `tools/vulkan-core10-runtime.h` and implemented in `tools/vulkan-core10-runtime.c`.

| Function | Responsibility |
| --- | --- |
| `vulkan_core10_runtime_create` | Create a logical device and queue, six Vulkan buffers, six jointly planned memory allocations, two mapped staging allocations, one four-binding storage descriptor set, three shader modules and Compute pipelines, a command pool/buffer and fence |
| `vulkan_core10_runtime_begin_commands` | Begin recording one Vulkan 1.0 primary command buffer without `VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT` |
| `vulkan_core10_runtime_end_commands` | Finalize the recorded command buffer for sequential reuse |
| `vulkan_core10_runtime_sync_staging` | Invoke `vkFlushMappedMemoryRanges` before GPU work or `vkInvalidateMappedMemoryRanges` after fence completion only when the mapped allocation is noncoherent |
| `vulkan_core10_runtime_submit_and_wait` | Submit the existing command buffer, await GPU completion with a fence, and reset a previously completed fence before the next submission |
| `vulkan_core10_runtime_destroy` | Wait for device idle; destroy fence/pools/pipelines/modules/layouts, unmap CPU memory, destroy buffers, free their Vulkan allocations, destroy device, and clear the owner |

Buffer requirements are declared using `vulkan_core10_runtime_buffer_spec_t`. All six sizes, `VkBufferUsageFlags`, access roles and forbidden memory flags are caller-supplied. The first four buffers use storage descriptors and the last two are CPU-visible transfer buffers. The helper `vulkan_multistage_plan_memory` searches an assignment using the *current Vulkan process's* `memoryTypeBits`, physical memory types/heaps and maximum memory allocation count. The entire `VkMemoryRequirements.size` is allocated for each buffer and each buffer is bound at offset zero. The application never guesses suballocation offsets.

The runtime owns **28 distinct acquired Vulkan resources/handles** in a complete instance:

1. Logical device.
2. Six Vulkan buffer handles.
3. Six device-memory allocations.
4. Two mapped host staging allocations.
5. Descriptor set layout and Compute pipeline layout.
6. Three shader modules and three Compute pipelines.
7. Descriptor pool and descriptor set.
8. Command pool, primary command buffer and fence.

The 28 acquisition points are numbered in precisely that order by the runtime. Shader code and the Vulkan instance remain caller-owned. A failed `vulkan_core10_runtime_create` releases all previously acquired objects. Calling `vulkan_core10_runtime_destroy` again on an empty owner is safe; it has no remaining driver resources to destroy.

The failure API returns real `VkResult` codes. An explicit `--fail-after N` diagnostic option (1–28) injects a failure immediately **after** the Nth genuine Vulkan acquisition succeeds; this is a controlled resource-cleanup test, not simulated driver behavior or an algorithm fallback. Each failure returns `VK_ERROR_INITIALIZATION_FAILED` and logs both the injection point and the objects released. Incompatible forced noncoherent memory returns the actual planner result `VK_ERROR_FEATURE_NOT_PRESENT`. No successful GPU dispatch is claimed for either negative case.

## Synthetic workload and fence lifetime

The caller `tools/vulkan-core10-runtime-diagnostic.c` checks the selected GPU against an exact three-stage public integer workload: local group **32 × 1 × 1**, **64** workgroups, four storage descriptors, **128 bytes** of shared workgroup memory and maximum buffer range **32,768 bytes**.

It records one command buffer containing a CPU-upload transfer, three separate Compute dispatches and a GPU-download transfer. The barriers are Vulkan 1.0 `vkCmdPipelineBarrier` dependencies for host writes, transfer writes, Compute reads/writes and host reads. Stage B uses a 32-lane shader `barrier()`. In each sequential submission the CPU supplies a different generation of 2,048 public integer inputs, flushes mapped memory if required, submits the **same previously recorded** command buffer, waits for the fence, invalidates download memory if required and compares every output word against an independent CPU implementation.

There are six resource buffers:

| Buffer | Requested bytes | Usage |
| --- | ---: | --- |
| GPU input | 8,192 | Storage + transfer destination |
| GPU stage A | 8,192 | Storage |
| GPU stage B | 8,192 | Storage |
| GPU output | 32,768 | Storage + transfer source |
| CPU staging upload | 8,192 | Host-visible transfer source |
| CPU staging download | 32,768 | Host-visible transfer destination |

Stage C's four-word output includes the actual A and B stage states, which are checked independently by CPU. Each successful submission therefore validates **12,288 logical 32-bit words** (2,048 stage-A, 2,048 stage-B and 8,192 final output). Intermediate A/B buffers are **not** copied separately to the CPU; verification uses stage C's embedded copies. This distinction is important when interpreting the result.

The known FNV-1a64 checksums for four sequential public input generations are:

| Submission | Stage A | Stage B | Final output |
| --- | --- | --- | --- |
| 0 | `de56d6d0aa66c6ec` | `c1259ad4f024499a` | `05de70c62f8d7cfd` |
| 1 | `66ee1b4b1385e0e3` | `27fb4f34e64d89a7` | `7509bfa6ed0455d8` |
| 2 | `69b8f65ae2e8be72` | `d8038be5522a0b58` | `e50a539d4459e45b` |
| 3 | `8fb63465a93a5631` | `ac37a5c3b35fc366` | `07d63cfd9773324d` |

The `--submissions` flag accepts an explicit 2–16. The archived real tests use four; the same C CPU verifier computes all generations on demand.

## Real Android results

The evidence was recorded from three actual Android process targets, with both optimized and unoptimized SPIR-V 1.0 shaders:

| Target | ABI | Standard GPU API modes | Forced noncoherent staging | Resource fault points |
| --- | --- | --- | --- | --- |
| RedMagic / Adreno 730 | ARM64 | Default 1.0, explicit 1.0, explicit 1.1: **6 PASS** | Two explicit unsupported-memory results | **28 PASS** |
| RedMagic / Adreno 730 | ARMv7 (32-bit) | Default 1.0, explicit 1.0, explicit 1.1: **6 PASS** | Two explicit unsupported-memory results | **28 PASS** |
| Pixel / Mali-G715 | ARM64 | Default 1.0, explicit 1.0, explicit 1.1: **6 PASS** | **2 PASS** with actual noncoherent flush and invalidate | **28 PASS** |

In total:

- **20 successful real GPU runs**, each with four sequential command submissions: 80 successful GPU submissions, 240 individual Compute dispatches and 983,040 logical checked 32-bit values.
- **Four explicit unsupported-memory tests** on RedMagic, returning `VK_ERROR_FEATURE_NOT_PRESENT` without silently selecting a coherent type.
- **84 real resource-acquisition fault trials**, spanning all 28 points on each of the three Android/ABI targets. Every trial logged exact partial resource counts and `vkDeviceWaitIdle == VK_SUCCESS`; none submitted GPU work.
- **15 malformed command-line tests** (five per actual process ABI), all rejected with exit code 2.
- Two independent native C memory-planner test runs (`-O0` / `-O2`) exercising heap backtracking, noncoherent candidate selection and exact errors.

Pixel used noncoherent `HOST_VISIBLE` staging type 1, reported `nonCoherentAtomSize = 64`, and executed actual Vulkan driver `vkFlushMappedMemoryRanges` and `vkInvalidateMappedMemoryRanges` around every submission. Both RedMagic process ABIs selected their compatible coherent staging type 6 by default, and rejected the explicitly forced noncoherent staging requirement.

The memory heaps are ABI-sensitive even on the same phone: RedMagic's **ARM64** Vulkan process reports **11,584,221,184 bytes** of heap zero, while its **ARMv7** process reports **4,294,967,296 bytes**. Both executed correctly because resource planning queries their actual process-local Vulkan properties.

Runtime cleanup logs are application-level evidence that every acquired handle was released. They do **not** establish absence of driver-internal leaks through an external Vulkan validation layer, which was not enabled.

## Build and run

Build two Android API 24 runtime binaries, six SPIR-V 1.0 shader binaries and native allocation-policy regression executables:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/build-vulkan-core10-runtime.ps1 -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865 -HostGcc C:\msys64\mingw64\bin\gcc.exe
```

Run the default Vulkan 1.0 Compute path with automatic ABI selection:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/run-vulkan-core10-runtime.ps1 -Serial $DeviceAdbSerial -ReportPrefix device
```

For an API-1.1-capable device, optionally include explicit Vulkan 1.1, real noncoherent staging and all 28 creation-failure points:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/run-vulkan-core10-runtime.ps1 -Serial $DeviceAdbSerial -ReportPrefix device -TestCore11 -TestNoncoherent -TestFaults
```

To require a particular reported Android ABI, use `-Abi arm64-v8a` or `-Abi armeabi-v7a`. An unsupported explicit ABI is rejected before uploading any files. Every runner invocation deletes its temporary Vulkan test artifacts from `/data/local/tmp` in a `finally` block.

The archive `tests/fixtures/android-gpu/vulkan-runtime-20261031/` contains exact LF-only original device/CPU logs and a SHA-256 manifest. Offline verification recomputes the public integer CPU oracle, checks resource counts and real ABI-specific Vulkan memory types/heaps, validates fault-injection cleanup and detects deliberately corrupted API, memory, synchronization, fence and checksum records:

```powershell
python -B tools/test-vulkan-core10-runtime-evidence.py --artifacts build/diagnostic-multimode/vulkan-runtime-20261031
```

The optional `--artifacts` argument verifies ten exact Android/host/SPIR-V build hashes; without it the original logs and source identities are still audited. `tools/archive-vulkan-core10-runtime-evidence.py` collects the finished local evidence without executing GPU programs or altering the original log bytes.

## Dynamic-count Vulkan Compute graph

A separate `tools/vulkan-core10-graph.c` resource owner and `tools/vulkan-core10-graph-memory.c` joint memory planner now accept explicit nonzero buffer, STORAGE_BUFFER descriptor and Compute pipeline counts, instead of allocating fixed arrays for one three-stage workload. The independent diagnostic executes the public integer shaders as one-, two- and three-stage graphs requiring four, five and six buffers, with two, three and four descriptors. A native CPU-only planner test additionally validates seven-buffer placement and heap backtracking.

On actual RedMagic ARM64, RedMagic ARMv7 and Pixel ARM64 processes, **60 real GPU runs** (240 sequential submissions) passed exact CPU output verification. Pixel exercised actual noncoherent flush/invalidate for all three graph sizes. Another **216 post-acquisition fault-injection cases** checked cleanup at every one of the 20, 24 and 28 owned-resource states per graph configuration. Original source/binary/device SHA-256 records and limitations are in `docs/vulkan-core10-dynamic-graph-portability.md` and `tests/fixtures/android-gpu/vulkan-dynamic-graph-20261031/`. This remains synthetic Vulkan infrastructure, without hashcat GPU algorithm integration.

## Follow-up: mixed storage/uniform multiple descriptor sets

The independent `tools/vulkan-core10-multiset.c` runtime manages a variable number of `VkDescriptorSetLayout` and `VkDescriptorSet` handles with explicit `VkPipelineLayout.pSetLayouts`, validates both `STORAGE_BUFFER` and `UNIFORM_BUFFER` descriptors against actual physical GPU limits, and supports CPU-updated uniform data on coherent and noncoherent HOST_VISIBLE memory. On real RedMagic ARM64, RedMagic ARMv7 and Pixel ARM64, one- and two-set public shader layouts completed **40 GPU runs with 160 independently checked submissions**, 144 genuine post-acquisition fault cleanup trials and 27 malformed input rejections. All changes remain separate from existing runtime evidence. Detailed reproduction and limitations: `docs/vulkan-core10-multiset-portability.md`.

## Not established

This is a fixed six-buffer/four-descriptor/three-pipeline resource owner for the current tested synthetic workload, not yet a runtime supporting arbitrary shader descriptor layouts or variable buffer counts. A future runtime must have explicit workload ABI contracts rather than inferred shader requirements.

Nothing here demonstrates real bcrypt correctness, OpenCL compatibility or successful integration into hashcat. The historical mode-3200 OpenCL self-test error **245** remains unresolved.
