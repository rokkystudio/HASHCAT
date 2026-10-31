/**
 * Exercise the exact six-buffer Vulkan 1.0 heap planner without a GPU.
 *
 * Tests driver-compatible DEVICE_LOCAL/HOST_VISIBLE assignments, nested
 * heap backtracking, noncoherent memory, allocation limits, invalid buffer
 * requirements and failure paths with an untouched result object.
 *
 * The test uses only synthetic public memory-property data. It does not
 * access Vulkan drivers or operate on cryptographic inputs.
 */

#include "vulkan-multistage-memory-plan.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define REQUIRE(condition) \
  do \
  { \
    if (!(condition)) \
    { \
      fprintf (stderr, "REJECT_TEST line=%d check=%s\n", \
        __LINE__, #condition); \
      return 1; \
    } \
  } while (0)

/**
 * Construct six realistic GPU/staging requests with exact synthetic sizes.
 */
static void initialize_requests (
  vulkan_heap_allocation_request_t requests[VULKAN_MULTISTAGE_BUFFER_COUNT])
{
  static const char *names[VULKAN_MULTISTAGE_BUFFER_COUNT] =
  {
    "gpu-input", "stage-a", "stage-b", "gpu-output",
    "staging-upload", "staging-download"
  };
  static const VkDeviceSize sizes[VULKAN_MULTISTAGE_BUFFER_COUNT] =
  {
    8u, 8u, 8u, 32u, 8u, 32u
  };

  memset (
    requests, 0,
    sizeof (*requests) * VULKAN_MULTISTAGE_BUFFER_COUNT
  );

  for (uint32_t index = 0u;
       index < VULKAN_MULTISTAGE_BUFFER_COUNT; index++)
  {
    requests[index].label = names[index];
    requests[index].role = index < 4u
      ? VULKAN_HEAP_BUFFER_GPU_STORAGE
      : VULKAN_HEAP_BUFFER_HOST_TRANSFER;
    requests[index].requirements.size = sizes[index];
    requests[index].requirements.alignment = 8u;
    requests[index].requirements.memoryTypeBits =
      index < 4u ? UINT32_C (0x7) : UINT32_C (0x6);
    requests[index].forbidden_flags =
      VK_MEMORY_PROPERTY_PROTECTED_BIT
      | VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD;
  }
}

/**
 * Set up two artificial Vulkan heaps with coherent/noncoherent host memory.
 */
static void initialize_properties (
  VkPhysicalDeviceMemoryProperties *properties)
{
  memset (properties, 0, sizeof (*properties));

  properties->memoryHeapCount = 2u;
  properties->memoryTypeCount = 3u;
  properties->memoryHeaps[0].size = 56u;
  properties->memoryHeaps[1].size = 40u;

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
 * Verify six Vulkan-compatible allocations and per-heap exact reservations.
 */
static int test_six_real_roles (void)
{
  VkPhysicalDeviceMemoryProperties memory = { 0 };
  vulkan_heap_allocation_request_t requests[VULKAN_MULTISTAGE_BUFFER_COUNT];
  vulkan_multistage_memory_plan_t plan = { 0 };

  initialize_properties (&memory);
  initialize_requests (requests);

  REQUIRE (vulkan_multistage_plan_memory (
    &memory, 6u, requests, &plan, stdout
  ) == VK_SUCCESS);

  for (uint32_t index = 0u; index < 4u; index++)
  {
    REQUIRE (plan.memory_type_indices[index] == 0u);
    REQUIRE (plan.heap_indices[index] == 0u);
  }

  REQUIRE (plan.memory_type_indices[4] == 1u);
  REQUIRE (plan.memory_type_indices[5] == 1u);
  REQUIRE (plan.coherent[4] == VK_TRUE);
  REQUIRE (plan.coherent[5] == VK_TRUE);
  REQUIRE (plan.reserved_per_heap[0] == 56u);
  REQUIRE (plan.reserved_per_heap[1] == 40u);

  puts ("PASS_MULTISTAGE_SIX all_resources capacity_and_roles");

  return 0;
}

/**
 * Verify the planner explores alternate heaps before rejecting later writes.
 */
static int test_backtracking (void)
{
  VkPhysicalDeviceMemoryProperties memory = { 0 };
  vulkan_heap_allocation_request_t requests[VULKAN_MULTISTAGE_BUFFER_COUNT];
  vulkan_multistage_memory_plan_t plan = { 0 };

  initialize_properties (&memory);
  initialize_requests (requests);

  memory.memoryHeaps[0].size = 48u;
  memory.memoryHeaps[1].size = 48u;
  memory.memoryTypes[1].propertyFlags |=
    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

  requests[0].requirements.memoryTypeBits = UINT32_C (0x3);
  requests[1].requirements.memoryTypeBits = UINT32_C (0x1);
  requests[2].requirements.memoryTypeBits = UINT32_C (0x1);
  requests[3].requirements.memoryTypeBits = UINT32_C (0x1);

  REQUIRE (vulkan_multistage_plan_memory (
    &memory, 6u, requests, &plan, stdout
  ) == VK_SUCCESS);

  REQUIRE (plan.memory_type_indices[0] == 1u);
  REQUIRE (plan.memory_type_indices[1] == 0u);
  REQUIRE (plan.reserved_per_heap[0] == 48u);
  REQUIRE (plan.reserved_per_heap[1] == 48u);

  puts ("PASS_MULTISTAGE_BACKTRACK six_requests_exact_placement");

  return 0;
}

/**
 * Ensure the real noncoherent role works or fails by the compatibility bits.
 */
static int test_noncoherent_and_rejection (void)
{
  VkPhysicalDeviceMemoryProperties memory = { 0 };
  vulkan_heap_allocation_request_t requests[VULKAN_MULTISTAGE_BUFFER_COUNT];
  vulkan_multistage_memory_plan_t plan = { 0 };

  initialize_properties (&memory);
  initialize_requests (requests);

  requests[4].forbidden_flags |= VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  requests[5].forbidden_flags |= VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

  REQUIRE (vulkan_multistage_plan_memory (
    &memory, 6u, requests, &plan, stdout
  ) == VK_SUCCESS);

  REQUIRE (plan.memory_type_indices[4] == 2u);
  REQUIRE (plan.memory_type_indices[5] == 2u);
  REQUIRE (plan.coherent[4] == VK_FALSE);
  REQUIRE (plan.coherent[5] == VK_FALSE);

  requests[4].requirements.memoryTypeBits = UINT32_C (0x2);
  plan.memory_type_indices[4] = UINT32_C (0xaabbccdd);

  REQUIRE (vulkan_multistage_plan_memory (
    &memory, 6u, requests, &plan, stdout
  ) == VK_ERROR_FEATURE_NOT_PRESENT);

  REQUIRE (plan.memory_type_indices[4] == UINT32_C (0xaabbccdd));

  puts ("PASS_MULTISTAGE_NONCOHERENT compatible_and_explicit_rejection");

  return 0;
}

/**
 * Verify unsupported requirements produce precise errors without mutations.
 */
static int test_invalid_requirements (void)
{
  VkPhysicalDeviceMemoryProperties memory = { 0 };
  vulkan_heap_allocation_request_t requests[VULKAN_MULTISTAGE_BUFFER_COUNT];
  vulkan_multistage_memory_plan_t plan = { 0 };

  initialize_properties (&memory);
  initialize_requests (requests);

  plan.memory_type_indices[0] = UINT32_C (0x11223344);

  REQUIRE (vulkan_multistage_plan_memory (
    &memory, 5u, requests, &plan, stdout
  ) == VK_ERROR_TOO_MANY_OBJECTS);

  REQUIRE (plan.memory_type_indices[0] == UINT32_C (0x11223344));

  requests[0].requirements.alignment = 0u;
  REQUIRE (vulkan_multistage_plan_memory (
    &memory, 6u, requests, &plan, stdout
  ) == VK_ERROR_INITIALIZATION_FAILED);
  requests[0].requirements.alignment = 8u;

  memory.memoryTypes[0].heapIndex = 2u;
  REQUIRE (vulkan_multistage_plan_memory (
    &memory, 6u, requests, &plan, stdout
  ) == VK_ERROR_INITIALIZATION_FAILED);
  memory.memoryTypes[0].heapIndex = 0u;

  memory.memoryHeaps[0].size = 1u;
  memory.memoryHeaps[1].size = 1u;
  REQUIRE (vulkan_multistage_plan_memory (
    &memory, 6u, requests, &plan, stdout
  ) == VK_ERROR_OUT_OF_DEVICE_MEMORY);

  REQUIRE (plan.memory_type_indices[0] == UINT32_C (0x11223344));

  puts ("PASS_MULTISTAGE_ERRORS count alignment heap capacity untouched");

  return 0;
}

/**
 * Execute all six-buffer Vulkan memory policy tests without a GPU driver.
 */
int main (void)
{
  if (test_six_real_roles () != 0) return 1;
  if (test_backtracking () != 0) return 1;
  if (test_noncoherent_and_rejection () != 0) return 1;
  if (test_invalid_requirements () != 0) return 1;

  puts ("PASS_MULTISTAGE_MEMORY_POLICY no_vulkan_driver");

  return 0;
}
