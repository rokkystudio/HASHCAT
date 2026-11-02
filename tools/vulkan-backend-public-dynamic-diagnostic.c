/**
 * Run a four-stage public Vulkan Compute program with dynamic ownership.
 *
 * Uses the exact backend_vulkan_kernel_verify_chain4 implementation linked
 * into Android HASHCAT information mode. Stage zero executes the original
 * public transform shader, while stages one through three reuse the public
 * rotate shader over five distinct storage buffers. Host ownership arrays
 * are allocated from the declared stage/buffer counts.
 *
 * No hash input, password candidate or HASHCAT hashing work is used.
 */

#include "backend_vulkan.h"
#include "backend_vulkan_kernel.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/**
 * Execute four public stages in one complete Vulkan catalogue lifetime.
 *
 * Invalid catalogue, device index and diagnostic controls are rejected
 * before VkDevice creation. A real physical GPU executes four ordered
 * dispatches, three explicit Compute dependencies and 256 CPU-verified
 * output words. All dynamically allocated host/Vulkan resources are
 * released before the parent VkInstance closes.
 */
static int backend_dynamic_trial (uint32_t minimum_api, uint32_t trial)
{
  backend_ctx_t backend = { 0 };

  if (backend_vulkan_kernel_verify_chain4 (
    &backend, 0u, stdout
  ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (stderr, "BACKEND_PUBLIC_DYNAMIC_UNOPENED_ACCEPTED\n");

    return 1;
  }

  VkResult result = backend_vulkan_catalog_open (
    &backend, minimum_api, stdout
  );

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr, "BACKEND_PUBLIC_DYNAMIC_CATALOG_FAILED result=%d\n",
      result
    );

    return 1;
  }

  int exit_status = 1;

  if (backend.vulkan_catalog == NULL
   || backend.vulkan_devices_cnt == 0u
   || backend_vulkan_kernel_verify_chain4 (
      &backend, backend.vulkan_devices_cnt, stdout
    ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_kernel_verify_chain4 (
      &backend, 0u, NULL
    ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (
      stderr,
      "BACKEND_PUBLIC_DYNAMIC_INVALID_INPUTS trial=%" PRIu32 "\n",
      trial
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
      "BACKEND_PUBLIC_DYNAMIC_GPU_FAILED trial=%" PRIu32
      " result=%d\n",
      trial, result
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
      "BACKEND_PUBLIC_DYNAMIC_SCHEDULER_MUTATED trial=%" PRIu32 "\n",
      trial
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
      "BACKEND_PUBLIC_DYNAMIC_INSTANCE_CLOSE_FAILED"
      " trial=%" PRIu32 " result=%d\n",
      trial, result
    );

    exit_status = 1;
  }

  if (exit_status == 0)
  {
    fprintf (
      stdout,
      "BACKEND_PUBLIC_DYNAMIC_TRIAL_PASS"
      " trial=%" PRIu32
      " buffers=5 descriptor_sets=4 pipelines=4"
      " dispatches=4 verified_words=256"
      " dynamic_resources=1 scheduled=0\n",
      trial
    );
  }

  return exit_status;
}

/**
 * Verify two independent four-stage dynamic Vulkan program executions.
 *
 * Requires exactly --minimum 1.0 or --minimum 1.1. Each trial opens its
 * own VkInstance and temporary Compute device, allocates five buffer records
 * and four stage-resource records from the program declaration, executes
 * four ordered public shaders and verifies 256 uint32 outputs. Native
 * success returns zero; invalid CLI returns 2 and any GPU, CPU-reference,
 * ownership or cleanup failure returns 1.
 */
int main (int argc, char **argv)
{
  if (argc != 3
   || strcmp (argv[1], "--minimum") != 0
   || (strcmp (argv[2], "1.0") != 0
    && strcmp (argv[2], "1.1") != 0))
  {
    fprintf (
      stderr, "Usage: %s --minimum 1.0|1.1\n",
      argv[0]
    );

    return 2;
  }

  const uint32_t api =
    strcmp (argv[2], "1.0") == 0
      ? VK_API_VERSION_1_0 : VK_API_VERSION_1_1;

  for (uint32_t trial = 0u; trial < 2u; trial++)
  {
    if (backend_dynamic_trial (api, trial) != 0)
    {
      return 1;
    }
  }

  fprintf (
    stdout,
    "BACKEND_PUBLIC_DYNAMIC_DIAGNOSTIC_PASS"
    " api=%s trials=2 dispatches=8"
    " buffers=5 descriptor_sets=4 pipelines=4"
    " verified_words=512 dynamic_resources=1"
    " password_inputs=0 scheduled=0\n",
    argv[2]
  );

  return 0;
}
