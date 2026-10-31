# Portable Vulkan 1.0 three-stage synthetic Compute (2026-10-31)

## Scope and Vulkan compatibility

This is a standalone, **noncryptographic** Vulkan Compute infrastructure diagnostic, not a hashcat GPU backend and not a password-processing implementation.

It runs three existing public GLSL integer shaders (stage A, B, C) as a single Vulkan 1.0 Compute command stream and verifies all data using an independent unsigned 32-bit CPU reference. The default instance API is **Vulkan 1.0**, even on a Vulkan 1.3/1.4 loader. The optional explicit requirement `--minimum 1.1` runs only when both loader and physical GPU support core 1.1. No optional device features or extensions are enabled.

The shaders target **SPIR-V 1.0**; all six compiled `-O0`/`-O` shader modules pass `spirv-val --target-env vulkan1.0` and `vulkan1.1`. The two standalone Android executables target **Android API 24** (`aarch64-linux-android24` and `armv7a-linux-androideabi24`). The `vkEnumerateInstanceVersion` function is queried dynamically, not linked as a mandatory Vulkan-1.1 symbol.

## Six-buffer, device-capability-driven memory plan

Four GPU storage buffers and two CPU-visible staging buffers are used:

| Buffer | Exact application bytes | Access |
| --- | ---: | --- |
| GPU input | 8,192 | Compute reads / transfer writes |
| GPU stage A | 8,192 | Compute stage A writes, B/C read |
| GPU stage B | 8,192 | Compute stage B writes, C reads |
| GPU output | 32,768 | Compute stage C writes / transfer reads |
| Staging upload | 8,192 | CPU writes, transfer reads |
| Staging download | 32,768 | Transfer writes, CPU reads |

The joint allocation planner, `tools/vulkan-multistage-memory-plan.c`, checks all six actual `VkMemoryRequirements.size`, `alignment` and `memoryTypeBits` masks, Vulkan-reported `VkMemoryType.heapIndex`, `VkMemoryHeap.size` and `VkPhysicalDeviceLimits.maxMemoryAllocationCount`. It searches memory-type assignments with backtracking and deterministic preferences:

- GPU storage favors **DEVICE_LOCAL** memory but does not require CPU mapping.
- Staging requires **HOST_VISIBLE**; coherent types are preferred, noncoherent types remain valid with Vulkan-mandated synchronization.
- Protected and AMD device-coherent types are not selected because the corresponding optional features are disabled.
- Every allocation has its own `VkDeviceMemory`, bound at offset zero, so no manually guessed suballocation offsets are used.
- A noncoherent staging trial is explicitly requested with `--require-noncoherent-staging`; it excludes `HOST_COHERENT` and returns the real incompatibility instead of silently reverting to coherent memory.

A placement satisfying declared heap sizes still may fail at `vkAllocateMemory` due to a live memory budget; that Vulkan error is propagated. Runtime selection never depends on an Adreno, Mali, PowerVR or phone-model profile.

The six-buffer planner is independent of the pre-existing four-buffer plan. This preserves the exact historical four-buffer evidence and allows the new planner to validate all six requests jointly rather than attempting two unrelated four-buffer plans that could overcommit the same heap.

## Compute, synchronization and validation

The known shader requirements are declared to the capability checker: workgroup size **32 × 1 × 1**, **64** dispatch groups, **four** storage buffer descriptors, largest range **32,768 bytes**, and **128 bytes** of stage-B workgroup shared memory. The device's actual Compute queue, descriptor limits, workgroup limits and shared-memory capacity must accept these requirements.

A single Vulkan 1.0 command buffer records:

1. Host-write-to-transfer-read barrier and upload copy (8 KiB).
2. Transfer-write-to-Compute-read barrier.
3. Dispatch A; Compute-write-to-Compute-read barrier.
4. Dispatch B (32-lane shared-memory barrier); Compute-write-to-Compute-read barrier.
5. Dispatch C; Compute-write-to-transfer-read barrier.
6. Download copy (32 KiB), transfer-write-to-host-read barrier, GPU fence wait and optional mapped-memory invalidate.

Only **two** transfer copies are required. The `uvec4` result emitted by stage C contains the actual stage-A and stage-B values in its first two lanes. The CPU separately calculates stage A and B and checks them alongside the other two final output lanes. Consequently **12,288 logical 32-bit values** are checked per GPU run (2,048 from A + 2,048 from B + 8,192 output words).

**Intermediate stage A/B GPU buffers are not separately copied to the CPU** in this two-transfer version. They are validated through stage C's output, which consumes both intermediate GPU buffers. This distinction is preserved in the archived evidence.

Stage-level independent FNV-1a 64-bit checksums:

| Public synthetic stage | Required checksum |
| --- | --- |
| A | `de56d6d0aa66c6ec` |
| B | `c1259ad4f024499a` |
| Final output C | `05de70c62f8d7cfd` |

