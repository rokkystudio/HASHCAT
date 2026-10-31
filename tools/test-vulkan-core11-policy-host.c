/**
 * Check the Vulkan core-1.1 API ceiling using only synthetic loader responses.
 *
 * Tests Vulkan 1.0 absence of vkEnumerateInstanceVersion, a real 1.1-style
 * loader version, newer Vulkan 1.4 loaders, physical device versions below
 * the requested minimum, invalid API versions and propagated query failures.
 * No Vulkan driver is loaded, linked or executed in these tests.
 */

#include "vulkan-core11-policy.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define REQUIRE(expression) \
  do \
  { \
    if (!(expression)) \
    { \
      fprintf (stderr, "FAIL_CORE11_POLICY line=%d check=%s\n", \
        __LINE__, #expression); \
      return 1; \
    } \
  } while (0)

static VkBool32 mock_has_version_query;
static uint32_t mock_loader_version;
static VkResult mock_query_result;
static uint32_t mock_calls;

/**
 * Return one version code from a synthetic Vulkan 1.1 loader function.
 *
 * The test can inject a precise Vulkan error code without depending on any
 * real Vulkan installation or implementation's reported version.
 */
static VKAPI_ATTR VkResult VKAPI_CALL mock_enumerate_instance_version (
  uint32_t *version)
{
  mock_calls++;

  if (version == NULL) return VK_ERROR_INITIALIZATION_FAILED;

  if (mock_query_result != VK_SUCCESS) return mock_query_result;

  *version = mock_loader_version;

  return VK_SUCCESS;
}

/**
 * Simulate vkGetInstanceProcAddr at a global Vulkan 1.0 dispatch address.
 *
 * Only vkEnumerateInstanceVersion is implemented by the synthetic loader.
 * The Vulkan 1.0 scenario returns NULL for this Vulkan 1.1 function.
 */
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL mock_get_instance_proc (
  VkInstance instance,
  const char *name)
{
  if (instance != VK_NULL_HANDLE || name == NULL
   || strcmp (name, "vkEnumerateInstanceVersion") != 0
   || mock_has_version_query == VK_FALSE)
  {
    return NULL;
  }

  return (PFN_vkVoidFunction) mock_enumerate_instance_version;
}

/**
 * Verify that the loader function is optional and 1.0 absence is supported.
 *
 * A valid loader function propagates its exact Vulkan error result rather
 * than silently substituting an API version.
 */
static int test_loader_query (void)
{
  FILE *logs = tmpfile ();

  REQUIRE (logs != NULL);

  uint32_t version = UINT32_MAX;

  mock_has_version_query = VK_FALSE;
  mock_calls = 0u;

  REQUIRE (vulkan_core11_query_loader_version (
    mock_get_instance_proc, &version, logs
  ) == VK_SUCCESS);
  REQUIRE (version == VK_API_VERSION_1_0);
  REQUIRE (mock_calls == 0u);

  mock_has_version_query = VK_TRUE;
  mock_loader_version = VK_MAKE_API_VERSION (0, 1, 1, 128);
  mock_query_result = VK_SUCCESS;

  REQUIRE (vulkan_core11_query_loader_version (
    mock_get_instance_proc, &version, logs
  ) == VK_SUCCESS);
  REQUIRE (version == mock_loader_version);
  REQUIRE (mock_calls == 1u);

  mock_loader_version = VK_MAKE_API_VERSION (0, 1, 4, 305);

  REQUIRE (vulkan_core11_query_loader_version (
    mock_get_instance_proc, &version, logs
  ) == VK_SUCCESS);
  REQUIRE (version == mock_loader_version);
  REQUIRE (mock_calls == 2u);

  mock_query_result = VK_ERROR_OUT_OF_HOST_MEMORY;
  version = UINT32_MAX;

  REQUIRE (vulkan_core11_query_loader_version (
    mock_get_instance_proc, &version, logs
  ) == VK_ERROR_OUT_OF_HOST_MEMORY);
  REQUIRE (version == UINT32_MAX);
  REQUIRE (mock_calls == 3u);

  mock_query_result = VK_SUCCESS;
  mock_loader_version = VK_MAKE_API_VERSION (1, 1, 2, 0);

  REQUIRE (vulkan_core11_query_loader_version (
    mock_get_instance_proc, &version, logs
  ) == VK_ERROR_INCOMPATIBLE_DRIVER);
  REQUIRE (version == UINT32_MAX);

  REQUIRE (vulkan_core11_query_loader_version (
    NULL, &version, logs
  ) == VK_ERROR_INITIALIZATION_FAILED);
  REQUIRE (vulkan_core11_query_loader_version (
    mock_get_instance_proc, NULL, logs
  ) == VK_ERROR_INITIALIZATION_FAILED);
  REQUIRE (vulkan_core11_query_loader_version (
    mock_get_instance_proc, &version, NULL
  ) == VK_ERROR_INITIALIZATION_FAILED);

  REQUIRE (fclose (logs) == 0);

  puts ("PASS_CORE11_LOADER dynamic_query core10_missing version_errors");

  return 0;
}

/**
 * Confirm that Vulkan 1.0 is selected for every core-1.0 workload.
 *
 * A newer Vulkan loader does not raise the requested instance API. Vulkan
 * 1.1 is selected only for an explicit Vulkan 1.1 workload requirement.
 */
static int test_instance_api (void)
{
  FILE *logs = tmpfile ();

  REQUIRE (logs != NULL);

  uint32_t instance = UINT32_MAX;

  REQUIRE (vulkan_core11_select_instance_version (
    VK_API_VERSION_1_0, VK_API_VERSION_1_0, &instance, logs
  ) == VK_SUCCESS);
  REQUIRE (instance == VK_API_VERSION_1_0);

  REQUIRE (vulkan_core11_select_instance_version (
    VK_API_VERSION_1_0, VK_API_VERSION_1_1, &instance, logs
  ) == VK_ERROR_INCOMPATIBLE_DRIVER);
  REQUIRE (instance == VK_API_VERSION_1_0);

  REQUIRE (vulkan_core11_select_instance_version (
    VK_MAKE_API_VERSION (0, 1, 1, 128), VK_API_VERSION_1_0,
    &instance, logs
  ) == VK_SUCCESS);
  REQUIRE (instance == VK_API_VERSION_1_0);

  REQUIRE (vulkan_core11_select_instance_version (
    VK_MAKE_API_VERSION (0, 1, 1, 128), VK_API_VERSION_1_1,
    &instance, logs
  ) == VK_SUCCESS);
  REQUIRE (instance == VK_API_VERSION_1_1);

  REQUIRE (vulkan_core11_select_instance_version (
    VK_MAKE_API_VERSION (0, 1, 4, 305), VK_API_VERSION_1_0,
    &instance, logs
  ) == VK_SUCCESS);
  REQUIRE (instance == VK_API_VERSION_1_0);

  REQUIRE (vulkan_core11_select_instance_version (
    VK_MAKE_API_VERSION (0, 1, 4, 305), VK_API_VERSION_1_1,
    &instance, logs
  ) == VK_SUCCESS);
  REQUIRE (instance == VK_API_VERSION_1_1);

  REQUIRE (vulkan_core11_select_instance_version (
    VK_API_VERSION_1_1, VK_API_VERSION_1_2, &instance, logs
  ) == VK_ERROR_INITIALIZATION_FAILED);
  REQUIRE (vulkan_core11_select_instance_version (
    VK_MAKE_API_VERSION (0, 2, 0, 0), VK_API_VERSION_1_1,
    &instance, logs
  ) == VK_ERROR_INCOMPATIBLE_DRIVER);
  REQUIRE (vulkan_core11_select_instance_version (
    VK_API_VERSION_1_1, VK_API_VERSION_1_1, NULL, logs
  ) == VK_ERROR_INITIALIZATION_FAILED);
  REQUIRE (vulkan_core11_select_instance_version (
    VK_API_VERSION_1_1, VK_API_VERSION_1_1, &instance, NULL
  ) == VK_ERROR_INITIALIZATION_FAILED);

  REQUIRE (fclose (logs) == 0);

  puts ("PASS_CORE11_INSTANCE default10 newer_loader_never_raises_api explicit11");

  return 0;
}

/**
 * Verify that each physical device is bounded independently of loader API.
 *
 * A Vulkan 1.0 device is valid when the workload's actual minimum is 1.0,
 * and incompatible when a particular test explicitly requires Vulkan 1.1.
 */
static int test_physical_device_api (void)
{
  FILE *logs = tmpfile ();

  REQUIRE (logs != NULL);

  uint32_t effective = UINT32_MAX;

  REQUIRE (vulkan_core11_select_device_version (
    VK_MAKE_API_VERSION (0, 1, 1, 128),
    VK_API_VERSION_1_1, VK_API_VERSION_1_1,
    &effective, logs
  ) == VK_SUCCESS);
  REQUIRE (effective == VK_API_VERSION_1_1);

  REQUIRE (vulkan_core11_select_device_version (
    VK_MAKE_API_VERSION (0, 1, 4, 305),
    VK_API_VERSION_1_1, VK_API_VERSION_1_1,
    &effective, logs
  ) == VK_SUCCESS);
  REQUIRE (effective == VK_API_VERSION_1_1);

  REQUIRE (vulkan_core11_select_device_version (
    VK_MAKE_API_VERSION (0, 1, 4, 305),
    VK_API_VERSION_1_0, VK_API_VERSION_1_0,
    &effective, logs
  ) == VK_SUCCESS);
  REQUIRE (effective == VK_API_VERSION_1_0);

  REQUIRE (vulkan_core11_select_device_version (
    VK_MAKE_API_VERSION (0, 1, 0, 20),
    VK_API_VERSION_1_1, VK_API_VERSION_1_0,
    &effective, logs
  ) == VK_SUCCESS);
  REQUIRE (effective == VK_API_VERSION_1_0);

  REQUIRE (vulkan_core11_select_device_version (
    VK_API_VERSION_1_0,
    VK_API_VERSION_1_1, VK_API_VERSION_1_1,
    &effective, logs
  ) == VK_ERROR_FEATURE_NOT_PRESENT);
  REQUIRE (effective == VK_API_VERSION_1_0);

  REQUIRE (vulkan_core11_select_device_version (
    VK_API_VERSION_1_1,
    VK_API_VERSION_1_0, VK_API_VERSION_1_0,
    &effective, logs
  ) == VK_SUCCESS);
  REQUIRE (effective == VK_API_VERSION_1_0);

  REQUIRE (vulkan_core11_select_device_version (
    VK_API_VERSION_1_1,
    VK_API_VERSION_1_2, VK_API_VERSION_1_1,
    &effective, logs
  ) == VK_ERROR_INITIALIZATION_FAILED);
  REQUIRE (vulkan_core11_select_device_version (
    VK_MAKE_API_VERSION (1, 1, 4, 0),
    VK_API_VERSION_1_1, VK_API_VERSION_1_1,
    &effective, logs
  ) == VK_ERROR_INCOMPATIBLE_DRIVER);
  REQUIRE (vulkan_core11_select_device_version (
    VK_API_VERSION_1_1,
    VK_API_VERSION_1_1, VK_API_VERSION_1_1,
    NULL, logs
  ) == VK_ERROR_INITIALIZATION_FAILED);
  REQUIRE (vulkan_core11_select_device_version (
    VK_API_VERSION_1_1,
    VK_API_VERSION_1_1, VK_API_VERSION_1_1,
    &effective, NULL
  ) == VK_ERROR_INITIALIZATION_FAILED);

  REQUIRE (fclose (logs) == 0);

  puts ("PASS_CORE11_DEVICE cap11 physical_version_independent lower_minimum");

  return 0;
}

/**
 * Run real C policy tests at both debug and optimized native compiler modes.
 *
 * All GPU/loader results above are in-process synthetic test values.
 */
int main (int argc, char **argv)
{
  if (argc != 1)
  {
    fprintf (stderr, "Usage: %s\n", argv[0]);

    return 2;
  }

  if (test_loader_query () != 0) return 1;
  if (test_instance_api () != 0) return 1;
  if (test_physical_device_api () != 0) return 1;

  puts ("PASS_CORE11_POLICY all three groups without Vulkan driver");

  return 0;
}
