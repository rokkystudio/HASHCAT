/**
 * Author......: See docs/credits.txt
 * License.....: MIT
 */

/**
 * Own Vulkan 1.0 instance discovery through hashcat's backend context.
 *
 * A backend-owned catalogue contains physical device handles and is
 * independent of the ordinary CUDA, HIP, Metal and OpenCL device lists.
 * Only explicit public Compute capabilities are checked; no hashcat
 * kernel is assigned to the discovered Vulkan physical devices.
 */

#include "backend_vulkan.h"

#include <inttypes.h>
#include <stdlib.h>

/**
 * Open and retain a Vulkan catalogue only after successful enumeration.
 *
 * Each failure returns its VkResult and keeps the backend-owned pointer
 * and physical-device count empty; it does not affect existing backends.
 */
VkResult backend_vulkan_catalog_open (
  backend_ctx_t *backend_ctx,
  uint32_t minimum_api,
  FILE *diagnostics)
{
  if (backend_ctx == NULL || diagnostics == NULL
   || backend_ctx->vulkan_catalog != NULL
   || backend_ctx->vulkan_devices_cnt != 0u
   || (minimum_api != VK_API_VERSION_1_0
    && minimum_api != VK_API_VERSION_1_1))
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  vulkan_core10_device_catalog_t *catalog =
    (vulkan_core10_device_catalog_t *) calloc (1u, sizeof (*catalog));

  if (catalog == NULL)
  {
    backend_ctx->rc_vulkan_init = VK_ERROR_OUT_OF_HOST_MEMORY;

    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }

  const VkResult result = vulkan_core10_device_catalog_open (
    catalog, minimum_api, "hashcat Vulkan Compute catalogue", diagnostics
  );

  backend_ctx->rc_vulkan_init = result;

  if (result != VK_SUCCESS)
  {
    free (catalog);

    return result;
  }

  backend_ctx->vulkan_catalog = catalog;
  backend_ctx->vulkan_devices_cnt = catalog->device_count;

  fprintf (
    diagnostics,
    "BACKEND_VULKAN_CATALOG_READY devices=%" PRIu32
    " requested_api=%" PRIu32 ".%" PRIu32
    " scheduled=0\n",
    backend_ctx->vulkan_devices_cnt,
    VK_API_VERSION_MAJOR (minimum_api),
    VK_API_VERSION_MINOR (minimum_api)
  );

  return VK_SUCCESS;
}

/**
 * Select a specific enumerated Vulkan device with the exact Compute
 * requirement, leaving the hashcat kernel-device list unchanged.
 */
VkResult backend_vulkan_catalog_select (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  const vulkan_auto_compute_requirements_t *workload,
  vulkan_auto_device_t *selected,
  uint32_t *effective_api)
{
  if (backend_ctx == NULL
   || backend_ctx->vulkan_catalog == NULL
   || device_index >= backend_ctx->vulkan_devices_cnt)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  return vulkan_core10_device_catalog_select (
    backend_ctx->vulkan_catalog, device_index,
    workload, selected, effective_api
  );
}

/**
 * Close the Vulkan-owned instance and release the catalogue allocation.
 *
 * This method does not access CUDA/HIP/Metal/OpenCL state or destroy
 * logical Vulkan devices, which must be released by their own owners.
 */
VkResult backend_vulkan_catalog_close (
  backend_ctx_t *backend_ctx)
{
  if (backend_ctx == NULL) return VK_ERROR_INITIALIZATION_FAILED;

  VkResult result = VK_SUCCESS;

  if (backend_ctx->vulkan_catalog != NULL)
  {
    result = vulkan_core10_device_catalog_close (
      backend_ctx->vulkan_catalog
    );

    free (backend_ctx->vulkan_catalog);
  }

  backend_ctx->vulkan_catalog = NULL;
  backend_ctx->vulkan_devices_cnt = 0u;
  backend_ctx->rc_vulkan_init = 0;

  return result;
}
