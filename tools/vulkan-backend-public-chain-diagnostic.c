/**
 * Run the production HASHCAT two-stage Vulkan public Compute chain.
 *
 * Uses the exact backend_vulkan_kernel_verify_chain method also linked
 * into the full Android HASHCAT information-mode process. The first
 * public SPIR-V 1.0 shader produces 64 intermediate uint32 values,
 * and a second SPIR-V 1.0 shader consumes those values after an
 * explicit COMPUTE write-to-read buffer barrier. Both 64-word outputs
 * are checked against independent host arithmetic.
 *
 * No hash input, password candidate or HASHCAT hashing work is used.
 */

#include "backend_vulkan.h"
#include "backend_vulkan_kernel.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/**
 * Execute two public GPU pipelines in one complete catalogue lifetime.
 *
 * Invalid catalogue, out-of-range index and NULL diagnostic controls
 * are rejected before opening a VkDevice. A real physical Vulkan GPU
 * executes both Compute stages and verifies all 128 output words.
 * VkDevice pipeline/memory resources are released before the parent
 * VkInstance; HASHCAT's existing hashing-device list remains empty.
 */
static int backend_chain_trial (uint32_t minimum_api, uint32_t trial)
{
  backend_ctx_t backend = { 0 };

  if (backend_vulkan_kernel_verify_chain (
    &backend, 0u, stdout
  ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (stderr, "BACKEND_PUBLIC_CHAIN_UNOPENED_ACCEPTED\n");

    return 1;
  }

  VkResult result = backend_vulkan_catalog_open (
    &backend, minimum_api, stdout
  );

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr, "BACKEND_PUBLIC_CHAIN_CATALOG_FAILED result=%d\n",
      result
    );

    return 1;
  }

  int exit_status = 1;

  if (backend.vulkan_catalog == NULL
   || backend.vulkan_devices_cnt == 0u
   || backend_vulkan_kernel_verify_chain (
      &backend, backend.vulkan_devices_cnt, stdout
    ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_kernel_verify_chain (
      &backend, 0u, NULL
    ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (
      stderr,
      "BACKEND_PUBLIC_CHAIN_INVALID_INPUTS trial=%" PRIu32 "\n",
      trial
    );

    goto cleanup;
  }

  result = backend_vulkan_kernel_verify_chain (
    &backend, 0u, stdout
  );

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr,
      "BACKEND_PUBLIC_CHAIN_GPU_FAILED trial=%" PRIu32
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
      "BACKEND_PUBLIC_CHAIN_SCHEDULER_MUTATED trial=%" PRIu32 "\n",
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
      "BACKEND_PUBLIC_CHAIN_INSTANCE_CLOSE_FAILED"
      " trial=%" PRIu32 " result=%d\n",
      trial, result
    );

    exit_status = 1;
  }

  if (exit_status == 0)
  {
    fprintf (
      stdout,
      "BACKEND_PUBLIC_CHAIN_TRIAL_PASS"
      " trial=%" PRIu32
      " descriptors=4 pipelines=2 dispatches=2"
      " verified_words=128 scheduled=0\n",
      trial
    );
  }

  return exit_status;
}

/**
 * Verify two independent real Vulkan instances and four GPU dispatches.
 *
 * Requires exactly --minimum 1.0 or --minimum 1.1. Both trials open
 * their own Vulkan catalogue and temporary Compute device, execute
 * two ordered shaders and verify 128 public words each. A complete
 * native success returns zero; invalid CLI returns 2 and any GPU,
 * CPU-reference, resource or ownership failure returns 1.
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
    if (backend_chain_trial (api, trial) != 0)
    {
      return 1;
    }
  }

  fprintf (
    stdout,
    "BACKEND_PUBLIC_CHAIN_DIAGNOSTIC_PASS"
    " api=%s trials=2 dispatches=4"
    " descriptors=4 storage_buffers=3"
    " verified_words=256"
    " shaders=2 password_inputs=0 scheduled=0\n",
    argv[2]
  );

  return 0;
}
