# Vulkan 1.1 command-buffer reuse on Adreno and Mali (2026-10-31)

## Scope and observed outcome

A separate, standalone Android ARM64 Vulkan Compute executable records **one command buffer** with three noncryptographic compute stages and then submits **the same executable command buffer four consecutive times**. Before each submission, the CPU writes a distinct set of public synthetic 32-bit input values to one mapped, host-coherent storage buffer. The GPU writes to the same two intermediate buffers and final storage buffer on every submission.

The host waits for completion via a Vulkan fence before reading results or writing new input. It resets the already-signaled fence before the next submission. Both intermediate states and the final output are compared word-for-word with a scalar CPU calculation **after every submission**; results must change across submissions and match both optimizer variants.

All four submissions passed on the RedMagic NX709S (Adreno 730) and Pixel 9 Pro XL (Mali-G715), both with unoptimized `-O0` and optimized `-O` SPIR-V. Each run tested 49,152 words (4 submissions × 12,288 verified words); each phone ran two variants, so **98,304 words per phone** were verified. All results matched a separate Python oracle for the first four input generations.

The experiment executes **no hashcat kernel, bcrypt, passwords, hashes, cracking workload or self-test bypass**. The production module and OpenCL kernel are untouched.

## Vulkan command-buffer lifecycle

The executable `tools/vulkan-resubmit-diagnostic.c` creates a Vulkan 1.1 device, queue, storage buffer allocations, descriptors, three compute shader modules and pipelines, a command pool, **one primary command buffer** and **one reusable fence**.

