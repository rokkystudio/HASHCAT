/**
 * Verify exact Vulkan memory heap allocation planning on synthetic devices.
 *
 * The native C test exercises alternate memory-type assignments, independent
 * buffer requirements, heap capacities, maximum allocation counts and error
 * codes. A separate exhaustive two-buffer oracle independently enumerates
 * every compatible type combination and compares planner feasibility.
 *
 * Only in-memory Vulkan property structures are used. No Vulkan driver,
 * GPU command, hashcat workload or credential-processing code is executed.
 */

#include "vulkan-memory-heap-policy.h"

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
        stderr, "FAIL_HEAP_TEST line=%d condition=%s\n", \
        __LINE__, #condition \
      ); \
      return 1; \
    } \
  } while (0)

/**
 * Construct three memory types across two synthetic Vulkan memory heaps.
 *
 * Memory type zero and one are DEVICE_LOCAL on different heaps. Type two
 * is HOST_VISIBLE|HOST_COHERENT on heap one. This structure is a synthetic
 * capability set, not a profile of any mobile GPU.
 */
static void initialize_heaps (VkPhysicalDeviceMemoryProperties *properties)
{
  memset (properties, 0, sizeof (*properties));

  properties->memoryTypeCount = 3u;
  properties->memoryHeapCount = 2u;
  properties->memoryHeaps[0].size = 8u;
  properties->memoryHeaps[1].size = 12u;

  properties->memoryTypes[0].heapIndex = 0u;
  properties->memoryTypes[0].propertyFlags =
    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

  properties->memoryTypes[1].heapIndex = 1u;
  properties->memoryTypes[1].propertyFlags =
    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

  properties->memoryTypes[2].heapIndex = 1u;
  properties->memoryTypes[2].propertyFlags =
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
    | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
}

/**
 * Create one exact buffer allocation request with a valid 1-byte alignment.
 *
 * The caller may set a different alignment to test input validation.
 */
static vulkan_heap_allocation_request_t request_buffer (
  const char *label,
  VkDeviceSize bytes,
  uint32_t memory_bits,
  vulkan_heap_buffer_role_t role)
{
  const vulkan_heap_allocation_request_t request =
  {
    .label = label,
    .requirements =
    {
      .size = bytes,
      .alignment = 1u,
      .memoryTypeBits = memory_bits
    },
    .role = role,
    .forbidden_flags = 0u
  };

  return request;
}

/**
 * Require the planner to reconsider an earlier valid but capacity-exhausting
 * memory type when a later buffer only permits the first device-local heap.
 */
static int test_full_assignment_backtracking (void)
{
  VkPhysicalDeviceMemoryProperties properties;

  initialize_heaps (&properties);

  vulkan_heap_allocation_request_t requests[2] =
  {
    request_buffer (
      "general-gpu", 8u, 0x3u, VULKAN_HEAP_BUFFER_GPU_STORAGE
    ),
    request_buffer (
      "restricted-gpu", 8u, 0x1u, VULKAN_HEAP_BUFFER_GPU_STORAGE
    )
  };
  vulkan_heap_allocation_plan_t plan = { 0 };

  REQUIRE (vulkan_heap_plan_allocations (
    &properties, 4u, 0u, requests, 2u, &plan, stdout
  ) == VK_SUCCESS);

  REQUIRE (plan.buffer_count == 2u);
  REQUIRE (plan.memory_type_indices[0] == 1u);
  REQUIRE (plan.heap_indices[0] == 1u);
  REQUIRE (plan.memory_type_indices[1] == 0u);
  REQUIRE (plan.heap_indices[1] == 0u);
  REQUIRE (plan.reserved_per_heap[0] == 8u);
  REQUIRE (plan.reserved_per_heap[1] == 8u);

  puts ("PASS_HEAP_BACKTRACK alternate_type_prevents_false_incompatibility");

  return 0;
}

/**
 * Assign two GPU buffers and two host-transfer buffers to their compatible
 * heaps while preserving independent VkMemoryRequirements for each buffer.
 */
