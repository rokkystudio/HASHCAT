# Vulkan byte-message hash primitives on Android

Date: 2026-11-02

## Production scope

The dynamic Android HASHCAT Vulkan Compute executor processes independent
caller-owned byte messages with nine cryptographic digest algorithms:

| Algorithm | Public enum | Rounds | Digest bytes | Byte order |
| --- | --- | ---: | ---: | --- |
| SHA-1 | `BACKEND_VULKAN_HASH_SHA1` | 80 | 20 | Big-endian |
| SHA-224 | `BACKEND_VULKAN_HASH_SHA224` | 64 | 28 | Big-endian |
| SHA-256 | `BACKEND_VULKAN_HASH_SHA256` | 64 | 32 | Big-endian |
| BLAKE2s-256 | `BACKEND_VULKAN_HASH_BLAKE2S` | 10 | 32 | Little-endian |
| SHA3-224 | `BACKEND_VULKAN_HASH_SHA3_224` | 24 | 28 | Little-endian |
| SHA3-256 | `BACKEND_VULKAN_HASH_SHA3_256` | 24 | 32 | Little-endian |
| SHA3-384 | `BACKEND_VULKAN_HASH_SHA3_384` | 24 | 48 | Little-endian |
| SHA3-512 | `BACKEND_VULKAN_HASH_SHA3_512` | 24 | 64 | Little-endian |
| Keccak-256 | `BACKEND_VULKAN_HASH_KECCAK_256` | 24 | 32 | Little-endian |

SHA3/Keccak round counts are the 24 Keccak-f[1600] permutation rounds
per absorbed sponge block. SHA-1/2 counts are compression rounds per
512-bit block. BLAKE2s runs ten mixing rounds per 64-byte block.

Each algorithm has an immutable Vulkan 1.0 SPIR-V module and uses
the same dynamically owned two-buffer Compute program graph.

These are cryptographic primitives. They do not implement an OpenCL
`mXXXXX_*` attack-mode kernel, password candidate generation, salt
processing, target digest comparison or registration with the
HASHCAT hashing scheduler.

## Native API

The public batch entry point is
`backend_vulkan_hash_batch(backend_ctx, device_index, algorithm, messages,
message_count, digests, diagnostics)` in
`source/include/backend_vulkan_hash.h`.

`backend_vulkan_hash_message_t` borrows one immutable byte pointer and its
exact uint32 length. A zero-length message permits a null pointer. The
caller owns a contiguous output array of `message_count * digest_bytes`.

The legacy `backend_vulkan_sha256_hash_batch()` interface remains available
and selects SHA-256 through the same executor.

### `backend_vulkan_hash_batch()`

Checks the Vulkan catalogue, physical-device index, explicit algorithm,
input pointers, message counts, checked 32-bit payload offsets and
algorithm-specific padded block address limits. Declares a one-stage
`backend_vulkan_program_t` with exact heterogeneous input/output buffer
sizes, the selected SPIR-V module, local size `(32, 1, 1)` and the
corresponding group count. The dynamic executor verifies physical GPU
limits, runs the declared shader and releases all owned resources.

Success requires completed GPU execution and successful resource cleanup.
Invalid arguments return `VK_ERROR_INITIALIZATION_FAILED`; unsupported
device limits return `VK_ERROR_FEATURE_NOT_PRESENT`, and other Vulkan
results propagate. Inputs and output remain caller-owned.

### `backend_vulkan_hash_batch_pack_input()`

Copies the input to binding zero. The buffer begins with two uint32 metadata
words per message: payload-relative byte offset, then byte length.
Unaligned messages are packed consecutively after the metadata table;
the final allocation padding is initialized to zero. The layout is common
to all nine hash algorithms.

### `backend_vulkan_hash_batch_dispatch()`

Flushes mapped input storage when noncoherent, executes an explicit
host-write to shader-read dependency, binds the selected pipeline and
descriptor set, and records a single `vkCmdDispatch`. A shader-write to
host-read dependency and fence completion precede output invalidation.

