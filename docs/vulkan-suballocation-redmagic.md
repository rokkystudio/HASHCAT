# Vulkan memory suballocation and controlled error handling (2026-10-31)

## Purpose

This standalone, noncryptographic Vulkan 1.1 diagnostic tests **four storage buffers bound into one `VkDeviceMemory` allocation** on the connected RedMagic NX709S / Adreno 730 and Pixel 9 Pro XL / Mali-G715.

The program compiles and executes the three **unchanged public synthetic GLSL compute stages** used by the earlier Vulkan pipeline experiment. It reads both intermediate storage buffers and the final result after GPU execution, and checks all 12,288 32-bit words against an independent scalar CPU reference. Each device is tested with unoptimized (`-O0`) and optimized (`-O`) SPIR-V.

Three controlled application failures separately exercise the reporting and cleanup paths for missing a suitable memory type, Vulkan allocation failure, and a simulated `VK_ERROR_DEVICE_LOST`. The errors are **injected in application code**. They are not real Vulkan-driver allocation failures or real GPU device loss. The program never exhausts device memory, forces a GPU reset, accesses passwords, loads hashcat, or executes any cryptographic shader.

## Memory allocator contract

The four storage buffers contain exactly the original synthetic pipeline's data:

| Buffer | Required payload | GPU role |
| --- | ---: | --- |
| Input | 8,192 bytes | 2,048 public synthetic unsigned 32-bit words |
| Stage A | 8,192 bytes | First compute stage intermediate output |
| Stage B | 8,192 bytes | Second compute stage intermediate output |
| Output | 32,768 bytes | 8,192 final 32-bit output words |

For each storage buffer, the program calls `vkCreateBuffer` followed by `vkGetBufferMemoryRequirements`. The allocation planner verifies the implementation-provided `size` is sufficient, the `alignment` is a positive power of two and the cumulative offset does not overflow. It rounds each buffer offset upward to its **own** required alignment, reserves the full `VkMemoryRequirements.size` region, intersects all four `memoryTypeBits` bitmasks, and selects a compatible **host-visible, host-coherent** memory type.

The program accepts only a plan of at most **1,048,576 bytes (1 MiB)**; this is an explicit diagnostic safety cap, not an allocation-pressure or driver-limit test. It then calls `vkAllocateMemory` **once**, uses four `vkBindBufferMemory` calls at their separately calculated offsets, and maps the allocation once. Each buffer receives a distinct pointer derived from its suballocation offset; its descriptor references its own `VkBuffer` at descriptor offset zero.

For the observed devices, all four requirements had an alignment of **64 bytes** and exact required sizes equal to their requested payloads:

| Buffer | Requested and required bytes | Offset | End |
| --- | ---: | ---: | ---: |
| Input | 8,192 | 0 | 8,192 |
| Stage A | 8,192 | 8,192 | 16,384 |
| Stage B | 8,192 | 16,384 | 24,576 |
| Output | 32,768 | 24,576 | 57,344 |

The total **57,344-byte single allocation** and all four memory ranges were observed directly in each device's own Vulkan output, and verified again by an offline validator. Other drivers are expected to report different requirements; the allocator computes offsets from the API rather than hardcoding this measured table.

The allocation's compatible memory-type mask and selected type did differ:

| GPU | Shared `memoryTypeBits` | Compatible chosen memory type | Allocation |
| --- | --- | ---: | ---: |
| Qualcomm Adreno 730 | `0x00000041` | 6 | 57,344 bytes |
| Arm Mali-G715 | `0x00000003` | 0 | 57,344 bytes |

The memory type number is a **device-local index**, not a cross-GPU memory class. The same fixed index must not be reused across devices without checking each physical device's memory types.

The Vulkan specification defines `VkMemoryRequirements.size`, `alignment`, and `memoryTypeBits`, and requires memory bindings to use a compatible type and an aligned offset:

- <https://docs.vulkan.org/refpages/latest/refpages/source/VkMemoryRequirements.html>
- <https://docs.vulkan.org/refpages/latest/refpages/source/vkBindBufferMemory.html>

## Real compute and synchronization

The three independent synthetic compute stages run with the same descriptor types, workgroup size (32) and dispatch count (64 workgroups per stage) as `tools/vulkan-multistage-diagnostic.c`.

All three GPU stages use the four suballocated buffers. Barriers ensure stage A writes are visible to stage B, stage B writes are visible to stage C, and the host reads results only after the compute-to-host barrier and a completed fence. Storage state buffers are initialized with different byte patterns before dispatch to detect unwritten output.

| Result | Expected independent CPU FNV-1a 64-bit | RedMagic `-O0` / `-O` | Pixel `-O0` / `-O` |
| --- | --- | --- | --- |
| Stage A | `de56d6d0aa66c6ec` | PASS | PASS |
| Stage B | `c1259ad4f024499a` | PASS | PASS |
| Final output | `05de70c62f8d7cfd` | PASS | PASS |
| Final allocation cleanup | 1 allocation freed, 4 buffers bound | PASS | PASS |