static int test_four_buffer_staging (void)
{
  VkPhysicalDeviceMemoryProperties properties;

  initialize_heaps (&properties);

  properties.memoryHeaps[0].size = 16u;
  properties.memoryHeaps[1].size = 10u;
  properties.memoryTypes[1].propertyFlags =
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
    | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  properties.memoryTypes[2].propertyFlags =
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
    | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;

  vulkan_heap_allocation_request_t requests[4] =
  {
    request_buffer (
      "gpu-upload-target", 8u, 0x1u, VULKAN_HEAP_BUFFER_GPU_STORAGE
    ),
    request_buffer (
      "gpu-download-source", 8u, 0x1u, VULKAN_HEAP_BUFFER_GPU_STORAGE
    ),
    request_buffer (
      "cpu-upload", 5u, 0x6u, VULKAN_HEAP_BUFFER_HOST_TRANSFER
    ),
    request_buffer (
      "cpu-download", 5u, 0x6u, VULKAN_HEAP_BUFFER_HOST_TRANSFER
    )
  };
  vulkan_heap_allocation_plan_t plan = { 0 };

  REQUIRE (vulkan_heap_plan_allocations (
    &properties, 4u, 0u, requests, 4u, &plan, stdout
  ) == VK_SUCCESS);

  REQUIRE (plan.buffer_count == 4u);
  REQUIRE (plan.memory_type_indices[0] == 0u);
  REQUIRE (plan.memory_type_indices[1] == 0u);
  REQUIRE (plan.memory_type_indices[2] == 1u);
  REQUIRE (plan.memory_type_indices[3] == 1u);
  REQUIRE (plan.reserved_per_heap[0] == 16u);
  REQUIRE (plan.reserved_per_heap[1] == 10u);
  REQUIRE (plan.coherent[2] == VK_TRUE);
  REQUIRE (plan.coherent[3] == VK_TRUE);

  puts ("PASS_HEAP_FOUR_BUFFERS two_gpu_two_staging_exact_heap_sizes");

  return 0;
}

/**
 * Search an alternate noncoherent host-visible type when a coherent heap
 * has insufficient remaining capacity for the requested staging buffer.
 */
static int test_noncoherent_host_fallback (void)
{
  VkPhysicalDeviceMemoryProperties properties;

  initialize_heaps (&properties);

  properties.memoryHeaps[0].size = 8u;
  properties.memoryHeaps[1].size = 8u;

  properties.memoryTypes[0].propertyFlags =
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
    | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  properties.memoryTypes[1].propertyFlags =
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
    | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
  properties.memoryTypes[2].propertyFlags =
    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

  properties.memoryTypes[0].heapIndex = 0u;
  properties.memoryTypes[1].heapIndex = 1u;
  properties.memoryTypes[2].heapIndex = 0u;

  vulkan_heap_allocation_request_t requests[2] =
  {
    request_buffer (
      "coherent-only", 8u, 0x1u, VULKAN_HEAP_BUFFER_HOST_TRANSFER
    ),
    request_buffer (
      "coherent-or-noncoherent", 8u, 0x3u,
      VULKAN_HEAP_BUFFER_HOST_TRANSFER
    )
  };
  vulkan_heap_allocation_plan_t plan = { 0 };

  REQUIRE (vulkan_heap_plan_allocations (
    &properties, 4u, 0u, requests, 2u, &plan, stdout
  ) == VK_SUCCESS);

  REQUIRE (plan.memory_type_indices[0] == 0u);
  REQUIRE (plan.memory_type_indices[1] == 1u);
  REQUIRE (plan.coherent[0] == VK_TRUE);
  REQUIRE (plan.coherent[1] == VK_FALSE);
  REQUIRE (plan.reserved_per_heap[0] == 8u);
  REQUIRE (plan.reserved_per_heap[1] == 8u);

  puts ("PASS_HEAP_NONCOHERENT_FALLBACK explicit_capacity_and_cache_flags");

  return 0;
}

/**
 * Test exact Vulkan capacity errors, allocation count, invalid heaps and
 * invalid buffer requirements without mutating the caller's output on error.
 */
