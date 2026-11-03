# Vulkan byte-message hash primitives on Android

Date: 2026-11-03

## Production scope

The dynamic Android HASHCAT Vulkan Compute executor processes independent
caller-owned byte messages with fourteen cryptographic digest algorithms:

| Algorithm | Public enum | Rounds | Digest bytes | Byte order |
| --- | --- | ---: | ---: | --- |
| MD4 | `BACKEND_VULKAN_HASH_MD4` | 48 | 16 | Little-endian |
| MD5 | `BACKEND_VULKAN_HASH_MD5` | 64 | 16 | Little-endian |
| RIPEMD-160 | `BACKEND_VULKAN_HASH_RIPEMD160` | 80 per lane | 20 | Little-endian |
| SHA-1 | `BACKEND_VULKAN_HASH_SHA1` | 80 | 20 | Big-endian |
| SHA-224 | `BACKEND_VULKAN_HASH_SHA224` | 64 | 28 | Big-endian |
| SHA-256 | `BACKEND_VULKAN_HASH_SHA256` | 64 | 32 | Big-endian |
| SHA-384 | `BACKEND_VULKAN_HASH_SHA384` | 80 | 48 | Big-endian |
| SHA-512 | `BACKEND_VULKAN_HASH_SHA512` | 80 | 64 | Big-endian |
| BLAKE2s-256 | `BACKEND_VULKAN_HASH_BLAKE2S` | 10 | 32 | Little-endian |
| SHA3-224 | `BACKEND_VULKAN_HASH_SHA3_224` | 24 | 28 | Little-endian |
| SHA3-256 | `BACKEND_VULKAN_HASH_SHA3_256` | 24 | 32 | Little-endian |
| SHA3-384 | `BACKEND_VULKAN_HASH_SHA3_384` | 24 | 48 | Little-endian |
| SHA3-512 | `BACKEND_VULKAN_HASH_SHA3_512` | 24 | 64 | Little-endian |
| Keccak-256 | `BACKEND_VULKAN_HASH_KECCAK_256` | 24 | 32 | Little-endian |

SHA3/Keccak round counts are the 24 Keccak-f[1600] permutation rounds
per absorbed sponge block. SHA-1, SHA-224 and SHA-256 use 512-bit
compression blocks; SHA-384 and SHA-512 use 1024-bit blocks and 80
rounds per block. MD4 and MD5 use 48 and 64 rounds per 512-bit block;
RIPEMD-160 runs two parallel 80-round lanes per 512-bit block.
BLAKE2s uses ten rounds per 64-byte block.

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
to all fourteen hash algorithms.

### `backend_vulkan_hash_batch_dispatch()`

Flushes mapped input storage when noncoherent, executes an explicit
host-write to shader-read dependency, binds the selected pipeline and
descriptor set, and records a single `vkCmdDispatch`. A shader-write to
host-read dependency and fence completion precede output invalidation.

SHA-1 and SHA-2 serialize digest words in big-endian order. MD4, MD5,
RIPEMD-160, BLAKE2s and SHA3/Keccak serialize little-endian state words
to the canonical digest byte sequence. The number of output words is selected from the requested
algorithm's digest byte count. No password workload is scheduled.

## Shader contracts

Each shader uses local size `(32, 1, 1)`. An invocation processes one
message and returns early for an index beyond the declared batch.
Descriptor binding zero is the packed byte input; binding one contains
one fixed-size digest per input.

- `tools/vulkan-backend-md4.comp`: MD4 48-round compression, standard
  64-bit little-endian bit-length trailer and 16-byte digest
- `tools/vulkan-backend-md5-batch.comp`: MD5 64-round compression
  for arbitrary byte-message batches, including multiple padded blocks
- `tools/vulkan-backend-ripemd160.comp`: RIPEMD-160 dual 80-round
  compression lanes with standard independent message order/rotation tables
- `tools/vulkan-backend-sha1.comp`: standard SHA-1 IV, 80-word schedule,
  80-round compression and 64-bit big-endian length trailer
