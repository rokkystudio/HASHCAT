# Variable-length SHA-256 Vulkan batch in Android HASHCAT

Date: 2026-11-02

## Scope

The Android Vulkan backend exposes a production SHA-256 byte-message batch API
through the same dynamic executor used by the public Compute graphs and the MD5
known-answer stage.

`backend_vulkan_sha256_hash_batch()` accepts caller-owned byte spans with an
independent length for every message, packs them into one storage buffer,
executes the complete SHA-256 padding and compression algorithm in one Compute
dispatch, and returns canonical 32-byte digests in input order.

The Vulkan program does not generate password candidates, ingest target
digests, compare hashes, process salts or register a hashing device with the
HASHCAT scheduler. The current integration executes from `hashcat -I` as a
cryptographic backend verification step.

## Common dynamic executor

SHA-256 does not own a parallel Vulkan resource implementation.

The program runs through `backend_vulkan_kernel_verify_program()`, which owns
the temporary:

- `VkDevice` and Compute queue;
- storage buffers and mapped memory;
- descriptor set layout, descriptor pool and descriptor set;
- shader module and Compute pipeline;
- command pool and command buffer;
- fence;
- submission, host-cache synchronization and cleanup.

The parent `VkInstance` and selected `VkPhysicalDevice` remain owned by the
backend Vulkan catalogue.

`source/src/backend_vulkan_sha256.c` is not part of the runtime object list.
The public SHA-256 API is implemented by the common executor in
`source/src/backend_vulkan_kernel.c`.

## Heterogeneous storage declarations

### `backend_vulkan_program_t`

A program supports two explicit storage-size forms:

```text
uniform:
  buffer_bytes != 0
  buffer_sizes == NULL

heterogeneous:
  buffer_bytes == 0
  buffer_sizes[0..buffer_count-1] contain exact nonzero ranges
```

Public arithmetic and MD5 programs use the uniform form.

SHA-256 uses the heterogeneous form because its packed input and digest output
have different logical sizes.

### `backend_vulkan_program_buffer_bytes(program, buffer_index)`

Returns the exact logical descriptor range for one declared storage buffer.
Malformed sizing declarations, zero ranges and out-of-range indices return
zero.

### `backend_vulkan_program_largest_buffer_bytes(program)`

Returns the largest exact buffer range only when every declared range is
valid. `backend_vulkan_program_validate()` uses this value when checking
`maxStorageBufferRange`.

The common allocator stores the resolved logical size in each
`backend_vulkan_public_buffer_t`. Buffer creation, descriptor ranges and
buffer memory barriers therefore use the exact size of the selected storage
buffer.

Uniform-program diagnostic event text remains stable. Heterogeneous programs
add `heterogeneous=1` explicitly:

```text
BACKEND_VULKAN_PROGRAM_ACCEPT stages=1 buffers=2 bytes=192 heterogeneous=1 descriptors_per_stage=2
```

## SHA-256 batch data contract

### `backend_vulkan_sha256_message_t`

Describes one immutable byte span:

```c
typedef struct backend_vulkan_sha256_message
{
  const uint8_t *data;
  uint32_t size;
} backend_vulkan_sha256_message_t;
```

A zero-length message may use a NULL data pointer. Nonempty messages require
exactly `size` readable bytes; no NUL terminator is required.

### Input storage buffer

For `N` messages, the input starts with `N` metadata records:

```text
uint32 payload_offset
uint32 byte_length
```

Offsets are relative to the packed payload region immediately following the
metadata table.

The host buffer therefore contains:

```text
metadata[0]
metadata[1]
...
metadata[N-1]
packed message bytes
alignment padding to 4 bytes
```

`backend_vulkan_sha256_pack_input()` zeroes the complete declared range,
writes metadata in native uint32 form and copies each caller-owned byte span
into the packed payload.

### Output storage buffer

The output contains exactly eight `uint32` words per message.

After real GPU completion, the host serializes every word in big-endian byte
order to produce canonical 32-byte SHA-256 digests.

## `backend_vulkan_sha256_hash_batch()`

