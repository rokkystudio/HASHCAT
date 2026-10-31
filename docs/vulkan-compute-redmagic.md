# Android Vulkan Compute compatibility experiment (2026-10-31)

## Goal and limits

This is a standalone, noncryptographic Vulkan Compute experiment for the connected RedMagic NX709S / Qualcomm Adreno 730 and Pixel 9 Pro XL / Mali-G715. It tests whether the two devices execute the same shader operations correctly when using Vulkan rather than the existing OpenCL path.

The experiment does **not** convert, compile, launch, bypass, or benchmark hashcat's bcrypt mode 3200; it does not accept hashes or password candidates. It does not establish that bcrypt will work under Vulkan. The earlier RedMagic OpenCL bcrypt self-test failure (exit 245) remains unresolved.

## Components

| File | Purpose |
| --- | --- |
| `tools/vulkan-compute-diagnostic.comp` | Vulkan 1.1 GLSL compute: 32-bit unsigned arithmetic, 32 lanes, shared-memory neighbor exchange and `barrier()` |
| `tools/vulkan-compute-shared32k.comp` | 8192 shared `uint` words (32768 bytes) per workgroup, a barrier and 256 cross-lane dependent reads per thread |
| `tools/vulkan-compute-diagnostic.c` | Standalone Android ARM64 Vulkan loader, compute pipeline, two mapped coherent storage buffers, dispatch, GPU-to-host barrier, 30-second fence and exact CPU comparison |
| `tools/build-vulkan-compute-diagnostic.ps1` | Compiles four Vulkan 1.1 SPIR-V shaders with `glslc`, validates them with `spirv-val` and cross-compiles the C runner with Android NDK Clang |
| `tools/run-vulkan-compute-diagnostic.ps1` | Runs all four variants on one explicitly selected ADB device, writes local logs and deletes the five staged files from `/data/local/tmp` |
| `tools/test-vulkan-compute-reference.py` | Independent Python arithmetic and shared-memory reference with expected final checksums |
| `tools/test-vulkan-compute-evidence.py` | Validates eight archived real-device logs, exact driver metadata, SHA-256 hashes and independently computed checksums without executing GPU code |
| `tests/fixtures/android-gpu/vulkan-compute-20261031/manifest.json` | Device profiles, toolchain identification, source/build hashes and archived execution provenance |
| `tests/fixtures/android-gpu/vulkan-compute-20261031/*.log` | Eight device execution logs with exit codes, limits and verified GPU output checksums (no device serial numbers) |

The synthetic input comprises 2048 distinct 32-bit words. Each dispatch has 64 workgroups of 32 local invocations and writes 8192 result words. Shared-memory results are checked word-for-word by the C runner on the device and independently by the Python reference on the Windows host.

## Reproduce on the Windows PC

Use the Android NDK already installed for the project, with the Vulkan headers, `libvulkan.so` linker stub, `glslc` and `spirv-val`. The installed toolchain is Android NDK `29.0.14206865`: Clang 21.0.0 and shaderc v2022.3. All four shaders target Vulkan 1.1; no system Vulkan SDK, external Vulkan library, Android Studio project or APK is required.

From `D:\PROJECTS\HASHCAT`:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/build-vulkan-compute-diagnostic.ps1 -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865

powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/run-vulkan-compute-diagnostic.ps1 -Serial $RedMagicAdbSerial -ReportPrefix redmagic

powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/run-vulkan-compute-diagnostic.ps1 -Serial $PixelAdbSerial -ReportPrefix pixel