static int test_failure_outcomes (void)
{
  VkPhysicalDeviceMemoryProperties properties;

  initialize_heaps (&properties);

  vulkan_heap_allocation_request_t requests[2] =
  {
    request_buffer (
      "buffer-a", 8u, 0x1u, VULKAN_HEAP_BUFFER_GPU_STORAGE
    ),
    request_buffer (
      "buffer-b", 8u, 0x1u, VULKAN_HEAP_BUFFER_GPU_STORAGE
    )
  };

  vulkan_heap_allocation_plan_t original = { 0 };
  vulkan_heap_allocation_plan_t plan = { 0 };

  plan.buffer_count = UINT32_C (0xaabbccdd);
  plan.memory_type_indices[0] = UINT32_C (0x12345678);

  original = plan;

  REQUIRE (vulkan_heap_plan_allocations (
    &properties, 4u, 0u, requests, 2u, &plan, stdout
  ) == VK_ERROR_OUT_OF_DEVICE_MEMORY);
  REQUIRE (memcmp (&plan, &original, sizeof (plan)) == 0);

  requests[1].requirements.memoryTypeBits = 0u;

  REQUIRE (vulkan_heap_plan_allocations (
    &properties, 4u, 0u, requests, 2u, &plan, stdout
  ) == VK_ERROR_FEATURE_NOT_PRESENT);
  REQUIRE (memcmp (&plan, &original, sizeof (plan)) == 0);

  requests[1].requirements.memoryTypeBits = 1u;
  requests[1].role = VULKAN_HEAP_BUFFER_HOST_TRANSFER;

  REQUIRE (vulkan_heap_plan_allocations (
    &properties, 4u, 0u, requests, 2u, &plan, stdout
  ) == VK_ERROR_FEATURE_NOT_PRESENT);

  requests[1].role = VULKAN_HEAP_BUFFER_GPU_STORAGE;
  requests[1].forbidden_flags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

  REQUIRE (vulkan_heap_plan_allocations (
    &properties, 4u, 0u, requests, 2u, &plan, stdout
  ) == VK_ERROR_FEATURE_NOT_PRESENT);

  requests[1].forbidden_flags = 0u;

  REQUIRE (vulkan_heap_plan_allocations (
    &properties, 1u, 0u, requests, 2u, &plan, stdout
  ) == VK_ERROR_TOO_MANY_OBJECTS);

  REQUIRE (vulkan_heap_plan_allocations (
    &properties, 4u, 3u, requests, 2u, &plan, stdout
  ) == VK_ERROR_TOO_MANY_OBJECTS);

  REQUIRE (vulkan_heap_plan_allocations (
    &properties, 4u, UINT32_MAX, requests, 2u, &plan, stdout
  ) == VK_ERROR_TOO_MANY_OBJECTS);

  properties.memoryTypes[2].heapIndex = 2u;

  REQUIRE (vulkan_heap_plan_allocations (
    &properties, 4u, 0u, requests, 2u, &plan, stdout
  ) == VK_ERROR_INITIALIZATION_FAILED);

  properties.memoryTypes[2].heapIndex = 1u;
  requests[0].requirements.alignment = 3u;

  REQUIRE (vulkan_heap_plan_allocations (
    &properties, 4u, 0u, requests, 2u, &plan, stdout
  ) == VK_ERROR_INITIALIZATION_FAILED);

  requests[0].requirements.alignment = 1u;
  requests[0].label = NULL;

  REQUIRE (vulkan_heap_plan_allocations (
    &properties, 4u, 0u, requests, 2u, &plan, stdout
  ) == VK_ERROR_INITIALIZATION_FAILED);

  requests[0].label = "buffer-a";
  requests[0].requirements.size = 0u;

  REQUIRE (vulkan_heap_plan_allocations (
    &properties, 4u, 0u, requests, 2u, &plan, stdout
  ) == VK_ERROR_INITIALIZATION_FAILED);

  requests[0].requirements.size = 8u;
  requests[0].role = (vulkan_heap_buffer_role_t) 7u;

  REQUIRE (vulkan_heap_plan_allocations (
    &properties, 4u, 0u, requests, 2u, &plan, stdout
  ) == VK_ERROR_INITIALIZATION_FAILED);

  requests[0].role = VULKAN_HEAP_BUFFER_GPU_STORAGE;
  properties.memoryHeapCount = 0u;

  REQUIRE (vulkan_heap_plan_allocations (
    &properties, 4u, 0u, requests, 2u, &plan, stdout
  ) == VK_ERROR_INITIALIZATION_FAILED);

  properties.memoryHeapCount = 2u;

  REQUIRE (vulkan_heap_plan_allocations (
    &properties, 4u, 0u, NULL, 2u, &plan, stdout
  ) == VK_ERROR_INITIALIZATION_FAILED);
  REQUIRE (vulkan_heap_plan_allocations (
    &properties, 4u, 0u, requests, 0u, &plan, stdout
  ) == VK_ERROR_INITIALIZATION_FAILED);
  REQUIRE (vulkan_heap_plan_allocations (
    &properties, 4u, 0u, requests, 5u, &plan, stdout
  ) == VK_ERROR_INITIALIZATION_FAILED);
  REQUIRE (vulkan_heap_plan_allocations (
    &properties, 4u, 0u, requests, 2u, NULL, stdout
  ) == VK_ERROR_INITIALIZATION_FAILED);
  REQUIRE (vulkan_heap_plan_allocations (
    &properties, 4u, 0u, requests, 2u, &plan, NULL
  ) == VK_ERROR_INITIALIZATION_FAILED);

  REQUIRE (memcmp (&plan, &original, sizeof (plan)) == 0);

  puts ("PASS_HEAP_FAILURES capacity masks counts invalid_input untouched");

  return 0;
}

