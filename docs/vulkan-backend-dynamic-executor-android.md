# Dynamic four-stage Vulkan 1.0 public Compute executor in Android HASHCAT

Date: 2026-11-02

## Purpose and boundary

The experimental Android `hashcat -I` Vulkan backend executes three explicitly
declared public Compute programs:

| Program | Storage buffers | Descriptor sets | Shader modules | Compute pipelines | GPU dispatches |
| --- | ---: | ---: | ---: | ---: | ---: |
| One stage | 2 | 1 | 1 | 1 | 1 |
| Two consecutive stages | 3 | 2 | 2 | 2 | 2 |
| Four consecutive stages | 5 | 4 | 4 | 4 | 4 |

The four-stage program reuses the already compiled immutable SPIR-V 1.0
`public_u32_rotate` module three times after one `public_u32_transform` stage:

```text
public uint32 input (buffer 0)
  -> public_u32_transform -> buffer 1
  -> public_u32_rotate    -> buffer 2
  -> public_u32_rotate    -> buffer 3
  -> public_u32_rotate    -> buffer 4
```

Every stage output contains exactly 64 synthetic unsigned 32-bit words.
Before a stage reads its predecessor's output, the command buffer inserts an
explicit `VK_ACCESS_SHADER_WRITE_BIT` to `VK_ACCESS_SHADER_READ_BIT`
dependency on the declared intermediate storage buffer. Final
Compute-to-host barriers, real `vkQueueSubmit`, `vkWaitForFences` and
host-cache synchronization precede CPU comparison of every stage output.

This is real Vulkan Compute execution **inside the complete Android HASHCAT
binary**. The same dynamic executor now also owns the follow-on
[one-block MD5 cryptographic stage](vulkan-backend-md5-android.md), while
the public graph documented here remains synthetic arithmetic. The Vulkan
path executes only in opt-in backend information mode, does not register a
hashing device and never processes HASHCAT password candidates, target
digests, salts or attack scheduling work.

## Source architecture

### `backend_vulkan_program_stage_t`

Declares an immutable Vulkan SPIR-V 1.0 module, the exact source and
destination buffer indices, `local_size[3]` and `group_count[3]`. The
four-stage program is an explicit 0→1→2→3→4 linear dependency graph.
No driver-name heuristics select alternate shaders, buffer indices or
workgroup geometry.

### `backend_vulkan_program_t`

Owns the immutable ordered stage declarations and the exact `stage_count`,
`buffer_count` and `buffer_bytes`. `backend_vulkan_program_validate()`
checks the graph, canonical SPIR-V 1.0 headers and Compute limits using
the real selected physical GPU's `VkPhysicalDeviceLimits` before the
temporary logical device is opened.

### `backend_vulkan_public_job_t`

Owns one temporary `VkDevice`, queue, command pool/buffer, fence, shared
descriptor layout, pipeline layout, descriptor pool and four host ownership
arrays:

```text
buffers         -> calloc(program->buffer_count, ...)
descriptor_set  -> calloc(program->stage_count, ...)
shader          -> calloc(program->stage_count, ...)
pipeline        -> calloc(program->stage_count, ...)
```

`VkInstance` and `VkPhysicalDevice` remain owned by the live
`backend_ctx` catalogue.

### `backend_vulkan_public_prepare(job, program)`

Validates nonzero declared counts, bounds the descriptor count
multiplication and checks each host-allocation size before `calloc`.
It allocates host ownership records from the program's exact stage and
buffer counts. A partial host allocation returns
`VK_ERROR_OUT_OF_HOST_MEMORY` after freeing acquired host records and
clearing the job. This method does not create a Vulkan object.

### `backend_vulkan_public_buffer_open(job, selected, buffer)`

Creates one storage buffer with the program's declared byte size,
selects a host-visible memory type using real buffer memory
requirements and physical memory properties, allocates and binds
the complete driver-reported memory size, and maps the allocation.
Coherent memory is preferred. Noncoherent memory uses explicit
`vkFlushMappedMemoryRanges` and `vkInvalidateMappedMemoryRanges`
via the existing Vulkan memory-plan policy.

### `backend_vulkan_public_pipeline_open(job)`

Creates one two-binding storage-buffer descriptor-set layout and
compatible pipeline layout. It sizes the descriptor pool from
`program->stage_count` and allocates individual descriptor sets
into the dynamic host array. Each declared stage provides the source
buffer, destination buffer, SPIR-V pointer and word count used for
the corresponding descriptor writes, `VkShaderModule` and
`VkComputePipeline`. The executor enables no optional device
features or Vulkan extensions.

