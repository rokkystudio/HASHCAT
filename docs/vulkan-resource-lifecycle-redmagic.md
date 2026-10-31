# Vulkan resource lifetime and deterministic failure cleanup (2026-10-31)

## Purpose and boundaries

A dedicated Android ARM64 Vulkan 1.1 program validates the ownership, acquisition, error reporting and release ordering of objects required for a generic compute pipeline. The program uses the existing **public synthetic stage-A GLSL shader** from the earlier three-stage experiment, a 4096-byte host-visible coherent storage buffer, a descriptor set and an allocated command buffer.

It creates a compute pipeline and records a command buffer but **never submits the command buffer or executes a compute shader**. It does not use the hashcat runtime, bcrypt kernels, credentials, passwords, or a self-test override. All changes are isolated in standalone diagnostic tools and archived evidence.

The original Vulkan multi-dispatch execution and real GPU arithmetic are separately tested in `docs/vulkan-multistage-redmagic.md`. This file verifies resource cleanup and deterministic failure injection, not application-level password recovery.

## Resource acquisition sequence

The C program `tools/vulkan-resource-lifecycle-diagnostic.c` acquires these 14 tracked resources in order:

| Checkpoint | Owned Vulkan object or state | Expected acquired count after checkpoint |
| --- | --- | ---: |
| `instance` | `VkInstance` | 1 |
| `device` | `VkDevice` for an explicitly compute-capable queue family | 2 |
| `buffer` | `VkBuffer` (4096-byte storage buffer) | 3 |
| `device-memory` | `VkDeviceMemory` with host-visible, host-coherent type | 4 |
| `memory-map` | Live host memory mapping from `vkMapMemory` | 5 |
| `descriptor-layout` | `VkDescriptorSetLayout` with two storage bindings | 6 |
| `pipeline-layout` | `VkPipelineLayout` | 7 |
| `shader-module` | `VkShaderModule` | 8 |
| `compute-pipeline` | `VkPipeline` | 9 |
| `descriptor-pool` | `VkDescriptorPool` permitting individual set release | 10 |
| `descriptor-set` | One allocated `VkDescriptorSet` | 11 |
| `command-pool` | `VkCommandPool` with reset capability | 12 |
| `command-buffer` | One allocated and recorded `VkCommandBuffer` | 13 |
| `fence` | One initially unsignaled `VkFence` | 14 |

Physical-device and queue-family enumeration are read-only and do not own device objects. Temporary C arrays for device/queue discovery are separately `free()`d. The buffer is bound to memory, mapped, filled with deterministic public integers and immediately checked on the CPU. Descriptor bindings refer to the same synthetic storage buffer for this object-lifetime test; no shader is dispatched.

On successful completion the fence must report `VK_NOT_READY`, because the program has never submitted GPU work. The program then waits for the device to be idle and releases every allocated Vulkan object, descriptor set, command buffer and mapped region in reverse dependency order. The final resource ledger checks that the number of successful acquisitions equals the number of corresponding releases.

### Failure injection

Passing `--fail-after <checkpoint>` to the standalone executable triggers a deliberate application error **after the actual Vulkan creation/API call succeeds** at that checkpoint. It does not simulate a failure inside the driver. A successful injected failure emits a `INJECTED_FAILURE stage=... code=42` diagnostic, releases all objects acquired so far, prints a matching `CLEANUP_OK ... created=N released=N` result, and exits with code **42**. This is tested for all fourteen checkpoints.

Invalid cycle counts (`0`, `17`, noninteger), an unknown failure checkpoint, and an absent shader are distinct input errors returning code **2**, before Vulkan object creation (shader file access is attempted only after CLI validation). The test does not hide arbitrary Vulkan API errors: unexpected Vulkan results or mismatched acquired/released counters are reported and fail the run.

Application acquisition/release accounting verifies that the corresponding Vulkan API cleanup calls execute. It does **not** prove the Qualcomm or Mali driver has no internal memory leak; detailed driver-internal allocation tracking would require external tooling.

## Files

