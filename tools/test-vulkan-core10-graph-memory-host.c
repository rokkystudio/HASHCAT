/**
 * Test exact Vulkan memory placement for dynamic 4, 5, 6 and 7-buffer graphs.
 *
 * Exercises coherent and noncoherent HOST_VISIBLE, DEVICE_LOCAL preference,
 * heap backtracking and stable rejection of impossible memory contracts.
 * Every scenario operates on synthetic public Vulkan properties without
 * linking a Vulkan driver, GPU implementation or hashcat.
 */

#include "vulkan-core10-graph-memory.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TEST_COUNT 7u

#define TEST_ASSERT(value) \
  do \
  { \
    if (!(value)) \
    { \
      fprintf (stderr, \
        "GRAPH_HOST_TEST_FAILED line=%d assertion=%s\n", \
        __LINE__, #value); \
      return 1; \
    } \
  } while (0)

/**
 * Describe two Vulkan heaps with a coherent and a noncoherent host type.
 */
static void test_properties (
  VkPhysicalDeviceMemoryProperties *properties)
{
  memset (properties, 0, sizeof (*properties));

  properties->memoryHeapCount = 2u;
  properties->memoryTypeCount = 3u;
  properties->memoryHeaps[0].size = 64u;
  properties->memoryHeaps[1].size = 64u;

  properties->memoryTypes[0].heapIndex = 0u;
  properties->memoryTypes[0].propertyFlags =
    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

  properties->memoryTypes[1].heapIndex = 1u;
  properties->memoryTypes[1].propertyFlags =
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
    | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

  properties->memoryTypes[2].heapIndex = 1u;
  properties->memoryTypes[2].propertyFlags =
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
    | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
}

/**
 * Build one exact graph allocation request for a GPU or CPU transfer buffer.
 */
static vulkan_heap_allocation_request_t test_request (
  const char *label,
  VkDeviceSize bytes,
  vulkan_heap_buffer_role_t role,
  uint32_t compatible_bits,
  VkMemoryPropertyFlags forbidden)
{
  const vulkan_heap_allocation_request_t request =
  {
    .label = label,
    .requirements =
    {
      .size = bytes,
      .alignment = 8u,
      .memoryTypeBits = compatible_bits
    },
    .role = role,
    .forbidden_flags = forbidden
  };

  return request;
}

/**
 * Verify 4/5/6/7 resource sets all select real memory-compatible types.
 */
static int test_variable_count (void)
{
  VkPhysicalDeviceMemoryProperties properties = { 0 };

  test_properties (&properties);

  const vulkan_heap_allocation_request_t requests[TEST_COUNT] =
  {
    test_request ("input", 8u, VULKAN_HEAP_BUFFER_GPU_STORAGE, 1u, 0u),
    test_request ("a", 8u, VULKAN_HEAP_BUFFER_GPU_STORAGE, 1u, 0u),
    test_request ("b", 8u, VULKAN_HEAP_BUFFER_GPU_STORAGE, 1u, 0u),
    test_request ("c", 32u, VULKAN_HEAP_BUFFER_GPU_STORAGE, 1u, 0u),
    test_request ("extra", 8u, VULKAN_HEAP_BUFFER_GPU_STORAGE, 1u, 0u),
    test_request ("upload", 8u, VULKAN_HEAP_BUFFER_HOST_TRANSFER, 6u, 0u),
    test_request ("download", 32u, VULKAN_HEAP_BUFFER_HOST_TRANSFER, 6u, 0u)
  };

  for (uint32_t count = 4u; count <= TEST_COUNT; count++)
  {
    vulkan_heap_allocation_request_t entries[TEST_COUNT] = { 0 };
    const uint32_t gpu_count = count - 2u;

    for (uint32_t index = 0u; index < gpu_count; index++)
    {
      entries[index] = requests[index];
    }

    entries[gpu_count] = requests[5];
    entries[gpu_count + 1u] = requests[6];

    vulkan_core10_graph_memory_t plan = { 0 };
    TEST_ASSERT (vulkan_core10_graph_memory_plan (
      &properties, 7u, 0u, entries, count, &plan, stdout
    ) == VK_SUCCESS);

    TEST_ASSERT (plan.count == count);
    TEST_ASSERT (plan.reserved_per_heap[1] == 40u);

    for (uint32_t index = 0u; index < gpu_count; index++)
    {
      TEST_ASSERT (plan.memory_type_indices[index] == 0u);
      TEST_ASSERT (plan.heap_indices[index] == 0u);
    }

    for (uint32_t index = gpu_count; index < count; index++)
    {
      TEST_ASSERT (plan.memory_type_indices[index] == 1u);
      TEST_ASSERT (plan.coherent[index] == VK_TRUE);
      TEST_ASSERT (plan.heap_indices[index] == 1u);
    }

    TEST_ASSERT (plan.reserved_per_heap[0]
      == (gpu_count == 5u ? 64u : (gpu_count == 4u ? 56u
      : (VkDeviceSize) gpu_count * 8u)));

    fprintf (
      stdout,
      "PASS_GRAPH_VARIABLE count=%" PRIu32
      " gpu=%" PRIu32 " mapped=2\n",
      count, gpu_count
    );

    vulkan_core10_graph_memory_destroy (&plan);

    TEST_ASSERT (plan.count == 0u);
    TEST_ASSERT (plan.memory_type_indices == NULL);
    TEST_ASSERT (plan.heap_indices == NULL);
    TEST_ASSERT (plan.coherent == NULL);
  }

  return 0;
}

/**
 * Force the planner to backtrack when the earliest type cannot fit all data.
 */
static int test_backtracking (void)
{
  VkPhysicalDeviceMemoryProperties properties = { 0 };

  test_properties (&properties);

  properties.memoryHeaps[0].size = 48u;
  properties.memoryHeaps[1].size = 48u;
  properties.memoryTypes[1].propertyFlags |=
    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

  vulkan_heap_allocation_request_t requests[6] =
  {
    test_request ("input", 8u, VULKAN_HEAP_BUFFER_GPU_STORAGE, 3u, 0u),
    test_request ("a", 8u, VULKAN_HEAP_BUFFER_GPU_STORAGE, 1u, 0u),
    test_request ("b", 8u, VULKAN_HEAP_BUFFER_GPU_STORAGE, 1u, 0u),
    test_request ("output", 32u, VULKAN_HEAP_BUFFER_GPU_STORAGE, 1u, 0u),
    test_request ("upload", 8u, VULKAN_HEAP_BUFFER_HOST_TRANSFER, 6u, 0u),
    test_request ("download", 32u, VULKAN_HEAP_BUFFER_HOST_TRANSFER, 6u, 0u)
  };
  vulkan_core10_graph_memory_t plan = { 0 };

  TEST_ASSERT (vulkan_core10_graph_memory_plan (
    &properties, 6u, 0u, requests, 6u, &plan, stdout
  ) == VK_SUCCESS);

  TEST_ASSERT (plan.memory_type_indices[0] == 1u);
  TEST_ASSERT (plan.memory_type_indices[1] == 0u);
  TEST_ASSERT (plan.reserved_per_heap[0] == 48u);
  TEST_ASSERT (plan.reserved_per_heap[1] == 48u);

  vulkan_core10_graph_memory_destroy (&plan);
  puts ("PASS_GRAPH_BACKTRACK six_buffers_exact_heap_capacity");

  return 0;
}

/**
 * Validate actual noncoherent host requirements and detailed type rejection.
 */
static int test_noncoherent (void)
{
  VkPhysicalDeviceMemoryProperties properties = { 0 };

  test_properties (&properties);

  vulkan_heap_allocation_request_t requests[4] =
  {
    test_request ("input", 8u, VULKAN_HEAP_BUFFER_GPU_STORAGE, 1u, 0u),
    test_request ("output", 8u, VULKAN_HEAP_BUFFER_GPU_STORAGE, 1u, 0u),
    test_request ("upload", 8u, VULKAN_HEAP_BUFFER_HOST_TRANSFER,
      6u, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    test_request ("download", 8u, VULKAN_HEAP_BUFFER_HOST_TRANSFER,
      6u, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
  };
  vulkan_core10_graph_memory_t plan = { 0 };

  TEST_ASSERT (vulkan_core10_graph_memory_plan (
    &properties, 4u, 0u, requests, 4u, &plan, stdout
  ) == VK_SUCCESS);

  TEST_ASSERT (plan.memory_type_indices[2] == 2u);
  TEST_ASSERT (plan.memory_type_indices[3] == 2u);
  TEST_ASSERT (plan.coherent[2] == VK_FALSE);
  TEST_ASSERT (plan.coherent[3] == VK_FALSE);

  vulkan_core10_graph_memory_destroy (&plan);

  requests[2].requirements.memoryTypeBits = 2u;

  TEST_ASSERT (vulkan_core10_graph_memory_plan (
    &properties, 4u, 0u, requests, 4u, &plan, stdout
  ) == VK_ERROR_FEATURE_NOT_PRESENT);

  TEST_ASSERT (plan.count == 0u);
  puts ("PASS_GRAPH_NONCOHERENT valid_and_explicit_type_rejection");

  return 0;
}

/**
 * Assert that invalid requirements, allocation count and heap capacity fail.
 */
static int test_negative (void)
{
  VkPhysicalDeviceMemoryProperties properties = { 0 };

  test_properties (&properties);

  vulkan_heap_allocation_request_t requests[4] =
  {
    test_request ("input", 8u, VULKAN_HEAP_BUFFER_GPU_STORAGE, 1u, 0u),
    test_request ("output", 8u, VULKAN_HEAP_BUFFER_GPU_STORAGE, 1u, 0u),
    test_request ("upload", 8u, VULKAN_HEAP_BUFFER_HOST_TRANSFER, 6u, 0u),
    test_request ("download", 8u, VULKAN_HEAP_BUFFER_HOST_TRANSFER, 6u, 0u)
  };
  vulkan_core10_graph_memory_t plan = { 0 };

  TEST_ASSERT (vulkan_core10_graph_memory_plan (
    &properties, 4u, 1u, requests, 4u, &plan, stdout
  ) == VK_ERROR_TOO_MANY_OBJECTS);

  requests[0].requirements.alignment = 3u;
  TEST_ASSERT (vulkan_core10_graph_memory_plan (
    &properties, 4u, 0u, requests, 4u, &plan, stdout
  ) == VK_ERROR_INITIALIZATION_FAILED);
  requests[0].requirements.alignment = 8u;

  properties.memoryHeaps[0].size = 1u;
  TEST_ASSERT (vulkan_core10_graph_memory_plan (
    &properties, 4u, 0u, requests, 4u, &plan, stdout
  ) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
  properties.memoryHeaps[0].size = 64u;

  properties.memoryTypes[0].heapIndex = 2u;
  TEST_ASSERT (vulkan_core10_graph_memory_plan (
    &properties, 4u, 0u, requests, 4u, &plan, stdout
  ) == VK_ERROR_INITIALIZATION_FAILED);
  properties.memoryTypes[0].heapIndex = 0u;

  TEST_ASSERT (vulkan_core10_graph_memory_plan (
    &properties, 4u, 0u, requests, 0u, &plan, stdout
  ) == VK_ERROR_INITIALIZATION_FAILED);

  TEST_ASSERT (plan.count == 0u);
  puts ("PASS_GRAPH_NEGATIVE allocation_limit_alignment_capacity_heap_zero_count");

  return 0;
}

/**
 * Execute all dynamic-count Vulkan memory tests without the Vulkan driver.
 */
int main (void)
{
  if (test_variable_count () != 0) return 1;
  if (test_backtracking () != 0) return 1;
  if (test_noncoherent () != 0) return 1;
  if (test_negative () != 0) return 1;

  puts ("PASS_GRAPH_MEMORY_POLICY dynamic_4_5_6_7 no_vulkan_driver");

  return 0;
}