### `backend_vulkan_public_dispatch(job, diagnostics)`

Writes deterministic synthetic public integers to the first stage's
declared input buffer and synchronizes host writes. It records one
command buffer containing the declared stage dispatches, inserting a
Compute write-to-read buffer barrier before every later stage.
The method makes every destination host-visible, submits the command
buffer, waits for its real fence, invalidates noncoherent allocations,
and independently computes and compares every 32-bit output value.

The first stage's CPU equation matches `public_u32_transform`.
Subsequent stages use the same rotate/add/xor equation as the
reused `public_u32_rotate` module. A mismatch returns
`VK_ERROR_INITIALIZATION_FAILED` and prints its exact stage and word
index rather than silently accepting incorrect output.

### `backend_vulkan_public_release(job)`

Waits for the device to become idle when a logical device exists,
destroys every acquired pipeline, shader, descriptor object,
command/fence object, storage buffer and memory allocation, and
finally destroys the logical device. All dynamic host arrays are
freed and the job is zeroed even if device creation or a later GPU
operation failed. The first `vkDeviceWaitIdle` result is retained
after cleanup. The parent `VkInstance` is not owned or destroyed here.

### `backend_vulkan_kernel_verify_program(...)`

Selects a backend-catalogue physical device, verifies the complete
program against exact driver limits, prepares dynamic host resources,
creates a temporary logical device, opens buffers and stage pipelines,
runs and checks the declared Compute work, then releases all temporary
resources before returning. It does not touch normal HASHCAT backend
devices.

### Exported methods

`backend_vulkan_kernel_verify()` executes the original one-stage
public transform, checking 64 words.

`backend_vulkan_kernel_verify_chain()` executes a two-stage program,
checking both sets of 64 words and the inter-stage dependency.

`backend_vulkan_kernel_verify_chain4()` executes four ordered public
stages over five buffers, checking all 256 outputs, three declared GPU
dependencies and dynamic resource ownership.

All methods require a live Vulkan catalogue, valid physical device
index and non-NULL diagnostics. They propagate real `VkResult` failures,
return `VK_SUCCESS` only for completely verified and cleaned-up work,
and never enable production Vulkan password processing.

## Android builds

Build the dedicated native diagnostic with the exact production backend
source, both API24 ABIs and `-Wall -Wextra -Werror`:

```powershell
.\tools\build-vulkan-backend-public-dynamic.ps1 -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865 -AndroidApi 24 -GlslangCompiler D:\ANDROID\SDK\emulator\lib64\vulkan\glslangValidator.exe
```

This build recompiles both GLSL modules with
`--target-env vulkan1.0`, and requires generated C array bytes to match
the committed embedded SPIR-V headers.

Build full opt-in Android HASHCAT frontends with the unchanged
production default for non-Vulkan builds:

```powershell
.\scripts\BUILD.ps1 -Target android-arm64-v8a -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865 -AndroidApi 24 -EnableVulkanDiscovery -VulkanOutputName vulkan-hashcat-dynamic-executor-20261102 -Package:$false -MakeJobs 4
.\scripts\BUILD.ps1 -Target android-armeabi-v7a -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865 -AndroidApi 24 -EnableVulkanDiscovery -VulkanOutputName vulkan-hashcat-dynamic-executor-20261102 -Package:$false -MakeJobs 4
```

`ENABLE_VULKAN_DISCOVERY=1` is opt-in and Android-specific. The regular
HASHCAT modules, in particular `module_00000.so`, have no direct
`libvulkan.so` dependency.

## Physical device test matrix

| Physical GPU / process ABI | Requested native Vulkan | Four-stage native diagnostic | Full `hashcat -I` |
| --- | --- | --- | --- |
| Adreno 730 / ARM64 | 1.0, 1.1 | 2 processes, PASS | PASS |
| Adreno 730 / ARMv7 | 1.0, 1.1 | 2 processes, PASS | PASS |
| Mali-G715 / ARM64 | 1.0, 1.1 | 2 processes, PASS | PASS |

Every standalone process executes two complete catalogue and logical-device
lifecycles. Each lifecycle validates the four-stage program against the
selected real driver limits, creates five buffers and four
sets/shaders/pipelines, performs four real `vkCmdDispatch` calls and
verifies 256 public `uint32` words.

Each complete `hashcat -I` process verifies its original empty Vulkan
queue session, the one-stage kernel, the two-stage chain and the new
four-stage chain. It then reports normal OpenCL device information
and exits with native status zero.

