/**
 * Select six synthetic Vulkan 1.0 Compute/staging buffer memory types.
 *
 * Requests are jointly assigned to Vulkan-reported heaps using a complete
 * backtracking search. Real memoryTypeBits, role requirements, forbidden
 * flags and declared heap capacities determine valid assignments. No
 * device-model profile, guessed memory index or fallback is used.
 */

#include "vulkan-multistage-memory-plan.h"

#include <inttypes.h>
#include <stdint.h>
#include <string.h>

/**
 * Check the exact type compatibility required by a Vulkan buffer.
 */
static VkBool32 multistage_type_compatible (
  const VkMemoryType *type,
  const vulkan_heap_allocation_request_t *request)
{
  const VkMemoryPropertyFlags flags = type->propertyFlags;

  if ((flags & request->forbidden_flags) != 0u) return VK_FALSE;

  if (request->role == VULKAN_HEAP_BUFFER_HOST_TRANSFER
   && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0u)
  {
    return VK_FALSE;
  }

  return VK_TRUE;
}

/**
 * Order memory types by declared buffer role, never by manufacturer.
 */
static VkBool32 multistage_type_preferred (
  const VkMemoryType *type,
  vulkan_heap_buffer_role_t role)
{
  const VkMemoryPropertyFlags wanted =
    role == VULKAN_HEAP_BUFFER_HOST_TRANSFER
    ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

  return (type->propertyFlags & wanted) != 0u ? VK_TRUE : VK_FALSE;
}

/**
 * Search every feasible six-buffer allocation assignment recursively.
 *
 * Every branch reserves only the actual requirement size against that
 * memory type's heap. Backtracking restores the preceding reservation.
 */
