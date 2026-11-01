# First production-owned Vulkan 1.0 Compute kernel in Android HASHCAT

Date: 2026-11-01

## Scope

The Android HASHCAT v7.1.2 executable now includes one genuine
**Vulkan 1.0 SPIR-V Compute pipeline**, invoked in the existing
`backend_ctx_devices_init` information-mode path for `hashcat -I`.

Unlike the earlier standalone Vulkan diagnostics, the actual
`hashcat` process now creates `VkShaderModule`, allocates a
`VkDescriptorSet` containing two storage buffers, creates a real
`VkComputePipeline`, records and submits `vkCmdDispatch(2,1,1)`,
waits on a real Vulkan fence, and independently compares **all
64 GPU output words** to a CPU reference implementation.

The first shader is `public_u32_transform`: an intentionally
noncryptographic unsigned 32-bit arithmetic test on public
generated integers. It verifies that the production backend
can execute shader bytecode rather than only enumerate devices
or submit an empty command buffer.

**No existing OpenCL hashcat kernel is converted here**.
There is no password candidate, hash, salt, target digest,
cryptographic shader, attack-mode dispatch or benchmark.
The production CUDA/HIP/Metal/OpenCL hashing schedulers and
device arrays remain unchanged.

## First GPU kernel: exact execution contract

Shader source:
`tools/vulkan-backend-public-kernel.comp`

The GLSL 450 shader targets SPIR-V **1.0**, with exactly:

- `layout(local_size_x=32,local_size_y=1,local_size_z=1)`.
- One `readonly` `std430` input storage-buffer array at
  descriptor set 0, binding 0.
- One `writeonly` `std430` output storage-buffer array at
  descriptor set 0, binding 1.
- Two 32-lane Compute workgroups, giving exactly 64 invocations.
- No shared memory, barriers, optional Vulkan features or
  extensions, atomics or cryptographic routines.

Each invocation evaluates:

```glsl
const uint index = gl_GlobalInvocationID.x;
const uint value = input_buffer.words[index];

output_buffer.words[index] =
  (value * 1664525u + 1013904223u) ^ (index * 2654435769u);
```

The CPU separately calculates the same specified 32-bit unsigned
arithmetic from 64 public inputs:

```c
input[i] = UINT32_C (0x10203040) ^ (i * UINT32_C (0x01010101));

expected =
  (input[i] * UINT32_C (1664525) + UINT32_C (1013904223))
  ^ (i * UINT32_C (2654435769));
```

The native method returns success **only when every mapped GPU
output equals its independently calculated CPU word**. A
single mismatch returns `VK_ERROR_INITIALIZATION_FAILED`,
prints the actual and expected 32-bit word and destroys the
complete temporary Vulkan device. The shader and CPU method
operate on unsigned arithmetic with defined modulo-2^32
overflow, without heuristic interpretation of GPU results.

The diagnostic performs 64×4 = **256 bytes** of storage
input and 256 bytes of storage output per dispatch.
The shader contains no bounds predicate because exactly
64 invocations are launched for two 64-element allocations.

## Exact portable resource requirements

Before opening a device,
`backend_vulkan_kernel_verify(backend_ctx,index,diagnostics)`
asks the existing physical-device catalogue to verify:

```c
{
  .local_size = { 32u, 1u, 1u },
  .group_count = { 2u, 1u, 1u },
  .storage_buffer_count = 2u,
  .largest_storage_buffer_bytes = 256u,
  .shared_memory_bytes = 0u
}
```

Driver-reported limits must support the explicit topology and
buffer descriptor sizes. The code uses the selected Compute
queue family and no GPU manufacturer or Android model heuristics.

The public Compute job creates its **own temporary VkDevice**
under the same backend-owned `VkInstance`. This allows it to
execute while the existing information-mode session retains
its separate VkDevice that performed the previous empty
queue submission.

The two VkBuffers use `VK_BUFFER_USAGE_STORAGE_BUFFER_BIT`.
Each receives an independent allocation sized according to
`vkGetBufferMemoryRequirements`. Memory selection uses the
driver's exact `memoryTypeBits` and the existing
`vulkan_auto_select_memory_type` policy:

- Prefer compatible `HOST_VISIBLE|HOST_COHERENT` memory.
- Use compatible `HOST_VISIBLE` noncoherent memory when no
  coherent type exists, with explicit
  `vkFlushMappedMemoryRanges` on input and
  `vkInvalidateMappedMemoryRanges` on output.
- Return a real Vulkan feature error when no compatible
  host-visible memory exists. The method does not silently
  invent a staging allocation or vendor-specific fallback.

The dispatched primary command buffer records a Vulkan
`VK_ACCESS_SHADER_WRITE_BIT` to `VK_ACCESS_HOST_READ_BIT`
buffer-memory barrier using
`VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT` and
`VK_PIPELINE_STAGE_HOST_BIT`.

A real `vkWaitForFences` completes the submission before
host readback. The logical device waits idle before its
pipeline, descriptor set/pool, shader, buffers, memory,
command pool, fence and VkDevice are destroyed.

