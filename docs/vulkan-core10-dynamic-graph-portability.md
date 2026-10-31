# Device-capability-driven Vulkan 1.0 dynamic Compute graphs

Date: 2026-10-31

## Scope

This is an independently tested, **noncryptographic** Vulkan Compute resource infrastructure module. It accepts explicit shader/buffer/descriptor requirements, selects Vulkan memory by the current physical device's real capabilities, executes synthetic integer shaders and releases every acquired Vulkan object. It is neither a password-processing GPU algorithm nor a hashcat backend.

The default requested API version is **Vulkan 1.0**. Vulkan 1.1 is required only through an explicit diagnostic argument and only after both loader and physical GPU capability checks. All three public compute shaders compile as **SPIR-V 1.0** and validate for both Vulkan 1.0 and 1.1.

Two Android API **24** executables are built: `aarch64-linux-android24` and `armv7a-linux-androideabi24`. Real GPU tests ran successfully on both ABI process types, but no physical Vulkan-1.0-only or Android 7.0 handset was available. The GPU versions reported by the tested devices are later than Vulkan 1.0; the executable explicitly requests the Vulkan 1.0 instance/device API.

## Runtime ownership and configuration contract

The standalone general-purpose resource owner is `tools/vulkan-core10-graph.h` and `tools/vulkan-core10-graph.c`. Its key public interface is `vulkan_core10_graph_create`, which accepts the exact counts and arrays for:

- `vulkan_core10_graph_buffer_spec_t`: buffer label, byte size, usage flags, memory role, forbidden flags and requested CPU mapping.
- `vulkan_core10_graph_descriptor_spec_t`: descriptor binding number, buffer index, byte offset and byte range. Nonconsecutive bindings are permitted; duplicate bindings are rejected.
- `vulkan_core10_graph_shader_spec_t`: caller-owned SPIR-V 1.0 words and byte size, one module per Compute pipeline.

**These are independent nonzero counts.** The graph implementation allocates the Vulkan-buffer, shader-module and pipeline arrays dynamically, and the descriptor binding/update arrays are sized from the caller's actual contract. It has no hardcoded six-buffer/three-shader runtime allocation limit, no GPU-manufacturer-specific memory type and no guessed Vulkan queue family.

Each runtime instance currently owns **one logical device, one queue, one storage-buffer descriptor set, one primary command buffer and one GPU fence**. It supports any number of declared storage-buffer descriptor bindings and Compute pipelines that fit the actual Vulkan device limits. These are explicit structural capabilities, not shader reflection: the caller remains responsible for declaring the actual shader binding layout and verifying its workgroup/dispatch topology with the capability checker. Non-storage descriptor types, multiple descriptor sets and concurrently submitted command buffers are **not** implemented.

The runtime validates every descriptor's buffer index, STORAGE_BUFFER usage, offset and range, duplicate binding number, `maxStorageBufferRange`, `minStorageBufferOffsetAlignment`, `maxDescriptorSetStorageBuffers`, `maxPerStageDescriptorStorageBuffers` and `maxPerStageResources`. Conservatively, the descriptor count must fit every named per-stage descriptor limit even when individual shaders use only a subset. Buffer requirements and mapping must also be explicit; the runtime does not silently change a buffer size, descriptor range, stage count or algorithm when a limit is exceeded.

### Shared memory planner

`tools/vulkan-core10-graph-memory.h` and `tools/vulkan-core10-graph-memory.c` provide a **dynamic-count joint memory planner**, independent of the older fixed four- and six-buffer experimental planners. It obtains each buffer's actual `VkMemoryRequirements.size`, `alignment` and `memoryTypeBits`, uses the device's `VkPhysicalDeviceMemoryProperties`, `VkMemoryType.heapIndex`, `VkMemoryHeap.size` and `maxMemoryAllocationCount`, and reserves exact allocation sizes against each declared heap.

GPU_STORAGE prefers DEVICE_LOCAL. CPU-mapped HOST_TRANSFER requires HOST_VISIBLE and prefers HOST_COHERENT, but accepts compatible noncoherent memory with required driver cache synchronization. Explicit forbidden flags always win over preference. Exhaustive backtracking tries the next compatible Vulkan memory type when earlier preferred choices prevent joint placement. It returns:

| Vulkan result | Condition |
| --- | --- |
| `VK_ERROR_INITIALIZATION_FAILED` | Null/inconsistent input, invalid buffer requirements, heap index or descriptor binding |
| `VK_ERROR_TOO_MANY_OBJECTS` | Required allocation count exceeds remaining device allocation slots |
| `VK_ERROR_FEATURE_NOT_PRESENT` | No memory type satisfies the exact required role, flags or compatibility bitmask, or device descriptor limits are insufficient |
| `VK_ERROR_OUT_OF_DEVICE_MEMORY` | No joint placement fits declared heap sizes |
| `VK_ERROR_OUT_OF_HOST_MEMORY` | Cannot allocate host arrays for a dynamic graph or plan |

