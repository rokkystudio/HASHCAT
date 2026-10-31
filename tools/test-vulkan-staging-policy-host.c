/**
 * Validate exact Vulkan 1.0 memory and compute selection in native C.
 *
 * The test links the production-independent auto-config and staging-memory
 * C implementations with mock Vulkan query and cache-coherency entry points.
 * It exercises deterministic physical-device capability sets, the complete
 * compatible-memory bitmask domain including index 31, staging fallback,
 * allocation overflow and explicit mapped-cache operations.
 *
 * No Vulkan driver is loaded and no GPU command is created or submitted.
 * All data are synthetic and unrelated to hashcat or cryptographic work.
 */

#include "vulkan-staging-memory.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define REQUIRE(condition) \
  do \
  { \
    if (!(condition)) \
    { \
      fprintf ( \
        stderr, "FAIL_HOST line=%d expression=%s\n", \
        __LINE__, #condition \
      ); \
      return 1; \
    } \
  } while (0)

static VkPhysicalDeviceProperties mock_properties;
static VkPhysicalDeviceMemoryProperties mock_memory;
static VkQueueFamilyProperties mock_queues[2];
static uint32_t mock_queue_count;
static uint32_t mock_query_calls;
static uint32_t mock_flush_calls;
static uint32_t mock_invalidate_calls;
static VkMappedMemoryRange mock_last_range;
static VkResult mock_flush_result = VK_SUCCESS;
static VkResult mock_invalidate_result = VK_SUCCESS;

static const VkPhysicalDevice mock_physical =
  (VkPhysicalDevice) (uintptr_t) 0x11u;
static const VkDevice mock_device =
  (VkDevice) (uintptr_t) 0x22u;
static const VkDeviceMemory mock_device_memory =
  (VkDeviceMemory) (uintptr_t) 0x33u;

/**
 * Supply exact synthetic Vulkan 1.0 API properties and two queue families.
 *
 * Queue family zero is graphics-only; queue family one is compute-capable.
 * Memory type zero is local, one is visible/noncoherent, and two is
 * visible/coherent. No device-specific property is inferred.
 */
static void reset_mock_device (void)
{
  memset (&mock_properties, 0, sizeof (mock_properties));
  memset (&mock_memory, 0, sizeof (mock_memory));
  memset (mock_queues, 0, sizeof (mock_queues));
  memset (&mock_last_range, 0, sizeof (mock_last_range));

  mock_query_calls = 0u;
  mock_flush_calls = 0u;
  mock_invalidate_calls = 0u;
  mock_flush_result = VK_SUCCESS;
  mock_invalidate_result = VK_SUCCESS;

  mock_properties.apiVersion = VK_API_VERSION_1_0;
  mock_properties.vendorID = UINT32_C (0x1111);
  mock_properties.deviceID = UINT32_C (0x2222);
  strcpy (mock_properties.deviceName, "synthetic-vulkan-1.0");

  VkPhysicalDeviceLimits *limits = &mock_properties.limits;

  limits->maxComputeWorkGroupSize[0] = 256u;
  limits->maxComputeWorkGroupSize[1] = 256u;
  limits->maxComputeWorkGroupSize[2] = 64u;
  limits->maxComputeWorkGroupCount[0] = 1024u;
  limits->maxComputeWorkGroupCount[1] = 1024u;
  limits->maxComputeWorkGroupCount[2] = 64u;
  limits->maxComputeWorkGroupInvocations = 512u;
  limits->maxComputeSharedMemorySize = 16384u;
  limits->maxStorageBufferRange = 65536u;
  limits->maxPerStageDescriptorStorageBuffers = 4u;
  limits->maxDescriptorSetStorageBuffers = 8u;
  limits->nonCoherentAtomSize = 256u;

  mock_memory.memoryTypeCount = 3u;
  mock_memory.memoryTypes[0].propertyFlags =
    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  mock_memory.memoryTypes[1].propertyFlags =
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
    | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
  mock_memory.memoryTypes[2].propertyFlags =
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
    | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

  mock_queue_count = 2u;
  mock_queues[0].queueCount = 1u;
  mock_queues[0].queueFlags = VK_QUEUE_GRAPHICS_BIT;
  mock_queues[1].queueCount = 1u;
  mock_queues[1].queueFlags = VK_QUEUE_COMPUTE_BIT;
}