The verified totals are:

| Verified event | Count |
| --- | ---: |
| Standalone native Android processes | 6 |
| Complete `hashcat -I` processes | 3 |
| Four-stage program lifetimes across both test types | 15 |
| Real public `vkCmdDispatch` calls | 69 |
| Public `uint32` results compared with CPU | 4,416 |
| Four-stage shader-write-to-read dependencies | 45 |
| All public program inter-stage dependencies | 48 |
| Rejected malformed native command-line requests | 9 |

There are 12 standalone four-stage lifetimes and three complete
HASHCAT four-stage lifetimes. The 69 dispatches also include the
one-stage and two-stage executions of the three full HASHCAT runs.

The native ARM64 and ARMv7 executables target Android API24 and use
Vulkan core 1.0 entry points. The tested OS versions and loader/driver
versions are newer; this does not establish testing on a physically
Vulkan-1.0-only GPU or Android 7.0 device.

## Archived hardware evidence

All normalized Android test transcripts and exact source/binary
identities live in:

```text
tests/fixtures/android-gpu/vulkan-backend-public-dynamic-20261102/
  manifest.json
  redmagic-arm64-1.0.log
  redmagic-arm64-1.1.log
  redmagic-arm64-hashcat-dynamic-info.log
  redmagic-arm64-negative-controls.log
  redmagic-armv7-1.0.log
  redmagic-armv7-1.1.log
  redmagic-armv7-hashcat-dynamic-info.log
  redmagic-armv7-negative-controls.log
  pixel-arm64-1.0.log
  pixel-arm64-1.1.log
  pixel-arm64-hashcat-dynamic-info.log
  pixel-arm64-negative-controls.log
```

The runner normalizes Android ADB stdout/stderr line endings to LF
before saving; the archive copies the saved runner bytes unchanged.
These logs are not represented as raw unprocessed ADB streams.

Archive:

```powershell
python -B tools/archive-vulkan-backend-dynamic-evidence.py
```

Validate all recorded stage, buffer, pipeline and GPU execution
events and SHA-256 identities offline:

```powershell
python -B tools/test-vulkan-backend-dynamic-evidence.py --native build/diagnostic-multimode/vulkan-backend-public-dynamic-20261102 --full build/diagnostic-multimode/vulkan-hashcat-dynamic-executor-20261102 --readelf D:\ANDROID\SDK\ndk\29.0.14206865\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe
```

The independent verifier also deliberately falsifies more than twenty
GPU events, including absent barriers, incorrect dispatches, false
cleanup, bad descriptor counts, fabricated password scheduling and
missing OpenCL information, and must reject every altered record.

## Remaining boundaries

Dynamic resource ownership and CPU-checked public Compute execution are
verified inside the full Android HASHCAT frontend. This is not
production password-hashing GPU execution.

The production Vulkan path now includes one cryptographic primitive:
the [fixed-vector one-block MD5 stage](vulkan-backend-md5-android.md).
It executes the complete MD5 compression function through the same dynamic
resource owner and passes real Adreno/Mali known-answer checks. It does not
yet accept HASHCAT candidate records or target digests, and the Vulkan device
is not inserted into the hashing scheduler. There is no public API for loading
arbitrary SPIR-V programs from users. The
[public resource-cleanup diagnostic](vulkan-backend-public-fault-android.md)
exercises 37 controlled post-acquisition or submission-boundary faults
against the four-stage arithmetic program, followed by new CPU-verified
Compute sessions on the same live Vulkan instance.

The [API-boundary fault diagnostic](vulkan-backend-public-api-fault-android.md)
also links the unchanged executor through dedicated test-only wrappers
for buffer, shader, pipeline, queue-submit and fence-wait calls. Negative
VkResults are injected at those exact call sites, cleanup performs a real
vkDeviceWaitIdle, and a fresh four-stage VkDevice then succeeds under the
same instance on Mali ARM64 and Adreno ARM64/ARMv7.

The [real fence-timeout diagnostic](vulkan-backend-public-fence-timeout-android.md)
uses no test-linker wrappers. It performs a real queue submission without
associating the separately owned unsignaled fence, observes the driver's
VK_TIMEOUT from a zero-duration vkWaitForFences call, releases the submitted
job through real vkDeviceWaitIdle and recreates a healthy four-stage device
on all three GPU/process configurations. Actual physical device loss, real
driver allocation failure, a driver-returned queue-submit failure and
recovery of a genuinely lost VkDevice are still unverified.