The method requires:

- an open backend Vulkan catalogue;
- a valid physical-device index;
- `message_count > 0`;
- non-NULL message, output and diagnostics pointers;
- non-NULL data for every nonempty message;
- `message_count <= INT32_MAX / 8`;
- every message length at most `UINT32_MAX - 72`;
- metadata, packed payload and aligned input to fit the 32-bit shader offset
  contract;
- output size to fit host allocation and Vulkan storage limits.

The per-message upper bound preserves the shader's 32-bit padded-length
arithmetic for the worst padding case.

The method builds one runtime program declaration:

```text
stage_count  = 1
buffer_count = 2
buffer_bytes = 0

buffer_sizes[0] = aligned metadata + payload bytes
buffer_sizes[1] = message_count * 32

stage
  source_buffer_index      = 0
  destination_buffer_index = 1
  local_size               = 32,1,1
  group_count              = ceil(message_count / 32)
```

The stage uses `backend_vulkan_sha256_spirv` and
`BACKEND_VULKAN_EXECUTION_SHA256`. The host execution context is explicit;
shader identity, GPU name and process ABI do not select the data contract.

## GPU execution

`backend_vulkan_sha256_dispatch()` performs:

```text
pack metadata and payload
-> flush input memory when noncoherent
-> begin command buffer
-> HOST_WRITE -> SHADER_READ input barrier
-> bind common descriptor set and Compute pipeline
-> one vkCmdDispatch
-> SHADER_WRITE -> HOST_READ output barrier
-> submit with the common owned fence
-> wait for the real fence
-> invalidate output memory when noncoherent
-> serialize eight words/message to canonical digest bytes
```

Successful execution reports:

```text
BACKEND_VULKAN_SHA256_DISPATCH_PASS messages=... input_bytes=...
output_bytes=... groups=... dispatches=1 digests=...
password_generation=0 target_digests=0 scheduled=0
```

The common cleanup path subsequently reports:

```text
BACKEND_VULKAN_SHA256_CLOSE index=... result=0 released=1
password_generation=0 target_digests=0 scheduled=0
```

## Compute shader

`tools/vulkan-backend-sha256.comp` is GLSL 450 compiled for
Vulkan 1.0 / SPIR-V 1.0.

Each invocation:

1. reads one metadata record;
2. derives the number of 512-bit blocks including SHA-256 padding;
3. builds each 64-byte block byte-for-byte from the packed payload;
4. inserts `0x80` and the 64-bit big-endian bit-length trailer;
5. executes all 64 SHA-256 rounds per block;
6. carries the eight state words across multiple blocks;
7. writes eight final state words.

The current embedded module is:

| Property | Value |
| --- | ---: |
| SPIR-V bytes | 12,876 |
| SPIR-V words | 3,219 |
| SHA-256 | `af3f8b163f233e8f358ef0af31e6c62fb2fcb12a94a60a59399bb665824eed6e` |

`tools/generate-vulkan-backend-sha256-kernel.py` validates canonical
little-endian SPIR-V magic, version 1.0, word alignment and schema zero before
writing the embedded header.

## Known-answer verification

### `backend_vulkan_sha256_verify_known_answers()`

One batch contains four public messages:

| Message | Padding shape | Expected SHA-256 |
| --- | --- | --- |
| empty | one block | `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855` |
| `abc` | one block | `ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad` |
| 56 × `a` | two blocks | `b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a` |
| 100 × `a` | two blocks | `2816597888e4a0d3a36b82b83316ab32680eb8f00f8cd3b904d681246d285a0e` |

The 56-byte message exercises the SHA-256 padding boundary where the
`0x80` byte and 64-bit length trailer require a second block. The 100-byte
message exercises ordinary multi-block compression.

The KAT batch has exact ranges:

```text
input_bytes  = 192
output_bytes = 128
messages     = 4
local_size   = 32
group_count  = 1
```

All 128 digest bytes are compared independently on the host.

## Android build

Build the standalone common-executor diagnostic with:

