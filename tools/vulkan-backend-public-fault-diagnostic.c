/**
 * Verify controlled cleanup and same-instance recovery of public Vulkan.
 *
 * Calls the exact production backend_vulkan_kernel_verify_chain4_fault
 * resource paths against an actual Android Vulkan GPU. At every explicit
 * checkpoint, one deterministic synthetic initialization error is returned
 * after real resources are acquired. The acquired objects are released,
 * and a new four-stage public Compute program executes on a fresh VkDevice
 * while the original backend-owned VkInstance remains live.
 *
 * Checkpoint 36 aborts before queue submission. Checkpoint 37 records a
 * real successful vkQueueSubmit and aborts before the fence wait; the
 * ordinary cleanup then waits for the device to become idle. No driver
 * return is forged and no actual device-loss condition is induced.
 * No HASHCAT candidates, hashes or attack scheduling are processed.
 */

#include "backend_vulkan.h"
#include "backend_vulkan_kernel.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define BACKEND_PUBLIC_FAULT_POINTS 37u

/**
 * Verify all explicit public-resource failure points on one real GPU.
 *
 * Opens one Vulkan catalogue/instance, rejects invalid injection
 * arguments, and runs checkpoints 1..37 in exact order. Each checkpoint
 * must return a synthetic VK_ERROR_INITIALIZATION_FAILED after releasing
 * its partial VkDevice resources. A new healthy four-stage GPU session
 * must then submit four public Compute dispatches and verify 256 words
 * on the same parent VkInstance. Hash scheduling is checked after every
 * recovery. The parent catalogue closes only after all checkpoints pass.
 */
static int backend_fault_recovery_trial (uint32_t minimum_api)
{
  backend_ctx_t backend = { 0 };

  if (backend_vulkan_kernel_verify_chain4_fault (
    &backend, 0u, stdout, 1u
  ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (
      stderr,
      "BACKEND_PUBLIC_FAULT_UNOPENED_ACCEPTED\n"
    );

    return 1;
  }

  VkResult result = backend_vulkan_catalog_open (
    &backend, minimum_api, stdout
  );

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr,
      "BACKEND_PUBLIC_FAULT_CATALOG_FAILED result=%d\n",
      result
    );

    return 1;
  }

  int exit_status = 1;

  if (backend.vulkan_catalog == NULL
   || backend.vulkan_devices_cnt == 0u
   || backend_vulkan_kernel_verify_chain4_fault (
      &backend, 0u, stdout, 0u
    ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_kernel_verify_chain4_fault (
      &backend, 0u, stdout, BACKEND_PUBLIC_FAULT_POINTS + 1u
    ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_kernel_verify_chain4_fault (
      &backend, backend.vulkan_devices_cnt, stdout, 1u
    ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_kernel_verify_chain4_fault (
      &backend, 0u, NULL, 1u
    ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (
      stderr,
      "BACKEND_PUBLIC_FAULT_INVALID_INPUTS_ACCEPTED\n"
    );

    goto cleanup;
  }

  fprintf (
    stdout,
    "BACKEND_PUBLIC_FAULT_CONTROLS_PASS"
    " invalid_points=2 invalid_context=3"
    " gpu_submissions=0 scheduled=0\n"
  );

  for (uint32_t point = 1u;
       point <= BACKEND_PUBLIC_FAULT_POINTS;
       point++)
  {
    result = backend_vulkan_kernel_verify_chain4_fault (
      &backend, 0u, stdout, point
    );

    if (result != VK_ERROR_INITIALIZATION_FAILED)
    {
      fprintf (
        stderr,
        "BACKEND_PUBLIC_FAULT_EXPECTED_FAILED"
        " point=%" PRIu32 " result=%d\n",
        point, result
      );

      goto cleanup;
    }

    result = backend_vulkan_kernel_verify_chain4 (
      &backend, 0u, stdout
    );

    if (result != VK_SUCCESS)
    {
      fprintf (
        stderr,
        "BACKEND_PUBLIC_FAULT_RECOVERY_FAILED"
        " point=%" PRIu32 " result=%d\n",
        point, result
      );

      goto cleanup;
    }

    if (backend.vulkan_session != NULL
     || backend.backend_devices_cnt != 0
     || backend.devices_param != NULL
     || backend.enabled != false)
    {
      fprintf (
        stderr,
        "BACKEND_PUBLIC_FAULT_SCHEDULER_MUTATED"
        " point=%" PRIu32 "\n",
        point
      );

      goto cleanup;
    }

    fprintf (
      stdout,
      "BACKEND_PUBLIC_FAULT_RECOVERY_PASS"
      " point=%" PRIu32
      " injected=1"
      " new_device=1"
      " dispatches=4"
      " verified_words=256"
      " scheduled=0\n",
      point
    );
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
      "BACKEND_PUBLIC_FAULT_CATALOG_CLOSE_FAILED result=%d\n",
      result
    );

    exit_status = 1;
  }

  return exit_status;
}

/**
 * Run all 37 controlled Vulkan resource checkpoints for an explicit API.
 *
 * Requires exactly --minimum 1.0 or --minimum 1.1. Successful native
 * execution creates a real Vulkan instance, verifies 37 controlled
 * cleanup paths, opens 37 new working VkDevices and verifies 9472 public
 * 32-bit outputs from 148 GPU dispatches. One additional queue submission
 * occurs at checkpoint 37 before ordinary device-idle cleanup.
 * Returns 2 for invalid CLI, 1 for any GPU, ownership, recovery or cleanup
 * failure, and 0 only for complete validation.
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

  if (backend_fault_recovery_trial (minimum_api) != 0)
  {
    return 1;
  }

  fprintf (
    stdout,
    "BACKEND_PUBLIC_FAULT_DIAGNOSTIC_PASS"
    " api=%s"
    " checkpoints=37"
    " injected=37"
    " recovered_devices=37"
    " recovered_dispatches=148"
    " verified_words=9472"
    " abandoned_before_submit=1"
    " abandoned_after_submit=1"
    " real_driver_losses=0"
    " password_inputs=0 scheduled=0\n",
    argv[2]
  );

  return 0;
}
