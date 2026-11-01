# Program-declared Vulkan 1.0 Compute execution inside Android HASHCAT

Date: 2026-11-01

## Scope

The public Vulkan Compute executor in Android `hashcat -I` now consumes
explicit `backend_vulkan_program_t` declarations instead of selecting
shader modules and storage-buffer pairs from hard-coded stage numbers.

The existing public one-stage and two-stage SPIR-V 1.0 tests remain the
only programs executed by this path:

```text
public_u32_transform:
  buffer 0 -> buffer 1

public_u32_transform -> public_u32_rotate:
  buffer 0 -> buffer 1 -> buffer 2
```

Each declaration supplies the immutable SPIR-V array, exact word count,
source/destination storage buffer indices, local workgroup dimensions
and dispatch group counts.

Before a temporary logical device is created, the program is validated
against the actual selected GPU's `VkPhysicalDeviceLimits` using
`backend_vulkan_program_validate()`. The same validated declaration is
then consumed by descriptor writes, shader-module creation, dispatch
recording and inter-stage barriers.

No HASHCAT cryptographic shader, password candidate, target hash, salt,
attack mode or normal hashing scheduler is connected to this executor.
The integration remains information-mode verification only.

## Executor contract

### Static program declarations

`source/src/backend_vulkan_kernel.c` owns two immutable declarations.

The one-stage declaration contains one stage, two buffers and a
256-byte logical descriptor range.

The two-stage declaration contains two stages, three buffers and the
same 256-byte range. Stage two explicitly declares buffer 1 as its
source, matching stage one's destination.

Both stages declare:

```text
local_size = (32, 1, 1)
group_count = (2, 1, 1)
```

The declarations reference the complete generated SPIR-V 1.0 arrays:

```text
backend_vulkan_public_spirv
backend_vulkan_public_stage2_spirv
```

### Physical-device validation

`backend_vulkan_kernel_verify_stages()` first uses the backend Vulkan
catalogue to select a physical device and retrieve its real properties.

The selected program then passes through:

```c
backend_vulkan_program_validate (
  program,
  &selected.properties.limits,
  diagnostics
);
```

A structural or unsupported-limit result is returned before
`vkCreateDevice`.

On the verified Pixel Mali-G715 process, the resulting event is:

```text
BACKEND_VULKAN_PROGRAM_ACCEPT stages=1 buffers=2 bytes=256 descriptors_per_stage=2
```

or:

```text
BACKEND_VULKAN_PROGRAM_ACCEPT stages=2 buffers=3 bytes=256 descriptors_per_stage=2
```

### Resource ownership

`backend_vulkan_public_job_t` stores a pointer to the exact validated
program. The temporary job still owns at most:

- one `VkDevice` and Compute queue;
- three host-visible storage buffers;
- two descriptor sets;
- two `VkShaderModule` objects;
- two `VkComputePipeline` objects;
- one command pool and command buffer;
- one fence.

`backend_vulkan_public_release()` reads the program stage/buffer counts
when releasing shader, pipeline and memory objects. The backend
catalogue's `VkInstance` remains owned by `backend_ctx`.

The current production information-mode executor intentionally caps
execution at two public stages even though the host-only program
validator can validate longer linear declarations.

### Descriptor and pipeline construction

`backend_vulkan_public_pipeline_open()` consumes each declared stage.

For every stage it uses:

```text
source_buffer_index      -> descriptor binding 0
destination_buffer_index -> descriptor binding 1
spirv + spirv_words      -> VkShaderModule
```

It no longer chooses the module or descriptor buffers from
`stage == 0` / `stage == 1` conditions.

Every pipeline still uses entry point `main`, two
`VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` bindings, no optional features,
no extensions and no push constants.

### Dispatch recording

`backend_vulkan_public_dispatch()` writes the public input to the
source buffer declared by stage zero.

For each later stage, the method inserts a buffer barrier on that
stage's declared source:

```text
srcAccess = VK_ACCESS_SHADER_WRITE_BIT
dstAccess = VK_ACCESS_SHADER_READ_BIT
srcStage  = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
dstStage  = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
```

The pipeline and descriptor set for that exact program stage are bound,
then `vkCmdDispatch` uses its declared three-dimensional `group_count`.

Final shader-write-to-host-read barriers target each stage's declared
destination buffer. A real Vulkan fence completes before noncoherent
memory invalidation and CPU comparison.

