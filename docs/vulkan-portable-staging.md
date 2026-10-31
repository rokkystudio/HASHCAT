# Portable Vulkan 1.0 staging memory and transfer policy (2026-10-31)

## Goal and observed scope

The generic Vulkan diagnostic selects CPU/GPU data movement from **actual storage-buffer memory requirements** rather than a list of phone models or driver versions. It has two explicit execution modes:

- `--mode auto`: access storage buffers directly when *both* allow a compatible host-visible memory type; otherwise require separate staging upload/download buffers.
- `--mode staging`: exercise the staging copy path even when direct mapping is supported. This is a diagnostic control, not a required production performance setting.

The sample runs the unchanged, **public noncryptographic stage-A shader** from `tools/vulkan-pipeline-stage-a.comp`, compiled for Vulkan 1.0. It checks all 2,048 GPU output words against an independent scalar CPU reference; the FNV-1a digest is `00d6d6e4837edebc`.

The standalone module is **not integrated into hashcat**, does not load a hashcat module, and does not execute any password-recovery algorithm.

## Reusable API and memory policy

| File | Responsibility |
| --- | --- |
| `tools/vulkan-auto-config.h`, `tools/vulkan-auto-config.c` | Existing **unchanged** capability, queue and host-visible memory selection API; previous reproducible source identities remain valid |
| `tools/vulkan-staging-memory.h`, `tools/vulkan-staging-memory.c` | New separate GPU-only memory selector and automatic direct/staging transfer policy |
| `tools/vulkan-staging-diagnostic.c` | Standalone Vulkan 1.0 compute program using direct mapping or two real GPU copies, with exact resource cleanup and error reporting |
| `tools/build-vulkan-staging.ps1` | Strict Android ARM64 `-Wall -Wextra -Werror` build, Vulkan 1.0 SPIR-V builds with `-O0` and `-O`, `spirv-val` validation |
| `tools/run-vulkan-staging.ps1` | Scripted two-mode, two-optimization GPU test for one explicitly selected ADB device, four invalid CLI cases, temporary file cleanup and logs |
| `tools/test-vulkan-staging-evidence.py` | Offline validation of real-device GPU logs, independent CPU checksum, memory type compatibility, failure controls, binary provenance and intentionally corrupted log cases |

The source contains **no GPU vendor-specific selection cases**. The module `vulkan_auto_choose_transfer_mode` first verifies that both storage buffers have at least one compatible GPU memory type. It then uses `vulkan_auto_select_memory_type` for each buffer:

- If *both* have `HOST_VISIBLE` compatible memory and staging is not forced, direct CPU mapping is selected. A `HOST_COHERENT` type is preferred when compatible.
- Otherwise the program requires staging. Compute buffers use `vulkan_auto_select_gpu_memory_type`, preferring compatible `DEVICE_LOCAL` memory. The staging upload and readback buffers independently select compatible `HOST_VISIBLE` memory, preferring `HOST_COHERENT`.
- If a required role has no compatible memory type, the program emits the actual role, compatibility bitmask and Vulkan failure code. No guessed memory index, vendor override or silent bypass is used.

All buffer requirements are obtained from `vkGetBufferMemoryRequirements` **after** the buffers are created. For this standalone diagnostic there are two storage buffers of 8,192 bytes; staging requires two additional transfer buffers of 8,192 bytes. Each buffer gets a separate, requirement-sized allocation. Allocation requests exceeding the diagnostic's explicit 1 MiB per-buffer safety cap fail rather than silently changing the workload. The cap belongs to this example, not to the reusable memory selector.

## Synchronization

For actual staging, the Vulkan 1.0 command buffer contains:

```text
CPU maps staging-upload, writes 2048 public 32-bit inputs
  -> flush full allocation if noncoherent
  -> HOST_WRITE -> TRANSFER_READ barrier
  -> vkCmdCopyBuffer(upload -> gpu-input)
  -> TRANSFER_WRITE -> SHADER_READ barrier
  -> vkCmdDispatch(stage A), read gpu-input, write gpu-output
  -> SHADER_WRITE -> TRANSFER_READ barrier
  -> vkCmdCopyBuffer(gpu-output -> staging-download)
  -> TRANSFER_WRITE -> HOST_READ barrier
  -> submit, wait fence
  -> invalidate full staging-download allocation if noncoherent
  -> verify all 2048 CPU/GPU words and 64-bit digest
  -> wait idle, destroy all Vulkan objects and free memory
```