python -B tools/test-vulkan-compute-reference.py
python -B tools/test-vulkan-compute-evidence.py
```

Set the two `$*AdbSerial` PowerShell variables to the corresponding devices' actual ADB serials before running the commands. Each script rejects compilation, SPIR-V validation, execution, missing output or CPU/GPU comparison errors instead of treating them as successful tests.

Build outputs and local execution logs reside under `build/diagnostic-multimode/vulkan-compute-20261031/`, which is excluded from Git. Exact binaries and shaders can be reproduced from the committed sources; the eight original on-device outputs and the manifest are separately versioned under `tests/fixtures/android-gpu/`.

## Real hardware results

| Feature | RedMagic NX709S | Pixel 9 Pro XL |
| --- | --- | --- |
| Vulkan device | Adreno (TM) 730 | Mali-G715 |
| Reported Vulkan API version | 1.1.128 | 1.4.305 |
| Reported Vulkan driver version (raw) | `0x80267000` | `0x0d802000` |
| Maximum compute workgroup size X | 1024 | 1024 |
| Maximum compute invocations | 1024 | 1024 |
| Maximum workgroup shared memory | 32768 bytes | 32768 bytes |
| Baseline SPIR-V without optimization | PASS, exit 0 | PASS, exit 0 |
| Baseline SPIR-V with optimization | PASS, exit 0 | PASS, exit 0 |
| 32 KiB shared-memory SPIR-V without optimization | PASS, exit 0 | PASS, exit 0 |
| 32 KiB shared-memory SPIR-V with optimization | PASS, exit 0 | PASS, exit 0 |

All eight runs executed the Vulkan API and real GPU compute pipeline, with 8192 of 8192 output words matching the scalar on-device CPU reference. The archived logs were generated by repeat runs of the versioned runner scripts; the standalone shaders use exactly the same source on both devices.

| Synthetic compute mode | CPU / RedMagic / Pixel FNV-1a 64-bit result |
| --- | --- |
| Baseline, 128-byte workgroup shared storage | `967d964855b53334` |
| Full 32768-byte workgroup shared storage | `0c099d93a929bc82` |

Both optimizations generated the same output for each mode. The independent Python scalar implementation verifies both checksums with explicit unsigned 32-bit wrapping. In the 32 KiB case, all 8192 scratch words are written and used; 32 threads perform 256 ordered cross-lane reads each after a workgroup memory barrier.

Build hashes of the standalone executable and four SPIR-V files are recorded in the manifest. `spirv-val --target-env vulkan1.1` passes all four modules. Neither device needed firmware changes or a privileged Vulkan loader.

## Interpretation

- **Confirmed:** The RedMagic's installed Qualcomm Vulkan driver creates compute pipelines and correctly executes these particular 32-bit arithmetic, barrier, and 32 KiB workgroup memory tests. Matching Pixel output independently supports their correctness.
- **Not confirmed:** All Vulkan compiler optimizations, execution with the bcrypt algorithm, handling of the original bcrypt temporary buffers or correct GPU password verification.
- **Not implied:** That the Adreno OpenCL compiler is correct or incorrect. A successful Vulkan Compute program follows a different shader compilation path, but both APIs still ultimately use Qualcomm's GPU driver.
- **Migration cost:** Hashcat mode 3200 currently uses the host OpenCL backend and three kernel stages. Vulkan requires an explicit device/queue/pipeline and descriptor/memory/synchronization host backend. This synthetic experiment does not introduce such a hashcat backend.

The existing Android application, original `source/OpenCL/m03200-pure.cl` and production `module_03200.so` were not modified or overwritten. Only dedicated files under `/data/local/tmp` were created for each test and then removed. The results support investigating Vulkan as an alternative execution API, but not declaring the OpenCL bcrypt defect resolved.

## Follow-up: real three-stage Vulkan pipeline

A subsequent noncryptographic prototype uses **three separate compute pipelines**, two intermediate storage buffers, compute-to-compute barriers, an output buffer and host readback. Both Adreno 730 and Mali-G715 passed all 12288 exact word comparisons on `-O0` and `-O` variants, including intermediate stages. See `docs/vulkan-multistage-redmagic.md`, `tools/vulkan-multistage-diagnostic.c` and archived evidence under `tests/fixtures/android-gpu/vulkan-pipeline-20261031/`. This demonstrates the multi-stage Vulkan synchronization path, not a migrated hashcat mode.