For HOST_VISIBLE memory without HOST_COHERENT, the CPU uploads and calls `vkFlushMappedMemoryRanges` on the entire mapped allocation before submit. After the successful GPU fence wait it invokes `vkInvalidateMappedMemoryRanges` on the entire mapped download allocation. Both use offset zero and `VK_WHOLE_SIZE`, respecting the driver's actual `nonCoherentAtomSize`. Coherent allocations do not require either driver operation.

## Actual GPU evidence

The **same ten compiled artifacts** were used in the archived real GPU trials (two Android executables, six SPIR-V 1.0 modules and two native C host tests).

| Device and process ABI | Vulkan instance | GPU staging memory | Result |
| --- | --- | --- | --- |
| RedMagic / Adreno 730 / ARM64 | default 1.0, explicit 1.0, explicit 1.1 | coherent type 6 | 6 GPU PASS |
| Pixel / Mali-G715 / ARM64 | default 1.0, explicit 1.0, explicit 1.1 | coherent type 0 | 6 GPU PASS |
| Pixel / Mali-G715 / ARM64 | default 1.0, explicitly noncoherent | noncoherent type 1; real `flush`/`invalidate` with 64-byte atom | 2 GPU PASS |
| RedMagic / Adreno 730 / ARM64 | default 1.0, explicitly noncoherent | no compatible HOST_VISIBLE noncoherent staging upload type | 2 exact rejections |

Each API mode above was executed using both `-O0` and `-O` shader compilation. The positive checksums matched on both GPUs. The real RedMagic rejection returned `VK_ERROR_FEATURE_NOT_PRESENT` and identified the incompatible staging upload requirement. The normal coherent path remained usable.

Both phones implement physical Vulkan versions above 1.0; a physical Vulkan-1.0-only GPU and a physical Android 7/API24 phone have not been tested. Both Android **ARM64 and ARMv7** executables compile and link strictly for API 24. **This specific six-buffer multistage GPU program has not been executed on ARMv7 hardware in the archived dataset.** The separate earlier single-stage Vulkan 1.0 diagnostic has independent real ARMv7 execution evidence; it must not be reported as multistage evidence.

## Reproduction

Build all native programs, native CPU tests and six shaders:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/build-vulkan-core10-multistage.ps1 -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865 -HostGcc C:\msys64\mingw64\bin\gcc.exe
```

Run the Vulkan 1.0 default, preserving all output logs locally:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/run-vulkan-core10-multistage.ps1 -Serial $DeviceAdbSerial -ReportPrefix device
```

Optional test of core 1.1 and noncoherent memory, only when 1.1 is known to be available:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/run-vulkan-core10-multistage.ps1 -Serial $DeviceAdbSerial -ReportPrefix device -TestCore11 -TestNoncoherentStaging
```

The runner chooses ARM64 or ARMv7 using Android's own `ro.product.cpu.abilist`; alternatively `-Abi arm64-v8a` or `-Abi armeabi-v7a` explicitly requires that supported architecture. A missing executable, unsupported ABI or insufficient Vulkan Compute limit produces an explicit error without replacing the workload requirements.

The runner always removes its temporary files from `/data/local/tmp` in `finally`.

Offline verification of the source identities, real GPU logs, actual memory properties, ten compiled artifacts, two native host tests, independently recalculated stage checksums and deliberately corrupted negative controls:

```powershell
python -B tools/test-vulkan-core10-multistage-evidence.py --artifacts build/diagnostic-multimode/vulkan-core-multistage-20261031
```

Without `--artifacts`, all archived source, actual GPU and host-test logs are still audited. Records are under `tests/fixtures/android-gpu/vulkan-core-multistage-20261031/` with original LF line endings and SHA-256 identities. No ADB serial identifiers appear in archived logs.

## Follow-up: common resource runtime

The later independent `tools/vulkan-core10-runtime.c` contains a reusable owner for the six GPU/staging memory allocations, pipeline and descriptor lifetimes, recorded command buffer and Vulkan 1.0 fence submission. It executes these same public three-stage shaders four times with one reused command buffer. The new owner passed actual RedMagic ARM64 **and ARMv7** GPU tests and Pixel ARM64 GPU tests, including real noncoherent staging on Pixel and complete 28-point resource-acquisition fault injection on each of the three ABI/device combinations.

The earlier monolithic executable documented above remains an independent, immutable diagnostic dataset. Full later runtime results, exact ABI-specific memory properties and limitations are in `docs/vulkan-core10-runtime-portability.md` and `tests/fixtures/android-gpu/vulkan-runtime-20261031/`.

## Integration boundary

This confirms a general-purpose, multi-dispatch Vulkan 1.0 Compute path, automatic six-buffer planning, memory barriers, staging and physical-device portability on two GPUs. It is **not** a Vulkan backend for hashcat and does not implement bcrypt or modify the production OpenCL kernel. The archived OpenCL bcrypt mode-3200 self-test failure **245** remains unresolved.