The command buffer is recorded exactly once with `VkCommandBufferBeginInfo.flags = 0`, specifically **without** `VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT`. A Vulkan command buffer recorded with the one-time-submit bit becomes invalid after completion; without that bit it returns from the pending state to the executable state after GPU execution completes. This is defined in the [Khronos Vulkan command-buffer lifecycle specification](https://docs.vulkan.org/spec/latest/chapters/cmdbuffers.html).

The recorded commands include the following synchronization and dispatch stages:

```text
Host writes new public data in mapped, host-coherent input buffer
  -> HOST_WRITE -> SHADER_READ memory barrier
  -> compute stage A, 2048 items, write StateA
  -> SHADER_WRITE -> SHADER_READ barrier
  -> compute stage B, 2048 items, read StateA and write StateB
  -> SHADER_WRITE -> SHADER_READ barrier
  -> compute stage C, 2048 items, read StateA and StateB; write Output
  -> SHADER_WRITE -> HOST_READ barrier
  -> command-buffer completion; signal fence
  -> CPU waits for fence; checks 2048 + 2048 + 8192 words
  -> CPU prepares a different input; reset signaled fence
  -> submit the SAME recorded command buffer again
```

The workload has **three dispatches per submission**, two compute-to-compute barriers and one compute-to-host readback dependency; the reusable command buffer adds a host-to-compute dependency for input updates. The runner never submits a pending command buffer twice concurrently. Its 30-second fence wait and all Vulkan return codes are checked explicitly. Host-visible coherent memory removes the requirement for noncoherent mapping flush/invalidate but does not remove synchronization.

Reference: [Khronos Vulkan synchronization examples](https://docs.vulkan.org/guide/latest/synchronization_examples.html). The code uses the original `vkCmdPipelineBarrier` APIs available on Vulkan 1.1, not features requiring Vulkan 1.3.

## Files and reproduction

| File | Action |
| --- | --- |
| `tools/vulkan-resubmit-diagnostic.c` | Reusable 3-stage command buffer, new input generations, one fence with reset/wait, CPU comparison after each submission, exact error reporting and resource cleanup |
| `tools/build-vulkan-resubmit-diagnostic.ps1` | Compiles the existing three public GLSL shaders at `-O0` and `-O`, validates each Vulkan SPIR-V 1.1 module, and builds a standalone ARM64 executable using `-Wall -Wextra -Werror` |
| `tools/run-vulkan-resubmit-diagnostic.ps1` | Runs both shader optimization variants on an explicitly selected ADB device, verifies per-submission output and optimizer parity, records three logs, checks four intentionally rejected invalid inputs and removes all staged files |
| `tools/test-vulkan-resubmit-reference.py` | Independently calculates the exact four 32-bit input generations, 12,288 words per generation and three CPU FNV-1a checksums |
| `tools/test-vulkan-resubmit-evidence.py` | Validates source/build SHA-256, actual GPU logs, driver profiles, independently computed output checksums, negative controls and a deliberately wrong stage-B check |
| `tools/vulkan-pipeline-stage-{a,b,c}.comp` | The **unchanged** synthetic GLSL stage implementations used by the earlier three-stage diagnostic |
| `tests/fixtures/android-gpu/vulkan-resubmit-20261031/` | Exact SHA-256 manifest, four sanitized actual GPU execution logs and two negative-control logs |

Use the existing Android NDK installation. From `D:\PROJECTS\HASHCAT`:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/build-vulkan-resubmit-diagnostic.ps1 -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865

powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/run-vulkan-resubmit-diagnostic.ps1 -Serial $RedMagicAdbSerial -ReportPrefix redmagic -Submissions 4
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/run-vulkan-resubmit-diagnostic.ps1 -Serial $PixelAdbSerial -ReportPrefix pixel -Submissions 4

python -B tools/test-vulkan-resubmit-reference.py
python -B tools/test-vulkan-resubmit-evidence.py --artifacts build/diagnostic-multimode/vulkan-resubmit-20261031
```

Set `$RedMagicAdbSerial` and `$PixelAdbSerial` to the real serials of connected devices. The native runner accepts an explicitly validated `--submissions N` argument with `2 <= N <= 16` (the archived test uses N=4). Invalid numbers and a missing SPIR-V file produce **exit code 2**, not an implicit fallback to a different mode. The PowerShell runner asserts these four negative controls on both devices.

The build output directory is `build/diagnostic-multimode/vulkan-resubmit-20261031/` and is ignored by Git. The evidence validator accepts `--artifacts` to require byte-identical recompilation of all seven files; without this option the historical logs remain independently verifiable.

## Verified results

| Recorded GPU / driver | `-O0`, four submissions | `-O`, four submissions | Expected invalid inputs |
| --- | --- | --- | --- |
| Adreno 730, Vulkan 1.1.128 | PASS, exit 0 | PASS, exit 0 | 4 rejections, exit 2 |
| Mali-G715, Vulkan 1.4.305 | PASS, exit 0 | PASS, exit 0 | 4 rejections, exit 2 |

All submissions compare 2,048 StateA words, 2,048 StateB words and 8,192 output words with scalar CPU results. Both GPU devices produce exactly the same stage hashes for each submitted input generation:

| Submission | State A FNV-1a | State B FNV-1a | Output FNV-1a |
| --- | --- | --- | --- |
| 0 | `de56d6d0aa66c6ec` | `c1259ad4f024499a` | `05de70c62f8d7cfd` |
| 1 | `66ee1b4b1385e0e3` | `27fb4f34e64d89a7` | `7509bfa6ed0455d8` |
| 2 | `69b8f65ae2e8be72` | `d8038be5522a0b58` | `e50a539d4459e45b` |
| 3 | `8fb63465a93a5631` | `ac37a5c3b35fc366` | `07d63cfd9773324d` |

The four negative controls are: zero requested submissions, a count exceeding the explicit 16-submission limit, a noninteger count and a missing shader path. Each returned code 2 on both phones. The offline evidence regression also rejects a deliberately incorrect intermediate state B value for submission 2.

The original six SPIR-V shader binaries were rebuilt byte-for-byte; their hashes match the prior three-stage experiment. Only the separate replay-enabled host executable is new. Reused commands read the same descriptor-bound buffers but receive distinct host input values on each submission.

## Follow-up: Vulkan 1.0 reusable resource runtime

A separate self-contained Vulkan 1.0 runtime now owns the GPU resources, descriptor/pipeline lifetimes, command buffer and fence. It runs the same public three-stage synthetic integer workload with distinct input before each of four sequential submissions, and also supports real noncoherent CPU staging on devices exposing a compatible memory type. The runtime was tested on RedMagic ARM64 and ARMv7 and Pixel ARM64, all with default core Vulkan 1.0; optional Vulkan 1.1 was requested separately. In each of the three process/ABI targets, all 28 deterministic post-acquisition fault points returned explicit errors and released the partially acquired Vulkan objects. Original logs and independent CPU verification are in `docs/vulkan-core10-runtime-portability.md` and `tests/fixtures/android-gpu/vulkan-runtime-20261031/`. No hashcat or cryptographic GPU workload was executed.

## Scope limitations and next infrastructure tests

**Confirmed:** The tested Vulkan 1.1 drivers correctly execute sequential multiple-submit command-buffer reuse with updated coherent input, two intermediate compute barriers, final host readback, fence wait/reset and exact output comparisons. The reuse path works without Vulkan 1.3 or firmware replacement.

**Not tested:** Simultaneous submissions, multiple GPU queues, asynchronous scheduling, submission while a command buffer is pending, noncoherent memory, device loss and large-scale resource pressure. No result claims a Vulkan backend for hashcat or a corrected bcrypt kernel. The last real RedMagic OpenCL bcrypt self-test failure (exit 245) is a separate unresolved problem.

## Follow-up: Vulkan resource lifetime and fault injection

A separate standalone Vulkan 1.1 resource diagnostic now exercises fourteen owned objects and states: instance, logical device, storage buffer, host-coherent memory/mapping, descriptor and pipeline layouts, shader module, compute pipeline, descriptor pool/set, command pool/buffer and an unsignaled fence. It creates and releases the entire graph four times with each original synthetic stage-A shader optimization, on both RedMagic and Pixel.

All eight lifecycles per device balanced fourteen successful acquisitions with fourteen cleanup actions. A deliberately injected error at each of the fourteen checkpoints resulted in balanced cleanup and exit code 42 on **both devices**, while five invalid CLI inputs were rejected with exit code 2. The program never submits GPU work. Exact outcomes, source/binary hashes and archived failure records are in `docs/vulkan-resource-lifecycle-redmagic.md` and `tests/fixtures/android-gpu/vulkan-lifecycle-20261031/`.

These are Vulkan **application-side** ownership checks; they cannot prove the proprietary drivers are free of internal memory leaks or that any cryptographic workload is correct.