A successful placement checks declared heap *capacity*, not the currently available heap budget. The actual `vkAllocateMemory` operation can return its own error, which is propagated without automatic workload reduction.

All buffers are individually allocated with full `VkMemoryRequirements.size` and bound at offset zero. This variant does not implement shared-`VkDeviceMemory` suballocation. The existing separate Vulkan suballocation diagnostic remains available as prior independent evidence.

## Runtime execution and fault cleanup

The resource owner exposes:

| Method | Current action |
| --- | --- |
| `vulkan_core10_graph_create` | Validate exact graph contract, select all memory allocations jointly, create device/buffers/maps/layouts/pipelines/descriptor set/command pool/command buffer/fence |
| `vulkan_core10_graph_begin` / `vulkan_core10_graph_end` | Record the caller's explicit Vulkan 1.0 commands into a single reusable primary command buffer |
| `vulkan_core10_graph_sync` | Perform full-allocation `vkFlushMappedMemoryRanges` before GPU input consumption or `vkInvalidateMappedMemoryRanges` after fence completion for noncoherent CPU memory |
| `vulkan_core10_graph_submit_and_wait` | Submit, wait for the GPU fence, reset the completed fence before a subsequent sequential submit |
| `vulkan_core10_graph_destroy` | Wait for device idle; destroy fence, pools, pipelines and descriptors; unmap CPU staging; destroy buffers and free all GPU allocations and dynamically allocated arrays |

A nonzero `fail_after` explicitly injects a failure **after** the given one-based resource acquisition succeeds. It returns `VK_ERROR_INITIALIZATION_FAILED`, prints the exact acquired resource and runs complete cleanup. This exercises partial resource ownership, not actual forced GPU device loss or a driver-internal allocation failure. Actual device-loss recovery and validation-layer driver leak detection remain untested.

## Actual tested GPU workload graph sizes

The new application `tools/vulkan-core10-graph-diagnostic.c` uses the original three public noncryptographic GLSL integer shaders, but deliberately creates three different **exact shader interfaces**:

| Selected stages | GPU buffers | CPU staging | Total buffers | Storage descriptors | Compute pipelines | Acquisition points |
| --- | --- | --- | --- | --- | --- | --- |
| Stage A only | Input + A | Upload + download | **4** | **2** | **1** | **20** |
| Stages A → B | Input + A + B | Upload + download | **5** | **3** | **2** | **24** |
| Stages A → B → C | Input + A + B + final | Upload + download | **6** | **4** | **3** | **28** |

All three stages use the exact local size 32 × 1 × 1 and dispatch 64 groups. Stage B's workgroup-shared array occupies 128 bytes and uses the shader's workgroup synchronization. The application checks these requirements using the actual GPU limits before requesting runtime resources.

For each graph, one Vulkan 1.0 command buffer includes precisely two staging transfers and explicit HOST → TRANSFER → COMPUTE → TRANSFER → HOST dependencies. The recorded command buffer is resubmitted four times with different public inputs after fence completion and correct flush/invalidate operations. The CPU recomputes every output word.

The stage-count-specific verification is deliberately precise:

- **One stage:** 2,048 directly downloaded stage-A words per GPU submission.
- **Two stages:** 2,048 directly downloaded stage-B words per GPU submission. Intermediate A is consumed by B and is not downloaded separately.
- **Three stages:** 8,192 directly downloaded final stage-C words plus 2,048 stage-A and 2,048 stage-B values embedded in stage-C's output, yielding **12,288 logical checks** per submission. The A/B intermediates are not mapped independently.

The CPU checksums for each input generation are independently calculated by `tools/test-vulkan-core10-graph-evidence.py`. All 1/2/3-stage runs match the original public reference:

| Submission | Stage A | Stage B | Stage C |
| --- | --- | --- | --- |
| 0 | `de56d6d0aa66c6ec` | `c1259ad4f024499a` | `05de70c62f8d7cfd` |
| 1 | `66ee1b4b1385e0e3` | `27fb4f34e64d89a7` | `7509bfa6ed0455d8` |
| 2 | `69b8f65ae2e8be72` | `d8038be5522a0b58` | `e50a539d4459e45b` |
| 3 | `8fb63465a93a5631` | `ac37a5c3b35fc366` | `07d63cfd9773324d` |

A checksum field printed for an unexecuted stage is the FNV-1a initial value `cbf29ce484222325`; it does **not** indicate that the absent stage ran on the GPU.

## Hardware results

The following tests ran on **real hardware**, with both optimized and unoptimized SPIR-V 1.0 and the same portable Android API24 executables:

