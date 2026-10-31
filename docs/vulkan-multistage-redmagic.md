# Three-stage Vulkan Compute execution on RedMagic and Pixel (2026-10-31)

## Scope and result

A standalone Android ARM64 Vulkan Compute program executes **three different synthetic GLSL compute shaders** in one Vulkan command buffer. It uses four storage buffers (input, intermediate A, intermediate B and output), a shared descriptor-set layout, three independently compiled compute pipelines, two compute-to-compute memory barriers, and one compute-to-host readback barrier.

The C runner checks **every intermediate and final word** against its own scalar CPU calculation. A separate Python oracle computes checksums from independently implemented formulas, and an offline log validator checks the original device evidence and source/build digests.

This is an evaluation of the compute runtime architecture needed for multi-stage GPU workloads, **not a port of bcrypt, not a hashcat Vulkan backend and not a successful bcrypt self-test**. All numerical inputs are fixed, public, synthetic values.

## Implementation and synchronization

The complete data flow is:

```text
InputBuffer (2048 uint32)
      |
      v
 stage A: affine arithmetic, shift/rotate, XOR
      |
      v
StateA (2048 uint32) -- shader-write -> shader-read barrier
      |
      v
 stage B: shared[32], barrier(), cross-lane read
      |
      v
StateB (2048 uint32) -- shader-write -> shader-read barrier
      |                       |
      |                       v
      +------------------> stage C (reads StateA and StateB)
                              |
                              v
                       OutputBuffer (8192 uint32)
                              |
                   shader-write -> host-read barrier
                              |
                          GPU fence
                              |
                              v
                CPU checks StateA, StateB and Output
```

The application uses Vulkan 1.1 (`vkCmdPipelineBarrier`, `vkCmdDispatch`, storage-buffer descriptors, host-coherent mappings and `vkWaitForFences`). For each of the first two dispatches, the barrier has `srcStageMask=VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT`, `dstStageMask=VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT`, `srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT` and `dstAccessMask=VK_ACCESS_SHADER_READ_BIT`. The final barrier uses `dstStageMask=VK_PIPELINE_STAGE_HOST_BIT` and `dstAccessMask=VK_ACCESS_HOST_READ_BIT`.

These are the documented synchronization principles for sequential Vulkan compute dispatches:

- <https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdPipelineBarrier.html>
- <https://docs.vulkan.org/guide/latest/synchronization_examples.html>

No swapchain, presentation surface or graphics queue is needed: each device exposes a compute-capable queue and the original diagnostic's exact host-visible, host-coherent buffer allocation logic is used.

## Files and purpose

| File | Purpose |
| --- | --- |
| `tools/vulkan-pipeline-stage-a.comp` | Read 2048 public synthetic values and write 2048 stage-A words |
| `tools/vulkan-pipeline-stage-b.comp` | Read stage-A words, synchronize 32 local lanes, read seven-lanes-forward neighbor state, write 2048 stage-B words |
| `tools/vulkan-pipeline-stage-c.comp` | Read both intermediate arrays and write 8192 words (four per work item) |
| `tools/vulkan-multistage-diagnostic.c` | Independent Android ARM64 Vulkan 1.1 host program with three SPIR-V modules, pipelines, barriers, and full intermediate/final CPU readback |
| `tools/build-vulkan-multistage-diagnostic.ps1` | Reproducible GLSL to Vulkan SPIR-V compilation (`-O0` / `-O`), `spirv-val` checks and strict ARM64 C compilation |
| `tools/run-vulkan-multistage-diagnostic.ps1` | Device-specific ADB staging, two runs per device, saved logs, checksum parity and temporary-file cleanup |
| `tools/test-vulkan-multistage-reference.py` | Independent Python arithmetic, local workgroup neighbor mapping and FNV-1a 64-bit stage checksum reference |
| `tools/test-vulkan-multistage-evidence.py` | Offline verification of four real-device logs, sources, build identities, CPU checksums and a negative intermediate-state control |
| `tests/fixtures/android-gpu/vulkan-pipeline-20261031/` | Machine-readable manifest and four sanitized, archived device execution logs |

## Reproduction

