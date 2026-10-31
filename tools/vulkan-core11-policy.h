#ifndef VULKAN_CORE11_POLICY_H
#define VULKAN_CORE11_POLICY_H

/**
 * Select the minimum sufficient Vulkan core API for a compute workload.
 *
 * Vulkan 1.0 is the default, including on loaders and devices implementing
 * later versions. A caller may require Vulkan 1.1 for a workload that needs
 * core-1.1 functionality. Neither path uses Vulkan 1.2+ APIs, extensions,
 * GPU vendor names or Android device-model profiles.
 */

#include <vulkan/vulkan.h>

#include <stdint.h>
#include <stdio.h>

/**
 * Query the actual loader API version using the Vulkan 1.0 function loader.
 *
 * A missing vkEnumerateInstanceVersion entry point denotes a Vulkan 1.0
 * loader, in which case the result is exactly VK_API_VERSION_1_0.
 * A present entry point is called and its VkResult is propagated. The
 * supplied getter permits deterministic host tests without a GPU driver.
 */
VkResult vulkan_core11_query_loader_version (
  PFN_vkGetInstanceProcAddr get_proc,
  uint32_t *loader_version,
  FILE *diagnostics);

/**
 * Request precisely the core Vulkan version required by the workload.
 *
 * Vulkan 1.0 workloads create a Vulkan 1.0 instance even on loaders 1.1+.
 * Only callers explicitly requiring Vulkan 1.1 create a 1.1 instance, after
 * verifying loader support. Versions above 1.1 are not requested. A loader
 * below the minimum returns VK_ERROR_INCOMPATIBLE_DRIVER without fallback.
 */
VkResult vulkan_core11_select_instance_version (
  uint32_t loader_version,
  uint32_t minimum_version,
  uint32_t *instance_version,
  FILE *diagnostics);

/**
 * Determine the effective physical-device core API for the chosen instance.
 *
 * The effective version is the lower of the physical device's reported API
 * and the instance API. A Vulkan 1.0 device supports the default workload;
 * an explicit core-1.1 requirement rejects it with
 * VK_ERROR_FEATURE_NOT_PRESENT. This function enables no optional features.
 */
VkResult vulkan_core11_select_device_version (
  uint32_t physical_device_version,
  uint32_t instance_version,
  uint32_t minimum_version,
  uint32_t *effective_version,
  FILE *diagnostics);

#endif
