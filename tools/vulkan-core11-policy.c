/**
 * Implement Vulkan 1.0/1.1 core API negotiation using real reported versions.
 *
 * Loader and device API versions are independent. This policy caps requested
 * instance-level functionality at 1.1, while treating newer Vulkan versions
 * as compatible without enabling extensions or optional device features.
 */

#include "vulkan-core11-policy.h"

#include <inttypes.h>
#include <string.h>

/**
 * Convert a reported Vulkan version to its core major/minor contract.
 *
 * Vulkan patch versions are implementation updates and are not used as
 * different capability tiers by this policy. Unknown major versions or
 * non-default API variants are rejected rather than guessed.
 */
static VkResult vulkan_core11_normalize_version (
  uint32_t value,
  uint32_t *normalized)
{
  if (normalized == NULL
   || VK_API_VERSION_VARIANT (value) != 0u
   || VK_API_VERSION_MAJOR (value) != 1u)
  {
    return VK_ERROR_INCOMPATIBLE_DRIVER;
  }

  const uint32_t minor = VK_API_VERSION_MINOR (value);

  *normalized = VK_MAKE_API_VERSION (0, 1, minor, 0);

  return VK_SUCCESS;
}

/**
 * Read the optional Vulkan 1.1 loader-version entry point through the
 * Vulkan 1.0 function pointer table, preserving Vulkan 1.0 compatibility.
 */
VkResult vulkan_core11_query_loader_version (
  PFN_vkGetInstanceProcAddr get_proc,
  uint32_t *loader_version,
  FILE *diagnostics)
{
  if (get_proc == NULL || loader_version == NULL
   || diagnostics == NULL)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  const PFN_vkEnumerateInstanceVersion enumerate =
    (PFN_vkEnumerateInstanceVersion) get_proc (
      VK_NULL_HANDLE, "vkEnumerateInstanceVersion"
    );

  if (enumerate == NULL)
  {
    *loader_version = VK_API_VERSION_1_0;

    fprintf (
      diagnostics,
      "VULKAN_LOADER_VERSION=1.0.0 source=core10_missing_entry_point\n"
    );

    return VK_SUCCESS;
  }

  uint32_t available = 0u;

  const VkResult query = enumerate (&available);

  if (query != VK_SUCCESS)
  {
    fprintf (
      diagnostics, "REJECT_LOADER_VERSION_QUERY result=%d\n", query
    );

    return query;
  }

  uint32_t normalized = 0u;

  const VkResult valid = vulkan_core11_normalize_version (
    available, &normalized
  );

  if (valid != VK_SUCCESS)
  {
    fprintf (
      diagnostics,
      "REJECT_LOADER_VERSION raw=0x%08" PRIx32 "\n", available
    );

    return valid;
  }

  *loader_version = available;

  fprintf (
    diagnostics,
    "VULKAN_LOADER_VERSION=%" PRIu32 ".%" PRIu32 ".%" PRIu32
    " source=vkEnumerateInstanceVersion\n",
    VK_API_VERSION_MAJOR (available),
    VK_API_VERSION_MINOR (available),
    VK_API_VERSION_PATCH (available)
  );

  return VK_SUCCESS;
}

/**
 * Request exactly the Vulkan core API version required by the workload.
 *
 * Vulkan 1.0 workloads always select an API 1.0 instance, including on
 * Vulkan 1.1-1.4 loaders. A workload explicitly requiring core 1.1 selects
 * version 1.1 after checking the loader. No Vulkan 1.2+ core API is used.
 */
VkResult vulkan_core11_select_instance_version (
  uint32_t loader_version,
  uint32_t minimum_version,
  uint32_t *instance_version,
  FILE *diagnostics)
{
  if (instance_version == NULL || diagnostics == NULL)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  uint32_t available = 0u;

  if (vulkan_core11_normalize_version (
    loader_version, &available
  ) != VK_SUCCESS)
  {
    return VK_ERROR_INCOMPATIBLE_DRIVER;
  }

  if (minimum_version != VK_API_VERSION_1_0
   && minimum_version != VK_API_VERSION_1_1)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  if (available < minimum_version)
  {
    fprintf (
      diagnostics,
      "REJECT_INSTANCE_API available=%" PRIu32 ".%" PRIu32
      " minimum=%" PRIu32 ".%" PRIu32 "\n",
      VK_API_VERSION_MAJOR (available),
      VK_API_VERSION_MINOR (available),
      VK_API_VERSION_MAJOR (minimum_version),
      VK_API_VERSION_MINOR (minimum_version)
    );

    return VK_ERROR_INCOMPATIBLE_DRIVER;
  }

  *instance_version = minimum_version;

  fprintf (
    diagnostics,
    "SELECT_INSTANCE_API=%" PRIu32 ".%" PRIu32
    " loader_minor=%" PRIu32 " cap=1.1\n",
    VK_API_VERSION_MAJOR (minimum_version),
    VK_API_VERSION_MINOR (minimum_version),
    VK_API_VERSION_MINOR (available)
  );

  return VK_SUCCESS;
}

/**
 * Bound a physical device's effective API by the selected instance API.
 *
 * Features from a higher reported physical device version remain unavailable
 * to this core-1.1-only path unless separately negotiated by the caller.
 */
VkResult vulkan_core11_select_device_version (
  uint32_t physical_device_version,
  uint32_t instance_version,
  uint32_t minimum_version,
  uint32_t *effective_version,
  FILE *diagnostics)
{
  if (effective_version == NULL || diagnostics == NULL)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  uint32_t available = 0u;

  if (vulkan_core11_normalize_version (
    physical_device_version, &available
  ) != VK_SUCCESS)
  {
    return VK_ERROR_INCOMPATIBLE_DRIVER;
  }

  if ((instance_version != VK_API_VERSION_1_0
    && instance_version != VK_API_VERSION_1_1)
   || (minimum_version != VK_API_VERSION_1_0
    && minimum_version != VK_API_VERSION_1_1))
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  const uint32_t selected =
    available < instance_version ? available : instance_version;

  if (selected < minimum_version)
  {
    fprintf (
      diagnostics,
      "REJECT_DEVICE_API available=%" PRIu32 ".%" PRIu32
      " instance=%" PRIu32 ".%" PRIu32
      " minimum=%" PRIu32 ".%" PRIu32 "\n",
      VK_API_VERSION_MAJOR (available),
      VK_API_VERSION_MINOR (available),
      VK_API_VERSION_MAJOR (instance_version),
      VK_API_VERSION_MINOR (instance_version),
      VK_API_VERSION_MAJOR (minimum_version),
      VK_API_VERSION_MINOR (minimum_version)
    );

    return VK_ERROR_FEATURE_NOT_PRESENT;
  }

  *effective_version = selected;

  fprintf (
    diagnostics,
    "SELECT_DEVICE_API=%" PRIu32 ".%" PRIu32
    " reported_minor=%" PRIu32 " instance_minor=%" PRIu32
    " cap=1.1\n",
    VK_API_VERSION_MAJOR (selected),
    VK_API_VERSION_MINOR (selected),
    VK_API_VERSION_MINOR (available),
    VK_API_VERSION_MINOR (instance_version)
  );

  return VK_SUCCESS;
}
