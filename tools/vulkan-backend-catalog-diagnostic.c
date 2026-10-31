/**
 * Exercise hashcat backend_ctx ownership of real Vulkan physical devices.
 *
 * Runs a public noncryptographic Compute capability check through the same
 * backend_vulkan_catalog_* methods used by the opt-in hashcat backend.
 * No password, hash module, GPU dispatch or scheduler call is performed.
 */

#include "backend_vulkan.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Open a backend-owned Vulkan catalogue, reject malformed requests,
 * reject a workload exceeding real device limits, select one compatible
 * Compute device and close the catalogue.
 *
 * Each trial reuses the same backend_ctx_t after the previous close.
 * Returns failure when a Vulkan operation, device requirement or owner
 * lifecycle check does not meet its declared contract.
 */
static int test_catalog_trial (
  backend_ctx_t *backend_ctx,
  uint32_t minimum_api,
  uint32_t trial)
{
  const vulkan_auto_compute_requirements_t workload =
  {
    .local_size = { 32u, 1u, 1u },
    .group_count = { 64u, 1u, 1u },
    .storage_buffer_count = 4u,
    .largest_storage_buffer_bytes = 8192u,
    .shared_memory_bytes = 0u
  };

  vulkan_auto_device_t selected = { 0 };
  uint32_t effective_api = 0u;

  if (backend_vulkan_catalog_select (
    backend_ctx, 0u, &workload, &selected, &effective_api
  ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (stderr, "BACKEND_VULKAN_REJECT_UNOPENED_FAILED\n");

    return 1;
  }

  VkResult result = backend_vulkan_catalog_open (
    backend_ctx, minimum_api, stdout
  );

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr, "BACKEND_VULKAN_OPEN_FAILED result=%d trial=%" PRIu32 "\n",
      result, trial
    );

    return 1;
  }

  int status = 1;

  if (backend_ctx->vulkan_catalog == NULL
   || backend_ctx->vulkan_devices_cnt == 0u
   || backend_ctx->rc_vulkan_init != VK_SUCCESS
   || backend_ctx->enabled != false
   || backend_ctx->backend_devices_cnt != 0
   || backend_ctx->devices_param != NULL)
  {
    fprintf (stderr, "BACKEND_VULKAN_OWNER_STATE_FAILED\n");

    goto cleanup;
  }

  const VkResult reject_reopen = backend_vulkan_catalog_open (
    backend_ctx, minimum_api, stdout
  );
  const VkResult reject_index = backend_vulkan_catalog_select (
    backend_ctx, backend_ctx->vulkan_devices_cnt,
    &workload, &selected, &effective_api
  );
  const VkResult reject_workload = backend_vulkan_catalog_select (
    backend_ctx, 0u, NULL, &selected, &effective_api
  );
  const VkResult reject_selected = backend_vulkan_catalog_select (
    backend_ctx, 0u, &workload, NULL, &effective_api
  );
  const VkResult reject_effective = backend_vulkan_catalog_select (
    backend_ctx, 0u, &workload, &selected, NULL
  );

  if (reject_reopen != VK_ERROR_INITIALIZATION_FAILED
   || reject_index != VK_ERROR_INITIALIZATION_FAILED
   || reject_workload != VK_ERROR_INITIALIZATION_FAILED
   || reject_selected != VK_ERROR_INITIALIZATION_FAILED
   || reject_effective != VK_ERROR_INITIALIZATION_FAILED
   || backend_ctx->vulkan_catalog->selections != 0u)
  {
    fprintf (stderr, "BACKEND_VULKAN_INVALID_CONTROLS_FAILED\n");

    goto cleanup;
  }

  fprintf (
    stdout,
    "BACKEND_VULKAN_INVALID_CONTROLS rejected=6 selections=0\n"
  );

  vulkan_auto_compute_requirements_t unsupported = workload;

  unsupported.local_size[0] = UINT32_MAX;

  result = backend_vulkan_catalog_select (
    backend_ctx, 0u, &unsupported, &selected, &effective_api
  );

  if (result != VK_ERROR_FEATURE_NOT_PRESENT
   || backend_ctx->vulkan_catalog->selections != 0u)
  {
    fprintf (
      stderr,
      "BACKEND_VULKAN_LIMIT_CHECK_FAILED result=%d\n", result
    );

    goto cleanup;
  }

  fprintf (
    stdout,
    "BACKEND_VULKAN_LIMIT_REJECT result=%d selections=0\n", result
  );

  result = backend_vulkan_catalog_select (
    backend_ctx, 0u, &workload, &selected, &effective_api
  );

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr, "BACKEND_VULKAN_COMPUTE_DEVICE_FAILED result=%d\n", result
    );

    goto cleanup;
  }

  if (selected.index != 0u
   || selected.compute_queue_family == UINT32_MAX
   || effective_api != minimum_api
   || backend_ctx->vulkan_catalog->selections != 1u)
  {
    fprintf (stderr, "BACKEND_VULKAN_SELECT_CONTRACT_FAILED\n");

    goto cleanup;
  }

  fprintf (
    stdout,
    "BACKEND_VULKAN_SELECT_PASS trial=%" PRIu32
    " devices=%" PRIu32 " requested_api=%" PRIu32
    ".%" PRIu32 " effective_api=%" PRIu32 ".%" PRIu32
    " scheduled=0\n",
    trial,
    backend_ctx->vulkan_devices_cnt,
    VK_API_VERSION_MAJOR (minimum_api),
    VK_API_VERSION_MINOR (minimum_api),
    VK_API_VERSION_MAJOR (effective_api),
    VK_API_VERSION_MINOR (effective_api)
  );

  status = 0;