static VkBool32 multistage_assign (
  const VkPhysicalDeviceMemoryProperties *properties,
  const vulkan_heap_allocation_request_t requests[VULKAN_MULTISTAGE_BUFFER_COUNT],
  uint32_t depth,
  vulkan_multistage_memory_plan_t *plan)
{
  if (depth == VULKAN_MULTISTAGE_BUFFER_COUNT) return VK_TRUE;

  const vulkan_heap_allocation_request_t *request = &requests[depth];
  const VkDeviceSize size = request->requirements.size;

  for (uint32_t preference = 0u; preference < 2u; preference++)
  {
    for (uint32_t index = 0u;
         index < properties->memoryTypeCount; index++)
    {
      if ((request->requirements.memoryTypeBits
        & (UINT32_C (1) << index)) == 0u)
      {
        continue;
      }

      const VkMemoryType *type = &properties->memoryTypes[index];

      if (multistage_type_compatible (type, request) == VK_FALSE)
      {
        continue;
      }

      const VkBool32 preferred = multistage_type_preferred (
        type, request->role
      );

      if (preferred != (preference == 0u ? VK_TRUE : VK_FALSE))
      {
        continue;
      }

      const uint32_t heap = type->heapIndex;
      const VkDeviceSize capacity = properties->memoryHeaps[heap].size;
      const VkDeviceSize reserved = plan->reserved_per_heap[heap];

      if (reserved > capacity || size > capacity - reserved)
      {
        continue;
      }

      plan->memory_type_indices[depth] = index;
      plan->heap_indices[depth] = heap;
      plan->coherent[depth] =
        (type->propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0u
        ? VK_TRUE : VK_FALSE;
      plan->reserved_per_heap[heap] = reserved + size;

      if (multistage_assign (
        properties, requests, depth + 1u, plan
      ) == VK_TRUE)
      {
        return VK_TRUE;
      }

      plan->reserved_per_heap[heap] = reserved;
      plan->memory_type_indices[depth] = UINT32_MAX;
      plan->heap_indices[depth] = UINT32_MAX;
      plan->coherent[depth] = VK_FALSE;
    }
  }

  return VK_FALSE;
}

/**
 * Validate six Vulkan allocation requirements and compute exact placement.
 *
 * The destination is assigned only after every buffer has a type and all
 * reservations fit driver-declared heap sizes. Incompatible requests and
 * insufficient capacities produce separate, documented Vulkan errors.
 */
VkResult vulkan_multistage_plan_memory (
  const VkPhysicalDeviceMemoryProperties *properties,
  uint32_t max_memory_allocations,
  const vulkan_heap_allocation_request_t requests[VULKAN_MULTISTAGE_BUFFER_COUNT],
  vulkan_multistage_memory_plan_t *result,
  FILE *diagnostics)
{
  if (properties == NULL || requests == NULL || result == NULL
   || diagnostics == NULL
   || properties->memoryTypeCount == 0u
   || properties->memoryTypeCount > VK_MAX_MEMORY_TYPES
   || properties->memoryHeapCount == 0u
   || properties->memoryHeapCount > VK_MAX_MEMORY_HEAPS)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  if (max_memory_allocations < VULKAN_MULTISTAGE_BUFFER_COUNT)
  {
    fprintf (
      diagnostics,
      "REJECT_MULTISTAGE_ALLOCATION_COUNT requested=%u limit=%" PRIu32 "\n",
      VULKAN_MULTISTAGE_BUFFER_COUNT, max_memory_allocations
    );

    return VK_ERROR_TOO_MANY_OBJECTS;
  }

  for (uint32_t index = 0u; index < properties->memoryTypeCount; index++)
  {
    if (properties->memoryTypes[index].heapIndex
      >= properties->memoryHeapCount)
    {
      fprintf (
        diagnostics,
        "REJECT_MULTISTAGE_HEAP_INDEX type=%" PRIu32
        " heap=%" PRIu32 "\n",
        index, properties->memoryTypes[index].heapIndex
      );

      return VK_ERROR_INITIALIZATION_FAILED;
    }
  }

  for (uint32_t index = 0u;
       index < VULKAN_MULTISTAGE_BUFFER_COUNT; index++)
  {
    const vulkan_heap_allocation_request_t *request = &requests[index];
    const VkDeviceSize alignment = request->requirements.alignment;

    if (request->label == NULL || request->label[0] == '\0'
     || (request->role != VULKAN_HEAP_BUFFER_GPU_STORAGE
      && request->role != VULKAN_HEAP_BUFFER_HOST_TRANSFER)
     || request->requirements.size == 0u
     || alignment == 0u || (alignment & (alignment - 1u)) != 0u)
    {
      fprintf (
        diagnostics,
        "REJECT_MULTISTAGE_REQUIREMENTS index=%" PRIu32 "\n", index
      );

      return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkBool32 compatible = VK_FALSE;

    for (uint32_t type_index = 0u;
         type_index < properties->memoryTypeCount; type_index++)
    {
      if ((request->requirements.memoryTypeBits
        & (UINT32_C (1) << type_index)) == 0u)
      {
        continue;
      }

      if (multistage_type_compatible (
        &properties->memoryTypes[type_index], request
      ) == VK_TRUE)
      {
        compatible = VK_TRUE;

        break;
      }
    }

    if (compatible == VK_FALSE)
    {
      fprintf (
        diagnostics,
        "REJECT_MULTISTAGE_MEMORY_TYPE index=%" PRIu32
        " role=%u bits=0x%08" PRIx32
        " forbidden=0x%08" PRIx32 "\n",
        index, (unsigned int) request->role,
        request->requirements.memoryTypeBits,
        request->forbidden_flags
      );

      return VK_ERROR_FEATURE_NOT_PRESENT;
    }
  }

  vulkan_multistage_memory_plan_t proposed = { 0 };

  for (uint32_t index = 0u;
       index < VULKAN_MULTISTAGE_BUFFER_COUNT; index++)
  {
    proposed.memory_type_indices[index] = UINT32_MAX;
    proposed.heap_indices[index] = UINT32_MAX;
  }

  if (multistage_assign (
    properties, requests, 0u, &proposed
  ) == VK_FALSE)
  {
    fprintf (
      diagnostics,
      "REJECT_MULTISTAGE_HEAP_CAPACITY requested=%u"
      " reason=no_joint_placement\n",
      VULKAN_MULTISTAGE_BUFFER_COUNT
    );

    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  }

  for (uint32_t index = 0u;
       index < VULKAN_MULTISTAGE_BUFFER_COUNT; index++)
  {
    fprintf (
      diagnostics,
      "MULTISTAGE_PLAN_BUFFER index=%" PRIu32
      " label=%s type=%" PRIu32 " heap=%" PRIu32
      " bytes=%" PRIu64 " coherent=%u\n",
      index, requests[index].label,
      proposed.memory_type_indices[index],
      proposed.heap_indices[index],
      (uint64_t) requests[index].requirements.size,
      proposed.coherent[index]
    );
  }

  for (uint32_t index = 0u;
       index < properties->memoryHeapCount; index++)
  {
    fprintf (
      diagnostics,
      "MULTISTAGE_PLAN_HEAP index=%" PRIu32
      " reserved=%" PRIu64 " declared=%" PRIu64 "\n",
      index, (uint64_t) proposed.reserved_per_heap[index],
      (uint64_t) properties->memoryHeaps[index].size
    );
  }

  *result = proposed;

  return VK_SUCCESS;
}