Use Windows PowerShell in `D:\PROJECTS\HASHCAT` with the installed Android NDK r29 toolchain, shaderc `glslc` and `spirv-val`. A separate Vulkan SDK is not necessary.

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/build-vulkan-multistage-diagnostic.ps1 -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865

powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/run-vulkan-multistage-diagnostic.ps1 -Serial $RedMagicAdbSerial -ReportPrefix redmagic

powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/run-vulkan-multistage-diagnostic.ps1 -Serial $PixelAdbSerial -ReportPrefix pixel

python -B tools/test-vulkan-multistage-reference.py
python -B tools/test-vulkan-multistage-evidence.py --artifacts build/diagnostic-multimode/vulkan-pipeline-20261031
```

Set `$RedMagicAdbSerial` and `$PixelAdbSerial` to the actual connected-device serials. Both scripts reject missing artifacts and nonzero compilation or execution exit codes, rather than masking failures. The build writes all seven artifacts to the ignored `build/diagnostic-multimode/vulkan-pipeline-20261031/` directory. The run script saves local logs and deletes its seven temporary `/data/local/tmp` files in a `finally` block. No files inside either hashcat application installation are modified.

The `manifest.json` source hashes and build-file hashes identify the exact binaries corresponding to the archived outcomes. When `--artifacts` is provided, the evidence validator verifies all seven locally reproduced binary hashes against the archived manifest. Without that option, it verifies the tracked sources and original GPU logs without requiring untracked binaries. The real-device binaries themselves remain ignored, locally reproducible build outputs.

## Actual device results

| GPU | Vulkan API | No-opt SPIR-V | Optimized SPIR-V | Verified words per run |
| --- | --- | --- | --- | --- |
| RedMagic NX709S / Adreno 730 | 1.1.128 | **PASS / 0** | **PASS / 0** | 12288 |
| Pixel 9 Pro XL / Mali-G715 | 1.4.305 | **PASS / 0** | **PASS / 0** | 12288 |

Each run launches three compute shaders, dispatches 2048 threads per stage (64 workgroups of 32 lanes), and reads both intermediate buffers and the final output. The checked 12288 words consist of 2048 stage-A words, 2048 stage-B words and 8192 stage-C output words.

| Host-visible buffer | Expected FNV-1a 64-bit checksum | RedMagic -O0 / -O | Pixel -O0 / -O |
| --- | --- | --- | --- |
| Stage A | `de56d6d0aa66c6ec` | identical | identical |
| Stage B | `c1259ad4f024499a` | identical | identical |
| Final output | `05de70c62f8d7cfd` | identical | identical |

The independent Python oracle reproduces all three stage checksums. The evidence validator also rejects an intentionally wrong stage-B checksum. Previous independent Vulkan tests already checked arithmetic and 32 KiB of workgroup-shared memory on the same devices (see `docs/vulkan-compute-redmagic.md`).

The real-device outputs are documented separately from the offline Python validator. The validator reads the archived GPU logs; it does not execute new device kernels.

## Interpretation and migration boundaries

**Established:** The installed Qualcomm Vulkan driver supports the tested multi-dispatch workflow, descriptor sets, coherent buffer readback and GPU memory dependencies. The Adreno and Mali devices return the same independently checked intermediate and final values for both shader compiler optimization modes.

**Not established:** Correctness of the actual bcrypt algorithm, performance on realistic hashcat workloads, transfer of hashcat's OpenCL kernel ABI, or a working Vulkan device backend inside hashcat. The original RedMagic bcrypt mode-3200 OpenCL self-test still fails (archived exit code 245). The existing production OpenCL kernel, native module and self-test remain untouched.

A full application backend entails resource management beyond this demonstration: per-device capability discovery and selection, shader compilation and module caching, memory allocation and buffer lifetime, descriptor-layout ownership, pipeline lifetime, submission scheduling, synchronization, error propagation, shader ABI contracts, and independent compatibility tests for each supported workload. Replacing every existing hashcat OpenCL kernel is not a source-level translation and cannot be inferred from this synthetic success.

The next noncryptographic infrastructure checks would be multi-submission command-buffer reuse, buffer-lifetime failures, and stage-specific error reporting. The three-stage sample provides an isolated reference for those checks without linking to a password-cracking workflow.
