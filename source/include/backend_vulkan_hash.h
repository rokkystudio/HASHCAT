/**
 * Author......: See docs/credits.txt
 * License.....: MIT
 */

#ifndef HC_BACKEND_VULKAN_HASH_H
#define HC_BACKEND_VULKAN_HASH_H

/**
 * Hash independent byte strings through the common Vulkan 1.0 executor.
 *
 * The caller provides immutable message spans and receives canonical digest
 * bytes. MD4/MD5, RIPEMD-160, SHA-1/2, BLAKE2s/BLAKE2b and SHA3/Keccak
 * use immutable SPIR-V modules with common input metadata, dynamic GPU
 * allocation, dispatch, fence, cache synchronization and cleanup.
 * No candidate generation, target hash comparison, salt processing or
 * HASHCAT attack scheduling is performed.
 */

#include "types.h"

#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>

/**
 * Identify the exact standard digest algorithm for one Vulkan hash batch.
 *
 * Every algorithm accepts packed byte messages. MD4/MD5 and RIPEMD-160
 * use little-endian 512-bit block padding; SHA-1/2 use big-endian length
 * trailers, including 1024-bit SHA-384/512 blocks. BLAKE2s and BLAKE2b
 * use message block counters, digest-size parameterization and finalization;
 * SHA3/Keccak uses Keccak-f[1600] with explicit domain separation. The enum
 * determines the shader module, digest length, padding and output byte order.
 */
typedef enum backend_vulkan_hash_algorithm
{
  BACKEND_VULKAN_HASH_SHA1 = 1,
  BACKEND_VULKAN_HASH_SHA224 = 2,
  BACKEND_VULKAN_HASH_SHA256 = 3,
  BACKEND_VULKAN_HASH_BLAKE2S = 4,
  BACKEND_VULKAN_HASH_SHA3_224 = 5,
  BACKEND_VULKAN_HASH_SHA3_256 = 6,
  BACKEND_VULKAN_HASH_SHA3_384 = 7,
  BACKEND_VULKAN_HASH_SHA3_512 = 8,
  BACKEND_VULKAN_HASH_KECCAK_256 = 9,
  BACKEND_VULKAN_HASH_SHA384 = 10,
  BACKEND_VULKAN_HASH_SHA512 = 11,
  BACKEND_VULKAN_HASH_MD5 = 12,
  BACKEND_VULKAN_HASH_RIPEMD160 = 13,
  BACKEND_VULKAN_HASH_MD4 = 14,
  BACKEND_VULKAN_HASH_BLAKE2B_256 = 15,
  BACKEND_VULKAN_HASH_BLAKE2B_384 = 16,
  BACKEND_VULKAN_HASH_BLAKE2B_512 = 17
} backend_vulkan_hash_algorithm_t;

/**
 * Describe one immutable input message for GPU hashing.
 *
 * data contains size accessible bytes and does not require a terminator.
 * Zero-length messages may have a NULL data pointer.
 */
typedef struct backend_vulkan_hash_message
{
  const uint8_t *data;
  uint32_t size;
} backend_vulkan_hash_message_t;

/**
 * Hash a batch of arbitrary byte messages with the selected Vulkan algorithm.
 *
 * Requires an open Vulkan backend catalogue, valid device index, supported
 * algorithm, nonempty messages array, writable digests array and non-NULL
 * diagnostics. Every nonempty message requires non-NULL data. Output has
 * exactly message_count consecutive digests: 16 bytes for MD4/MD5, 20 bytes
 * for RIPEMD-160/SHA-1, 28 bytes for SHA-224/SHA3-224, 32 bytes for
 * SHA-256/SHA3-256/Keccak-256/BLAKE2s/BLAKE2b-256, 48 bytes for
 * SHA-384/SHA3-384/BLAKE2b-384 or 64 bytes for SHA-512/SHA3-512/
 * BLAKE2b-512.
 *
 * The method packs lengths and byte offsets into one source storage buffer,
 * validates device limits and sizes, executes one real Compute dispatch and
 * serializes SHA-1/2 words in big-endian order and MD4/MD5/RIPEMD-160,
 * BLAKE2s/BLAKE2b/SHA3/Keccak words in little-endian order. Every input
 * receives algorithm-specific padding, domain separation or finalization and all
 * required compression or permutation blocks.
 *
 * VK_SUCCESS means GPU completion and complete Vulkan resource cleanup.
 * Invalid input returns VK_ERROR_INITIALIZATION_FAILED. Unsupported GPU
 * limits return VK_ERROR_FEATURE_NOT_PRESENT and other driver errors
 * propagate. Input and output remain caller-owned throughout execution.
 */
VkResult backend_vulkan_hash_batch (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  backend_vulkan_hash_algorithm_t algorithm,
  const backend_vulkan_hash_message_t *messages,
  uint32_t message_count,
  uint8_t *digests,
  FILE *diagnostics);

#endif // HC_BACKEND_VULKAN_HASH_H
