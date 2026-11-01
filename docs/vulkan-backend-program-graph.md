# Declarative Vulkan 1.0 Compute program graph for HASHCAT Android

Date: 2026-11-01

## Scope

`source/include/backend_vulkan_program.h` and
`source/src/backend_vulkan_program.c` define a host-only contract for
describing an ordered sequence of Vulkan 1.0 Compute shader stages.

This layer exists between embedded SPIR-V modules and the GPU executor.
It validates the structure of a program before the caller creates
Vulkan pipelines, descriptor sets or command buffers.

The graph validator currently performs **no Vulkan driver call**.
It does not link `libvulkan.so`, create a `VkInstance`, query a physical
device, record `vkCmdDispatch`, or connect to HASHCAT's hashing
scheduler. Physical limits are supplied explicitly by the caller as a
`VkPhysicalDeviceLimits` value.

The already published two-stage public Compute chain remains the active
GPU proof in `hashcat -I`. This new declaration layer is intentionally
not wired into `backend_vulkan_kernel.c` yet, because doing so would
change the exact source revision covered by the existing Adreno
ARM64/ARMv7 and Mali ARM64 hardware evidence. The RedMagic device was
not connected while this contract was created.

No cryptographic SPIR-V module, password candidate, hash, salt or attack
mode is part of this milestone.

## Program declaration

A stage is represented by `backend_vulkan_program_stage_t`:

- `name` is a non-empty diagnostic identifier.
- `spirv` points to the complete immutable SPIR-V module.
- `spirv_words` is the exact 32-bit word count.
- `source_buffer_index` declares the storage buffer read by the stage.
- `destination_buffer_index` declares its storage output.
- `local_size[3]` declares the exact local workgroup dimensions.
- `group_count[3]` declares the exact dispatch group count.

A complete `backend_vulkan_program_t` contains:

- the stage array;
- explicit stage count;
- explicit storage-buffer count;
- one logical descriptor range in bytes shared by the declared stages.

No dependency, buffer role or dispatch size is inferred from a GPU name,
driver vendor, SPIR-V disassembly or file name.

## Sequential dependency contract

For stage zero, source and destination may be any distinct valid buffer
indices.

For every later stage:

```text
stage[N].source_buffer_index ==
stage[N - 1].destination_buffer_index
```

Every destination buffer index must be unique across the graph. A stage
may not use the same buffer as both its source and destination.

The rule supports a linear Compute pipeline such as:

```text
buffer 0 -> shader A -> buffer 1 -> shader B -> buffer 2
```

The validator rejects undeclared discontinuities and destination
aliasing. It does not insert or infer the future Vulkan barriers; the
executor will use these explicit dependencies when the contract is
connected to GPU command recording.

## SPIR-V contract

Each stage must contain at least the canonical five-word SPIR-V header.

The validator requires:

```text
magic   = 0x07230203
version = 0x00010000
schema  = 0
```

Therefore the current program layer accepts SPIR-V 1.0 modules only.
Generator and bound fields remain opaque because they do not describe
the host-side buffer dependency or dispatch topology validated here.

The Android diagnostic uses the same embedded public modules already
present in HASHCAT:

```text
public_u32_transform
public_u32_rotate
```

Both modules operate only on synthetic unsigned 32-bit values.

## Physical limit validation

`backend_vulkan_program_validate()` accepts an explicit
`VkPhysicalDeviceLimits` structure and checks every declared stage
against:

- `maxStorageBufferRange`;
- `maxPerStageDescriptorStorageBuffers`;
- `maxComputeWorkGroupSize[3]`;
- `maxComputeWorkGroupCount[3]`;
- `maxComputeWorkGroupInvocations`.

Each stage requires exactly two storage-buffer descriptors.

Zero local dimensions or zero group counts are malformed declarations
and return `VK_ERROR_INITIALIZATION_FAILED`.

A structurally correct declaration that exceeds a supplied physical
limit returns `VK_ERROR_FEATURE_NOT_PRESENT`. The function never clamps
or rewrites the requested topology.

