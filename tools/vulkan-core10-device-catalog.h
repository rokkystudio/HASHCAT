#ifndef VULKAN_CORE10_DEVICE_CATALOG_H
#define VULKAN_CORE10_DEVICE_CATALOG_H

/**
 * Own Vulkan 1.0 instance creation and exact Compute-device discovery.
 *
 * An opened catalogue owns VkInstance and an indexed physical-device list.
 * It does not create VkDevice, queues, command buffers or descriptor sets;
 * those are owned by the existing Vulkan multi-set runtime.
 *
 * Physical-device selection uses the caller's actual shader and dispatch
 * requirements, the queried driver properties and the requested API level.
 * No GPU name, vendor, Android model or unstated driver capability is used.
 */

#include "vulkan-auto-config.h"

#include <stdint.h>
#include <stdio.h>

/**
 * Hold one Vulkan instance and its enumerated physical-device handles.
 *
 * The handles are valid until vulkan_core10_device_catalog_close.
 * The caller zero-initializes the catalogue and closes it after every
 * VkDevice created from the catalogue's physical devices is destroyed.
 * opened is true only after the full physical-device list is obtained.
 */
typedef struct vulkan_core10_device_catalog
{
  VkInstance instance;
  VkPhysicalDevice *physical;
  uint32_t device_count;
  uint32_t instance_api;
  uint32_t minimum_api;
  uint32_t selections;
  VkBool32 opened;
  FILE *diagnostics;
} vulkan_core10_device_catalog_t;

/**
 * Create one Vulkan 1.0/1.1 instance and enumerate its physical devices.
 *
 * The requested minimum is exactly VK_API_VERSION_1_0 or
 * VK_API_VERSION_1_1. The loader is queried through the core-1.0
 * vkGetInstanceProcAddr entry point and the exact requested version is
 * selected by the shared API policy. No extensions, layers or optional
 * device features are enabled. A Vulkan 1.0 loader without the optional
 * vkEnumerateInstanceVersion entry point remains supported.
 *
 * Returns an actual Vulkan failure or VK_ERROR_FEATURE_NOT_PRESENT for an
 * empty device list, without retaining partially created resources.
 * On success the catalogue owns its instance and physical-device array.
 */
VkResult vulkan_core10_device_catalog_open (
  vulkan_core10_device_catalog_t *catalog,
  uint32_t minimum_api,
  const char *application_name,
  FILE *diagnostics);

/**
 * Check one explicitly indexed physical device against the workload.
 *
 * Verifies API compatibility and actual compute queue, workgroup, descriptor
 * count, storage-range and shared-memory limits. Writes a populated
 * vulkan_auto_device_t and effective API version only on success.
 *
 * The caller supplies the exact device index from enumeration; this method
 * never silently selects another device or changes the requested API.
 * All selected Vulkan handles remain owned by the catalogue.
 */
VkResult vulkan_core10_device_catalog_select (
  vulkan_core10_device_catalog_t *catalog,
  uint32_t device_index,
  const vulkan_auto_compute_requirements_t *workload,
  vulkan_auto_device_t *selected,
  uint32_t *effective_api);

/**
 * Destroy the owned Vulkan instance and indexed physical-device list.
 *
 * The application must first destroy every VkDevice created using a
 * physical device in this catalogue. Closes an empty catalogue safely
 * and resets all catalogue state, including the selection counter.
 */
VkResult vulkan_core10_device_catalog_close (
  vulkan_core10_device_catalog_t *catalog);

#endif
