# Capability-driven Vulkan 1.0 Compute configuration (2026-10-31)

## Objective

The default Vulkan Compute configuration is determined by **public Vulkan API properties and the declared requirements of the actual shader**. The source contains no GPU vendor-name matches, Android-model profiles, driver-version overrides or inferred memory-type indices.

The standalone ARM64 diagnostic proves this policy with one public synthetic compute shader on both connected test devices. Its input is deterministic integer data, not passwords or hashes. The program does not load hashcat, implement bcrypt, install Android applications or modify the existing OpenCL runtime.

The reusable module lives in:

- `tools/vulkan-auto-config.h` — documented API, compute requirements, selected device and memory-plan structures
- `tools/vulkan-auto-config.c` — device capability validation, host-visible memory policy, aligned suballocation and mapped-memory cache synchronization
- `tools/vulkan-auto-config-diagnostic.c` — independent real-device GPU test and explicit negative workload checks

The module is generic Vulkan infrastructure in `tools/` and **is not integrated into the production hashcat GPU backend**.

## Requirements come from the shader, not a device model

For the public diagnostic, the known workload is:

| Requirement | Exact public shader need |
| --- | ---: |
| Minimum Vulkan API | 1.0 |
| Local workgroup size | 32 × 1 × 1 |
| Workgroup count | 64 × 1 × 1 |
| Storage-buffer descriptors in the shared layout | 4 |
| Largest storage buffer | 32,768 bytes |
| Shader-declared workgroup shared memory | 128 bytes |
| CPU-visible storage buffer payloads | 8,192 + 8,192 + 8,192 + 32,768 bytes |

A shader's local workgroup size is a compiled shader property, not a number that can safely be improvised at runtime. The capability checker compares every dimension against `maxComputeWorkGroupSize`, each dispatch group count against `maxComputeWorkGroupCount`, the local invocation product against `maxComputeWorkGroupInvocations`, and the explicit descriptor, storage-buffer range and shared-memory requirements against the device limits.

It selects the **first actual compute-capable queue family** with a nonzero queue count. If the device does not support the declared requirements, it reports a concrete `REJECT_COMPUTE`, `REJECT_STORAGE`, `REJECT_QUEUE` or `REJECT_VULKAN_API` result, rather than silently altering the shader workload or guessing an alternative workgroup size.

The Vulkan instance requests core API **1.0**. All three unchanged public synthetic GLSL shaders were compiled separately with `--target-env=vulkan1.0` at `-O0` and `-O`; all six SPIR-V modules passed `spirv-val --target-env vulkan1.0`. The standalone runtime actually executes stage A; compilation of stages B and C does not constitute a new three-stage GPU execution for this Vulkan 1.0 test.

Four separate real-API rejection controls verify the checker rejects: local size zero, group count zero, storage descriptor count zero and a shared-memory request greater than the physical device limit. Each returns the specified `VK_ERROR_FEATURE_NOT_PRESENT` result on both tested devices.

