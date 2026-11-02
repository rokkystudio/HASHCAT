/**
 * Observe a real Vulkan fence timeout in the production four-stage executor.
 *
 * The production backend submits the complete public four-stage command buffer
 * without associating its separately owned unsignaled fence. A zero-duration
 * vkWaitForFences call therefore returns the actual driver VK_TIMEOUT status.
 * The ordinary production cleanup then calls vkDeviceWaitIdle before releasing
 * the submitted command and every dynamically owned resource. A fresh VkDevice
 * under the same VkInstance must subsequently execute the normal four-stage
 * program and verify all 256 public uint32 outputs.
 *
 * No Vulkan function is wrapped or replaced. No HASHCAT password candidate,
 * digest, salt or hashing scheduler participates in this diagnostic.
 */

#include "backend_vulkan.h"
#include "backend_vulkan_kernel.h"

#include <stdio.h>
#include <string.h>

/**
 * Confirm that the Vulkan information-mode diagnostic did not register a
 * HASHCAT hashing device or mutate ordinary scheduler-owned backend fields.
 */
static int backend_fence_timeout_scheduler_is_clean (
  const backend_ctx_t *backend)
{
  return backend != NULL
      && backend->vulkan_session == NULL
      && backend->backend_devices_cnt == 0
      && backend->devices_param == NULL
      && backend->enabled == false;
}

/**
 * Run one real zero-timeout fence wait and one healthy same-instance recovery.
 *
 * Opens one production Vulkan catalogue, rejects invalid public diagnostic
 * inputs, then executes backend_vulkan_kernel_verify_chain4_fence_timeout().
 * VK_TIMEOUT is the expected successful observation. Production cleanup must
 * finish before a fresh backend_vulkan_kernel_verify_chain4() call creates a
 * new VkDevice, dispatches four public stages and verifies 256 output words.
 */
static int backend_fence_timeout_trial (uint32_t minimum_api)
{
  backend_ctx_t backend = { 0 };

  if (backend_vulkan_kernel_verify_chain4_fence_timeout (
    &backend, 0u, stdout
  ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (stderr, "BACKEND_PUBLIC_FENCE_UNOPENED_ACCEPTED\n");

    return 1;
  }

  VkResult result = backend_vulkan_catalog_open (
    &backend, minimum_api, stdout
  );

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr,
      "BACKEND_PUBLIC_FENCE_CATALOG_FAILED result=%d\n",
      result
    );

    return 1;
  }

  int exit_status = 1;

  if (backend.vulkan_catalog == NULL
   || backend.vulkan_devices_cnt == 0u
   || backend_fence_timeout_scheduler_is_clean (&backend) == 0
   || backend_vulkan_kernel_verify_chain4_fence_timeout (
      &backend, backend.vulkan_devices_cnt, stdout
    ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_kernel_verify_chain4_fence_timeout (
      &backend, 0u, NULL
    ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (
      stderr,
      "BACKEND_PUBLIC_FENCE_INVALID_INPUTS_ACCEPTED\n"
    );

    goto cleanup;
  }

  result = backend_vulkan_kernel_verify_chain4_fence_timeout (
    &backend, 0u, stdout
  );

  if (result != VK_TIMEOUT
   || backend_fence_timeout_scheduler_is_clean (&backend) == 0)
  {
    fprintf (
      stderr,
      "BACKEND_PUBLIC_FENCE_TIMEOUT_FAILED"
      " result=%d scheduled=%d\n",
      result,
      backend_fence_timeout_scheduler_is_clean (&backend) == 0
    );

    goto cleanup;
  }

  result = backend_vulkan_kernel_verify_chain4 (
    &backend, 0u, stdout
  );

  if (result != VK_SUCCESS
   || backend_fence_timeout_scheduler_is_clean (&backend) == 0)
  {
    fprintf (
      stderr,
      "BACKEND_PUBLIC_FENCE_RECOVERY_FAILED"
      " result=%d scheduled=%d\n",
      result,
      backend_fence_timeout_scheduler_is_clean (&backend) == 0
    );

    goto cleanup;
  }

  fprintf (
    stdout,
    "BACKEND_PUBLIC_FENCE_RECOVERY_PASS"
    " timeout=1"
    " new_device=1"
    " dispatches=4"
    " verified_words=256"
    " driver_wait_timeout=1"
    " password_inputs=0 scheduled=0\n"
  );

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
      "BACKEND_PUBLIC_FENCE_CATALOG_CLOSE_FAILED result=%d\n",
      result
    );

    exit_status = 1;
  }

  return exit_status;
}

/**
 * Execute one production fence-timeout/recovery trial for Vulkan 1.0 or 1.1.
 *
 * Requires exactly "--minimum 1.0" or "--minimum 1.1". Returns 0 only when a
 * real vkWaitForFences call returns VK_TIMEOUT, production cleanup succeeds,
 * and a fresh four-stage VkDevice verifies all public outputs. Returns 2 for
 * malformed CLI and 1 for any unexpected Vulkan, cleanup or scheduler result.
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

  if (backend_fence_timeout_trial (minimum_api) != 0)
  {
    return 1;
  }

  fprintf (
    stdout,
    "BACKEND_PUBLIC_FENCE_DIAGNOSTIC_PASS"
    " api=%s"
    " real_queue_submissions=2"
    " real_fence_timeouts=1"
    " cleanup_wait_idle=1"
    " recovered_devices=1"
    " recovered_dispatches=4"
    " verified_words=256"
    " synthetic_driver_results=0"
    " password_inputs=0 scheduled=0\n",
    argv[2]
  );

  return 0;
}
