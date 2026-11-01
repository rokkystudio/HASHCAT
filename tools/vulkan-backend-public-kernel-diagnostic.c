/**
 * Run the production HASHCAT Vulkan public Compute kernel on real Android.
 *
 * Uses the same backend_vulkan_kernel_verify function linked into the
 * Android HASHCAT information-mode executable. Opens a real Vulkan
 * instance/physical-device catalogue, verifies a public 64-word
 * vkCmdDispatch on a temporary logical device, and closes all Vulkan
 * resources. No hashing kernel, password or scheduler is involved.
 */

#include "backend_vulkan.h"
#include "backend_vulkan_kernel.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/**
 * Execute two independent kernel-owner lifetimes and invalid controls.
 *
 * Requires an explicit Vulkan 1.0/1.1 minimum. Invalid backend, index
 * and diagnostic inputs must be rejected before any Vulkan resources
 * are created. Each trial opens one catalogue, dispatches the real
 * production-owned public shader, verifies its output and closes the
 * GPU device before the Vulkan instance.
 */
static int backend_kernel_trial (uint32_t minimum_api, uint32_t trial)
{
  backend_ctx_t backend = { 0 };

  if (backend_vulkan_kernel_verify (
    &backend, 0u, stdout
  ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (stderr, "BACKEND_PUBLIC_KERNEL_UNOPENED_ACCEPTED\n");

    return 1;
  }

  VkResult result = backend_vulkan_catalog_open (
    &backend, minimum_api, stdout
  );

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr,
      "BACKEND_PUBLIC_KERNEL_CATALOG_FAILED result=%d\n", result
    );

    return 1;
  }

  int exit_status = 1;

  if (backend.vulkan_catalog == NULL || backend.vulkan_devices_cnt == 0u
   || backend_vulkan_kernel_verify (
      &backend, backend.vulkan_devices_cnt, stdout
    ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_kernel_verify (
      &backend, 0u, NULL
    ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (
      stderr,
      "BACKEND_PUBLIC_KERNEL_INVALID_INPUTS trial=%" PRIu32 "\n",
      trial
    );

    goto cleanup;
  }

  result = backend_vulkan_kernel_verify (
    &backend, 0u, stdout
  );

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr,
      "BACKEND_PUBLIC_KERNEL_GPU_FAILED trial=%" PRIu32
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
      "BACKEND_PUBLIC_KERNEL_SCHEDULER_MUTATED trial=%" PRIu32 "\n",
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
      "BACKEND_PUBLIC_KERNEL_INSTANCE_CLOSE_FAILED"
      " trial=%" PRIu32 " result=%d\n",
      trial, result
    );

    exit_status = 1;
  }

  if (exit_status == 0)
  {
    fprintf (
      stdout,
      "BACKEND_PUBLIC_KERNEL_TRIAL_PASS"
      " trial=%" PRIu32
      " descriptors=2 pipelines=1"
      " dispatches=1 verified_words=64"
      " scheduled=0\n",
      trial
    );
  }

  return exit_status;
}

/**
 * Run the production-owned public Compute kernel on a physical GPU.
 *
 * Requires a single Vulkan minimum API argument. Runs two complete
 * catalogue and kernel lifetimes; exits zero only when both real
 * shader dispatches compare against the CPU reference. Invalid CLI
 * exits 2; an unexpected Vulkan driver or result error exits 1.
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

  const uint32_t api =
    strcmp (argv[2], "1.0") == 0
      ? VK_API_VERSION_1_0 : VK_API_VERSION_1_1;

  for (uint32_t trial = 0u; trial < 2u; trial++)
  {
    if (backend_kernel_trial (api, trial) != 0)
    {
      return 1;
    }
  }

  fprintf (
    stdout,
    "BACKEND_PUBLIC_KERNEL_DIAGNOSTIC_PASS"
    " api=%s trials=2 dispatches=2"
    " descriptors=2 storage_buffers=2"
    " verified_words=128"
    " shaders=1 password_inputs=0 scheduled=0\n",
    argv[2]
  );

  return 0;
}
