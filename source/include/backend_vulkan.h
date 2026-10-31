/**
 * Author......: See docs/credits.txt
 * License.....: MIT
 */

#ifndef HC_BACKEND_VULKAN_H
#define HC_BACKEND_VULKAN_H

/**
 * Manage a Vulkan 1.0 device catalogue within hashcat's backend context.
 *
 * An opened catalogue belongs to backend_ctx_t and owns VkInstance and
 * its physical-device array. This component provides Vulkan discovery and
 * explicit GPU capability checks only. It does not insert devices into
 * backend_ctx->devices_param or dispatch password-processing kernels.
 *
 * The caller must close the catalogue after all logical Vulkan devices
 * created from the enumerated physical devices have been destroyed.
 */

#include "types.h"
#include "vulkan-core10-device-catalog.h"

#include <stdint.h>
#include <stdio.h>

/**
 * Open one Vulkan catalogue and record its enumerated device count.
 *
 * Requires a backend context without an active Vulkan catalogue and one
 * exact minimum core API: 1.0 or 1.1. On success backend_ctx owns the
 * allocated catalogue, publishes the physical-device count, and records
 * VK_SUCCESS in rc_vulkan_init.
 *
 * On failure the allocation is released, vulkan_devices_cnt stays zero
 * and rc_vulkan_init contains the exact VkResult. Other GPU backends and
 * backend_ctx->devices_param are not modified.
 */
VkResult backend_vulkan_catalog_open (
  backend_ctx_t *backend_ctx,
  uint32_t minimum_api,
  FILE *diagnostics);

/**
 * Validate one explicitly indexed Vulkan device for a public Compute task.
 *
 * Requires an opened backend-owned catalogue and exact caller workload
 * metadata. Returns a driver-validated physical device and queue-family
 * selection without opening VkDevice or adding a hashcat kernel device.
 * Vulkan failures are propagated directly to the caller.
 */
VkResult backend_vulkan_catalog_select (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  const vulkan_auto_compute_requirements_t *workload,
  vulkan_auto_device_t *selected,
  uint32_t *effective_api);

/**
 * Destroy the backend-owned Vulkan physical-device catalogue.
 *
 * Frees the catalogue after closing its VkInstance and owned handle
 * array, then clears its pointer, device count and result status.
 * Closing an empty Vulkan catalogue succeeds. The ordinary hashcat
 * CUDA, HIP, Metal and OpenCL contexts remain untouched.
 */
VkResult backend_vulkan_catalog_close (
  backend_ctx_t *backend_ctx);

#endif // HC_BACKEND_VULKAN_H
