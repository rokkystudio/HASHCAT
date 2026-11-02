# First cryptographic MD5 SPIR-V stage in Android HASHCAT

Date: 2026-11-02

## Scope

The opt-in Android Vulkan backend now executes one real cryptographic primitive
inside the complete `hashcat -I` process: the 64-round MD5 compression used
for a single fully padded 512-bit message block.

This milestone uses the production dynamic Vulkan resource owner and program
validator that already execute the public arithmetic stages. It does not create
a second Vulkan runtime, special device path or scheduler-owned hashing device.
The MD5 program is declared as one ordinary `backend_vulkan_program_t` stage
with two storage buffers.

The inputs are four immutable public known-answer messages:

| Vector | Message | Expected MD5 |
| --- | --- | --- |
| 0 | empty string | `d41d8cd98f00b204e9800998ecf8427e` |
| 1 | `a` | `0cc175b9c0f1b6a831c399e269772661` |
| 2 | `abc` | `900150983cd24fb0d6963f7d28e17f72` |
| 3 | `message digest` | `f96b697d7cb7938d525a2f31aaf161d0` |

No HASHCAT password candidate, target digest, salt, rule, mask, attack mode or
hashing scheduler entry is consumed. This is therefore the first
cryptographic Vulkan Compute stage in the production backend, but not yet a
Vulkan implementation of HASHCAT mode 0 attack scheduling.

## Shader contract

`tools/vulkan-backend-md5.comp` is GLSL 450 compiled for
`--target-env vulkan1.0`.

Its declared execution shape is:

```text
local_size_x = 4
group_count_x = 1

binding 0: 4 x 64-byte pre-padded MD5 blocks
binding 1: 4 x 16-byte digest states + unused sentinel space
```

Each invocation reads sixteen little-endian `uint32` words, initializes the
standard MD5 state, executes all 64 rounds with the standard rotation schedule
and constants, adds the initial state and writes four digest words.

The round structure, initial state and constants correspond to the MD5
primitive used by `source/OpenCL/inc_hash_md5.cl`. The Vulkan stage is an
independent GLSL implementation rather than a source-level OpenCL translator.

The compiled module is SPIR-V 1.0:

| Property | Value |
| --- | --- |
| Bytes | 6,824 |
| Words | 1,706 |
| SHA-256 | `7aa9f9f96a2d6ce58e93d34df50e0bd841e0ed021d9963c4b2c76d0f9b853604` |

`tools/generate-vulkan-backend-md5-kernel.py` validates the canonical
little-endian SPIR-V magic, version 1.0, complete 32-bit words and schema zero,
then embeds the exact module bytes in
`source/include/backend_vulkan_md5_spirv.h`.

## Production program declaration

The MD5 stage is declared explicitly in
`source/src/backend_vulkan_kernel.c`:

```text
backend_vulkan_md5_program
  stage_count  = 1
  buffer_count = 2
  buffer_bytes = 256

stage 0
  source_buffer_index      = 0
  destination_buffer_index = 1
  local_size               = 4,1,1
  group_count              = 1,1,1
```

Both storage buffers use the same 256-byte allocation size required by the
existing declarative executor. The input therefore contains exactly four
64-byte MD5 blocks. The output uses its first sixteen words for four digest
states; the remaining forty-eight words are diagnostic sentinels.

No device name, GPU vendor, process ABI or shader byte identity selects the
MD5 path. The host passes an explicit `BACKEND_VULKAN_EXECUTION_MD5`
execution kind.

## Host input preparation

### `backend_vulkan_md5_prepare_blocks(blocks)`

Builds the four public messages directly as little-endian `uint32` words.

For each vector it:

1. clears all sixteen block words;
2. inserts the message bytes at their exact byte offsets;
3. appends the MD5 `0x80` padding bit;
4. stores the original message bit length in word 14;
5. leaves word 15 zero because all four messages are shorter than 2^32 bits.