/**
 * Return exact synthetic physical-device properties without calling a driver.
 */
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties (
  VkPhysicalDevice physicalDevice,
  VkPhysicalDeviceProperties *properties)
{
  if (physicalDevice == mock_physical && properties != NULL)
  {
    *properties = mock_properties;
    mock_query_calls++;
  }
}

/**
 * Return the two synthetic queue families, respecting Vulkan's count query.
 */
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties (
  VkPhysicalDevice physicalDevice,
  uint32_t *count,
  VkQueueFamilyProperties *families)
{
  if (physicalDevice != mock_physical || count == NULL) return;

  mock_query_calls++;

  if (families == NULL)
  {
    *count = mock_queue_count;

    return;
  }

  const uint32_t provided = *count;
  const uint32_t copied =
    provided < mock_queue_count ? provided : mock_queue_count;

  memcpy (families, mock_queues, copied * sizeof (*families));

  *count = copied;
}

/**
 * Return synthetic physical-device memory properties without loading Vulkan.
 */
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties (
  VkPhysicalDevice physicalDevice,
  VkPhysicalDeviceMemoryProperties *properties)
{
  if (physicalDevice == mock_physical && properties != NULL)
  {
    *properties = mock_memory;
    mock_query_calls++;
  }
}

/**
 * Record an exact mapped-range flush and return a controllable Vulkan result.
 */
VKAPI_ATTR VkResult VKAPI_CALL vkFlushMappedMemoryRanges (
  VkDevice device,
  uint32_t count,
  const VkMappedMemoryRange *ranges)
{
  if (device != mock_device || count != 1u || ranges == NULL)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  mock_flush_calls++;
  mock_last_range = ranges[0];

  return mock_flush_result;
}

/**
 * Record an exact mapped-range invalidate and return a configured result.
 */
VKAPI_ATTR VkResult VKAPI_CALL vkInvalidateMappedMemoryRanges (
  VkDevice device,
  uint32_t count,
  const VkMappedMemoryRange *ranges)
{
  if (device != mock_device || count != 1u || ranges == NULL)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  mock_invalidate_calls++;
  mock_last_range = ranges[0];

  return mock_invalidate_result;
}

/**
 * Verify DEVICE_LOCAL priority, mapped-memory priority and bit 31 support.
 *
 * The two selectors receive explicit compatibility masks. Invalid masks,
 * null results and out-of-range type counts must not produce a selection.
 */