| Device / process ABI | 1-, 2-, 3-stage GPU runs | Noncoherent staging | Real Vulkan resource acquisition cleanup |
| --- | --- | --- | --- |
| RedMagic / Adreno 730 / ARM64 | 18 PASS (default 1.0 / explicit 1.0 / optional 1.1) | 6 exact incompatible-memory rejections | 20 + 24 + 28 = 72 PASS |
| RedMagic / Adreno 730 / ARMv7 | 18 PASS (same configurations) | 6 exact incompatible-memory rejections | 72 PASS |
| Pixel / Mali-G715 / ARM64 | 18 PASS (same configurations) | 6 PASS with genuine noncoherent flush/invalidate | 72 PASS |

Combined verified observations:

- **60 successful GPU test runs**, each with four sequential submissions: **240 Vulkan queue submissions** and **480 synthetic Compute dispatches**.
- **1,310,720 logical 32-bit CPU-verified values** across the 60 GPU runs. This counts the embedded A/B channels in three-stage C output separately, as documented.
- **12 precise incompatible noncoherent staging rejections** (two RedMagic process ABIs × three sizes × two shader optimizations). No automatic switch to coherent memory was made for a forced noncoherent request.
- **216 genuine Vulkan post-acquisition fault-injection tests** across the three process/ABI targets; acquired objects were released in the exact observed partial graph states and no GPU submissions occurred in fault trials.
- **27 malformed CLI controls** were rejected with exit 2.
- **Two native C memory planner test executions** (`-O0` and `-O2`) additionally verified exact 4-, 5-, 6- and **7-buffer** placement, heap-capacity backtracking, coherent/noncoherent selection, and detailed errors without a GPU driver.

On Pixel, Vulkan reports 64-byte `nonCoherentAtomSize`; actual `vkFlushMappedMemoryRanges` and `vkInvalidateMappedMemoryRanges` calls pass on all three graph sizes. RedMagic's constrained staging buffer masks expose no compatible noncoherent HOST_VISIBLE candidate, so its six forced noncoherent trials per ABI correctly reject the request.

The same RedMagic GPU reports **11,584,221,184 bytes** for its primary heap to ARM64, but **4,294,967,296 bytes** to the ARMv7 process. Every dynamic graph uses the actual current-process heap properties, without an architecture/model lookup table.

## Reproduction and offline checks

Build ARM64 and ARMv7 Android 24 executables, six Vulkan 1.0 shader binaries, and two strict native C memory planner tests:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/build-vulkan-core10-graph.ps1 -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865 -HostGcc C:\msys64\mingw64\bin\gcc.exe
```

Run all stage counts on a connected Android phone, choosing the executable from `ro.product.cpu.abilist`:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/run-vulkan-core10-graph.ps1 -Serial $DeviceAdbSerial -ReportPrefix device
```

Optional Vulkan 1.1, noncoherent memory checks and all 72 fault points:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/run-vulkan-core10-graph.ps1 -Serial $DeviceAdbSerial -ReportPrefix device -TestCore11 -TestNoncoherent -TestFaults
```

The explicit `-Abi arm64-v8a` or `-Abi armeabi-v7a` option rejects unsupported device ABIs rather than trying the wrong executable. The runner always removes its staged files from Android in a `finally` block.

Original device/CPU observations are in `tests/fixtures/android-gpu/vulkan-dynamic-graph-20261031/`: **80** LF-only logs and SHA-256 manifest. The offline verifier recalculates every public stage reference, checks source/binary SHA-256, exact Vulkan memory-type masks and heap reservations by ABI, all 72 faulty acquisition cleanup points per target and deliberately corrupted log rejections:

```powershell
python -B tools/test-vulkan-core10-graph-evidence.py --artifacts build/diagnostic-multimode/vulkan-dynamic-graph-20261031
```

Without `--artifacts`, the verifier checks the archived real-device and native-CPU evidence and source file identities without requiring an Android device, NDK or compiled executable. `tools/archive-vulkan-core10-graph-evidence.py` records exact original local observations without rerunning any Vulkan program.

## Integration boundaries

The older six-buffer resource runtime and its SHA-256-certified logs remain unchanged; they serve as independent regression evidence. The dynamic graph runtime extends **resource infrastructure**, not hashcat GPU algorithms.

The real phones can test a Vulkan 1.0 *requested API*, but cannot prove operation on a physically Vulkan-1.0-only GPU or Android 7.0 device. No simultaneous multi-queue submission, device-loss recovery, non-storage descriptors, shader-interface reflection or full driver leak analysis is claimed.

The historical hashcat OpenCL bcrypt mode-3200 self-test result **245** is not resolved by these synthetic Compute experiments. No bcrypt kernel, hashcat backend or password-recovery functionality was implemented or modified.