The arithmetic verifier still independently checks all 64 values
produced by each public shader. Command recording follows the program
declaration; the CPU arithmetic is intentionally specific to these
two diagnostic modules.

## Build integration

When `ENABLE_VULKAN_DISCOVERY=1` is selected for Android,
`source/src/Makefile` now links:

```text
obj/backend_vulkan_program.NATIVE.o
```

beside the existing catalogue, session and public-kernel executor
objects.

The standalone public-kernel and public-chain build scripts also link
`source/src/backend_vulkan_program.c` so they exercise the same
validator used by the full HASHCAT process.

Both complete Android frontends were built for API24 targets:

```text
arm64-v8a
armeabi-v7a
```

The ARMv7 executable was compiled and its exact SHA-256 archived, but
this current revision was not run on physical ARMv7 hardware because
the RedMagic device was disconnected.

## Physical Pixel validation

The current source revision was executed on Pixel 9 Pro XL / Mali-G715.

### Standalone production-linked diagnostic

Two real Android processes were saved:

```text
requested Vulkan 1.0
requested Vulkan 1.1
```

Each process opens two independent Vulkan catalogue/device lifecycles.
Each lifecycle performs:

1. real physical-device selection;
2. exact program validation against driver limits;
3. creation of the two declared pipelines and descriptor sets;
4. two real `vkCmdDispatch` calls;
5. shader-write-to-shader-read synchronization;
6. real fence completion;
7. CPU verification of 64 intermediate plus 64 final words;
8. complete temporary VkDevice teardown before VkInstance close.

Across both native processes:

```text
program validations: 4
vkCmdDispatch calls: 8
public uint32 words verified: 512
```

### Full HASHCAT information mode

A newly built ARM64 `hashcat -I` process completed with native exit zero.

It performed:

```text
1 empty Vulkan session verification
1 one-stage program validation
1 one-stage vkCmdDispatch
1 two-stage program validation
2 two-stage vkCmdDispatch calls
192 total public uint32 words verified
normal OpenCL Mali-G715 enumeration
```

The process emitted both program-accept events before their respective
temporary device executions and later printed the ordinary OpenCL
backend information.

Across the standalone and full-HASHCAT current-revision evidence:

```text
real Android processes: 3
program validations against real GPU limits: 6
real vkCmdDispatch calls: 11
public uint32 words verified: 704
```

## Evidence and independent verification

Permanent evidence is stored in:

```text
tests/fixtures/android-gpu/vulkan-backend-program-executor-20261101/
  manifest.json
  pixel-arm64-1.0.log
  pixel-arm64-1.1.log
  pixel-arm64-hashcat-program-info.log
  pixel-arm64-negative-controls.log
```

Archive the current build products and saved process logs:

```powershell
python -B tools/archive-vulkan-backend-program-executor-evidence.py
```

Verify source identities, Android ARM64/ARMv7 native diagnostics,
production backend objects, complete HASHCAT executables, module
artifacts and saved Pixel traces:

```powershell
python -B tools/test-vulkan-backend-program-executor-evidence.py --native build/diagnostic-multimode/vulkan-backend-program-executor-20261101 --full build/diagnostic-multimode/vulkan-hashcat-program-executor-20261101 --readelf D:\ANDROID\SDK\ndk\29.0.14206865\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe
```

The verifier requires Vulkan shader/pipeline/dispatch symbols in the
native GPU executables and complete HASHCAT frontends. It also rejects
deliberately falsified validation, dispatch, barrier, scheduling and
OpenCL-device records.

## Historical hardware evidence

The earlier two-stage evidence contains physical runs on:

```text
RedMagic Adreno 730 ARM64
RedMagic Adreno 730 ARMv7
Pixel Mali-G715 ARM64
```

Those records remain tied to commit `5a62e5d`. Their verifier resolves
source hashes from the commit containing that exact saved manifest, so
current executor refactoring does not relabel historical hardware output
as current-revision execution.

The current program-driven executor evidence intentionally claims only
the Pixel ARM64 runtime tested after this integration.

## Verification boundary

The declaration object is now connected to public Compute execution,
but the ordinary HASHCAT hashing scheduler remains separate.

This milestone does not port a password-processing OpenCL kernel to
SPIR-V, load arbitrary external shaders, accept attack-mode buffers,
or register Vulkan as a hashing backend.

A later safe executor step can remove the fixed two-stage resource
capacity and allocate descriptor/pipeline/buffer arrays from a validated
program's declared counts. That can be tested again using the public
integer shaders before any cryptographic scheduling work is considered.