SHA-1 and SHA-2 serialize digest words in big-endian order. BLAKE2s and
SHA3/Keccak serialize little-endian state words to the canonical digest
byte sequence. The number of output words is selected from the requested
algorithm's digest byte count. No password workload is scheduled.

## Shader contracts

Each shader uses local size `(32, 1, 1)`. An invocation processes one
message and returns early for an index beyond the declared batch.
Descriptor binding zero is the packed byte input; binding one contains
one fixed-size digest per input.

- `tools/vulkan-backend-sha1.comp`: standard SHA-1 IV, 80-word schedule,
  80-round compression and 64-bit big-endian length trailer
- `tools/vulkan-backend-sha224.comp`: SHA-224 IV, SHA-2 compression and
  the first seven 32-bit state words
- `tools/vulkan-backend-sha256.comp`: SHA-256 IV, SHA-2 compression
  and all eight 32-bit state words
- `tools/vulkan-backend-blake2s.comp`: unkeyed 32-byte parameter block,
  ten-round sigma schedule, processed-byte counters and finalization flag
- `tools/vulkan-backend-keccak.comp`: Keccak-f[1600] sponge absorption,
  24 theta/rho/pi/chi/iota rounds and fixed-length digest extraction

### Keccak-f[1600] variants

Five immutable modules are compiled from the shared Keccak GLSL file with
explicit definitions of `VULKAN_HASH_RATE_BYTES`,
`VULKAN_HASH_SUFFIX` and `VULKAN_HASH_DIGEST_BYTES`.

| Variant | Absorb rate, bytes | Domain suffix | Digest bytes |
| --- | ---: | ---: | ---: |
| SHA3-224 | 144 | `0x06` | 28 |
| SHA3-256 | 136 | `0x06` | 32 |
| SHA3-384 | 104 | `0x06` | 48 |
| SHA3-512 | 72 | `0x06` | 64 |
| Keccak-256 | 136 | `0x01` | 32 |

Every variant uses 200 bytes of permutation state and standard multirate
padding: the domain suffix follows the message and the final byte of
the last absorb block includes `0x80`. Messages aligned to the absorb
rate receive an additional padding block. Output is squeezed from the
first state lanes using little-endian lane bytes.

The 64-bit Keccak lanes are represented as `uvec2` containing low/high
32-bit words. The shader implements 64-bit rotations using explicit
32-bit shifts and never requires the optional Vulkan `shaderInt64`
feature. SHA3-256 and Keccak-256 differ in the domain suffix despite
their equal rate and output length.

The public BLAKE2s mode remains unkeyed with a fixed 256-bit digest;
keyed operation, custom salt, personalization and variable-length
BLAKE2s output are not exposed.

## Reproducible Android compilation

Compile all eight tracked non-SHA-256 SPIR-V modules and the common
executor for Android API 24 ARM64 and ARMv7:

```powershell
.\tools\build-vulkan-backend-hash-primitives.ps1 `
  -AndroidNdk D:\Android\SDK\ndk\29.0.14206865 `
  -GlslangCompiler D:\Android\SDK\emulator\lib64\vulkan\glslangValidator.exe
```

The build generates all shader variants and verifies that the byte streams
are identical to the checked-in SPIR-V headers. It compiles the common
Vulkan C executor with `-Wall -Wextra -Werror` for both Android ABIs.
The separate `tools/build-vulkan-backend-sha256.ps1` also links the
common executor with the existing SHA-256 diagnostic program.

**Current verification boundary:** shader compilation, embedded SPIR-V
reproducibility and ARM64/ARMv7 native compilation have passed. Hardware
execution and digest-output validation for the new SHA-1, SHA-224,
BLAKE2s-256, SHA3-224/256/384/512 and Keccak-256 paths are not
confirmed by these compilation results. Earlier SHA-256 hardware
validation remains documented separately.

## Integration boundary

The nine-algorithm Vulkan batch API consumes application-owned byte
messages under an open Vulkan catalogue. It does not register a Vulkan
HASHCAT hashing device or connect candidate generation, attack scheduling,
digest matching or OpenCL `mXXXXX` hash modes to Vulkan.
