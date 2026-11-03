/**
 * Author......: See docs/credits.txt
 * License.....: MIT
 */

#ifndef HC_BACKEND_VULKAN_MD5_FORMAT_H
#define HC_BACKEND_VULKAN_MD5_FORMAT_H

#include "backend_vulkan_hash.h"

/**
 * Select the byte transformation performed before Vulkan MD5 hashing.
 *
 * The input is an explicitly supplied byte message rather than a generated
 * password candidate. UTF-16LE widens each input byte into one code unit.
 */
typedef enum backend_vulkan_md5_format
{
  BACKEND_VULKAN_MD5_MESSAGE_SALT = 1,
  BACKEND_VULKAN_MD5_SALT_MESSAGE = 2,
  BACKEND_VULKAN_MD5_UTF16LE_MESSAGE_SALT = 3
} backend_vulkan_md5_format_t;

/**
 * Compute MD5 for supplied messages and a shared binary salt on Vulkan.
 *
 * Each digest is 16 bytes in canonical MD5 byte order. The selected format
 * concatenates message and salt in the declared order, or encodes message
 * bytes as UTF-16LE code units before appending salt. This operation does not
 * apply HASHCAT rules, compare target digests or schedule attack workloads.
 *
 * Returns VK_ERROR_INITIALIZATION_FAILED for invalid inputs or lengths
 * outside the uint32 message representation; returns
 * VK_ERROR_OUT_OF_HOST_MEMORY when temporary input allocation fails.
 * Otherwise returns the underlying Vulkan execution result.
 */
VkResult backend_vulkan_md5_format_batch (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  backend_vulkan_md5_format_t format,
  const backend_vulkan_hash_message_t *messages,
  uint32_t message_count,
  const uint8_t *salt,
  uint32_t salt_size,
  uint8_t *digests,
  FILE *diagnostics);

#endif // HC_BACKEND_VULKAN_MD5_FORMAT_H
