/**
 * Author......: See docs/credits.txt
 * License.....: MIT
 */

#include "backend_vulkan_md5_format.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/**
 * Construct complete byte messages for one of the three MD5 formats.
 *
 * All temporary message storage remains alive until the common Vulkan
 * executor completes, then every allocation is released on all paths.
 * UTF-16LE conversion maps each supplied input byte to a zero-extended
 * 16-bit code unit and does not perform Unicode decoding.
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
  FILE *diagnostics)
{
  if (format != BACKEND_VULKAN_MD5_MESSAGE_SALT &&
      format != BACKEND_VULKAN_MD5_SALT_MESSAGE &&
      format != BACKEND_VULKAN_MD5_UTF16LE_MESSAGE_SALT)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  if (messages == NULL || message_count == 0u ||
      digests == NULL || diagnostics == NULL ||
      (salt_size != 0u && salt == NULL))
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  if ((size_t) message_count > SIZE_MAX / sizeof (backend_vulkan_hash_message_t))
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  backend_vulkan_hash_message_t *prepared = (backend_vulkan_hash_message_t *)
    calloc (message_count, sizeof (*prepared));

  if (prepared == NULL) return VK_ERROR_OUT_OF_HOST_MEMORY;

  VkResult result = VK_ERROR_INITIALIZATION_FAILED;

  for (uint32_t i = 0u; i < message_count; i++)
  {
    const backend_vulkan_hash_message_t *message = &messages[i];

    if (message->size != 0u && message->data == NULL) goto cleanup;

    const uint64_t message_bytes =
      (uint64_t) message->size *
      (format == BACKEND_VULKAN_MD5_UTF16LE_MESSAGE_SALT ? 2u : 1u);
    const uint64_t total_bytes = message_bytes + salt_size;

    if (total_bytes > UINT32_MAX) goto cleanup;

    uint8_t *data = (uint8_t *) malloc ((size_t) total_bytes == 0u ? 1u : (size_t) total_bytes);

    if (data == NULL)
    {
      result = VK_ERROR_OUT_OF_HOST_MEMORY;

      goto cleanup;
    }

    prepared[i].data = data;
    prepared[i].size = (uint32_t) total_bytes;

    if (format == BACKEND_VULKAN_MD5_SALT_MESSAGE)
    {
      if (salt_size != 0u) memcpy (data, salt, salt_size);
      if (message->size != 0u) memcpy (data + salt_size, message->data, message->size);
    }
    else if (format == BACKEND_VULKAN_MD5_UTF16LE_MESSAGE_SALT)
    {
      for (uint32_t j = 0u; j < message->size; j++)
      {
        data[(size_t) j * 2u] = message->data[j];
        data[(size_t) j * 2u + 1u] = 0u;
      }

      if (salt_size != 0u) memcpy (data + (size_t) message_bytes, salt, salt_size);
    }
    else
    {
      if (message->size != 0u) memcpy (data, message->data, message->size);
      if (salt_size != 0u) memcpy (data + message->size, salt, salt_size);
    }
  }

  result = backend_vulkan_hash_batch (
    backend_ctx,
    device_index,
    BACKEND_VULKAN_HASH_MD5,
    prepared,
    message_count,
    digests,
    diagnostics);

cleanup:

  for (uint32_t i = 0u; i < message_count; i++)
  {
    free ((void *) prepared[i].data);
  }

  free (prepared);

  return result;
}