Local invocation multiplication uses 64-bit host arithmetic before
comparison with `maxComputeWorkGroupInvocations`.

## Public methods

### `backend_vulkan_program_validate(program, limits, diagnostics)`

Validates a complete ordered program without modifying caller state.

Returns:

- `VK_SUCCESS` when every declaration is valid and supported;
- `VK_ERROR_INITIALIZATION_FAILED` for malformed program structure,
  invalid SPIR-V header, bad indices, aliasing, zero topology or missing
  input pointers;
- `VK_ERROR_FEATURE_NOT_PRESENT` when descriptor, storage-range or
  Compute topology exceeds the exact supplied limits.

The method does not call Vulkan.

### `backend_vulkan_program_spirv_valid(stage)`

Internal helper that validates only the canonical SPIR-V 1.0 header.
It does not parse shader instructions or infer descriptor interfaces.

### `backend_vulkan_program_validate_topology(stage, limits, index, diagnostics)`

Internal helper that checks exact local dimensions, group counts and
total local invocations. Unsupported values are reported instead of
being altered.

## Native diagnostic coverage

`tools/vulkan-backend-program-diagnostic.c` links the exact program
validator source and the two public embedded SPIR-V arrays.

It runs 16 explicit cases:

```text
Accepted:
  two_stage_valid
  one_stage_valid

Structural rejection:
  null_program
  null_limits
  broken_dependency
  source_destination_alias
  reused_destination
  zero_group
  zero_local
  spirv_magic
  spirv_version
  empty_name

Physical-limit rejection:
  group_limit
  invocation_limit
  storage_range
  descriptor_limit
```

The repeated-destination case uses an explicit three-stage declaration:

```text
0 -> 1 -> 2 -> 1
```

The third stage therefore reuses an earlier destination without relying
on an implicit alias rule.

## Android builds

Build both Android API24 executables:

```powershell
.\tools\build-vulkan-backend-program.ps1 -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865 -AndroidApi 24
```

Both ARM64 and ARMv7 builds use:

```text
-Wall -Wextra -Werror
```

Static ELF checks require:

- ARM64 ELF64 / AArch64 and ARMv7 ELF32 / ARM identities;
- Android API24 notes;
- **no `libvulkan.so` dependency**.

The two executables therefore cannot issue a Vulkan driver call through
this program layer.

Run the validator on a compatible Android process:

```powershell
.\tools\run-vulkan-backend-program.ps1 -Serial $DeviceAdbSerial -Abi arm64-v8a -ReportPrefix pixel-arm64
```

The saved transcript requires all 16 case results and the completion
marker:

```text
BACKEND_VULKAN_PROGRAM_DIAGNOSTIC_PASS accepted=2 structural_rejects=10 limit_rejects=4 driver_calls=0 gpu_dispatches=0 password_inputs=0 scheduled=0
```

## Evidence

Archive the exact Android transcript and source/ELF identities:

```powershell
python -B tools/archive-vulkan-backend-program-evidence.py
```

Verify the archive and Android ELFs offline:

```powershell
python -B tools/test-vulkan-backend-program-evidence.py --artifacts build/diagnostic-multimode/vulkan-backend-program-20261101 --readelf D:\ANDROID\SDK\ndk\29.0.14206865\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe
```

Evidence location:

```text
tests/fixtures/android-gpu/vulkan-backend-program-20261101/
  manifest.json
  pixel-arm64-program-graph.log
```

The independent verifier also rejects 12 deliberately falsified records,
including fabricated GPU activity, incorrect structural/limit results and
password-processing claims.

## Current verification boundary

This milestone establishes the declaration and validation layer only.

The next executor milestone can consume
`backend_vulkan_program_stage_t` declarations to create descriptor sets,
pipelines and shader-write-to-shader-read barriers dynamically. That
integration requires new physical GPU evidence because it modifies the
production source currently covered by the published two-stage hardware
archive.

ARMv7 compilation and ELF identity are verified, but the program-graph
diagnostic was not run in an ARMv7 process for this milestone because
the RedMagic device was disconnected. The Pixel ARM64 process completed
all 16 host-only cases with native exit code zero.