static int test_memory_types (void)
{
  reset_mock_device ();

  uint32_t index = UINT32_MAX;
  VkBool32 coherent = VK_FALSE;

  REQUIRE (vulkan_auto_select_gpu_memory_type (
    &mock_memory, UINT32_C (0x7), &index
  ) == VK_SUCCESS);
  REQUIRE (index == 0u);

  REQUIRE (vulkan_auto_select_gpu_memory_type (
    &mock_memory, UINT32_C (0x6), &index
  ) == VK_SUCCESS);
  REQUIRE (index == 1u);

  REQUIRE (vulkan_auto_select_memory_type (
    &mock_memory, UINT32_C (0x7), &index, &coherent
  ) == VK_SUCCESS);
  REQUIRE (index == 2u && coherent == VK_TRUE);

  REQUIRE (vulkan_auto_select_memory_type (
    &mock_memory, UINT32_C (0x2), &index, &coherent
  ) == VK_SUCCESS);
  REQUIRE (index == 1u && coherent == VK_FALSE);

  REQUIRE (vulkan_auto_select_memory_type (
    &mock_memory, UINT32_C (0x1), &index, &coherent
  ) == VK_ERROR_FEATURE_NOT_PRESENT);

  REQUIRE (vulkan_auto_select_gpu_memory_type (
    &mock_memory, 0u, &index
  ) == VK_ERROR_FEATURE_NOT_PRESENT);

  REQUIRE (vulkan_auto_select_gpu_memory_type (
    NULL, 1u, &index
  ) == VK_ERROR_INITIALIZATION_FAILED);

  REQUIRE (vulkan_auto_select_memory_type (
    &mock_memory, 7u, NULL, &coherent
  ) == VK_ERROR_INITIALIZATION_FAILED);

  mock_memory.memoryTypeCount = 32u;
  mock_memory.memoryTypes[31].propertyFlags =
    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
    | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
    | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

  REQUIRE (vulkan_auto_select_gpu_memory_type (
    &mock_memory, UINT32_C (0x80000000), &index
  ) == VK_SUCCESS);
  REQUIRE (index == 31u);

  REQUIRE (vulkan_auto_select_memory_type (
    &mock_memory, UINT32_C (0x80000000), &index, &coherent
  ) == VK_SUCCESS);
  REQUIRE (index == 31u && coherent == VK_TRUE);

  mock_memory.memoryTypeCount = 33u;

  REQUIRE (vulkan_auto_select_gpu_memory_type (
    &mock_memory, UINT32_C (0x80000000), &index
  ) == VK_ERROR_INITIALIZATION_FAILED);

  REQUIRE (vulkan_auto_select_memory_type (
    &mock_memory, UINT32_C (0x80000000), &index, &coherent
  ) == VK_ERROR_INITIALIZATION_FAILED);

  puts ("PASS_HOST_MEMORY_TYPES coherent noncoherent gpu_local bit31 invalid");

  return 0;
}

/**
 * Verify each exact direct/staging decision from synthetic memory properties.
 *
 * GPU-only storage must select staging, while compatible host-visible
 * storage must select direct unless staging is explicitly requested.
 */
static int test_transfer_modes (void)
{
  reset_mock_device ();

  vulkan_auto_transfer_mode_t mode = VULKAN_AUTO_TRANSFER_STAGING;

  REQUIRE (vulkan_auto_choose_transfer_mode (
    &mock_memory, 7u, 6u, VK_FALSE, &mode, stdout
  ) == VK_SUCCESS);
  REQUIRE (mode == VULKAN_AUTO_TRANSFER_DIRECT);

  REQUIRE (vulkan_auto_choose_transfer_mode (
    &mock_memory, 1u, 1u, VK_FALSE, &mode, stdout
  ) == VK_SUCCESS);
  REQUIRE (mode == VULKAN_AUTO_TRANSFER_STAGING);

  REQUIRE (vulkan_auto_choose_transfer_mode (
    &mock_memory, 1u, 2u, VK_FALSE, &mode, stdout
  ) == VK_SUCCESS);
  REQUIRE (mode == VULKAN_AUTO_TRANSFER_STAGING);

  REQUIRE (vulkan_auto_choose_transfer_mode (
    &mock_memory, 7u, 6u, VK_TRUE, &mode, stdout
  ) == VK_SUCCESS);
  REQUIRE (mode == VULKAN_AUTO_TRANSFER_STAGING);

  mode = VULKAN_AUTO_TRANSFER_DIRECT;

  REQUIRE (vulkan_auto_choose_transfer_mode (
    &mock_memory, 0u, 6u, VK_FALSE, &mode, stdout
  ) == VK_ERROR_FEATURE_NOT_PRESENT);
  REQUIRE (mode == VULKAN_AUTO_TRANSFER_DIRECT);

  REQUIRE (vulkan_auto_choose_transfer_mode (
    &mock_memory, 7u, 6u, VK_FALSE, NULL, stdout
  ) == VK_ERROR_INITIALIZATION_FAILED);

  REQUIRE (vulkan_auto_choose_transfer_mode (
    &mock_memory, 7u, 6u, VK_FALSE, &mode, NULL
  ) == VK_ERROR_INITIALIZATION_FAILED);

  puts ("PASS_HOST_TRANSFER_MODES direct local_only mixed forced invalid");

  return 0;
}