## Complete production methods and files

`source/src/backend_vulkan_kernel.c` implements:

### `backend_vulkan_public_release(job)`

Waits for the temporary Vulkan device to become idle,
destroys the pipeline/shader/descriptor resources, fence
and command objects, unmaps and frees both storage
allocations, destroys the logical device and clears the
owner. Partial construction is allowed. The VkInstance
always remains owned by `backend_ctx`.

### `backend_vulkan_public_buffer_open(job,selected,buffer)`

Creates one 256-byte storage buffer, requests actual
`VkMemoryRequirements`, selects a matching host-visible
memory type, allocates the driver's required size, binds
the memory and maps the complete allocation. Returns the
exact Vulkan error on failure without guessing a memory type.

### `backend_vulkan_public_pipeline_open(job)`

Creates a two-binding storage descriptor set layout,
pipeline layout, descriptor pool and descriptor set,
updates the two exact buffer descriptors, loads the
embedded SPIR-V 1.0 module and creates a one-stage
Compute pipeline with entry point `main`.

### `backend_vulkan_public_dispatch(job,diagnostics)`

Fills 64 public input integers, flushes noncoherent CPU
writes, binds the pipeline and descriptors, issues one
`vkCmdDispatch(2,1,1)`, records the buffer memory
visibility dependency, submits to the GPU, waits for
the fence and invalidates output memory when required.
Checks all 64 output words with the CPU arithmetic
reference and emits a deterministic success or mismatch.

### `backend_vulkan_kernel_verify(backend_ctx,index,diagnostics)`

Selects the exact physical device from the already
opened HASHCAT Vulkan catalogue, checks the workload's
real limits, owns a temporary VkDevice and its Compute
resources, dispatches the public shader, verifies the
output, releases everything and propagates the actual
`VkResult`. It is invoked from the
`user_options->backend_info > 0` path in
`source/src/backend.c` only.

Declarations and individual method documentation are
in `source/include/backend_vulkan_kernel.h` and in the
complete implementation. The generated immutable shader
array is in
`source/include/backend_vulkan_public_spirv.h`.

`source/src/Makefile` compiles the new Vulkan kernel
object only when `ENABLE_VULKAN_DISCOVERY=1` on an
explicit Android target. Standard non-Vulkan builds
do not add a `libvulkan.so` dependency.

## Embedded SPIR-V reproducibility

The source of the SPIR-V module is checked in as
`tools/vulkan-backend-public-kernel.comp`. The
generated header stores exactly **326 SPIR-V words,
1,304 bytes**, containing the SPIR-V 1.0 magic and
version 0x00010000.

The public shader was compiled with
`glslangValidator 14.3.0 --target-env vulkan1.0`.

Recompile the bytecode with an installed GLSL-to-SPIR-V
compiler:

```powershell
& D:\ANDROID\SDK\emulator\lib64\vulkan\glslangValidator.exe -V --target-env vulkan1.0 -S comp -e main -o build/diagnostic-multimode/vulkan-backend-public-kernel.spv tools/vulkan-backend-public-kernel.comp

python -B tools/generate-vulkan-backend-public-kernel.py --spirv build/diagnostic-multimode/vulkan-backend-public-kernel.spv --header source/include/backend_vulkan_public_spirv.h
```

The independent offline verifier requires byte-for-byte
identity between compiled SPIR-V and the embedded 326
`uint32_t` words. It also regenerates the C header and
checks that its complete bytes match the checked-in
file, so the native shader source and packaged bytecode
cannot silently diverge.

SPIR-V SHA-256:
`bd1f85baee40cb3c4db70ddd4a1e86af8428521951a8363445ef95d1b92f77a9`.

## Building real Android HASHCAT executables

Run the NDK standalone API24 compile/link probe:

```powershell
.\tools\build-vulkan-backend-public-kernel.ps1 -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865 -AndroidApi 24
```

This compiles the real production `backend.c` with and
without `WITH_VULKAN_DISCOVERY` and links the exact
`backend_vulkan_kernel_verify` implementation into
two standalone physical GPU diagnostics.

Build complete HASHCAT frontends and native plugins for
both process ABIs, without creating a release package:

```powershell
.\scripts\BUILD.ps1 -Target android-arm64-v8a -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865 -AndroidApi 24 -EnableVulkanDiscovery -VulkanOutputName vulkan-hashcat-public-kernel-20261101 -Package:$false -MakeJobs 4

.\scripts\BUILD.ps1 -Target android-armeabi-v7a -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865 -AndroidApi 24 -EnableVulkanDiscovery -VulkanOutputName vulkan-hashcat-public-kernel-20261101 -Package:$false -MakeJobs 4
```

Both ABI outputs contain real `hashcat`, its native
modules, bridges and OpenCL runtime assets.
No `HASHCAT-APP` APK, installed app or published release
archive is modified.

## Real Android GPU results

All tests below ran on physically connected Android GPUs.