The direct-mapped path uses `HOST_WRITE -> SHADER_READ`, Compute dispatch, `SHADER_WRITE -> HOST_READ`, fence wait and the required cache operations. Both paths also initialize the output buffer with a fixed byte pattern and flush it when memory is noncoherent before submitting GPU work.

Mapped memory uses `VK_WHOLE_SIZE` for flush/invalidate and is mapped from offset zero for the entire allocation. This avoids partially aligned cache range problems; in both paths allocations for different roles are separate. The program does not assume host-visible memory is coherent.

These barrier and buffer-copy patterns follow the Vulkan [synchronization examples](https://docs.vulkan.org/guide/latest/synchronization_examples.html), [copy command specification](https://docs.vulkan.org/spec/latest/chapters/copies.html) and [staging-buffer guide](https://docs.vulkan.org/tutorial/latest/04_Vertex_buffers/02_Staging_buffer.html).

## Device observations

The **earlier combined-source test executable** was run against both phones using both modes and both shader optimization variants:

| Device | Requested mode | Selected mode | GPU copies | Output |
| --- | --- | --- | ---: | --- |
| RedMagic NX709S / Adreno 730 | auto | direct | 0 | PASS, `-O0` and `-O` |
| RedMagic NX709S / Adreno 730 | staging | staging | 2 | PASS, `-O0` and `-O` |
| Pixel 9 Pro XL / Mali-G715 | auto | direct | 0 | PASS, `-O0` and `-O` |
| Pixel 9 Pro XL / Mali-G715 | staging | staging | 2 | PASS, `-O0` and `-O` |

All eight observed GPU runs calculated the same 2,048-word FNV-1a checksum `00d6d6e4837edebc`; each phone also rejected four invalid CLI requests with exit code 2.

The chosen memory types were:

| Device | Direct input/output | Staging GPU input/output | Staging CPU upload/download |
| --- | --- | --- | --- |
| Adreno 730 | type 6, coherent | type 0, not CPU-mapped | type 6, coherent |
| Mali-G715 | type 0, coherent | type 0, not CPU-mapped | type 0, coherent |

The same Mali memory type can be used for GPU-only storage and mapped staging, because the API permits both. For staging, `cpu_mapped=0` in the logs describes the **allocation's use in this program**, not necessarily the physical memory type's `HOST_VISIBLE` property.

A separate synthetic memory-property test confirmed that GPU-only memory with no compatible host-visible type selects staging automatically. The two connected real devices both support direct mapping and therefore selected direct mode for `--mode auto`. **Automatic fallback on a real GPU that lacks directly mappable storage memory has not yet been observed.** Neither device exercised the noncoherent staging cache path; its selection and explicit synchronization code compiled, but a real GPU validation using noncoherent memory remains unverified.

## Precise build provenance

The GPU observations were captured with the initial standalone executable that compiled the added staging selection functions in the same translation unit as the universal config functions:

- **Actually tested on both GPUs:** ELF SHA-256 `8127abccf5beb7368877f1db2fef1b857b495e655ceac17e3c19738c5ff52f90`, 30,368 bytes.
- **Current modular build:** ELF SHA-256 `104110b5d176c1eb43e79b00b1e9ace5fccb912a481d97d997c593cb0073dfa3`. It builds with strict compiler warnings and links the existing universal API plus the separate staging module. **The current modular binary has not been executed on the devices following this separation.**

The archive at `tests/fixtures/android-gpu/vulkan-staging-20261031/` preserves this distinction in its JSON manifest. It contains eight real GPU logs and two invalid-input logs recorded before the separate-library build. The regression validator checks that the current modular binary hash differs from the actually tested historical binary; it **must not** report the newer binary as hardware validated.

The unchanged `tools/vulkan-auto-config.c` and `tools/vulkan-auto-config.h` match their prior archived SHA-256 in `tests/fixtures/android-gpu/vulkan-autoconfig-20261031/manifest.json`. The existing Vulkan 1.0 capability and memory evidence remains independently verifiable.

## Offline and eventual hardware reproduction

Build on the Windows project checkout using Android NDK r29:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/build-vulkan-staging.ps1 -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865

python -B tools/test-vulkan-staging-evidence.py --artifacts build/diagnostic-multimode/vulkan-staging-20261031
```

The standalone hardware runner, when its use is permitted, takes an explicit ADB device identifier:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/run-vulkan-staging.ps1 -Serial $DeviceAdbSerial -ReportPrefix device
```

That command is provided to document the exact test interface, not as evidence of a run of the final modular build. The runner executes `auto` and `staging` on both `-O0` and `-O`, checks every buffer's permitted type and readback checksum, records device logs, tests four invalid inputs and deletes its staged files using a `finally` block.

## Exhaustive native-C policy verification for unknown GPUs (2026-10-31)

A separate test `tools/test-vulkan-staging-policy-host.c` compiles the **actual** universal Vulkan 1.0 and staging memory selection sources into a Windows native program. It replaces only Vulkan's physical-device query and mapped-memory cache entry points with deterministic, in-process mock functions. No Vulkan loader, Android device or GPU driver is linked or called. The PE import table has only `KERNEL32.dll` and `msvcrt.dll`.

The test uses four [Vulkan-supported memory property classes](https://docs.vulkan.org/refpages/latest/refpages/source/VkPhysicalDeviceMemoryProperties.html): `DEVICE_LOCAL`, `HOST_VISIBLE|HOST_CACHED` (noncoherent), `HOST_VISIBLE|HOST_COHERENT`, and `DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT`. It assigns each class to three synthetic memory-type slots and enumerates all seven nonempty three-bit compatibility masks.

| Exact C policy coverage | Number |
| --- | ---: |
| Distinct three-type memory-property layouts | 64 (4³) |
| Nonempty compatibility masks per input/output buffer | 7 |
| Direct-auto and forced-staging requests | 2 |
| Actual selector checks per individual mask | 448 |
| **Actual transfer-path decisions** | **6,272** (64 × 7 × 7 × 2) |
| Compiler optimization builds | `-O0` and `-O2` |
| Native-C test groups | 6 |

Additional deterministic checks cover the extreme 32nd Vulkan memory type (bit 31), missing or disjoint type bitmasks, invalid memory requirements, 64-bit allocation overflow, alignment to a noncoherent 256-byte cache atom, compute workgroup/invocation limits, descriptor/storage limits, and queue-family discovery when the first family does **not** support Compute.

The mock `vkFlushMappedMemoryRanges` and `vkInvalidateMappedMemoryRanges` record and assert calls with `offset=0`, `size=VK_WHOLE_SIZE` and the correct `VkDeviceMemory` handle. They also inject Vulkan error codes to verify that they propagate. When an entire memory allocation is mapped from zero, `VK_WHOLE_SIZE` covers the mapped allocation end and obeys the specified [noncoherent mapped-range alignment constraints](https://docs.vulkan.org/refpages/latest/refpages/source/VkMappedMemoryRange.html).

Both native GCC builds pass all 6,272 transfer decisions with **identical** outputs. The regression scripts are:

- `tools/run-vulkan-staging-policy-host.ps1` — strict MSYS2 GCC build, actual compiled C execution at `-O0` and `-O2`, PE import check and reproducible logs
- `tools/test-vulkan-staging-policy-host-evidence.py` — offline source and executable SHA-256 checks, exact log validation, independent optimization parity and deliberate corrupted-log rejection
- `tests/fixtures/vulkan-host-policy-20261031/` — two original native host logs and provenance manifest

The manifest records source-file SHA-256 over UTF-8 text with **CRLF normalized to LF**, so Windows Git checkout line endings do not cause false source mismatches. Windows executables and original execution logs are always verified by their **exact raw bytes**. The native runner checks the PE imports of both binaries before accepting the results.

Reproduction from the Windows project root:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/run-vulkan-staging-policy-host.ps1 -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865 -HostGcc C:\msys64\mingw64\bin\gcc.exe

python -B tools/test-vulkan-staging-policy-host-evidence.py --artifacts build/diagnostic-multimode/vulkan-staging-policy-host-20261031
```

This validates the policy's exact **C behavior**, not a third GPU model's Vulkan driver. Automatic staging fallback and noncoherent cache synchronization still require hardware observation on actual devices exposing those capabilities. No hashcat execution is involved.

## Compatibility boundaries

This is a portable **synthetic Vulkan Compute data-transfer module**. It does not implement generic pipeline scheduling, an integrated Vulkan backend for hashcat, a cryptographic kernel, vendor-specific GPU workarounds or a guarantee that every device supports the shader's requirements.

The OpenCL bcrypt mode-3200 self-test failure 245 on RedMagic remains a separate unresolved issue. The original hashcat CPU and OpenCL bcrypt source/binaries are not modified by this experiment.
