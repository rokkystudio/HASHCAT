# Vulkan byte-message hash primitives on Android

Date: 2026-11-02

## Production scope

The dynamic Android HASHCAT Vulkan Compute executor can compute independent
SHA-1, SHA-224, SHA-256 and unkeyed BLAKE2s-256 digests for caller-owned byte
messages. Each algorithm has an immutable Vulkan 1.0 SPIR-V module, uses the
same two-buffer dynamic storage graph and returns canonical digest bytes.

These are reusable cryptographic primitives. They do not implement an OpenCL
`mXXXXX_*` attack-mode kernel, password candidate generation, salt handling,
digest target comparison or registration with HASHCAT's hashing scheduler.

## Native API

The primary entry point is
`backend_vulkan_hash_batch(backend_ctx, device_index, algorithm, messages,
message_count, digests, diagnostics)` in
`source/include/backend_vulkan_hash.h`.

`backend_vulkan_hash_message_t` owns no memory. Each element borrows a byte
pointer and exact uint32 length. A zero-length message permits a null pointer.
The caller allocates a contiguous output array sized by the selected digest
length and message count:

| Algorithm | Enum constant | GPU rounds per compression | Bytes per digest |
| --- | --- | ---: | ---: |
| SHA-1 | `BACKEND_VULKAN_HASH_SHA1` | 80 | 20 |
| SHA-224 | `BACKEND_VULKAN_HASH_SHA224` | 64 | 28 |
| SHA-256 | `BACKEND_VULKAN_HASH_SHA256` | 64 | 32 |
| BLAKE2s-256 | `BACKEND_VULKAN_HASH_BLAKE2S` | 10 | 32 |

The existing `backend_vulkan_sha256_hash_batch()` interface remains
available and selects SHA-256 through the same dynamic executor.

### `backend_vulkan_hash_batch()`

Validates the backend catalogue, requested physical-device index, algorithm,
input pointers, lengths, checked 32-bit payload offsets, host sizes and
Vulkan device limits. Creates a one-stage
`backend_vulkan_program_t` with exact input/output buffer ranges and the
selected immutable SPIR-V program. Dispatches through the existing resource
owner and copies the completed canonical digests into caller memory.
`VK_SUCCESS` requires GPU completion and successful cleanup.

### `backend_vulkan_hash_batch_pack_input()`

Writes two uint32 metadata words per message: the byte offset relative to the
payload start, followed by the message length. The payload immediately follows
all metadata records and is densely packed. Allocated terminal padding is zero.
All input data are copied before queue submission.

### `backend_vulkan_hash_batch_dispatch()`

Flushes mapped input storage where noncoherent, emits a host-write to
shader-read barrier, binds the selected pipeline and descriptor set, then
executes exactly one `vkCmdDispatch`. A shader-write to host-read dependency
and real fence wait precede output invalidation and copying.

Each SHA shader writes uint32 digest words in big-endian SHA word order;
these are serialized as big-endian bytes. BLAKE2s writes uint32 state words
in little-endian byte order, which are serialized correspondingly.
No candidate or target digest buffers are inspected.

## Shader contracts

All shaders use workgroup size `(32, 1, 1)`. The logical invocation index
selects one input message; out-of-range lanes return without writing. Input
buffer binding zero contains the shared offset/length table and packed bytes.
Output binding one contains one fixed-size digest per input.

- `tools/vulkan-backend-sha1.comp`: SHA-1 IV, 80-word schedule and 80-round
  compression, standard 64-bit big-endian message length trailer.
- `tools/vulkan-backend-sha224.comp`: SHA-224 initial state, SHA-2 64-round
  compression and the first seven state words as the 224-bit digest.
- `tools/vulkan-backend-sha256.comp`: SHA-256 IV, 64-round SHA-2 compression
  and all eight state words as the 256-bit digest.
- `tools/vulkan-backend-blake2s.comp`: unkeyed 32-byte BLAKE2s parameter
  block, standard ten-round sigma schedule, 64-byte message blocks, processed
  byte counter and last-block finalization.

SHA-1, SHA-224 and SHA-256 implement standard multi-block SHA padding.
BLAKE2s processes a final partial block padded with zeros, while exact
64-byte block multiples finalize their last full block. The empty input
processes one final zero-filled BLAKE2s block.

BLAKE2s is currently restricted to unkeyed BLAKE2s-256: no keyed mode,
digest truncation, personalization or salt configuration.

## Build and availability

Build SPIR-V modules and both Android ABI native executor objects:

```powershell
.\tools\build-vulkan-backend-hash-primitives.ps1 `
  -AndroidNdk D:\Android\SDK\ndk\29.0.14206865 `
  -GlslangCompiler D:\Android\SDK\emulator\lib64\vulkan\glslangValidator.exe
```

The build compiles Vulkan 1.0 SPIR-V, regenerates the embedded headers and
requires byte-exact reproducibility. The native kernel compiles for Android
ARM64 and ARMv7 with `-Wall -Wextra -Werror`; the same compiled executor
also links into the existing SHA-256 diagnostic binary.

The algorithms have passed compilation and linkage on both Android ABIs.
These specific new SHA-1, SHA-224 and BLAKE2s-256 kernels have not yet been
verified by execution on physical GPU hardware. Passing compilation is not
presented as evidence of confirmed digest equality or runtime performance.

## Integration boundary

The Vulkan hash batch API is callable with application-owned data under an
open Vulkan catalogue. It is not connected to HASHCAT candidate generation,
hash-mode modules, attack scheduling or digest matching. The production
OpenCL backend continues to handle those workloads.
