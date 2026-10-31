/**
 * Discover real Vulkan 1.0 Compute devices with explicit instance ownership.
 *
 * The catalogue creates one instance and stores every physical-device
 * handle in enumeration order. Each caller-selected index is checked
 * against the exact public Compute workload without driver-specific rules.
 */

#include "vulkan-core10-device-catalog.h"
#include "vulkan-core11-policy.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/**
 * Destroy an instance and release a physical-device array in safe order.
 *
 * The caller closes all dependent VkDevices before invoking this function.
 * The owned instance handle is the only Vulkan resource of the catalogue.
 */
VkResult vulkan_core10_device_catalog_close (
  vulkan_core10_device_catalog_t *catalog)
{
  if (catalog == NULL) return VK_ERROR_INITIALIZATION_FAILED;

  if (catalog->opened == VK_TRUE && catalog->diagnostics != NULL)
  {
    fprintf (
      catalog->diagnostics,
      "DEVICE_CATALOG_CLOSE devices=%" PRIu32
      " selections=%" PRIu32 "\n",
      catalog->device_count, catalog->selections
    );
  }

  free (catalog->physical);

  if (catalog->instance != VK_NULL_HANDLE)
  {
    vkDestroyInstance (catalog->instance, NULL);
  }

  memset (catalog, 0, sizeof (*catalog));

  return VK_SUCCESS;
}

/**
 * Negotiate the requested core API, create VkInstance and enumerate devices.
 *
 * Partial Vulkan instance creation and host array allocation are released
 * on each failure. A nonempty physical-device list is required.
 */
VkResult vulkan_core10_device_catalog_open (
  vulkan_core10_device_catalog_t *catalog,
  uint32_t minimum_api,
  const char *application_name,
  FILE *diagnostics)
{
  if (catalog == NULL || application_name == NULL
   || application_name[0] == '\0' || diagnostics == NULL
   || (minimum_api != VK_API_VERSION_1_0
    && minimum_api != VK_API_VERSION_1_1)
   || catalog->opened == VK_TRUE
   || catalog->instance != VK_NULL_HANDLE
   || catalog->physical != NULL
   || catalog->device_count != 0u)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  uint32_t loader = 0u;

  VkResult result = vulkan_core11_query_loader_version (
    vkGetInstanceProcAddr, &loader, diagnostics
  );

  if (result != VK_SUCCESS) return result;

  uint32_t instance_api = 0u;

  result = vulkan_core11_select_instance_version (
    loader, minimum_api, &instance_api, diagnostics
  );

  if (result != VK_SUCCESS) return result;

  const VkApplicationInfo application =
  {
    .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
    .pApplicationName = application_name,
    .applicationVersion = VK_MAKE_VERSION (1, 0, 0),
    .pEngineName = "None",
    .engineVersion = VK_MAKE_VERSION (1, 0, 0),
    .apiVersion = instance_api
  };
  const VkInstanceCreateInfo instance_info =
  {
    .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
    .pApplicationInfo = &application
  };

  result = vkCreateInstance (
    &instance_info, NULL, &catalog->instance
  );

  if (result != VK_SUCCESS)
  {
    fprintf (
      diagnostics, "DEVICE_CATALOG_INSTANCE_FAILED result=%d\n", result
    );

    vulkan_core10_device_catalog_close (catalog);

    return result;
  }

  uint32_t count = 0u;

  result = vkEnumeratePhysicalDevices (
    catalog->instance, &count, NULL
  );

  if (result != VK_SUCCESS || count == 0u)
  {
    fprintf (
      diagnostics,
      "DEVICE_CATALOG_ENUM_FAILED result=%d devices=%" PRIu32 "\n",
      result, count
    );

    vulkan_core10_device_catalog_close (catalog);

    return result == VK_SUCCESS ? VK_ERROR_FEATURE_NOT_PRESENT : result;
  }

#if SIZE_MAX <= UINT32_MAX
  if (count > SIZE_MAX / sizeof (VkPhysicalDevice))
  {
    vulkan_core10_device_catalog_close (catalog);

    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }
#endif

  VkPhysicalDevice *physical = (VkPhysicalDevice *) calloc (
    count, sizeof (*physical)
  );

  if (physical == NULL)
  {
    vulkan_core10_device_catalog_close (catalog);

    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }

  catalog->physical = physical;

  result = vkEnumeratePhysicalDevices (
    catalog->instance, &count, physical
  );

  if (result != VK_SUCCESS || count == 0u)
  {
    fprintf (
      diagnostics,
      "DEVICE_CATALOG_ENUM_FAILED result=%d devices=%" PRIu32 "\n",
      result, count
    );

    vulkan_core10_device_catalog_close (catalog);

    return result == VK_SUCCESS ? VK_ERROR_FEATURE_NOT_PRESENT : result;
  }

  catalog->device_count = count;
  catalog->minimum_api = minimum_api;
  catalog->instance_api = instance_api;
  catalog->diagnostics = diagnostics;
  catalog->opened = VK_TRUE;

  fprintf (
    diagnostics,
    "DEVICE_CATALOG_OPEN requested_api=%" PRIu32 ".%" PRIu32
    " instance_api=%" PRIu32 ".%" PRIu32
    " devices=%" PRIu32 " extensions=0 layers=0\n",
    VK_API_VERSION_MAJOR (minimum_api),
    VK_API_VERSION_MINOR (minimum_api),
    VK_API_VERSION_MAJOR (instance_api),
    VK_API_VERSION_MINOR (instance_api),
    count
  );

  return VK_SUCCESS;
}

/**
 * Select exactly the requested physical-device index after checking API
 * and workload limits against real Vulkan driver capabilities.
 */
VkResult vulkan_core10_device_catalog_select (
  vulkan_core10_device_catalog_t *catalog,
  uint32_t device_index,
  const vulkan_auto_compute_requirements_t *workload,
  vulkan_auto_device_t *selected,
  uint32_t *effective_api)
{
  if (catalog == NULL || catalog->opened != VK_TRUE
   || catalog->instance == VK_NULL_HANDLE
   || catalog->physical == NULL
   || catalog->diagnostics == NULL
   || workload == NULL || selected == NULL || effective_api == NULL
   || device_index >= catalog->device_count
   || catalog->selections == UINT32_MAX)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  VkPhysicalDeviceProperties properties = { 0 };

  vkGetPhysicalDeviceProperties (
    catalog->physical[device_index], &properties
  );

  uint32_t version = 0u;

  VkResult result = vulkan_core11_select_device_version (
    properties.apiVersion, catalog->instance_api,
    catalog->minimum_api, &version, catalog->diagnostics
  );

  if (result != VK_SUCCESS) return result;

  vulkan_auto_device_t device = { 0 };

  result = vulkan_auto_check_device (
    catalog->physical[device_index], device_index,
    workload, &device, catalog->diagnostics
  );

  if (result != VK_SUCCESS) return result;

  *selected = device;
  *effective_api = version;
  catalog->selections++;

  fprintf (
    catalog->diagnostics,
    "DEVICE_CATALOG_SELECT index=%" PRIu32
    " queue_family=%" PRIu32 " effective_api=%" PRIu32
    ".%" PRIu32 " selections=%" PRIu32 "\n",
    device_index, device.compute_queue_family,
    VK_API_VERSION_MAJOR (version),
    VK_API_VERSION_MINOR (version),
    catalog->selections
  );

  return VK_SUCCESS;
}