| Path | Function |
| --- | --- |
| `tools/vulkan-resource-lifecycle-diagnostic.c` | Standalone ARM64 Vulkan 1.1 object graph, 14 injectable cleanup checkpoints, strict arguments, 4096-byte coherent buffer and resource ledger |
| `tools/build-vulkan-resource-lifecycle.ps1` | Vulkan 1.1 SPIR-V builds (`-O0`/`-O`), `spirv-val` checks, Android ARM64 C build with `-Wall -Wextra -Werror` |
| `tools/run-vulkan-resource-lifecycle.ps1` | Explicit ADB device selection, four normal object graph cycles for each optimization, 14 deliberate cleanup failures, five rejected invalid inputs, logs and staged-file cleanup |
| `tools/test-vulkan-resource-lifecycle-evidence.py` | Offline verification of source hashes, optional three build artifact hashes, exact device metadata, cleanup logs and a deliberately unbalanced resource ledger |
| `tools/vulkan-pipeline-stage-a.comp` | Existing unchanged public synthetic compute shader; compiled but never dispatched by this experiment |
| `tests/fixtures/android-gpu/vulkan-lifecycle-20261031/` | Four real-device success logs, two fault-injection logs and SHA-256/build/device provenance manifest |

The scripts operate independently of `D:\PROJECTS\HASHCAT-APP` and do not install or edit the Android hashcat app.

## Reproduction from the Windows repository

The tested toolchain was Android NDK `29.0.14206865` (Clang 21.0.0; shaderc `glslc` from the bundled shader-tools), targeting Android AArch64 API 26 and Vulkan 1.1.

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/build-vulkan-resource-lifecycle.ps1 -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865

powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/run-vulkan-resource-lifecycle.ps1 -Serial $RedMagicAdbSerial -ReportPrefix redmagic -Cycles 4
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/run-vulkan-resource-lifecycle.ps1 -Serial $PixelAdbSerial -ReportPrefix pixel -Cycles 4

python -B tools/test-vulkan-resource-lifecycle-evidence.py --artifacts build/diagnostic-multimode/vulkan-lifecycle-20261031
```

Supply the actual device serials in the two PowerShell variables. Each run stages three files under `/data/local/tmp`, deletes them in the script's `finally` block and stores its three execution logs in the ignored `build/diagnostic-multimode/vulkan-lifecycle-20261031/` directory.

The offline validator also runs without the `--artifacts` option and then requires only the versioned source files and archived real-device evidence. With `--artifacts`, it additionally verifies the locally rebuilt ARM64 executable and both SPIR-V binaries are byte-identical to those used in the device executions. Source changes must update the recorded manifest and be accompanied by actual new device logs; the validator does not infer GPU success from local compilation alone.

## Real hardware outcomes

| Case | RedMagic NX709S / Adreno 730 | Pixel 9 Pro XL / Mali-G715 |
| --- | --- | --- |
| `-O0` public stage-A SPIR-V: four full lifecycles | PASS / exit 0 | PASS / exit 0 |
| `-O` public stage-A SPIR-V: four full lifecycles | PASS / exit 0 | PASS / exit 0 |
| Application resource acquisition/release ledger | 14 acquired, 14 released per normal cycle | 14 acquired, 14 released per normal cycle |
| All 14 injected failures after real Vulkan object creation | PASS / exit 42 | PASS / exit 42 |
| Five intentionally invalid CLI inputs | PASS / exit 2 | PASS / exit 2 |
| Compute dispatches submitted | 0 | 0 |

Both devices completed eight full object-graph lifecycles, **112 recorded resource acquisitions and 112 corresponding releases per device**. The 14 injected failures test 105 additional acquired-and-released objects per device across checkpoints (sum of 1 through 14), plus five input errors that do not create Vulkan objects. All fault-injection sequences released their acquired Vulkan resources before returning.

The manifest includes the original library vendor/driver IDs and reported Vulkan API versions taken from the same connected devices. Neither device was updated or flashed. All temporary diagnostic executables and SPIR-V modules were removed after testing, and the app's original bcrypt/OpenCL files have their previous SHA-256 hashes.

## What this establishes

The observed Vulkan 1.1 API on Adreno and Mali supports the tested generic pipeline/resource creation path and emits successful cleanup for early exits at each explicit checkpoint. Together with the previous independent multi-stage execution and command-buffer replay tests, this is useful evidence for a separate general-purpose compute runtime.

This does **not** implement a hashcat Vulkan backend, validate arbitrary concurrent GPU workloads, diagnose a Qualcomm driver-internal memory leak, or demonstrate a working Vulkan bcrypt implementation. The archived RedMagic OpenCL mode-3200 self-test exit **245** remains unresolved.