/**
 * Validate the maximum Vulkan memory-type index and an allocation exceeding
 * a 64-bit heap total even when the individual requirements appear valid.
 */
static int test_type_31_and_large_allocations (void)
{
  VkPhysicalDeviceMemoryProperties properties;

  initialize_heaps (&properties);

  properties.memoryTypeCount = 32u;
  properties.memoryTypes[31].heapIndex = 0u;
  properties.memoryTypes[31].propertyFlags =
    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

  vulkan_heap_allocation_request_t requests[2] =
  {
    request_buffer (
      "last-valid-memory-type", 8u, UINT32_C (0x80000000),
      VULKAN_HEAP_BUFFER_GPU_STORAGE
    ),
    request_buffer (
      "other-type", 8u, 0x2u, VULKAN_HEAP_BUFFER_GPU_STORAGE
    )
  };
  vulkan_heap_allocation_plan_t plan = { 0 };

  REQUIRE (vulkan_heap_plan_allocations (
    &properties, 4u, 0u, requests, 2u, &plan, stdout
  ) == VK_SUCCESS);

  REQUIRE (plan.memory_type_indices[0] == 31u);
  REQUIRE (plan.memory_type_indices[1] == 1u);
  REQUIRE (plan.heap_indices[0] == 0u);
  REQUIRE (plan.heap_indices[1] == 1u);

  properties.memoryTypeCount = 3u;
  properties.memoryHeaps[0].size = UINT64_MAX;
  properties.memoryHeaps[1].size = 1u;

  requests[0] = request_buffer (
    "large-first-buffer", UINT64_MAX - 100u, 1u,
    VULKAN_HEAP_BUFFER_GPU_STORAGE
  );
  requests[1] = request_buffer (
    "second-buffer", 200u, 1u,
    VULKAN_HEAP_BUFFER_GPU_STORAGE
  );

  REQUIRE (vulkan_heap_plan_allocations (
    &properties, 4u, 0u, requests, 2u, &plan, stdout
  ) == VK_ERROR_OUT_OF_DEVICE_MEMORY);

  puts ("PASS_HEAP_BOUNDARIES bit31 unsigned64_capacity");

  return 0;
}

/**
 * Decide feasibility independently by enumerating every concrete type pair.
 *
 * This oracle ignores the planner's selection order and checks only whether
 * the two resources can both be placed on their compatible declared heaps.
 * The returned flag is true precisely when a capacity-valid assignment exists.
 */
