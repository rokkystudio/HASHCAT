/**
 * Author......: See docs/credits.txt
 * License.....: MIT
 */

#ifndef HC_BACKEND_VULKAN_SHA256_H
#define HC_BACKEND_VULKAN_SHA256_H

/**
 * Compute SHA-256 digests of independent byte strings on Vulkan 1.0.
 *
 * The API accepts application-owned byte spans, packs them into GPU storage
 * buffers, performs the complete SHA-256 padding and compression algorithm
 * in a Compute shader and returns canonical 32-byte digests. Every message
 * has its own byte length and may require multiple 512-bit blocks.
 *
 * The caller owns the parent Vulkan catalogue, input spans and output array.
 * Temporary VkDevice, storage, descriptor, pipeline, command and fence
 * ownership is provided by the common backend_vulkan_kernel dynamic executor
 * used by the public arithmetic and MD5 programs. This API does not generate
 * messages, compare digests with targets or use HASHCAT attack scheduling.
 */

#include "types.h"

#include <vulkan/vulkan.h>
#include <stdint.h>
#include <stdio.h>

/**
 * Describe one immutable byte string passed to the SHA-256 GPU batch.
 *
 * data points to exactly size available bytes. A zero-sized message may
 * use a NULL data pointer. No NUL termination is required.
 */
typedef struct backend_vulkan_sha256_message
{
  const uint8_t *data;
  uint32_t size;
} backend_vulkan_sha256_message_t;

/**
 * Hash arbitrary byte messages in one Vulkan Compute dispatch.
 *
 * Requires an open backend-owned Vulkan catalogue, valid device_index,
 * message_count greater than zero, non-NULL messages, digests and
 * diagnostics, and non-NULL data for each nonempty message. The caller
 * allocates exactly 32 * message_count writable bytes for digests.
 *
 * Each shader invocation reads one packed byte message and executes
 * standard SHA-256 with 64 rounds per 512-bit block, including padding
 * and the 64-bit bit-length trailer. The output consists of canonical
 * 32-byte SHA-256 digests in the input order. Individual message lengths,
 * packed 32-bit offsets, host allocation bounds, exact heterogeneous storage
 * ranges and physical device limits are validated before memory allocation.
 *
 * VK_SUCCESS requires completed GPU execution and successful resource
 * cleanup. Invalid arguments return VK_ERROR_INITIALIZATION_FAILED,
 * unsupported device limits return VK_ERROR_FEATURE_NOT_PRESENT, and
 * allocation or Vulkan driver errors propagate to the caller.
 *
 * No HASHCAT candidate generator, target-digest matching, salts or attack
 * scheduler participates in this operation.
 */
VkResult backend_vulkan_sha256_hash_batch (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  const backend_vulkan_sha256_message_t *messages,
  uint32_t message_count,
  uint8_t *digests,
  FILE *diagnostics);

/**
 * Verify the SHA-256 batch API with four public known-answer messages.
 *
 * Requires an open backend Vulkan catalogue, valid physical-device index and
 * non-NULL diagnostics. The batch contains an empty message, "abc", 56 bytes
 * of 'a' and 100 bytes of 'a'. The first two require one padded block; the
 * latter two require two blocks and exercise the 56-byte padding boundary.
 *
 * VK_SUCCESS requires all four canonical 32-byte GPU digests to match their
 * independent known answers. The method uses backend_vulkan_sha256_hash_batch()
 * and therefore the common dynamic Vulkan resource owner. It does not consume
 * HASHCAT candidates, target digests, salts or scheduler state.
 */
VkResult backend_vulkan_sha256_verify_known_answers (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  FILE *diagnostics);

#endif // HC_BACKEND_VULKAN_SHA256_H