Every real device run exited **0** after verifying all **12,288 output words**. Neither device required any firmware change.

## Controlled errors and what they prove

The host program also accepts an exact `--simulate` mode:

| Mode | Application injection point | Simulated Vulkan return | Process exit | Allocation freed |
| --- | --- | --- | ---: | --- |
| `memory-type` | After validating the real memory plan, before allocation | `VK_ERROR_FEATURE_NOT_PRESENT` (-8) | 42 | None allocated |
| `allocation` | Immediately before `vkAllocateMemory` | `VK_ERROR_OUT_OF_DEVICE_MEMORY` (-2) | 43 | None allocated |
| `device-lost` | After an **actual successful GPU fence wait**, before CPU result interpretation | `VK_ERROR_DEVICE_LOST` (-4) | 44 | 1 shared allocation |

All three controlled errors were observed on both devices, and the cleanup summary showed the correct number of freed allocations and previously bound buffers. Device-loss simulation prints `real_fence_wait=0` to prove the **real** Vulkan fence wait returned success before the fake error was introduced.

These controls check application-level error propagation, resource destruction and clean termination. **They do not establish behavior when a physical or logical GPU really becomes lost**. In particular, they cannot reproduce a true `VK_ERROR_DEVICE_LOST` return from `vkWaitForFences`. For real device loss, Vulkan specifies that even potentially indefinite waits must return in finite time; Vulkan objects still require correct cleanup. See [Vulkan lost-device behavior](https://docs.vulkan.org/spec/latest/chapters/devsandqueues.html) and [device destruction](https://docs.vulkan.org/refpages/latest/refpages/source/vkDestroyDevice.html).

Three additional invalid inputs (unrecognized simulation, missing SPIR-V file and incorrect simulation flag) returned **exit 2** on each device without dispatching GPU compute.

## Project files

| File | Purpose |
| --- | --- |
| `tools/vulkan-suballocation-diagnostic.c` | Full standalone Vulkan 1.1 host implementation, independent aligned memory planner, one allocation for four buffers, three GPU stages, error handling and cleanup |
| `tools/build-vulkan-suballocation-diagnostic.ps1` | Reproducible shaderc `glslc` builds for both optimization modes, `spirv-val` checks and strict Android ARM64 C compilation |
| `tools/run-vulkan-suballocation-diagnostic.ps1` | Real-device execution, dynamic memory-layout validation, two positive GPU checks, six negative tests, ADB file cleanup and local logs |
| `tools/test-vulkan-suballocation-evidence.py` | Offline verification of source/binary SHA-256, actual driver memory layouts and intermediate CPU checksums; negative control rejects an incorrectly aligned buffer binding |
| `tools/test-vulkan-multistage-reference.py` | Existing, unchanged independent scalar Python oracle for all three synthetic stage outputs |
| `tools/vulkan-pipeline-stage-{a,b,c}.comp` | Unchanged public noncryptographic GLSL compute stages |
| `tests/fixtures/android-gpu/vulkan-suballocation-20261031/` | Four real Vulkan GPU logs, two error-control logs and an exact provenance/limitations JSON manifest |

The original three synthetic shader binaries are **byte-for-byte identical** to their outputs in the previous experiment. The new native program uses the existing shaders with a different memory allocation architecture.

## Reproduction

From the Windows project `D:\PROJECTS\HASHCAT`:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/build-vulkan-suballocation-diagnostic.ps1 -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865

powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/run-vulkan-suballocation-diagnostic.ps1 -Serial $RedMagicAdbSerial -ReportPrefix redmagic
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/run-vulkan-suballocation-diagnostic.ps1 -Serial $PixelAdbSerial -ReportPrefix pixel

python -B tools/test-vulkan-suballocation-evidence.py --artifacts build/diagnostic-multimode/vulkan-suballocation-20261031
```

Supply each explicitly chosen device's real ADB serial in the two PowerShell variables. The build outputs and local logs are stored in the Git-ignored `build/diagnostic-multimode/vulkan-suballocation-20261031/` directory. The run script stages and deletes seven isolated test files under `/data/local/tmp` and never writes to the hashcat app directory.

The offline evidence checker can run without locally compiled artifacts, or with `--artifacts` to prove all seven local ARM64/SPIR-V outputs are byte-identical to the archived observations. It checks the actual Vulkan-reported offsets, alignment, sizes, memory-type bits, final GPU checksums and cleanup, rather than accepting a successful process exit alone.

## Scope and next steps

This is a validated **generic Vulkan Compute memory management pattern**, not a functioning Vulkan implementation of bcrypt or a hashcat Vulkan backend. Driver-internal leaks, noncoherent memory, memory defragmentation, actual `VK_ERROR_DEVICE_LOST`, concurrent GPU submissions and production-scale allocation pressure have **not** been tested.

The RedMagic Qualcomm OpenCL bcrypt self-test failure (archived exit code **245**) remains separate and unresolved. The current original bcrypt module and OpenCL kernel are not part of this test.
