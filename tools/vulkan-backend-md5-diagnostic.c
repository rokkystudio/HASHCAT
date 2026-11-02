/**
 * Verify the first cryptographic Vulkan Compute stage on Android hardware.
 *
 * Opens the production Vulkan catalogue, executes backend_vulkan_kernel_verify_md5()
 * against four fixed public one-block MD5 known-answer messages and closes the
 * catalogue. The production executor owns all VkDevice resources dynamically.
 * No HASHCAT password candidate, target digest, salt, attack mode or hashing
 * scheduler entry participates in the diagnostic.
 */

#include "backend_vulkan.h"
#include "backend_vulkan_kernel.h"

#include <stdio.h>
#include <string.h>

/**
 * Confirm that information-mode Vulkan work has not registered a hashing
 * device or populated ordinary scheduler-owned backend fields.
 */
static int backend_md5_scheduler_is_clean (
  const backend_ctx_t *backend)
{
  return backend != NULL
      && backend->vulkan_session == NULL
      && backend->backend_devices_cnt == 0
      && backend->devices_param == NULL
      && backend->enabled == false;
}

/**
 * Execute one real MD5 SPIR-V known-answer trial under a live VkInstance.
 *
 * Rejects unopened, out-of-range and NULL-diagnostics calls, then runs the
 * production MD5 executor on physical device zero. VK_SUCCESS requires four
 * exact 128-bit known answers, preserved destination sentinels and complete
 * cleanup. Scheduler-owned HASHCAT fields must remain unchanged.
 */
static int backend_md5_trial (uint32_t minimum_api)
{
  backend_ctx_t backend = { 0 };

  if (backend_vulkan_kernel_verify_md5 (
    &backend, 0u, stdout
  ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (stderr, "BACKEND_VULKAN_MD5_UNOPENED_ACCEPTED\n");

    return 1;
  }

  VkResult result = backend_vulkan_catalog_open (
    &backend, minimum_api, stdout
  );

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr,
      "BACKEND_VULKAN_MD5_CATALOG_FAILED result=%d\n",
      result
    );

    return 1;
  }

  int exit_status = 1;

  if (backend.vulkan_catalog == NULL
   || backend.vulkan_devices_cnt == 0u
   || backend_md5_scheduler_is_clean (&backend) == 0
   || backend_vulkan_kernel_verify_md5 (
      &backend, backend.vulkan_devices_cnt, stdout
    ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_kernel_verify_md5 (
      &backend, 0u, NULL
    ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (
      stderr,
      "BACKEND_VULKAN_MD5_INVALID_INPUTS_ACCEPTED\n"
    );

    goto cleanup;
  }

  result = backend_vulkan_kernel_verify_md5 (
    &backend, 0u, stdout
  );

  if (result != VK_SUCCESS
   || backend_md5_scheduler_is_clean (&backend) == 0)
  {
    fprintf (
      stderr,
      "BACKEND_VULKAN_MD5_VERIFY_FAILED"
      " result=%d scheduled=%d\n",
      result,
      backend_md5_scheduler_is_clean (&backend) == 0
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
      "BACKEND_VULKAN_MD5_CATALOG_CLOSE_FAILED result=%d\n",
      result
    );

    exit_status = 1;
  }

  return exit_status;
}

/**
 * Run the production MD5 SPIR-V known-answer trial for Vulkan 1.0 or 1.1.
 *
 * Requires exactly "--minimum 1.0" or "--minimum 1.1". Returns 0 only after
 * the real GPU computes all four public MD5 vectors exactly and all temporary
 * resources are released. Returns 2 for malformed CLI and 1 for any Vulkan,
 * digest, sentinel, cleanup or scheduler contract failure.
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

  if (backend_md5_trial (minimum_api) != 0)
  {
    return 1;
  }

  fprintf (
    stdout,
    "BACKEND_VULKAN_MD5_DIAGNOSTIC_PASS"
    " api=%s"
    " vectors=4"
    " blocks=4"
    " rounds=64"
    " dispatches=1"
    " verified_digest_words=16"
    " sentinel_words=48"
    " password_inputs=0"
    " target_digests=0"
    " scheduled=0\n",
    argv[2]
  );

  return 0;
}