cleanup:
  result = backend_vulkan_catalog_close (backend_ctx);

  if (result != VK_SUCCESS
   || backend_ctx->vulkan_catalog != NULL
   || backend_ctx->vulkan_devices_cnt != 0u
   || backend_ctx->rc_vulkan_init != 0)
  {
    fprintf (
      stderr, "BACKEND_VULKAN_CLOSE_FAILED result=%d\n", result
    );

    status = 1;
  }

  if (status == 0)
  {
    fprintf (
      stdout, "BACKEND_VULKAN_CLOSE_PASS trial=%" PRIu32 "\n", trial
    );
  }

  return status;
}

/**
 * Validate public Vulkan discovery through hashcat's actual backend_ctx_t.
 *
 * Accepts one explicit Vulkan core API requirement and independently
 * completes two instance/device catalogue lifecycles on a physical GPU.
 * Exit 0 means all native driver and owner checks completed, 1 means
 * a Vulkan contract failed and 2 denotes an invalid command line.
 */
int main (int argc, char **argv)
{
  if (argc != 3 || strcmp (argv[1], "--minimum") != 0
   || (strcmp (argv[2], "1.0") != 0
    && strcmp (argv[2], "1.1") != 0))
  {
    fprintf (
      stderr, "Usage: %s --minimum 1.0|1.1\n", argv[0]
    );

    return 2;
  }

  const uint32_t minimum_api =
    strcmp (argv[2], "1.0") == 0
      ? VK_API_VERSION_1_0 : VK_API_VERSION_1_1;

  backend_ctx_t *backend_ctx = (backend_ctx_t *) calloc (
    1u, sizeof (*backend_ctx)
  );

  if (backend_ctx == NULL) return 1;

  int status = 0;

  for (uint32_t trial = 0u; trial < 2u; trial++)
  {
    if (test_catalog_trial (
      backend_ctx, minimum_api, trial
    ) != 0)
    {
      status = 1;

      break;
    }
  }

  if (backend_vulkan_catalog_close (backend_ctx) != VK_SUCCESS)
  {
    status = 1;
  }

  free (backend_ctx);

  if (status == 0)
  {
    fprintf (
      stdout,
      "BACKEND_VULKAN_DIAGNOSTIC_PASS api=%" PRIu32 ".%" PRIu32
      " trials=2 selections=2 invalid_controls=12 scheduled=0\n",
      VK_API_VERSION_MAJOR (minimum_api),
      VK_API_VERSION_MINOR (minimum_api)
    );
  }

  return status;
}
