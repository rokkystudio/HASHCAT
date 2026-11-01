# Two-stage Vulkan 1.0 Compute execution inside Android HASHCAT

Date: 2026-11-01

## Scope

The experimental Android HASHCAT information mode `hashcat -I` executes
two **independent public, noncryptographic SPIR-V 1.0 Compute modules**
as one GPU command buffer. The first transforms 64 unsigned 32-bit
integers; the second reads the first stage's output from a Vulkan
storage buffer and transforms it again.

The first module is `public_u32_transform` from
`tools/vulkan-backend-public-kernel.comp`. The second is
`public_u32_rotate` from `tools/vulkan-backend-public-stage2.comp`.
Both use Vulkan core 1.0, `local_size_x=32` and two workgroups.

The production HASHCAT backend retains its original single-stage
public shader and empty-queue verification. In `hashcat -I` the
actual process opens an empty Compute session, runs the original
single-stage public kernel, executes the new dependent two-stage
chain, then reports normal OpenCL GPU information. The Vulkan
instance and physical-device catalogue remain backend-owned.

**No HASHCAT password-processing OpenCL kernel is ported by this
milestone.** Neither shader reads password candidates, hashes,
salts, attack modes or key material. Active Vulkan hashing
and scheduler integration remain disabled.

## Exact two-stage GPU contract

For each index `i` from 0 through 63 the first kernel evaluates
the following unsigned modulo-2^32 arithmetic:

```c
uint32_t input =
  UINT32_C (0x10203040) ^ (i * UINT32_C (0x01010101));

uint32_t intermediate =
  (input * UINT32_C (1664525) + UINT32_C (1013904223))
  ^ (i * UINT32_C (2654435769));
```

The second kernel reads the **GPU intermediate buffer**, not a
CPU-modified copy:

```c
uint32_t rotated = (intermediate << 9u) | (intermediate >> 23u);

uint32_t output =
  (rotated + i * UINT32_C (2246822519))
  ^ UINT32_C (0x6c8e9cf5);
```

One `VkDevice` owns three 256-byte storage buffers, two
`VkDescriptorSet`s, two `VkShaderModule`s, two
`VkComputePipeline`s, one command buffer and one VkFence.

Descriptor set 0 binds buffers 0/1 at bindings 0/1.
Descriptor set 1 binds buffers 1/2 at bindings 0/1.
The two Compute dispatches share the same actual physical queue:

1. Bind first pipeline and descriptor set, then `vkCmdDispatch(2,1,1)`.
2. Insert `VK_ACCESS_SHADER_WRITE_BIT` to
   `VK_ACCESS_SHADER_READ_BIT` for buffer 1, from
   `VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT` to itself.
3. Bind the second pipeline and descriptor set, then
   `vkCmdDispatch(2,1,1)`.
4. Insert shader-write-to-host-read barriers for **both**
   intermediate and final buffers.
5. Submit the command buffer, wait for its real Vulkan fence,
   invalidate noncoherent output memory and independently compare
   all **64 intermediate plus 64 final** words against CPU arithmetic.

A single discrepancy returns `VK_ERROR_INITIALIZATION_FAILED`
with the index, actual and expected hexadecimal words.
Each GPU buffer allocation uses the driver's exact
`VkMemoryRequirements`, `memoryTypeBits` and required allocation size.
If the chosen HOST_VISIBLE memory is noncoherent, cache synchronization
uses real `vkFlushMappedMemoryRanges` and
`vkInvalidateMappedMemoryRanges` and `nonCoherentAtomSize`.
There is no device-name or vendor-specific fallback.

The selected GPU must support `local_size=(32,1,1)`,
`group_count=(2,1,1)`, `storage_buffer_count=4` and
`largest_storage_buffer_bytes=256`. Unsupported limits fail directly.

## Method and resource documentation

The complete C implementation and per-method documentation are in
`source/src/backend_vulkan_kernel.c` and
`source/include/backend_vulkan_kernel.h`.