```powershell
.\tools\build-vulkan-backend-sha256.ps1 \
  -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865 \
  -AndroidApi 24 \
  -GlslangCompiler D:\ANDROID\SDK\emulator\lib64\vulkan\glslangValidator.exe
```

The script:

- recompiles the GLSL shader for Vulkan 1.0;
- requires the embedded SPIR-V header to reproduce exactly;
- compiles production `backend.c` for ARM64 and ARMv7 with
  `-Wall -Wextra -Werror`;
- checks the default non-Vulkan `backend.c` path;
- links only the common Vulkan executor sources;
- requires `libvulkan.so`;
- rejects test-only `__wrap_vk*` symbols.

Current standalone artifacts:

| Artifact | Bytes | SHA-256 |
| --- | ---: | --- |
| `vulkan-backend-sha256.spv` | 12,876 | `af3f8b163f233e8f358ef0af31e6c62fb2fcb12a94a60a59399bb665824eed6e` |
| ARM64 backend object | 199,976 | `3ef175c74269e5196eb3252689558d91c79583d5b6bcdd991bf2f253f78ed48c` |
| ARM64 diagnostic | 69,072 | `ce97858724346957675b356bd97c5b507e267bb03e2780caf943e2fb8894c526` |
| ARMv7 backend object | 173,940 | `a48e03023fd0f00858be22a0a376a445a3e6566d80e04d1bcdabc42c2733525d` |
| ARMv7 diagnostic | 69,196 | `48324dd7385889aeb6aaf3f4cdd622d43a2fc4e1d0cce7a28790f6b5a2b77e4f` |

## Physical GPU verification

The variable-length batch completed on two independent GPU/process
configurations using a requested Vulkan 1.0 instance:

| GPU / ABI | Result |
| --- | --- |
| Pixel 9 Pro XL / Mali-G715 ARM64 | PASS |
| RedMagic 7S Pro / Adreno 730 ARMv7 | PASS |

Each process executed one SHA-256 dispatch over all four messages, including
both two-block cases, compared 128 digest bytes and exited 0.

This milestone deliberately does not repeat the earlier Vulkan 1.0/1.1
cross-product because the new contract is host data packing and heterogeneous
storage sizing rather than a new API-level dependency.

## Full HASHCAT integration

Opt-in full Android frontends were built with
`-EnableVulkanDiscovery -VulkanOutputName vulkan-hashcat-sha256-20261102`:

| ABI | Frontend bytes | SHA-256 |
| --- | ---: | --- |
| ARM64 | 1,572,768 | `bc92ada65ce0c007132285a8079a4235e0b559dd5417da5aa2c64afde8e255e9` |
| ARMv7 | 1,828,820 | `6f18c0a25110493350adf2a21b4592b832cdd99a0f0feb4561f5106a54cff47b` |

`tools/run-vulkan-hashcat-sha256.ps1` stages an isolated complete runtime and
runs only `hashcat -I`.

Production integration passed on:

| Device / ABI | Result |
| --- | --- |
| Pixel 9 Pro XL / Mali-G715 ARM64 | PASS |
| RedMagic 7S Pro / Adreno 730 ARMv7 | PASS |

Both processes execute the existing MD5 stage, then the SHA-256 variable-length
batch, then retain ordinary OpenCL device discovery. Both native processes exit
0.

## Current boundary

The Vulkan backend now contains two real cryptographic execution contracts:

- MD5 one-block fixed public known-answer vectors;
- SHA-256 arbitrary caller-owned byte-message batches with variable lengths and
  multi-block padding.

SHA-256 therefore establishes a reusable candidate-shaped byte input contract,
but the bytes are still supplied directly by the diagnostic/information-mode
caller.

The remaining scheduler boundary is explicit:

- map HASHCAT candidate records to byte spans without heuristic conversion;
- represent target digest records for the selected hash mode;
- execute target comparison and result reporting;
- connect attack amplifiers/rules/masks where required;
- register Vulkan as a hashing device only after those contracts are validated.

No current Vulkan path consumes `pw_t` records or participates in normal
password cracking.
