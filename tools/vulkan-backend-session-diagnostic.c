/**
 * Verify Vulkan 1.0 logical-device ownership in an actual backend_ctx_t.
 *
 * Runs only a fence-completed empty Vulkan command buffer on a real
 * Compute queue, and verifies rejection of invalid API and resource
 * lifecycle requests. No shader, candidate, hash or kernel is loaded.
 */

#include "backend_vulkan.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Verify two independently owned Vulkan Compute sessions and instances.
 *
 * Each session uses an explicit physical-device index and public
 * 1x1x1 workload declaration. Invalid host calls and a physical-device
 * limit refusal must leave the owner empty before a successful open.
 * An active session also prevents premature VkInstance destruction.
 */
static int verify_session_trial (
  backend_ctx_t *backend_ctx,
  uint32_t minimum_api,
  uint32_t trial)
{
  const vulkan_auto_compute_requirements_t workload =
  {
    .local_size = { 1u, 1u, 1u },
    .group_count = { 1u, 1u, 1u },
    .storage_buffer_count = 1u,
    .largest_storage_buffer_bytes = 4u,
    .shared_memory_bytes = 0u
  };

  if (backend_vulkan_session_open (
    backend_ctx, 0u, &workload, stdout
  ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (stderr, "BACKEND_SESSION_UNOPENED_ACCEPTED\n");

    return 1;
  }

  if (backend_vulkan_session_close (backend_ctx) != VK_SUCCESS)
  {
    fprintf (stderr, "BACKEND_SESSION_EMPTY_CLOSE_FAILED\n");

    return 1;
  }

  VkResult result = backend_vulkan_catalog_open (
    backend_ctx, minimum_api, stdout
  );

  if (result != VK_SUCCESS)
  {
    fprintf (stderr, "BACKEND_SESSION_CATALOG_OPEN_FAILED result=%d\n", result);

    return 1;
  }

  int status = 1;

  if (backend_ctx->vulkan_catalog == NULL
   || backend_ctx->vulkan_devices_cnt == 0u
   || backend_ctx->vulkan_session != NULL)
  {
    fprintf (stderr, "BACKEND_SESSION_CATALOG_STATE_INVALID\n");

    goto cleanup_catalog;
  }

  const VkResult reject_index = backend_vulkan_session_open (
    backend_ctx, backend_ctx->vulkan_devices_cnt, &workload, stdout
  );
  const VkResult reject_workload = backend_vulkan_session_open (
    backend_ctx, 0u, NULL, stdout
  );
  const VkResult reject_diagnostics = backend_vulkan_session_open (
    backend_ctx, 0u, &workload, NULL
  );

  if (reject_index != VK_ERROR_INITIALIZATION_FAILED
   || reject_workload != VK_ERROR_INITIALIZATION_FAILED
   || reject_diagnostics != VK_ERROR_INITIALIZATION_FAILED
   || backend_ctx->vulkan_session != NULL)
  {
    fprintf (stderr, "BACKEND_SESSION_INVALID_CONTROLS_FAILED\n");

    goto cleanup_catalog;
  }

  vulkan_auto_compute_requirements_t unsupported = workload;

  unsupported.local_size[0] = UINT32_MAX;

  result = backend_vulkan_session_open (
    backend_ctx, 0u, &unsupported, stdout
  );

  if (result != VK_ERROR_FEATURE_NOT_PRESENT
   || backend_ctx->vulkan_session != NULL)
  {
    fprintf (stderr, "BACKEND_SESSION_LIMIT_CONTROL_FAILED result=%d\n", result);

    goto cleanup_catalog;
  }

  fprintf (
    stdout,
    "BACKEND_SESSION_INVALID_CONTROLS trial=%" PRIu32
    " invalid=4 unsupported=1 active=0\n",
    trial
  );

  result = backend_vulkan_session_open (
    backend_ctx, 0u, &workload, stdout
  );

  if (result != VK_SUCCESS || backend_ctx->vulkan_session == NULL)
  {
    fprintf (stderr, "BACKEND_SESSION_OPEN_FAILED result=%d\n", result);

    goto cleanup_catalog;
  }

  const VkResult reject_reopen = backend_vulkan_session_open (
    backend_ctx, 0u, &workload, stdout
  );
  const VkResult reject_catalog_close = backend_vulkan_catalog_close (
    backend_ctx
  );

  if (reject_reopen != VK_ERROR_INITIALIZATION_FAILED
   || reject_catalog_close != VK_ERROR_INITIALIZATION_FAILED
   || backend_ctx->vulkan_session == NULL
   || backend_ctx->vulkan_catalog == NULL)
  {
    fprintf (stderr, "BACKEND_SESSION_OWNER_ORDER_FAILED\n");

    goto cleanup_session;
  }

  fprintf (
    stdout,
    "BACKEND_SESSION_OWNER_CONTROLS trial=%" PRIu32
    " rejects=2 active=1\n", trial
  );

  if (backend_ctx->backend_devices_cnt != 0
   || backend_ctx->devices_param != NULL
   || backend_ctx->enabled != false)
  {
    fprintf (stderr, "BACKEND_SESSION_SCHEDULER_STATE_FAILED\n");

    goto cleanup_session;
  }

  status = 0;

cleanup_session:
  result = backend_vulkan_session_close (backend_ctx);

  if (result != VK_SUCCESS || backend_ctx->vulkan_session != NULL)
  {
    fprintf (stderr, "BACKEND_SESSION_DEVICE_CLOSE_FAILED result=%d\n", result);

    status = 1;
  }

  if (backend_vulkan_session_close (backend_ctx) != VK_SUCCESS)
  {
    fprintf (stderr, "BACKEND_SESSION_SECOND_CLOSE_FAILED\n");

    status = 1;
  }

cleanup_catalog:
  if (backend_vulkan_catalog_close (backend_ctx) != VK_SUCCESS
   || backend_ctx->vulkan_catalog != NULL
   || backend_ctx->vulkan_devices_cnt != 0u)
  {
    fprintf (stderr, "BACKEND_SESSION_INSTANCE_CLOSE_FAILED\n");

    status = 1;
  }

  if (status == 0)
  {
    fprintf (
      stdout,
      "BACKEND_SESSION_TRIAL_PASS trial=%" PRIu32
      " invalid=4 owner_rejects=2 empty_submissions=1 scheduled=0\n",
      trial
    );
  }

  return status;
}

/**
 * Run two Vulkan 1.0 or 1.1 real-driver session ownership trials.
 *
 * Accepts only a requested minimum API. Exits 0 after two complete
 * device/instance lifecycles, 1 for a native driver or ownership failure,
 * and 2 for a malformed command line.
 */
int main (int argc, char **argv)
{
  if (argc != 3 || strcmp (argv[1], "--minimum") != 0
   || (strcmp (argv[2], "1.0") != 0
    && strcmp (argv[2], "1.1") != 0))
  {
    fprintf (stderr, "Usage: %s --minimum 1.0|1.1\n", argv[0]);

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
    if (verify_session_trial (
      backend_ctx, minimum_api, trial
    ) != 0)
    {
      status = 1;

      break;
    }
  }

  if (backend_vulkan_session_close (backend_ctx) != VK_SUCCESS)
  {
    status = 1;
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
      "BACKEND_SESSION_DIAGNOSTIC_PASS api=%" PRIu32 ".%" PRIu32
      " trials=2 invalid=8 owner_rejects=4"
      " unsupported=2 empty_submissions=2 scheduled=0\n",
      VK_API_VERSION_MAJOR (minimum_api),
      VK_API_VERSION_MINOR (minimum_api)
    );
  }

  return status;
}
