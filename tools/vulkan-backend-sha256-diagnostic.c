/**
 * Verify the production variable-length SHA-256 Vulkan batch on Android.
 *
 * Opens the normal Vulkan catalogue, executes four public known-answer
 * messages through backend_vulkan_sha256_verify_known_answers() and closes
 * the catalogue. The batch uses the common dynamic Vulkan executor, including
 * heterogeneous storage ranges, descriptors, pipeline, command buffer, fence
 * and cleanup. HASHCAT candidates, targets and scheduler state are absent.
 */

#include "backend_vulkan.h"
#include "backend_vulkan_sha256.h"

#include <stdio.h>
#include <string.h>

/**
 * Confirm that information-mode Vulkan work did not register a hashing device.
 */
static int backend_sha256_scheduler_is_clean (
  const backend_ctx_t *backend)
{
  return backend != NULL
      && backend->vulkan_session == NULL
      && backend->backend_devices_cnt == 0
      && backend->devices_param == NULL
      && backend->enabled == false;
}

/**
 * Execute one SHA-256 known-answer batch under a live production VkInstance.
 *
 * The batch covers empty and "abc" one-block messages plus 56-byte and
 * 100-byte two-block messages. VK_SUCCESS requires all 128 canonical digest
 * bytes to match and the common executor to release its temporary VkDevice.
 */
static int backend_sha256_trial (uint32_t minimum_api)
{
  backend_ctx_t backend = { 0 };

  VkResult result = backend_vulkan_catalog_open (
    &backend, minimum_api, stdout
  );

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr,
      "BACKEND_VULKAN_SHA256_CATALOG_FAILED result=%d\n",
      result
    );

    return 1;
  }

  int exit_status = 1;

  if (backend.vulkan_catalog == NULL
   || backend.vulkan_devices_cnt == 0u
   || backend_sha256_scheduler_is_clean (&backend) == 0)
  {
    fprintf (
      stderr,
      "BACKEND_VULKAN_SHA256_CATALOG_INVALID\n"
    );

    goto cleanup;
  }

  result = backend_vulkan_sha256_verify_known_answers (
    &backend, 0u, stdout
  );

  if (result != VK_SUCCESS
   || backend_sha256_scheduler_is_clean (&backend) == 0)
  {
    fprintf (
      stderr,
      "BACKEND_VULKAN_SHA256_VERIFY_FAILED"
      " result=%d scheduled=%d\n",
      result,
      backend_sha256_scheduler_is_clean (&backend) == 0
    );

    goto cleanup;
  }

  exit_status = 0;

cleanup:
  result = backend_vulkan_catalog_close (&backend);

  if (result != VK_SUCCESS
   || backend.vulkan_catalog != NULL
   || backend.vulkan_devices_cnt != 0u
   || backend.vulkan_session != NULL)
  {
    fprintf (
      stderr,
      "BACKEND_VULKAN_SHA256_CATALOG_CLOSE_FAILED result=%d\n",
      result
    );

    exit_status = 1;
  }

  return exit_status;
}

/**
 * Run one production SHA-256 batch trial for Vulkan 1.0 or 1.1.
 *
 * Returns 0 only when the GPU computes all four variable-length messages
 * exactly and the common dynamic executor leaves scheduler state untouched.
 */
int main (int argc, char **argv)
{
  if (argc != 3
   || strcmp (argv[1], "--minimum") != 0
   || (strcmp (argv[2], "1.0") != 0
    && strcmp (argv[2], "1.1") != 0))
  {
    fprintf (
      stderr,
      "Usage: %s --minimum 1.0|1.1\n",
      argv[0]
    );

    return 2;
  }

  const uint32_t minimum_api =
    strcmp (argv[2], "1.0") == 0
      ? VK_API_VERSION_1_0 : VK_API_VERSION_1_1;

  if (backend_sha256_trial (minimum_api) != 0)
  {
    return 1;
  }

  fprintf (
    stdout,
    "BACKEND_VULKAN_SHA256_DIAGNOSTIC_PASS"
    " api=%s"
    " messages=4"
    " one_block=2"
    " two_block=2"
    " verified_digest_bytes=128"
    " dispatches=1"
    " password_generation=0"
    " target_digests=0"
    " scheduled=0\n",
    argv[2]
  );

  return 0;
}