| Actual Android GPU | Process ABI | Requested API in standalone test | Actual dispatches | Verified public words |
| --- | --- | --- | ---: | ---: |
| RedMagic Adreno 730 | ARM64 | Vulkan 1.0 and 1.1 | 4 | 256 |
| RedMagic Adreno 730 | ARMv7 | Vulkan 1.0 and 1.1 | 4 | 256 |
| Pixel 9 Pro XL Mali-G715 | ARM64 | Vulkan 1.0 and 1.1 | 4 | 256 |
| **Standalone total** | | | **12** | **768** |

Additionally, **three complete production HASHCAT
`hashcat -I` processes** (RedMagic ARM64, RedMagic
ARMv7 and Pixel ARM64) each executed the embedded
public kernel through `backend_ctx_devices_init`:

| Actual hashcat -I process | Requested Vulkan API | GPU dispatches | Result |
| --- | --- | ---: | --- |
| RedMagic ARM64 | 1.0 | 1 | PASS, OpenCL GPU detected |
| RedMagic ARMv7 | 1.0 | 1 | PASS, OpenCL GPU detected |
| Pixel ARM64 | 1.0 | 1 | PASS, OpenCL GPU detected |
| **Total** | | **3** | **3/3 PASS** |

Across all nine native diagnostic and full HASHCAT
processes: **15 real `vkCmdDispatch` operations and
960 exact public integer outputs verified on GPU**.

The full production `hashcat -I` logs additionally
show the existing empty Vulkan queue session, two
physical-device selections under the same VkInstance,
the new Compute pipeline, and independent Vulkan
resource closure. Normal OpenCL GPU enumeration
continues after the shader verification. The exit code
is 0 for all three processes.

Physical hardware reports newer Vulkan loader/device
versions, even though the app requests Vulkan 1.0.
No physical Vulkan-1.0-only GPU or Android 7.0 OS
was available during these tests.

## Running real device diagnostics

Standalone public shader diagnostic:

```powershell
.\tools\run-vulkan-backend-public-kernel.ps1 -Serial $DeviceAdbSerial -Abi arm64-v8a -ReportPrefix pixel-arm64 -TestCore11
```

Full HASHCAT information-mode process with the correct
original OpenCL assets and `module_00000.so` staged
in an isolated Android directory:

```powershell
.\tools\run-vulkan-hashcat-public-kernel.ps1 -Serial $DeviceAdbSerial -Abi arm64-v8a -ReportPrefix pixel-arm64
```

The runner never enters a password-cracking mode.
It runs exactly `hashcat -I`, checks the shader pass
event, normal OpenCL GPU discovery and native exit,
archives stdout/stderr and cleans its temporary
device directory on completion.

## Permanent evidence and offline regression

Source and GPU proof:

```text
tests/fixtures/android-gpu/vulkan-backend-public-kernel-20261101/
  manifest.json
  redmagic-arm64-{1.0,1.1}.log
  redmagic-arm64-hashcat-info.log
  redmagic-arm64-negative-controls.log
  redmagic-armv7-{1.0,1.1}.log
  redmagic-armv7-hashcat-info.log
  redmagic-armv7-negative-controls.log
  pixel-arm64-{1.0,1.1}.log
  pixel-arm64-hashcat-info.log
  pixel-arm64-negative-controls.log
```

Create and validate the exact public evidence snapshot:

```powershell
python -B tools/archive-vulkan-backend-public-kernel-evidence.py

python -B tools/test-vulkan-backend-public-kernel-evidence.py --artifacts build/diagnostic-multimode/vulkan-backend-public-kernel-20261101 --full build/diagnostic-multimode/vulkan-hashcat-public-kernel-20261101 --readelf D:\ANDROID\SDK\ndk\29.0.14206865\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe
```

The verifier checks exact SHA-256 identities of
24 source files from the committed archive revision, both standalone ELF files,
both compiled production backend objects, both full
HASHCAT executable files, original module_00000.so
for each ABI, and the compiled SPIR-V. It independently
parses all native test runs and full `hashcat -I`
GPU/ownership events, checks Android API24 machine
types and mandatory Vulkan 1.0 Compute imports, and
rejects deliberately mutated GPU logs.

**Historical evidence retains historical hashes.**
Earlier source-provenance fixtures identify the
previously published source snapshot, and are not
rewritten to pretend that this new shader executed
during an earlier experiment.

The next verified milestone is the
[two-stage public SPIR-V Compute chain](vulkan-backend-public-chain-android.md).
It preserves this original one-stage result and demonstrates two
ordered GPU shaders with a real intermediate buffer dependency in
full Android `hashcat -I`. This historical archive remains tied to
the exact committed source revision that generated it.

## Remaining kernel port work

This is the **first production-owned executable
SPIR-V Compute kernel**, not a cryptographic
hashcat mode port.

Before any future runtime can schedule real
application GPU workloads, the backend still needs
an explicit registered Vulkan device model,
job buffer lifetime/synchronization contracts,
parameterized pipeline/descriptor layouts, correct
per-mode build and validation requirements,
multi-dispatch error isolation and equivalence
testing against trusted references.

This milestone deliberately does not connect
cryptographic candidate processing or a
password-search dispatcher.