**`backend_vulkan_public_job_t`** owns exactly two or three
host-visible storage buffers and one or two pipeline/descriptor-set
pairs, according to its explicit stage count, plus the temporary
logical device, queue, command pool/buffer and fence. It does not
own the backend catalogue's VkInstance.

**`backend_vulkan_public_release(job)`** waits for VkDevice idle,
releases each non-null pipeline/shader/descriptor object, frees
command and fence resources, unmaps and frees buffer allocations
and destroys the temporary VkDevice. Partial construction is
supported. The actual idle result is returned after cleanup.

**`backend_vulkan_public_buffer_open(job,selected,buffer)`** creates
one storage buffer, selects an explicitly compatible host-visible
memory type from driver requirements, allocates/binds the required
memory and maps it. Errors propagate to the caller, which owns
partially acquired resources.

**`backend_vulkan_public_pipeline_open(job)`** accepts exactly one
or two stages, creates the two-binding STORAGE_BUFFER layout,
allocates one descriptor set per stage, binds consecutive buffer
pairs and creates a distinct Vulkan 1.0 shader/pipeline per stage.

**`backend_vulkan_public_dispatch(job,diagnostics)`** writes
public CPU inputs, flushes noncoherent memory, records stage
dispatches and their exact buffer barriers, submits one command
buffer, waits for the real fence, invalidates mapped outputs and
compares every intermediate/final output with independent CPU values.

**`backend_vulkan_kernel_verify_stages(ctx,index,stream,stages)`**
requires an open Vulkan catalogue, valid index, diagnostic stream
and stage count exactly 1 or 2. It validates Compute topology
and descriptor count using the real GPU's limits; creates, runs
and releases the temporary VkDevice without scheduling hashes.

**`backend_vulkan_kernel_verify(ctx,index,stream)`** verifies the
original first SPIR-V kernel and its 64 output values, preserving
its existing public diagnostic behavior.

**`backend_vulkan_kernel_verify_chain(ctx,index,stream)`** verifies
both ordered stages and all 128 intermediate/final words. The
two-stage path does not create a HASHCAT hashing device.

**`write_public_kernel_header(spirv_path,header_path,symbol,guard,source)`**
in `tools/generate-vulkan-backend-public-kernel.py` validates the
complete canonical SPIR-V 1.0 header and instruction alignment,
then writes a deterministic embedded C uint32 array. The first
shader's original header is byte-identical with default arguments;
explicit names generate the second shader's header.

## SPIR-V provenance and native builds

The first module contains **326 SPIR-V words / 1,304 bytes**.
The second module contains **363 words / 1,452 bytes**.
The SHA-256 of the second stage's compiled Vulkan 1.0 SPIR-V is:

```text
bb1ce39b44be765920bbf52fd7db66229e7b83a49b1487bdc64e1e190614e0a5
```

Both modules are independently recompiled and their complete embedded
headers checked by the two-stage build script using
`glslangValidator 14.3.0 --target-env vulkan1.0`. The new generated header lives at
`source/include/backend_vulkan_public_stage2_spirv.h` and embeds
the complete byte-identical SPIR-V module.

Compile real ARM64 and ARMv7 standalone diagnostics linked against
the same production function:

```powershell
.\tools\build-vulkan-backend-public-chain.ps1 -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865 -AndroidApi 24 -GlslangCompiler D:\ANDROID\SDK\emulator\lib64\vulkan\glslangValidator.exe
```

Build the complete, opt-in Android HASHCAT executables without APK
or release packaging:

```powershell
.\scripts\BUILD.ps1 -Target android-arm64-v8a -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865 -AndroidApi 24 -EnableVulkanDiscovery -VulkanOutputName vulkan-hashcat-public-chain-20261101 -Package:$false -MakeJobs 4

.\scripts\BUILD.ps1 -Target android-armeabi-v7a -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865 -AndroidApi 24 -EnableVulkanDiscovery -VulkanOutputName vulkan-hashcat-public-chain-20261101 -Package:$false -MakeJobs 4
```

