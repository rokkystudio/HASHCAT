# Vulkan 1.0: multiple descriptor sets and mixed storage/uniform buffers

Date: 2026-10-31

## Purpose

This stage implements a standalone, **noncryptographic** Vulkan 1.0 Compute resource owner with a variable number of numbered descriptor sets, independent descriptor binding positions, and two explicitly supported descriptor types: `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` and `VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER`.

The implementation uses neither GPU manufacturer names nor phone-specific memory type indices. Every allocation, bound descriptor and shader limit is checked against the *current Vulkan physical device* and its ABI-specific memory properties. The application supplies the complete shader interface; the runtime never infers or silently substitutes an algorithm, descriptor type or buffer range.

The previous dynamic single-set graph, all existing public three-stage shader binaries and their SHA-256-certified hardware logs remain unchanged. This module is an independent superset experiment for descriptor layouts rather than an integration into hashcat or its cryptographic kernels.

## Public source and exact APIs

| File | Responsibility |
| --- | --- |
| `tools/vulkan-core10-descriptor-policy.h` / `.c` | Validate explicit `set`, `binding`, `type`, `buffer_index`, offset/range and exact physical device descriptor limits without a GPU driver |
| `tools/vulkan-core10-multiset.h` / `.c` | Own the VkDevice, arbitrary declared buffer/descriptor-set/shader counts, VkDescriptorPool, one `VkPipelineLayout`, all `VkDescriptorSetLayout`/`VkDescriptorSet` objects, Compute pipelines, CPU mappings, command buffer and fence |
| `tools/vulkan-core10-multiset-diagnostic.c` | Execute and independently CPU-verify an explicitly known public integer shader through one or two sparse-binding descriptor set layouts |
| `tools/vulkan-multiset-one.comp` | Public SPIR-V 1.0 shader consuming two storage descriptors and one std140 uniform descriptor in set 0 |
| `tools/vulkan-multiset-two.comp` | Same public computation using one storage descriptor in set 0 and storage/uniform descriptors in set 1 |
| `tools/test-vulkan-core10-descriptor-policy-host.c` | Strict driver-free C validation of real Vulkan 1.0 descriptor rules and rejected input requirements |

The resource owner exposes these documented methods:

- `vulkan_core10_multiset_create` constructs an explicit graph from `buffer_count`, `binding_count`, `set_count` and `stage_count`. It validates every buffer requirement and descriptor type, creates each numbered set layout and descriptor set, updates `VkDescriptorBufferInfo` for all bindings and creates a compatible pipeline layout and shader pipelines.
- `vulkan_core10_multiset_begin` and `vulkan_core10_multiset_end` record exactly one reusable Vulkan 1.0 primary command buffer. The workload—not the resource owner—specifies dispatches, copies and barriers.
- `vulkan_core10_multiset_submit_and_wait` submits that command buffer, waits for its fence, and resets the completed fence before subsequent submissions.
- `vulkan_core10_multiset_sync` flushes full mapped noncoherent allocations before CPU→GPU transfers/uniform reads and invalidates noncoherent GPU→CPU readback after the GPU fence. Coherent memory needs no explicit driver cache operation.
- `vulkan_core10_multiset_destroy` waits for device idle, destroys every owned pipeline/layout/pool, unmaps CPU memory, destroys buffers and allocations, and frees all dynamic arrays even when resource creation stops partway through.
- `vulkan_core10_descriptor_validate` validates the complete known descriptor layout before any Vulkan object is created. It returns `VK_ERROR_INITIALIZATION_FAILED` for malformed slots and `VK_ERROR_FEATURE_NOT_PRESENT` for unsupported requested Vulkan capabilities.

The runtime creates precisely the requested number of descriptor set layouts in `VkPipelineLayoutCreateInfo.pSetLayouts`, with set indices 0..N−1. It allocates each descriptor set separately from a pool whose storage and uniform descriptor capacities are computed from exact declarations. A set must have at least one binding; sparse binding numbers within a set are supported. All descriptor sets are bound with `vkCmdBindDescriptorSets`.

Supported descriptor types are deliberately limited to the two buffer types above. Unsupported image/sampler/texel/dynamic descriptor types return `VK_ERROR_FEATURE_NOT_PRESENT`, not an inferred replacement.