/**
 * Exhaustively compare real C selectors against independent bitmask rules.
 *
 * All 64 three-type arrangements use valid core Vulkan memory property
 * classes. Every nonempty three-bit compatibility mask is checked against
 * each device arrangement. Each pair of storage masks is then tested in
 * auto and explicitly requested staging modes, for 6,272 exact decisions.
 *
 * Vulkan API calls are intercepted by the native mocks and no GPU is used.
 */
static int test_exhaustive_memory_matrix (void)
{
  static const VkMemoryPropertyFlags flag_classes[4] =
  {
    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
      | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
      | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
      | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
      | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
  };

  reset_mock_device ();

  FILE *diagnostics = tmpfile ();

  REQUIRE (diagnostics != NULL);

  uint32_t verified_selections = 0u;
  uint32_t verified_transfers = 0u;

  for (uint32_t combination = 0u; combination < 64u; combination++)
  {
    uint32_t encoded = combination;

    for (uint32_t index = 0u; index < 3u; index++)
    {
      mock_memory.memoryTypes[index].propertyFlags =
        flag_classes[encoded % 4u];

      encoded /= 4u;
    }

    for (uint32_t mask = 1u; mask < 8u; mask++)
    {
      uint32_t gpu_expected = UINT32_MAX;
      uint32_t visible_expected = UINT32_MAX;
      VkBool32 coherent_expected = VK_FALSE;

      for (uint32_t index = 0u; index < 3u; index++)
      {
        const VkMemoryPropertyFlags flags =
          mock_memory.memoryTypes[index].propertyFlags;

        if ((mask & (1u << index)) != 0u
         && (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0u
         && gpu_expected == UINT32_MAX)
        {
          gpu_expected = index;
        }
      }

      if (gpu_expected == UINT32_MAX)
      {
        for (uint32_t index = 0u; index < 3u; index++)
        {
          if ((mask & (1u << index)) != 0u)
          {
            gpu_expected = index;

            break;
          }
        }
      }

      for (uint32_t index = 0u; index < 3u; index++)
      {
        const VkMemoryPropertyFlags flags =
          mock_memory.memoryTypes[index].propertyFlags;

        if ((mask & (1u << index)) != 0u
         && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0u
         && (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0u)
        {
          visible_expected = index;
          coherent_expected = VK_TRUE;

          break;
        }
      }

      if (visible_expected == UINT32_MAX)
      {
        for (uint32_t index = 0u; index < 3u; index++)
        {
          const VkMemoryPropertyFlags flags =
            mock_memory.memoryTypes[index].propertyFlags;

          if ((mask & (1u << index)) != 0u
           && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0u)
          {
            visible_expected = index;

            break;
          }
        }
      }

      uint32_t selected = UINT32_MAX;
      VkBool32 coherent = VK_FALSE;

      REQUIRE (vulkan_auto_select_gpu_memory_type (
        &mock_memory, mask, &selected
      ) == VK_SUCCESS);
      REQUIRE (selected == gpu_expected);

      const VkResult visible_result = vulkan_auto_select_memory_type (
        &mock_memory, mask, &selected, &coherent
      );

      if (visible_expected == UINT32_MAX)
      {
        REQUIRE (visible_result == VK_ERROR_FEATURE_NOT_PRESENT);
      }
      else
      {
        REQUIRE (visible_result == VK_SUCCESS);
        REQUIRE (selected == visible_expected);
        REQUIRE (coherent == coherent_expected);
      }

      verified_selections++;
    }

    for (uint32_t input_mask = 1u; input_mask < 8u; input_mask++)
    {
      for (uint32_t output_mask = 1u; output_mask < 8u; output_mask++)
      {
        VkBool32 input_visible = VK_FALSE;
        VkBool32 output_visible = VK_FALSE;

        for (uint32_t index = 0u; index < 3u; index++)
        {
          const VkMemoryPropertyFlags flags =
            mock_memory.memoryTypes[index].propertyFlags;

          if ((flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0u)
          {
            continue;
          }

          if ((input_mask & (1u << index)) != 0u) input_visible = VK_TRUE;
          if ((output_mask & (1u << index)) != 0u) output_visible = VK_TRUE;
        }

        for (uint32_t forced = 0u; forced < 2u; forced++)
        {
          vulkan_auto_transfer_mode_t mode = VULKAN_AUTO_TRANSFER_DIRECT;

          REQUIRE (vulkan_auto_choose_transfer_mode (
            &mock_memory, input_mask, output_mask,
            forced ? VK_TRUE : VK_FALSE,
            &mode, diagnostics
          ) == VK_SUCCESS);

          const vulkan_auto_transfer_mode_t expected =
            forced != 0u || input_visible == VK_FALSE
            || output_visible == VK_FALSE
            ? VULKAN_AUTO_TRANSFER_STAGING
            : VULKAN_AUTO_TRANSFER_DIRECT;

          REQUIRE (mode == expected);

          verified_transfers++;
        }
      }
    }
  }

  REQUIRE (fclose (diagnostics) == 0);
  REQUIRE (verified_selections == 448u);
  REQUIRE (verified_transfers == 6272u);

  printf (
    "PASS_HOST_EXHAUSTIVE_MATRIX layouts=64 masks=7"
    " selections=%" PRIu32 " transfers=%" PRIu32 "\n",
    verified_selections, verified_transfers
  );

  return 0;
}

/**
 * Test exact driver-aligned memory planning and noncoherent atom padding.
 *
 * The planner must respect each supplied memoryTypeBits intersection, reject
 * disjoint type sets and out-of-range buffer sizes, and fail on 64-bit overflow.
 */
static int test_memory_plans (void)
{
  reset_mock_device ();

  const VkDeviceSize requested[2] = { 384u, 257u };
  VkMemoryRequirements actual[2] =
  {
    { .size = 384u, .alignment = 64u, .memoryTypeBits = 2u },
    { .size = 257u, .alignment = 64u, .memoryTypeBits = 2u }
  };
  VkDeviceSize offsets[2] = { UINT64_MAX, UINT64_MAX };
  vulkan_auto_memory_plan_t plan = { 0 };

  REQUIRE (vulkan_auto_plan_shared_memory (
    &mock_memory, 256u, actual, requested, 2u,
    offsets, &plan, stdout
  ) == VK_SUCCESS);
  REQUIRE (offsets[0] == 0u && offsets[1] == 512u);
  REQUIRE (plan.allocation_bytes == 769u);
  REQUIRE (plan.memory_type_index == 1u && plan.coherent == VK_FALSE);
  REQUIRE (plan.compatible_memory_type_bits == 2u);
  REQUIRE (plan.non_coherent_atom_size == 256u);

  actual[0].memoryTypeBits = 6u;
  actual[1].memoryTypeBits = 6u;

  REQUIRE (vulkan_auto_plan_shared_memory (
    &mock_memory, 256u, actual, requested, 2u,
    offsets, &plan, stdout
  ) == VK_SUCCESS);
  REQUIRE (offsets[0] == 0u && offsets[1] == 384u);
  REQUIRE (plan.allocation_bytes == 641u);
  REQUIRE (plan.memory_type_index == 2u && plan.coherent == VK_TRUE);

  actual[0].memoryTypeBits = 1u;
  actual[1].memoryTypeBits = 1u;

  REQUIRE (vulkan_auto_plan_shared_memory (
    &mock_memory, 256u, actual, requested, 2u,
    offsets, &plan, stdout
  ) == VK_ERROR_FEATURE_NOT_PRESENT);

  actual[0].memoryTypeBits = 2u;
  actual[1].memoryTypeBits = 4u;

  REQUIRE (vulkan_auto_plan_shared_memory (
    &mock_memory, 256u, actual, requested, 2u,
    offsets, &plan, stdout
  ) == VK_ERROR_FEATURE_NOT_PRESENT);

  actual[0].memoryTypeBits = 6u;
  actual[1].memoryTypeBits = 6u;
  actual[0].size = 383u;

  REQUIRE (vulkan_auto_plan_shared_memory (
    &mock_memory, 256u, actual, requested, 2u,
    offsets, &plan, stdout
  ) == VK_ERROR_INITIALIZATION_FAILED);

  actual[0].size = 384u;
  actual[1].alignment = 3u;

  REQUIRE (vulkan_auto_plan_shared_memory (
    &mock_memory, 256u, actual, requested, 2u,
    offsets, &plan, stdout
  ) == VK_ERROR_INITIALIZATION_FAILED);

  actual[1].alignment = 64u;

  REQUIRE (vulkan_auto_plan_shared_memory (
    &mock_memory, 0u, actual, requested, 2u,
    offsets, &plan, stdout
  ) == VK_ERROR_INITIALIZATION_FAILED);

  REQUIRE (vulkan_auto_plan_shared_memory (
    &mock_memory, 3u, actual, requested, 2u,
    offsets, &plan, stdout
  ) == VK_ERROR_INITIALIZATION_FAILED);

  REQUIRE (vulkan_auto_plan_shared_memory (
    &mock_memory, 256u, actual, requested, 0u,
    offsets, &plan, stdout
  ) == VK_ERROR_INITIALIZATION_FAILED);

  actual[0].size = UINT64_MAX - 63u;

  REQUIRE (vulkan_auto_plan_shared_memory (
    &mock_memory, 256u, actual, requested, 2u,
    offsets, &plan, stdout
  ) == VK_ERROR_OUT_OF_DEVICE_MEMORY);

  puts ("PASS_HOST_MEMORY_PLANS atomic_alignment coherent disjoint overflow");

  return 0;
}

/**
 * Verify cache synchronization uses full mapped ranges and propagates errors.
 *
 * The mock Vulkan APIs record exactly which offset, size, allocation and
 * structure type each flush/invalidate call receives.
 */
static int test_mapped_sync (void)
{
  reset_mock_device ();

  vulkan_auto_memory_plan_t plan =
  {
    .allocation_bytes = 769u,
    .memory_type_index = 1u,
    .compatible_memory_type_bits = 2u,
    .coherent = VK_TRUE,
    .non_coherent_atom_size = 256u
  };

  REQUIRE (vulkan_auto_sync_mapped_memory (
    mock_device, mock_device_memory, &plan, VK_FALSE
  ) == VK_SUCCESS);
  REQUIRE (mock_flush_calls == 0u && mock_invalidate_calls == 0u);

  plan.coherent = VK_FALSE;

  REQUIRE (vulkan_auto_sync_mapped_memory (
    mock_device, mock_device_memory, &plan, VK_FALSE
  ) == VK_SUCCESS);
  REQUIRE (mock_flush_calls == 1u && mock_invalidate_calls == 0u);
  REQUIRE (mock_last_range.sType == VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE);
  REQUIRE (mock_last_range.memory == mock_device_memory);
  REQUIRE (mock_last_range.offset == 0u);
  REQUIRE (mock_last_range.size == VK_WHOLE_SIZE);

  REQUIRE (vulkan_auto_sync_mapped_memory (
    mock_device, mock_device_memory, &plan, VK_TRUE
  ) == VK_SUCCESS);
  REQUIRE (mock_flush_calls == 1u && mock_invalidate_calls == 1u);
  REQUIRE (mock_last_range.memory == mock_device_memory);
  REQUIRE (mock_last_range.offset == 0u);
  REQUIRE (mock_last_range.size == VK_WHOLE_SIZE);

  mock_flush_result = VK_ERROR_OUT_OF_HOST_MEMORY;

  REQUIRE (vulkan_auto_sync_mapped_memory (
    mock_device, mock_device_memory, &plan, VK_FALSE
  ) == VK_ERROR_OUT_OF_HOST_MEMORY);
  REQUIRE (mock_flush_calls == 2u);

  mock_invalidate_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;

  REQUIRE (vulkan_auto_sync_mapped_memory (
    mock_device, mock_device_memory, &plan, VK_TRUE
  ) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
  REQUIRE (mock_invalidate_calls == 2u);

  REQUIRE (vulkan_auto_sync_mapped_memory (
    VK_NULL_HANDLE, mock_device_memory, &plan, VK_TRUE
  ) == VK_ERROR_INITIALIZATION_FAILED);
  REQUIRE (vulkan_auto_sync_mapped_memory (
    mock_device, VK_NULL_HANDLE, &plan, VK_FALSE
  ) == VK_ERROR_INITIALIZATION_FAILED);
  REQUIRE (vulkan_auto_sync_mapped_memory (
    mock_device, mock_device_memory, NULL, VK_FALSE
  ) == VK_ERROR_INITIALIZATION_FAILED);

  plan.allocation_bytes = 0u;

  REQUIRE (vulkan_auto_sync_mapped_memory (
    mock_device, mock_device_memory, &plan, VK_FALSE
  ) == VK_ERROR_INITIALIZATION_FAILED);

  REQUIRE (mock_flush_calls == 2u && mock_invalidate_calls == 2u);

  puts ("PASS_HOST_MAPPED_SYNC coherent flush invalidate full_range errors");

  return 0;
}

/**
 * Reject exact unsupported Vulkan 1.0 compute workloads and queue families.
 *
 * The native test uses the real C capability-checker and the synthetic
 * vkGetPhysicalDevice* query functions declared above.
 */
static int test_compute_capabilities (void)
{
  reset_mock_device ();

  const vulkan_auto_compute_requirements_t baseline =
  {
    .local_size = { 32u, 1u, 1u },
    .group_count = { 64u, 1u, 1u },
    .storage_buffer_count = 2u,
    .largest_storage_buffer_bytes = 8192u,
    .shared_memory_bytes = 128u
  };
  vulkan_auto_compute_requirements_t request = baseline;
  vulkan_auto_device_t selected = { 0 };

  REQUIRE (vulkan_auto_check_device (
    mock_physical, 0u, &request, &selected, stdout
  ) == VK_SUCCESS);
  REQUIRE (selected.physical == mock_physical);
  REQUIRE (selected.compute_queue_family == 1u);
  REQUIRE (selected.index == 0u);
  REQUIRE (selected.properties.apiVersion == VK_API_VERSION_1_0);
  REQUIRE (selected.memory_properties.memoryTypeCount == 3u);
  REQUIRE (mock_query_calls == 4u);

  request = baseline;
  request.local_size[0] = 0u;
  REQUIRE (vulkan_auto_check_device (
    mock_physical, 0u, &request, &selected, stdout
  ) == VK_ERROR_FEATURE_NOT_PRESENT);

  request = baseline;
  request.local_size[0] = 257u;
  REQUIRE (vulkan_auto_check_device (
    mock_physical, 0u, &request, &selected, stdout
  ) == VK_ERROR_FEATURE_NOT_PRESENT);

  request = baseline;
  request.local_size[0] = 256u;
  request.local_size[1] = 3u;
  REQUIRE (vulkan_auto_check_device (
    mock_physical, 0u, &request, &selected, stdout
  ) == VK_ERROR_FEATURE_NOT_PRESENT);

  request = baseline;
  request.group_count[0] = 0u;
  REQUIRE (vulkan_auto_check_device (
    mock_physical, 0u, &request, &selected, stdout
  ) == VK_ERROR_FEATURE_NOT_PRESENT);

  request = baseline;
  request.group_count[0] = 1025u;
  REQUIRE (vulkan_auto_check_device (
    mock_physical, 0u, &request, &selected, stdout
  ) == VK_ERROR_FEATURE_NOT_PRESENT);

  request = baseline;
  request.storage_buffer_count = 0u;
  REQUIRE (vulkan_auto_check_device (
    mock_physical, 0u, &request, &selected, stdout
  ) == VK_ERROR_FEATURE_NOT_PRESENT);

  request = baseline;
  request.storage_buffer_count = 5u;
  REQUIRE (vulkan_auto_check_device (
    mock_physical, 0u, &request, &selected, stdout
  ) == VK_ERROR_FEATURE_NOT_PRESENT);

  request = baseline;
  request.largest_storage_buffer_bytes = 65537u;
  REQUIRE (vulkan_auto_check_device (
    mock_physical, 0u, &request, &selected, stdout
  ) == VK_ERROR_FEATURE_NOT_PRESENT);

  request = baseline;
  request.shared_memory_bytes = 16385u;
  REQUIRE (vulkan_auto_check_device (
    mock_physical, 0u, &request, &selected, stdout
  ) == VK_ERROR_FEATURE_NOT_PRESENT);

  request = baseline;
  mock_queue_count = 0u;
  REQUIRE (vulkan_auto_check_device (
    mock_physical, 0u, &request, &selected, stdout
  ) == VK_ERROR_FEATURE_NOT_PRESENT);

  mock_queue_count = 2u;
  mock_queues[1].queueFlags = VK_QUEUE_TRANSFER_BIT;
  REQUIRE (vulkan_auto_check_device (
    mock_physical, 0u, &request, &selected, stdout
  ) == VK_ERROR_FEATURE_NOT_PRESENT);

  mock_queues[1].queueFlags = VK_QUEUE_COMPUTE_BIT;
  mock_queues[1].queueCount = 0u;
  REQUIRE (vulkan_auto_check_device (
    mock_physical, 0u, &request, &selected, stdout
  ) == VK_ERROR_FEATURE_NOT_PRESENT);

  mock_queues[1].queueCount = 1u;
  mock_properties.apiVersion = 0u;
  REQUIRE (vulkan_auto_check_device (
    mock_physical, 0u, &request, &selected, stdout
  ) == VK_ERROR_FEATURE_NOT_PRESENT);

  mock_properties.apiVersion = VK_API_VERSION_1_0;
  REQUIRE (vulkan_auto_check_device (
    VK_NULL_HANDLE, 0u, &request, &selected, stdout
  ) == VK_ERROR_INITIALIZATION_FAILED);
  REQUIRE (vulkan_auto_check_device (
    mock_physical, 0u, NULL, &selected, stdout
  ) == VK_ERROR_INITIALIZATION_FAILED);
  REQUIRE (vulkan_auto_check_device (
    mock_physical, 0u, &request, NULL, stdout
  ) == VK_ERROR_INITIALIZATION_FAILED);
  REQUIRE (vulkan_auto_check_device (
    mock_physical, 0u, &request, &selected, NULL
  ) == VK_ERROR_INITIALIZATION_FAILED);

  puts ("PASS_HOST_COMPUTE_CAPABILITIES queue limits requirements errors");

  return 0;
}

/**
 * Run native-C tests without linking or opening any Vulkan driver library.
 *
 * A test failure terminates immediately with a nonzero exit code and line.
 */
int main (int argc, char **argv)
{
  if (argc != 1)
  {
    fprintf (stderr, "Usage: %s\n", argv[0]);

    return 2;
  }

  if (test_memory_types () != 0) return 1;
  if (test_transfer_modes () != 0) return 1;
  if (test_exhaustive_memory_matrix () != 0) return 1;
  if (test_memory_plans () != 0) return 1;
  if (test_mapped_sync () != 0) return 1;
  if (test_compute_capabilities () != 0) return 1;

  puts ("PASS_HOST_POLICY all six native-C test groups, no Vulkan driver");

  return 0;
}