The method does not cast byte arrays to `uint32_t *`, so its block encoding
does not depend on host pointer alignment or native byte order.

## GPU dispatch and verification

### `backend_vulkan_md5_dispatch(job, diagnostics)`

Requires the declared one-stage, two-buffer, 256-byte MD5 program and the
common dynamically allocated Vulkan resources.

The method:

```text
prepares four padded public blocks
-> writes binding 0
-> fills all 64 output words with 0xa5a5a5a5
-> flushes mapped input/output memory when required
-> records one vkCmdDispatch(1,1,1)
-> records shader-write -> host-read buffer visibility
-> submits through the production queue and owned fence
-> waits for the real fence
-> invalidates mapped output memory when required
-> compares 16 digest words with the four known answers
-> requires all remaining 48 words to preserve the sentinel
```

The sentinel check detects writes beyond the declared four digest records.
A digest mismatch or sentinel mismatch returns
`VK_ERROR_INITIALIZATION_FAILED`.

Successful execution reports:

```text
BACKEND_VULKAN_MD5_DISPATCH_PASS vectors=4 blocks=4 rounds=64
dispatches=1 verified_digest_words=16 sentinel_words=48
... password_inputs=0 target_digests=0 scheduled=0
```

### `backend_vulkan_kernel_verify_program(..., execution_kind, ...)`

The common production executor now receives an explicit execution kind.

`BACKEND_VULKAN_EXECUTION_PUBLIC_U32` retains the public arithmetic,
controlled-resource-fault and real fence-timeout diagnostics.

`BACKEND_VULKAN_EXECUTION_MD5` selects the MD5 host preparation and
known-answer verifier. Controlled arithmetic fault checkpoints and the
unsignaled-fence probe are rejected for this execution kind.

Device selection, program validation, storage-buffer allocation, descriptor
sets, shader modules, Compute pipelines, command pool/buffer, fence ownership
and release remain the common dynamic implementation.

### `backend_vulkan_kernel_verify_md5(backend_ctx, device_index, diagnostics)`

Requires an open Vulkan catalogue, valid physical-device index and non-NULL
diagnostics. It executes `backend_vulkan_md5_program` through the common
executor.

`VK_SUCCESS` means:

- all four real GPU MD5 results match their known answers;
- all forty-eight unused destination words retain the sentinel;
- the real queue submission and fence complete;
- common production cleanup releases all temporary resources.

The method does not register the Vulkan device with HASHCAT's hashing
scheduler.

## HASHCAT information-mode integration

After the existing one-, two- and four-stage public arithmetic programs
succeed, the opt-in Vulkan branch in `backend_ctx_devices_init()` calls
`backend_vulkan_kernel_verify_md5()`.

A successful full frontend reports:

```text
Vulkan 1.0 MD5 SPIR-V known-answer vectors dispatched and verified; hashing remains disabled.
```

The ordinary OpenCL discovery path continues afterward. Vulkan is still not
inserted into the production hashing-device list.

## Build

Build the standalone production-executor diagnostic for both Android ABIs:

```powershell
.\tools\build-vulkan-backend-md5.ps1 \
  -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865 \
  -AndroidApi 24 \
  -GlslangCompiler D:\ANDROID\SDK\emulator\lib64\vulkan\glslangValidator.exe
```

The build:

- recompiles the GLSL stage for Vulkan 1.0;
- regenerates the embedded header and requires byte-for-byte identity;
- compiles production `backend.c` with Vulkan discovery for ARM64 and ARMv7;
- syntax-checks the default non-Vulkan `backend.c` path;
- links the diagnostic against the production dynamic Vulkan executor;
- requires `libvulkan.so` and rejects test-only `__wrap_vk*` symbols;
- uses `-Wall -Wextra -Werror`.