## Strict device limits and memory requirements

The policy reads and enforces:

| Current Vulkan property | Required check |
| --- | --- |
| `maxBoundDescriptorSets` | Count of explicit numbered descriptor sets |
| `maxDescriptorSetStorageBuffers`, `maxPerStageDescriptorStorageBuffers` | Number of storage descriptors |
| `maxDescriptorSetUniformBuffers`, `maxPerStageDescriptorUniformBuffers` | Number of uniform descriptors |
| `maxPerStageResources` | Conservative maximum of the entire declared layout |
| `minStorageBufferOffsetAlignment`, `minUniformBufferOffsetAlignment` | Actual byte offsets for each descriptor type |
| `maxStorageBufferRange`, `maxUniformBufferRange` | Actual maximum descriptor-visible byte range |
| `VkMemoryRequirements.size`, `alignment`, `memoryTypeBits` | Exactly required buffer allocations |
| `VkPhysicalDeviceMemoryProperties`, `maxMemoryAllocationCount` | Compatible Vulkan memory type indices and jointly available declared heap capacity |
| `nonCoherentAtomSize`, selected memory-type flags | Required CPU/GPU flush and invalidate operations |

Every bound range must fit the **logical declared buffer size** and must use the buffer's matching usage flags (`VK_BUFFER_USAGE_STORAGE_BUFFER_BIT` or `VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT`). The independently tested dynamic joint memory planner (`tools/vulkan-core10-graph-memory.c`) searches real memory-type compatibility and heap-size constraints. Each buffer has its own VkDeviceMemory allocation bound at offset zero, with no manually chosen suballocation alignment.

Vulkan declared heap capacity is not a live free-memory budget: a successful joint plan does not guarantee that `vkAllocateMemory` succeeds. Any actual driver error is returned directly. The automatic selection prefers coherent memory when compatible, but supports noncoherent HOST_VISIBLE memory with actual cache driver calls. A diagnostic request specifically forbidding HOST_COHERENT never falls back to coherent memory.

The shaders themselves require exactly 32 × 1 × 1 local invocations, 64 dispatch groups, two storage descriptors, one uniform descriptor and zero shared workgroup bytes. The capability checker verifies the actual GPU's workgroup/dispatch and storage limits, and the new descriptor policy checks the uniform limits separately.