- `tools/vulkan-backend-sha224.comp`: SHA-224 IV, SHA-2 compression and
  the first seven 32-bit state words
- `tools/vulkan-backend-sha256.comp`: SHA-256 IV, SHA-2 compression
  and all eight 32-bit state words
- `tools/vulkan-backend-sha512.comp`: SHA-384/SHA-512 IV,
  eighty-round 1024-bit block compression, a 128-bit big-endian
  length trailer and 384-/512-bit digest extraction
- `tools/vulkan-backend-blake2s.comp`: unkeyed 32-byte parameter block,
  ten-round sigma schedule, processed-byte counters and finalization flag
- `tools/vulkan-backend-keccak.comp`: Keccak-f[1600] sponge absorption,
  24 theta/rho/pi/chi/iota rounds and fixed-length digest extraction

### MD4, MD5 and RIPEMD-160 byte-message hashing

MD4, MD5 and RIPEMD-160 use the same input metadata and tightly packed
byte payload as the SHA-family batch. Every invocation hashes one
independent message with as many 512-bit compression blocks as needed.
The host validates message offsets and padded-block arithmetic before
device allocation; the GPU computes the 0x80 separator and little-endian
64-bit message length trailer.

MD4 applies 48 compression steps with standard F/G/H functions,
message-word permutations and round shifts from
`source/OpenCL/inc_hash_md4.cl` and `source/OpenCL/inc_types.h`.
MD5 uses all 64 compression steps, message permutations, constants and
rotations from the existing fixed-block
`tools/vulkan-backend-md5.comp`. The MD5 batch shader has a separate
module and is selected by `BACKEND_VULKAN_HASH_MD5`; the original
four-fixed-vector information-mode MD5 diagnostic remains independent.

RIPEMD-160 executes both parallel 80-round chains with independent
message permutations, shift tables and round constants. Its tables
are taken from `source/OpenCL/inc_hash_ripemd160.cl` and
`source/OpenCL/inc_types.h`. After each block, the two chains are
combined with the five RIPEMD-160 state words. Each of the three
algorithms writes uint32 state words that the executor serializes as
canonical little-endian digest bytes.

These are generic cryptographic digest primitives. They do not consume
HASHCAT password candidates or compare against target digest arrays.

### SHA-384/512 variants

SHA-384 and SHA-512 compile two immutable modules from one GLSL source
using `VULKAN_SHA512_DIGEST_BYTES=48` and
`VULKAN_SHA512_DIGEST_BYTES=64`. SHA-384 uses the SHA-384 initial
state and returns the first six 64-bit state words. SHA-512 uses the
SHA-512 initial state and returns all eight 64-bit state words.

The shader implements 80 standard SHA-512 compression rounds with
an 80-word message schedule, 1024-bit blocks, standard 0x80 padding
and the 128-bit big-endian bit-length trailer. Input lengths are uint32,
so the high 64 length bits are zero and the low 64 bits are composed of
two uint32 values. Message sizes that overflow padded block addressing
are rejected before dispatch.

The 64-bit state uses `uvec2(low32, high32)`. The shader implements
modulo-2^64 addition with carry propagation and all right rotations
and logical shifts with 32-bit operations. Neither algorithm requires
the optional Vulkan `shaderInt64` feature. Each output 64-bit word is
stored as its high uint32 then low uint32 so the host's big-endian
serialization emits canonical SHA-384/512 digest bytes.

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

Compile all thirteen tracked non-SHA-256 SPIR-V modules and the common
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
execution and digest-output validation for the new MD4, MD5 batch,
RIPEMD-160, SHA-1, SHA-224, BLAKE2s-256, SHA-384/512,
SHA3-224/256/384/512 and Keccak-256 paths are not
confirmed by these compilation results. Earlier SHA-256 hardware
validation remains documented separately.

## Integration boundary

The fourteen-algorithm Vulkan batch API consumes application-owned byte
messages under an open Vulkan catalogue. It does not register a Vulkan
HASHCAT hashing device or connect candidate generation, attack scheduling,
digest matching or OpenCL `mXXXXX` hash modes to Vulkan.