Standalone artifacts:

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `vulkan-backend-md5.spv` | 6,824 | `7aa9f9f96a2d6ce58e93d34df50e0bd841e0ed021d9963c4b2c76d0f9b853604` |
| ARM64 backend object | 199,512 | `19c102f92f18d986ff82b0cd3360e4be463f3a86ffa52616d57e73e42c7d5055` |
| ARM64 diagnostic | 51,480 | `33f17cc2524e19de647dc56a9fcfce01122cbdd461121ac2ba40345d3b02162f` |
| ARMv7 backend object | 173,488 | `5174e17c6aac6ac290577196cdceba3652618b12c3483f0552d8ab1254cd4678` |
| ARMv7 diagnostic | 51,220 | `ed9cec8b21db71b0183353986a186088e0713267a52adacd6d86d2f88a79a310` |

Full opt-in Android frontends were built with
`-EnableVulkanDiscovery -VulkanOutputName vulkan-hashcat-md5-20261102`:

| ABI | Frontend bytes | SHA-256 |
| --- | ---: | --- |
| ARM64 | 1,555,456 | `4effccef1f331dd112030412e297e93f30783938a68172bfe08d46cea3d56308` |
| ARMv7 | 1,811,124 | `d2e03c76c643c7c55a8ebcb4e0fc4e699d3b5377ba747c2113f52bab8b5ec77f` |

## Physical standalone matrix

The same embedded SPIR-V module and production executor completed on all
current GPU/process configurations for both requested Vulkan instance levels:

| GPU / process ABI | Vulkan 1.0 | Vulkan 1.1 |
| --- | --- | --- |
| Pixel 9 Pro XL / Mali-G715 ARM64 | PASS | PASS |
| RedMagic 7S Pro / Adreno 730 ARM64 | PASS | PASS |
| RedMagic 7S Pro / Adreno 730 ARMv7 | PASS | PASS |

Across the six native processes:

| Property | Count |
| --- | ---: |
| Cryptographic GPU dispatches | 6 |
| MD5 message blocks | 24 |
| Verified digest words | 96 |
| Verified sentinel words | 288 |
| Password inputs | 0 |
| Target digests | 0 |
| HASHCAT scheduler jobs | 0 |

Every native process exited 0.

## Full HASHCAT integration

`tools/run-vulkan-hashcat-md5.ps1` stages an isolated complete Android
HASHCAT runtime and runs only `hashcat -I`. It requires the MD5 READY,
DISPATCH_PASS and CLOSE events, the successful HASHCAT informational message,
ordinary OpenCL information and native exit zero.

Two full frontend integrations were used after the six-run standalone matrix:

| Device / ABI | Result |
| --- | --- |
| Pixel 9 Pro XL / Mali-G715 ARM64 | PASS |
| RedMagic 7S Pro / Adreno 730 ARMv7 | PASS |

Both retained normal OpenCL device discovery and exited 0. The ARM64 Adreno
standalone path had already executed the identical MD5 SPIR-V and production
executor, so an additional full frontend process was not required for this
milestone.

## Established boundary

A real cryptographic MD5 compression stage now executes through the production
Android Vulkan backend, including the same declarative program validation,
dynamic ownership, Vulkan synchronization and cleanup used by the synthetic
executor.

The current stage is deliberately narrower than HASHCAT mode 0 execution. It
does not yet provide:

- scheduler registration of a Vulkan hashing device;
- HASHCAT candidate-buffer or `pw_t` ingestion;
- variable candidate lengths or multi-block MD5 messages;
- target digest ingestion or GPU compare/reporting;
- attack-rule, mask, combinator or amplifier integration;
- batched candidate geometry selected by HASHCAT;
- mode-0 self-test or cracking dispatch through the normal scheduler;
- performance tuning.

The companion [SHA-256 batch contract](vulkan-backend-sha256-android.md)
demonstrates variable-length caller-owned byte spans and multi-block padding
through the same executor. MD5 remains intentionally narrower: its current
program consumes four fixed one-block public messages. The mode-0 boundary
therefore remains explicit candidate records, target digest records and result
reporting before Vulkan scheduler registration.