These rules correspond to the Vulkan specification: [Descriptor sets and pipeline layouts](https://docs.vulkan.org/spec/latest/chapters/descriptorsets.html) and [Vulkan descriptor limits](https://docs.vulkan.org/spec/latest/chapters/limits.html).

## Real public integer workload

The demonstration builds both GLSL shaders as **SPIR-V 1.0** and validates them for Vulkan 1.0 and 1.1. The compute formula is identical in both variants and takes four public uint32 uniform values from one std140 `uvec4`. Before **each** GPU submission, the host changes the 2,048 input words and all four uniform parameters.

The diagnostic uses exactly five buffers:

| Buffer | Logical bytes | Vulkan use |
| --- | ---: | --- |
| GPU input | 8,192 | STORAGE_BUFFER and transfer destination |
| GPU output | 8,192 | STORAGE_BUFFER and transfer source |
| Host upload | 8,192 | Mapped transfer source |
| Host download | 8,192 | Mapped transfer destination |
| Uniform parameters | 16 | Mapped UNIFORM_BUFFER containing one std140 `uvec4` |

The exact descriptor layouts are:

| Variant | Set | Binding | Type | Buffer |
| --- | ---: | ---: | --- | --- |
| One set | 0 | 2 | STORAGE_BUFFER | GPU input |
| One set | 0 | 7 | STORAGE_BUFFER | GPU output |
| One set | 0 | 4 | UNIFORM_BUFFER | Uniform parameters |
| Two sets | 0 | 2 | STORAGE_BUFFER | GPU input |
| Two sets | 1 | 1 | STORAGE_BUFFER | GPU output |
| Two sets | 1 | 5 | UNIFORM_BUFFER | Uniform parameters |

The original integer formula, public input and four uniform generations are calculated independently by the CPU in `tools/test-vulkan-core10-multiset-evidence.py`. The same GPU words are expected from both descriptor layouts:

| Submission | GPU FNV-1a64 checksum, 2,048 output words |
| --- | --- |
| 0 | `da420145579e3ab0` |
| 1 | `3dc96f338cbbffac` |
| 2 | `fc6cfc42aeaff265` |
| 3 | `00ba58c7ff9a53e2` |

One command buffer, recorded just once, contains an upload copy, a Compute dispatch and a download copy. Vulkan 1.0 barriers express host→transfer, host→uniform shader, transfer→shader, shader→transfer and transfer→host dependencies. The CPU flushes upload and uniform memory if noncoherent, waits for GPU fence completion and invalidates readback before validating output.

For the one-set graph, the complete runtime acquires **23 Vulkan resources**; for the two-set graph, **25**. The application-level fault parameter deliberately stops after one successful acquisition at a time and logs exact partial cleanup counts. These faults do **not** trigger actual device loss or GPU allocation exhaustion.

## Results from real Android Vulkan drivers

Both native executables target **Android API 24**, using ARM64 and ARMv7. Real tests were performed on three actual device/process-ABI combinations, with both `-O0` and optimized `-O` SPIR-V:

| Device and ABI | Regular GPU modes (default 1.0, explicit 1.0, optional 1.1) | Forced noncoherent mapped uniform | Resource fault points |
| --- | --- | --- | --- |
| RedMagic / Adreno 730 / ARM64 | 12 GPU runs PASS | 4 exact incompatible-memory rejections | 23 + 25 = 48 PASS |
| RedMagic / Adreno 730 / ARMv7 | 12 GPU runs PASS | 4 exact incompatible-memory rejections | 48 PASS |
| Pixel / Mali-G715 / ARM64 | 12 GPU runs PASS | 4 GPU runs PASS with true flush/invalidate | 48 PASS |

The combined evidence contains:

- **40 successful real GPU runs**, four fence-separated submissions each: **160 successful GPU submits** and **327,680 CPU-verified 32-bit GPU outputs**.
- **Eight explicit memory incompatibility results** on the two RedMagic process ABIs, with `VK_ERROR_FEATURE_NOT_PRESENT` and no GPU dispatch; none substituted coherent memory.
- **144 real Vulkan post-acquisition fault-injection trials**, testing 23 acquisition points for one set and 25 for two sets across all three process configurations. Every trial returned an error and logged exact resource cleanup without submitting Compute work.
- **27 malformed CLI controls**, all rejected with exit 2.
- **Two native C descriptor-validation policy test runs**, at `-O0` and `-O2`, validating one/two-set layouts, sparse bindings, both descriptor types, actual byte ranges and negative capability contracts. They link **no Vulkan driver**.

The Pixel's selected noncoherent memory reports `nonCoherentAtomSize = 64`. In its two-set case, real Vulkan calls flush the upload at buffer index 2, **flush the uniform buffer at index 4**, and invalidate the downloaded result at index 3 for every repeated submission. RedMagic selects compatible HOST_COHERENT memory by default and explicitly rejects a request for noncoherent memory when its buffers expose no compatible HOST_VISIBLE noncoherent type.

The RedMagic GPU's memory heaps are ABI-dependent: its ARM64 Vulkan process reports **11,584,221,184 bytes**, while ARMv7 reports **4,294,967,296 bytes** for heap zero. Both pass because the runtime uses the memory properties reported to that actual process, not any phone profile.

## Reproduction

Build both Android API24 ABIs, all four SPIR-V 1.0 modules and two standalone C descriptor policy tests. The native MinGW link uses `-Wl,--no-insert-timestamp` so the Windows host-test executable SHA-256 remains reproducible; two consecutive full builds are required to produce identical bytes for all eight outputs:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/build-vulkan-core10-multiset.ps1 -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865 -HostGcc C:\msys64\mingw64\bin\gcc.exe
```

The native GCC tests use `-Wl,--no-insert-timestamp` to write a zero PE/COFF timestamp. With the same compiler, sources and arguments, repeated `-O0` and `-O2` builds yield identical SHA-256 executables. The binary identities in the manifest therefore remain stable when the documented build is repeated with the same toolchain.

Run one/two descriptor sets with both shader optimization modes on any connected compatible ARM Android Vulkan device:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/run-vulkan-core10-multiset.ps1 -Serial $DeviceAdbSerial -ReportPrefix device
```

Optional explicit Vulkan 1.1, real noncoherent memory and all **48** post-acquisition cleanup points:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/run-vulkan-core10-multiset.ps1 -Serial $DeviceAdbSerial -ReportPrefix device -TestCore11 -TestNoncoherent -TestFaults
```

`-Abi arm64-v8a` and `-Abi armeabi-v7a` require an actually advertised Android ABI. `auto` selects one of the advertised ARM variants without looking at GPU models. Temporary Vulkan executables and shaders are deleted from `/data/local/tmp` by the runner's `finally` block.

The archived fixtures `tests/fixtures/android-gpu/vulkan-multiset-20261031/` include **56 original LF-only GPU, fault, invalid-input and native host C logs**, and a SHA-256 manifest identifying 19 exact source files, eight built binaries and three independent historical ABI-specific memory surveys.

Offline verification independently calculates the uniform CPU reference, checks descriptor types, set layout ordering, real memory-type bitmasks and heaps, coherent/noncoherent cache operation counts, all fence-separated GPU outputs and all 144 resource-cleanup states. It also rejects deliberately corrupted API, descriptor type, cache call, memory assignment, fence reuse and output checksum evidence:

```powershell
python -B tools/test-vulkan-core10-multiset-evidence.py --artifacts build/diagnostic-multimode/vulkan-multiset-20261031
```

Without `--artifacts`, the same independent verifier needs only the archived logs, sources and memory surveys; it does not invoke GPU drivers. The archiver `tools/archive-vulkan-core10-multiset-evidence.py` copies already recorded local results without altering their bytes.

## Sequential multi-stage graph extension

The separate `vulkan-core10-multiset-graph.c` recorder accepts explicit stage dependencies, per-stage readbacks, 1–3 Compute pipelines and one or two mixed storage/uniform descriptor sets. It uses the resource-owner API documented above and records Vulkan 1.0 transfer/compute and shader-write→shader-read barriers. The new public integer graph matrix executed on RedMagic ARM64, RedMagic ARMv7 and Pixel ARM64, with **120 successful GPU runs, 480 fence-separated submits, 1,966,080 scalar-CPU-verified stage words**, 12 actual Pixel noncoherent GPU runs, 24 precise RedMagic incompatible-memory refusals and 186 post-acquisition cleanup checks. The graph diagnostic uses the host-facing `vulkan_core10_job_port_t` with independently owned input, uniform and output arrays. On Android GPUs it rejects 680 invalid task contracts before any submission, while 480 valid jobs reuse recorded command buffers. See `docs/vulkan-core10-job-port-integration.md` for task lifecycle. The independent `vulkan_core10_device_catalog_t` opens VkInstance, enumerates and checks indexed physical devices, and closes after logical-device teardown. In 330 hardware sessions it rejects 1,650 invalid catalogue requests before selection. Its contracts and evidence are in `docs/vulkan-core10-device-catalog-portability.md`. Source, build, hardware coverage and archived evidence are detailed in `docs/vulkan-core10-multiset-graph-portability.md`.

## Explicit limits

The resource owner can create more than two sets when supplied an exact supported layout, but the two public shaders exercise only **one and two**. Uniform and storage descriptors are the only supported types; dynamic descriptors, sampled images, texel buffers and samplers are not implemented. There is one queue, one command buffer and one fence per runtime, without concurrent multi-queue dispatch.

No phone with a *physically Vulkan 1.0-only GPU* or Android 7.0/API24 OS was available. Tests requested Vulkan 1.0 on newer GPUs and compiled native binaries for API24; this does not prove execution on actual Android 7.0 hardware.

The tests do not run a Vulkan validation layer, provoke device loss, prove absence of driver-internal leaks, or implement a bcrypt shader/hashcat backend. The original OpenCL bcrypt mode-3200 self-test exit 245 and separate no-optimization compiler SIGSEGV 139 remain unresolved.