All diagnostic builds use strict `-Wall -Wextra -Werror`.
The project `HASHCAT-APP`, its installed APK and its shipped
package archives are not altered.

## Physical Android GPU results

| Physical GPU and process ABI | Requested native Vulkan APIs | Standalone dispatches | Standalone verified words | Full HASHCAT -I dispatches |
| --- | --- | ---: | ---: | ---: |
| RedMagic Adreno 730 ARM64 | 1.0, 1.1 | 8 | 512 | 3 |
| RedMagic Adreno 730 ARMv7 | 1.0, 1.1 | 8 | 512 | 3 |
| Pixel 9 Pro XL Mali-G715 ARM64 | 1.0, 1.1 | 8 | 512 | 3 |
| **Total** | | **24** | **1,536** | **9** |

The three full `hashcat -I` processes each verified **192**
values: 64 from the original single-stage shader and both 64-word
buffers of the subsequent two-stage chain. All three reported normal
OpenCL GPU discovery and native exit zero.

Across nine physical process executions: **33 actual
`vkCmdDispatch` calls, 2,112 independently checked uint32 values,
and nine malformed native CLI requests rejected**.

The executable targets Android API 24, while the physical test
phones run newer operating systems and Vulkan drivers. No physical
Android 7.0 or Vulkan-1.0-only GPU was available for testing.

Run the two-stage GPU diagnostic:

```powershell
.\tools\run-vulkan-backend-public-chain.ps1 -Serial $DeviceAdbSerial -Abi arm64-v8a -ReportPrefix pixel-arm64 -TestCore11
```

Run a complete HASHCAT information-mode process:

```powershell
.\tools\run-vulkan-hashcat-public-chain.ps1 -Serial $DeviceAdbSerial -Abi arm64-v8a -ReportPrefix pixel-arm64
```

RedMagic additionally supports `-Abi armeabi-v7a`.

## Archived evidence and offline verifier

All 12 native UTF-8 LF-only runner logs and their SHA-256 identities
are archived at:

```text
tests/fixtures/android-gpu/vulkan-backend-public-chain-20261101/
  manifest.json
  redmagic-arm64-{1.0,1.1}.log
  redmagic-arm64-hashcat-chain-info.log
  redmagic-arm64-negative-controls.log
  redmagic-armv7-{1.0,1.1}.log
  redmagic-armv7-hashcat-chain-info.log
  redmagic-armv7-negative-controls.log
  pixel-arm64-{1.0,1.1}.log
  pixel-arm64-hashcat-chain-info.log
  pixel-arm64-negative-controls.log
```

Recreate and validate the archive using:

```powershell
python -B tools/archive-vulkan-backend-public-chain-evidence.py

python -B tools/test-vulkan-backend-public-chain-evidence.py --artifacts build/diagnostic-multimode/vulkan-backend-public-chain-20261101 --full build/diagnostic-multimode/vulkan-hashcat-public-chain-20261101 --readelf D:\ANDROID\SDK\ndk\29.0.14206865\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe
```

The independent verifier checks exact source hashes, two embedded
SPIR-V C arrays, Android API24 ELF architecture and `libvulkan.so`
imports, all 12 original device transcript identities, complete
event order, OpenCL coexistence and deliberate result mutations.

The archived logs preserve the **LF-normalized results saved by the
PowerShell ADB runner**, not raw unprocessed ADB byte streams.

## Verification boundaries

This confirms two real dependent Vulkan Compute shader pipelines
inside the actual HASHCAT information-mode process with intermediate
and final CPU verification. It does not test cryptographic kernels,
hash mode 0/22000, candidate generation, actual password recovery,
HASHCAT attack scheduling, real GPU device loss or concurrent
multi-job dispatch.

The next noncryptographic boundary is bounded repeated submissions
and buffer/pipeline reuse under one logical VkDevice.