Vulkan core capability and queue-family queries are documented in the [Vulkan device selection guide](https://docs.vulkan.org/guide/latest/). The workgroup limits are properties of `VkPhysicalDeviceLimits`.

## Memory type and suballocation

The memory planner consumes each actual buffer's `VkMemoryRequirements` from `vkGetBufferMemoryRequirements`. It verifies required byte counts and power-of-two alignment, intersects all `memoryTypeBits`, and applies a fixed **functional** priority:

1. A compatible memory type with `HOST_VISIBLE | HOST_COHERENT`.
2. Otherwise, a compatible type with `HOST_VISIBLE` and **explicit flush/invalidate**.
3. Otherwise, a documented `VK_ERROR_FEATURE_NOT_PRESENT` and `REJECT_MEMORY_TYPE ... fallback=staging-required`.

These are capability checks, not device heuristics. The planner returns the chosen `memoryTypeIndex`, aligned offsets and total allocation size; the caller binds each buffer at its own calculated offset. Each actual Vulkan memory type is a per-physical-device index. The module never assumes that type 6, 0 or any other index has the same meaning across devices.

For a noncoherent memory type, the effective suballocation alignment is the larger of `VkMemoryRequirements.alignment` and `nonCoherentAtomSize`. No two buffer ranges share a noncoherent host cache atom. Mapped-memory synchronization uses `vkFlushMappedMemoryRanges` after CPU writes, `vkInvalidateMappedMemoryRanges` after the GPU memory barrier and a successful fence wait, and `VK_WHOLE_SIZE` for the full allocation mapped at offset zero. Vulkan's [memory specification](https://docs.vulkan.org/spec/latest/chapters/memory.html) describes the required availability/visibility and alignment rules.

If a GPU exposes only device-local storage memory without a compatible host-visible memory type, the current **direct-map** test returns an explicit incompatibility. A more complete cross-device backend requires a separate staging-buffer path using transfer-compatible allocations and explicit transfer/synchronization. Such a path must be implemented and verified before claiming support for these devices. No hidden fallback currently masks that limitation.

## Real Adreno and Mali results

The runtime selects devices based on their actual Vulkan properties. The observed profiles are evidence, **not selection tables**:

| Property | RedMagic NX709S / Adreno 730 | Pixel 9 Pro XL / Mali-G715 |
| --- | ---: | ---: |
| Reported Vulkan API | 1.1.128 | 1.4.305 |
| Selected compute queue family | 0 | 0 |
| `maxComputeWorkGroupSize[0]` | 1,024 | 1,024 |
| `maxComputeWorkGroupInvocations` | 1,024 | 1,024 |
| `maxComputeSharedMemorySize` | 32,768 bytes | 32,768 bytes |
| `nonCoherentAtomSize` | 1 byte | 64 bytes |
| Compatible storage memory-type mask | `0x00000041` | `0x00000003` |
| Automatically selected host-coherent type | 6 | 0 |
| Actual one-allocation byte count | 57,344 | 57,344 |
| Vulkan 1.0 shader `-O0` | **PASS / 0** | **PASS / 0** |
| Vulkan 1.0 shader `-O` | **PASS / 0** | **PASS / 0** |

Both devices run the public first-stage GPU shader for 2,048 work items with the automatically selected device, queue and memory. The CPU verifies every output word against its scalar formula, as well as all 14,336 mapped 32-bit words in the four buffers. The independent FNV-1a 64-bit checksum of the actual GPU output matches Python:

```text
stage-A GPU output (2048 uint32): 00d6d6e4837edebc
```

All four device/optimization combinations produced this exact checksum. A real GPU submission, a fence wait, a compute-to-host memory barrier, and the mapped-memory synchronization policy are part of this test.

The selected types in **this original auto-configuration experiment** are host-coherent on both devices. Its host-visible noncoherent fallback is independently verified using synthetic `VkPhysicalDeviceMemoryProperties` and `VkMemoryRequirements`, including 256-byte cache-atom padding and rejection of incompatible memory. A later standalone Vulkan 1.0 Compute program explicitly selects real noncoherent HOST_VISIBLE staging on Pixel and successfully invokes `vkFlushMappedMemoryRanges`, GPU compute/readback and `vkInvalidateMappedMemoryRanges`. RedMagic explicitly rejects the forced type when its per-buffer memory compatibility mask disallows it. See `docs/vulkan-core10-portability.md` and `tests/fixtures/android-gpu/vulkan-core-baseline-20261031/`.

## Reproduce

Windows PowerShell, in `D:\PROJECTS\HASHCAT`:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/build-vulkan-auto-config.ps1 -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865

powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/run-vulkan-auto-config.ps1 -Serial $RedMagicAdbSerial -ReportPrefix redmagic

powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/run-vulkan-auto-config.ps1 -Serial $PixelAdbSerial -ReportPrefix pixel

python -B tools/test-vulkan-auto-config-evidence.py --artifacts build/diagnostic-multimode/vulkan-autoconfig-20261031
```

Set the two ADB serial variables to the connected devices' actual serials. The build uses the Android NDK r29 bundled Clang/shaderc and targets Android API 26. Its ignored local directory contains **one ARM64 executable and six Vulkan 1.0 SPIR-V binaries**.

The run script stages the standalone executable and the `-O0` / `-O` stage-A shaders under `/data/local/tmp`, runs both optimizations, checks device-reported memory offsets, validates negative CLI cases (missing shader argument and nonexistent SPIR-V file, both exit 2), saves local logs, and removes the temporary files in a `finally` block. It does not alter application runtime files or firmware.

The offline evidence validator reads `tests/fixtures/android-gpu/vulkan-autoconfig-20261031/` and independently validates:

- Exact SHA-256 identities of the eight source files and seven build artifacts (when `--artifacts` is provided)
- Four original GPU execution logs, reported physical-device limits and memory layouts
- Four real-device capability rejections per positive run, and four invalid CLI cases across both phones
- Actual GPU FNV-1a output against an independent Python oracle
- Negative controls that reject a tampered GPU output checksum and an unaligned buffer offset

The validator runs without ADB or GPU access; omitting `--artifacts` retains source/log validation without requiring the locally compiled binaries.

## Compatibility policy and limitations

The intended defaults for an application using this module are:

- Target **core Vulkan 1.0** for workloads that need no newer features.
- Validate each declared workload against the actual physical device; no manufacturer- or model-specific presets.
- Select queue families by declared `VK_QUEUE_COMPUTE_BIT` support and available queue count.
- Select storage memory by compatibility bitmask and required memory flags; prefer coherent memory for simpler synchronization, otherwise use the verified noncoherent synchronization path.
- Keep actual buffer alignment, descriptor sizes and shader workgroup sizes explicit and enforceable.
- Treat unsupported capabilities, memory types, pipeline creation errors, timeouts and incorrect GPU output as visible failures; never silently skip verification.

The module does **not** translate arbitrary shaders, guarantee every Vulkan GPU supports a given workload, implement staging buffers, guarantee low-memory success, or measure hardware performance. Portability and available memory budgets remain constrained by each driver and runtime environment.

It is also **not a Vulkan hashcat backend**, and the archived RedMagic OpenCL bcrypt mode-3200 self-test exit code **245** remains unresolved.

## Follow-up: separate portable staging policy

The independent `tools/vulkan-staging-memory.h` and `tools/vulkan-staging-memory.c` module now determines direct-vs-staging transfers using the actual memory-type compatibility of two storage buffers. When both permit host-visible memory, direct mapping is selected; otherwise the staging path allocates independent CPU upload/download transfer buffers, selects compatible GPU memory and copies data using Vulkan 1.0 `vkCmdCopyBuffer` with explicit barriers.

The earlier combined-source staging implementation **actually passed both paths on RedMagic and Pixel** with the original noncryptographic stage-A shader at both `-O0` and `-O`. The separate modular source builds and its historical archive identity checks pass offline; its new binary has not been executed on either device after the separation. See `docs/vulkan-portable-staging.md` and `tests/fixtures/android-gpu/vulkan-staging-20261031/`. The existing universal source and original GPU results remain unchanged.

The real C capability/memory-selection methods are also covered by a CPU-only native GCC regression harness: 64 valid three-type Vulkan memory-property layouts, all seven nonempty storage masks on each of two buffers and both transfer requests yield **6,272 exact staging/direct decisions**, plus 448 individual memory-type checks. Vulkan cache synchronization is tested with mock `flush`/`invalidate` entry points. This tests policy behavior on synthetic devices, not new GPU hardware. Reproduction and evidence are in `docs/vulkan-portable-staging.md` and `tests/fixtures/vulkan-host-policy-20261031/`.

An additional **four-buffer Vulkan memory heap planner** now validates per-buffer `VkMemoryRequirements`, memory type-to-heap indices, declared heap capacities and `maxMemoryAllocationCount`. A backtracking search avoids rejecting a device merely because an earlier valid memory type would crowd a later resource out of its heap. Two phones passed actual `vkAllocateMemory`/`vkBindBufferMemory` for all four small synthetic buffers using runtime-selected types, with no GPU work submitted. Native `-O0`/`-O2` C implementations and an independent Python oracle agree across 200,704 memory-layout cases. See `docs/vulkan-memory-heap-portability.md` and `tests/fixtures/android-gpu/vulkan-heap-20261031/`.

## Vulkan 1.0 is the default core API contract

The `vulkan_core11_select_instance_version` policy now requests **exactly the minimum core API required by the workload**. For the unchanged synthetic shader this is Vulkan **1.0**, including when the loader reports 1.3 or 1.4. An explicit 1.1 requirement is checked against both the loader and the physical GPU and never silently downgraded. Missing `vkEnumerateInstanceVersion` is the standard Vulkan 1.0-loader case and is covered by native C tests.

A separate full synthetic GPU diagnostic combines four heap-aware allocations, two staging copies, core 1.0 memory barriers and a real Compute dispatch. Both phones passed default 1.0, explicit 1.0 and optional 1.1 at `-O0` and `-O`: **12 verified GPU runs, zero optional enabled features/extensions**. A real physical Vulkan-1.0-only driver has not been tested. The standalone diagnostic builds both `arm64-v8a` and `armeabi-v7a` for Android API 24 and selects the correct binary from the device's reported ABI list. RedMagic passed six additional real ARMv7 GPU runs and an independent 32-bit Vulkan heap survey; its ARMv7 driver reports a 4-GiB device-local heap versus the larger ARM64 view. Full evidence is in `docs/vulkan-core10-portability.md` and `tests/fixtures/android-gpu/vulkan-core-baseline-20261031/`.