static VkBool32 brute_force_two_buffers (
  const VkPhysicalDeviceMemoryProperties *properties,
  const vulkan_heap_allocation_request_t *requests,
  VkBool32 *any_candidate)
{
  VkBool32 found[2] = { VK_FALSE, VK_FALSE };

  for (uint32_t request = 0u; request < 2u; request++)
  {
    for (uint32_t type = 0u; type < properties->memoryTypeCount; type++)
    {
      if ((requests[request].requirements.memoryTypeBits
        & (1u << type)) == 0u)
      {
        continue;
      }

      const VkMemoryPropertyFlags flags =
        properties->memoryTypes[type].propertyFlags;

      if (requests[request].role == VULKAN_HEAP_BUFFER_HOST_TRANSFER
       && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0u)
      {
        continue;
      }

      found[request] = VK_TRUE;
    }
  }

  *any_candidate = found[0] == VK_TRUE && found[1] == VK_TRUE
    ? VK_TRUE : VK_FALSE;

  if (*any_candidate == VK_FALSE) return VK_FALSE;

  for (uint32_t first = 0u; first < properties->memoryTypeCount; first++)
  {
    if ((requests[0].requirements.memoryTypeBits & (1u << first)) == 0u)
    {
      continue;
    }

    if (requests[0].role == VULKAN_HEAP_BUFFER_HOST_TRANSFER
     && (properties->memoryTypes[first].propertyFlags
      & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0u)
    {
      continue;
    }

    for (uint32_t second = 0u;
         second < properties->memoryTypeCount; second++)
    {
      if ((requests[1].requirements.memoryTypeBits & (1u << second)) == 0u)
      {
        continue;
      }

      if (requests[1].role == VULKAN_HEAP_BUFFER_HOST_TRANSFER
       && (properties->memoryTypes[second].propertyFlags
        & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0u)
      {
        continue;
      }

      const uint32_t heap_a = properties->memoryTypes[first].heapIndex;
      const uint32_t heap_b = properties->memoryTypes[second].heapIndex;
      const VkDeviceSize size_a = requests[0].requirements.size;
      const VkDeviceSize size_b = requests[1].requirements.size;
      const VkDeviceSize capacity_a = properties->memoryHeaps[heap_a].size;
      const VkDeviceSize capacity_b = properties->memoryHeaps[heap_b].size;

      if (size_a > capacity_a || size_b > capacity_b) continue;

      if (heap_a == heap_b && size_b > capacity_a - size_a)
      {
        continue;
      }

      return VK_TRUE;
    }
  }

  return VK_FALSE;
}

/**
 * Cross-check 200,704 synthetic heap/type/role combinations with an
 * independent brute-force two-buffer feasibility oracle.
 *
 * The matrix enumerates four valid memory-property classes for each of
 * three Vulkan memory types (64 arrangements), two possible heaps for each
 * type (eight arrangements), seven masks for both buffers, four combinations
 * of GPU-only/host-visible roles, and two request-size pairs.
 */
static int test_exhaustive_heap_matrix (void)
{
  static const VkMemoryPropertyFlags classes[4] =
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

  VkPhysicalDeviceMemoryProperties properties;

  initialize_heaps (&properties);

  FILE *diagnostics = tmpfile ();

  REQUIRE (diagnostics != NULL);

  uint32_t total = 0u;
  uint32_t accepted = 0u;
  uint32_t unsupported = 0u;
  uint32_t insufficient_capacity = 0u;

  for (uint32_t layout = 0u; layout < 64u; layout++)
  {
    uint32_t value = layout;

    for (uint32_t type = 0u; type < 3u; type++)
    {
      properties.memoryTypes[type].propertyFlags =
        classes[value % 4u];

      value /= 4u;
    }

    for (uint32_t heap_bits = 0u; heap_bits < 8u; heap_bits++)
    {
      for (uint32_t type = 0u; type < 3u; type++)
      {
        properties.memoryTypes[type].heapIndex =
          (heap_bits >> type) & 1u;
      }

      for (uint32_t mask_a = 1u; mask_a < 8u; mask_a++)
      {
        for (uint32_t mask_b = 1u; mask_b < 8u; mask_b++)
        {
          for (uint32_t role_pair = 0u; role_pair < 4u; role_pair++)
          {
            for (uint32_t profile = 0u; profile < 2u; profile++)
            {
              vulkan_heap_allocation_request_t requests[2] =
              {
                request_buffer (
                  "first", profile == 0u ? 8u : 12u, mask_a,
                  (role_pair & 1u) != 0u
                    ? VULKAN_HEAP_BUFFER_HOST_TRANSFER
                    : VULKAN_HEAP_BUFFER_GPU_STORAGE
                ),
                request_buffer (
                  "second", profile == 0u ? 8u : 4u, mask_b,
                  (role_pair & 2u) != 0u
                    ? VULKAN_HEAP_BUFFER_HOST_TRANSFER
                    : VULKAN_HEAP_BUFFER_GPU_STORAGE
                )
              };

              VkBool32 any_candidate = VK_FALSE;

              const VkBool32 feasible = brute_force_two_buffers (
                &properties, requests, &any_candidate
              );

              vulkan_heap_allocation_plan_t plan = { 0 };

              const VkResult actual = vulkan_heap_plan_allocations (
                &properties, 4u, 0u, requests, 2u, &plan, diagnostics
              );

              if (feasible == VK_TRUE)
              {
                REQUIRE (actual == VK_SUCCESS);

                VkDeviceSize expected_reserved[2] = { 0u, 0u };

                for (uint32_t index = 0u; index < 2u; index++)
                {
                  const uint32_t type = plan.memory_type_indices[index];

                  REQUIRE (type < properties.memoryTypeCount);
                  REQUIRE ((requests[index].requirements.memoryTypeBits
                    & (1u << type)) != 0u);

                  const VkMemoryType *selected =
                    &properties.memoryTypes[type];

                  if (requests[index].role
                    == VULKAN_HEAP_BUFFER_HOST_TRANSFER)
                  {
                    REQUIRE ((selected->propertyFlags
                      & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0u);
                  }

                  REQUIRE (plan.heap_indices[index] == selected->heapIndex);

                  expected_reserved[selected->heapIndex] +=
                    requests[index].requirements.size;
                }

                REQUIRE (plan.buffer_count == 2u);

                for (uint32_t heap = 0u; heap < 2u; heap++)
                {
                  REQUIRE (plan.reserved_per_heap[heap]
                    == expected_reserved[heap]);
                  REQUIRE (plan.reserved_per_heap[heap]
                    <= properties.memoryHeaps[heap].size);
                }

                accepted++;
              }
              else if (any_candidate == VK_FALSE)
              {
                REQUIRE (actual == VK_ERROR_FEATURE_NOT_PRESENT);

                unsupported++;
              }
              else
              {
                REQUIRE (actual == VK_ERROR_OUT_OF_DEVICE_MEMORY);

                insufficient_capacity++;
              }

              total++;
            }
          }
        }
      }
    }
  }

  REQUIRE (fclose (diagnostics) == 0);
  REQUIRE (total == 200704u);
  REQUIRE (total == accepted + unsupported + insufficient_capacity);
  REQUIRE (accepted > 0u);
  REQUIRE (unsupported > 0u);
  REQUIRE (insufficient_capacity > 0u);

  printf (
    "PASS_HEAP_EXHAUSTIVE layouts=64 heap_maps=8 masks=7"
    " roles=4 sizes=2 cases=%" PRIu32
    " accepted=%" PRIu32 " unsupported=%" PRIu32
    " capacity=%" PRIu32 "\n",
    total, accepted, unsupported, insufficient_capacity
  );

  return 0;
}

/**
 * Run deterministic and exhaustive Vulkan heap-placement tests on the CPU.
 *
 * Failing assumptions report the original C source line and return status 1.
 * No Vulkan loader, Android device, queue or GPU allocation is used.
 */
int main (int argc, char **argv)
{
  if (argc != 1)
  {
    fprintf (stderr, "Usage: %s\n", argv[0]);

    return 2;
  }

  if (test_full_assignment_backtracking () != 0) return 1;
  if (test_four_buffer_staging () != 0) return 1;
  if (test_noncoherent_host_fallback () != 0) return 1;
  if (test_failure_outcomes () != 0) return 1;
  if (test_type_31_and_large_allocations () != 0) return 1;
  if (test_exhaustive_heap_matrix () != 0) return 1;

  puts ("PASS_HEAP_POLICY all six groups without Vulkan driver");

  return 0;
}
