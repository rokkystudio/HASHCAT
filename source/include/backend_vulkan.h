/**
 * Author......: See docs/credits.txt
 * License.....: MIT
 */

#ifndef HC_BACKEND_VULKAN_H
#define HC_BACKEND_VULKAN_H

/**
 * Manage opt-in Vulkan 1.0 discovery and an isolated Compute device session.
 *
 * backend_ctx_t owns the physical-device catalogue and at most one
 * logical-device session. The catalogue owns VkInstance and its indexed
 * physical devices; the session owns VkDevice, queue, command pool, one
 * primary command buffer and fence. A session validates its queue using
 * a single empty submission without a shader or storage-buffer binding.
 *
 * Vulkan devices are never inserted into devices_param or scheduled for
 * password/hash computation. The session closes before the catalogue.
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
 * Requires no active backend-owned Vulkan logical-device session.
 * Frees the catalogue after closing its VkInstance and owned handle
 * array, then clears its pointer, device count and result status.
 * Closing an empty catalogue succeeds. An active session returns
 * VK_ERROR_INITIALIZATION_FAILED and retains all Vulkan ownership.
 * The ordinary CUDA, HIP, Metal and OpenCL contexts remain untouched.
 */
VkResult backend_vulkan_catalog_close (
  backend_ctx_t *backend_ctx);

/**
 * Open one exact-index Vulkan 1.0 Compute device session in backend_ctx_t.
 *
 * Requires an active backend-owned catalogue, an explicit compatible
 * physical-device index and exact public workload requirements. Checks
 * the requested device against driver limits, creates VkDevice with one
 * Compute queue and no enabled optional features or extensions, creates
 * a command pool, primary command buffer and fence, then submits one
 * empty command buffer and waits for that fence to signal.
 *
 * On success backend_ctx owns the complete session until close. On
 * failure every partially created Vulkan resource is released and
 * backend_ctx->vulkan_session remains NULL. No hashcat hashing device,
 * password data, shader, descriptor set or GPU kernel is involved.
 */
VkResult backend_vulkan_session_open (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  const vulkan_auto_compute_requirements_t *workload,
  FILE *diagnostics);

/**
 * Destroy the backend-owned Vulkan Compute session before the catalogue.
 *
 * Waits for the logical device to idle, releases the fence, command
 * buffer, pool and device, then clears the owned session pointer.
 * Closing an empty session succeeds. A NULL backend context returns
 * VK_ERROR_INITIALIZATION_FAILED. Any VkDeviceWaitIdle error is
 * returned after the remaining session resources are released.
 */
VkResult backend_vulkan_session_close (
  backend_ctx_t *backend_ctx);

#endif // HC_BACKEND_VULKAN_H
