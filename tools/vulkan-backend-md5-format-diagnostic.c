/**
 * Verify caller-supplied MD5 message/salt formats on an Android Vulkan GPU.
 *
 * Executes public known-answer inputs through the production Vulkan catalogue
 * and shared MD5 Compute executor. No candidate rules, target digests or
 * HASHCAT attack scheduling participate.
 */

#include "backend_vulkan.h"
#include "backend_vulkan_md5_format.h"

#include <stdio.h>
#include <string.h>

/**
 * Check one canonical MD5 digest against its hexadecimal known answer.
 */
static int verify_digest (const uint8_t *digest, const char *expected)
{
  static const char digits[] = "0123456789abcdef";

  for (uint32_t i = 0u; i < 16u; i++)
  {
    if (digits[digest[i] >> 4u] != expected[i * 2u] ||
        digits[digest[i] & 15u] != expected[i * 2u + 1u])
    {
      return 0;
    }
  }

  return 1;
}

/**
 * Run public format vectors on a live Vulkan device and release its catalogue.
 *
 * Covers both concatenation orders, byte-to-UTF-16LE expansion, empty inputs,
 * binary salt and a padded two-block MD5 message. Each invocation uses the
 * existing production MD5 SPIR-V shader and verifies all returned digest bytes.
 */
static int verify_formats (uint32_t minimum_api)
{
  backend_ctx_t backend = { 0 };

  VkResult result = backend_vulkan_catalog_open (&backend, minimum_api, stdout);

  if (result != VK_SUCCESS) return 1;

  int status = 1;

  static const uint8_t abc[] = { 'a', 'b', 'c' };
  static const uint8_t salt[] = { 's', 'a', 'l', 't' };
  static const uint8_t binary_message[] = { 0x00u, 0xffu };
  static const uint8_t binary_salt[] = { 0x80u, 0x00u };
  uint8_t long_message[56];

  memset (long_message, 'A', sizeof (long_message));

  static const uint8_t long_salt[] = { 'Z', 'Z', 'Z', 'Z', 'Z', 'Z', 'Z', 'Z' };

  const backend_vulkan_hash_message_t messages[] =
  {
    { abc, sizeof (abc) },
    { NULL, 0u },
    { long_message, sizeof (long_message) },
    { binary_message, sizeof (binary_message) }
  };

  const struct
  {
    backend_vulkan_md5_format_t format;
    uint32_t message_index;
    const uint8_t *salt;
    uint32_t salt_size;
    const char *expected;
  } cases[] =
  {
    { BACKEND_VULKAN_MD5_MESSAGE_SALT, 0u, salt, sizeof (salt), "1e21f6da01c5047e4bf45b0496bd6937" },
    { BACKEND_VULKAN_MD5_SALT_MESSAGE, 0u, salt, sizeof (salt), "10374787ed585803bc4cdb63f92a545b" },
    { BACKEND_VULKAN_MD5_UTF16LE_MESSAGE_SALT, 0u, salt, sizeof (salt), "9609bf5d07578677eee0209a6509e931" },
    { BACKEND_VULKAN_MD5_MESSAGE_SALT, 1u, NULL, 0u, "d41d8cd98f00b204e9800998ecf8427e" },
    { BACKEND_VULKAN_MD5_MESSAGE_SALT, 2u, long_salt, sizeof (long_salt), "7cf73be9831f88d47b84ef6548aeb6ec" },
    { BACKEND_VULKAN_MD5_SALT_MESSAGE, 3u, binary_salt, sizeof (binary_salt), "22ab2473804ea5a3370887505aca45bb" }
  };

  if (backend.vulkan_catalog == NULL || backend.vulkan_devices_cnt == 0u)
  {
    goto cleanup;
  }

  for (uint32_t i = 0u; i < sizeof (cases) / sizeof (cases[0]); i++)
  {
    uint8_t digest[16] = { 0 };

    result = backend_vulkan_md5_format_batch (
      &backend, 0u, cases[i].format, &messages[cases[i].message_index],
      1u, cases[i].salt, cases[i].salt_size, digest, stdout);

    if (result != VK_SUCCESS || !verify_digest (digest, cases[i].expected))
    {
      fprintf (stderr, "VULKAN_MD5_FORMAT_FAILED case=%u result=%d\n", i, result);

      goto cleanup;
    }
  }

  status = 0;

cleanup:

  result = backend_vulkan_catalog_close (&backend);

  if (result != VK_SUCCESS) status = 1;

  return status;
}

/**
 * Execute the Vulkan 1.0 or 1.1 known-answer suite.
 */
int main (int argc, char **argv)
{
  if (argc != 3 || strcmp (argv[1], "--minimum") != 0 ||
      (strcmp (argv[2], "1.0") != 0 && strcmp (argv[2], "1.1") != 0))
  {
    fprintf (stderr, "Usage: %s --minimum 1.0|1.1\n", argv[0]);

    return 2;
  }

  const uint32_t minimum_api =
    strcmp (argv[2], "1.0") == 0 ? VK_API_VERSION_1_0 : VK_API_VERSION_1_1;

  if (verify_formats (minimum_api) != 0) return 1;

  fprintf (stdout, "VULKAN_MD5_FORMAT_PASS api=%s vectors=6\n", argv[2]);

  return 0;
}
