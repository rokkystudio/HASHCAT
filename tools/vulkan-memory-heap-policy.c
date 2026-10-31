/**
 * Plan Vulkan 1.0 buffer allocations against driver-reported memory heaps.
 *
 * The assignment searches all compatible memory types for up to four buffer
 * requirements. It respects independent heap capacities, the current number
 * of memory allocations and the maximum allocation count. Memory access
 * roles provide explicit preference classes but never override compatibility.
 *
 * No Vulkan driver function is called and no memory is allocated here.
 */

#include "vulkan-memory-heap-policy.h"

#include <inttypes.h>
#include <stdint.h>
#include <string.h>

/**
 * Check the exact access flags needed for one Vulkan buffer memory role.
 *
 * GPU storage accepts all Vulkan-compatible memory flags. Host transfer
 * buffers require HOST_VISIBLE; caller-supplied forbidden flags exclude
 * memory types needing optional features not enabled by the application.
 */
static VkBool32 vulkan_heap_type_compatible (
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
 * Classify one compatible type according to the buffer's declared role.
 *
 * DEVICE_LOCAL is preferred for GPU-only data and HOST_COHERENT for CPU
 * transfers. This only defines deterministic search order, not validity.
 */
static VkBool32 vulkan_heap_type_preferred (
  const VkMemoryType *type,
  vulkan_heap_buffer_role_t role)
{
  const VkMemoryPropertyFlags flag =
    role == VULKAN_HEAP_BUFFER_GPU_STORAGE
    ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
    : VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

  return (type->propertyFlags & flag) != 0u ? VK_TRUE : VK_FALSE;
}

/**
 * Search every compatible type recursively without a greedy heap assignment.
 *
 * Each attempted allocation is counted against its actual heap index. When a
 * later buffer cannot fit, the search restores the prior reservation and
 * considers the next valid memory type for the earlier buffer.
 */
static VkBool32 vulkan_heap_assign_buffers (
  const VkPhysicalDeviceMemoryProperties *properties,
  const vulkan_heap_allocation_request_t *requests,
  uint32_t request_count,
  uint32_t depth,
  vulkan_heap_allocation_plan_t *plan)
{
  if (depth == request_count) return VK_TRUE;

  const vulkan_heap_allocation_request_t *request = &requests[depth];
  const VkDeviceSize bytes = request->requirements.size;

  for (uint32_t pass = 0u; pass < 2u; pass++)
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

      if (vulkan_heap_type_compatible (type, request) == VK_FALSE)
      {
        continue;
      }

      const VkBool32 preferred = vulkan_heap_type_preferred (
        type, request->role
      );

      if (preferred != (pass == 0u ? VK_TRUE : VK_FALSE))
      {
        continue;
      }

      const uint32_t heap_index = type->heapIndex;
      const VkDeviceSize capacity =
        properties->memoryHeaps[heap_index].size;
      const VkDeviceSize already_reserved =
        plan->reserved_per_heap[heap_index];

      if (already_reserved > capacity || bytes > capacity - already_reserved)
      {
        continue;
      }

      plan->memory_type_indices[depth] = index;
      plan->heap_indices[depth] = heap_index;
      plan->coherent[depth] =
        (type->propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0u
        ? VK_TRUE : VK_FALSE;
      plan->reserved_per_heap[heap_index] = already_reserved + bytes;

      if (vulkan_heap_assign_buffers (
        properties, requests, request_count, depth + 1u, plan
      ) == VK_TRUE)
      {
        return VK_TRUE;
      }

      plan->reserved_per_heap[heap_index] = already_reserved;
      plan->memory_type_indices[depth] = UINT32_MAX;
      plan->heap_indices[depth] = UINT32_MAX;
      plan->coherent[depth] = VK_FALSE;
    }
  }

  return VK_FALSE;
}

/**
 * Validate and plan up to four independent Vulkan storage/transfer allocations.
 *
 * All types must refer to a valid driver-declared heap. The entire requested
 * VkMemoryRequirements.size is reserved per allocation; individual resources
 * may have different compatible memory masks. The search handles alternate
 * types when a preferred heap lacks capacity. No result is written on error.
 */
VkResult vulkan_heap_plan_allocations (
  const VkPhysicalDeviceMemoryProperties *properties,
  uint32_t max_memory_allocations,
  uint32_t live_allocation_count,
  const vulkan_heap_allocation_request_t *requests,
  uint32_t request_count,
  vulkan_heap_allocation_plan_t *result,
  FILE *diagnostics)
{
  if (properties == NULL || requests == NULL || result == NULL
   || diagnostics == NULL || request_count == 0u
   || request_count > VULKAN_HEAP_PLAN_MAX_BUFFERS
   || properties->memoryTypeCount == 0u
   || properties->memoryTypeCount > VK_MAX_MEMORY_TYPES
   || properties->memoryHeapCount == 0u
   || properties->memoryHeapCount > VK_MAX_MEMORY_HEAPS)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  for (uint32_t index = 0u; index < properties->memoryTypeCount; index++)
  {
    if (properties->memoryTypes[index].heapIndex
      >= properties->memoryHeapCount)
    {
      fprintf (
        diagnostics,
        "REJECT_HEAP_INDEX type=%" PRIu32
        " heap=%" PRIu32 " heap_count=%" PRIu32 "\n",
        index, properties->memoryTypes[index].heapIndex,
        properties->memoryHeapCount
      );

      return VK_ERROR_INITIALIZATION_FAILED;
    }
  }

  if (live_allocation_count > max_memory_allocations
   || request_count > max_memory_allocations - live_allocation_count)
  {
    fprintf (
      diagnostics,
      "REJECT_ALLOCATION_COUNT existing=%" PRIu32
      " requested=%" PRIu32 " maximum=%" PRIu32 "\n",
      live_allocation_count, request_count, max_memory_allocations
    );

    return VK_ERROR_TOO_MANY_OBJECTS;
  }

  for (uint32_t index = 0u; index < request_count; index++)
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
        "REJECT_ALLOCATION_REQUIREMENTS index=%" PRIu32 "\n", index
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

      if (vulkan_heap_type_compatible (
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
        "REJECT_ALLOCATION_TYPE index=%" PRIu32
        " role=%u bits=0x%08" PRIx32
        " forbidden=0x%08" PRIx32 "\n",
        index, (unsigned int) request->role,
        request->requirements.memoryTypeBits,
        request->forbidden_flags
      );

      return VK_ERROR_FEATURE_NOT_PRESENT;
    }
  }

  vulkan_heap_allocation_plan_t proposed = { 0 };

  proposed.buffer_count = request_count;

  for (uint32_t index = 0u; index < request_count; index++)
  {
    proposed.memory_type_indices[index] = UINT32_MAX;
    proposed.heap_indices[index] = UINT32_MAX;
  }

  if (vulkan_heap_assign_buffers (
    properties, requests, request_count, 0u, &proposed
  ) == VK_FALSE)
  {
    fprintf (
      diagnostics,
      "REJECT_HEAP_CAPACITY buffers=%" PRIu32
      " reason=no_combination_fits_declared_heaps\n",
      request_count
    );

    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  }

  for (uint32_t index = 0u; index < request_count; index++)
  {
    fprintf (
      diagnostics,
      "PLAN_BUFFER index=%" PRIu32
      " label=%s type=%" PRIu32 " heap=%" PRIu32
      " bytes=%" PRIu64 " coherent=%u\n",
      index, requests[index].label,
      proposed.memory_type_indices[index],
      proposed.heap_indices[index],
      (uint64_t) requests[index].requirements.size,
      proposed.coherent[index]
    );
  }

  for (uint32_t index = 0u; index < properties->memoryHeapCount; index++)
  {
    fprintf (
      diagnostics,
      "PLAN_HEAP index=%" PRIu32
      " reserved=%" PRIu64 " declared=%" PRIu64 "\n",
      index,
      (uint64_t) proposed.reserved_per_heap[index],
      (uint64_t) properties->memoryHeaps[index].size
    );
  }

  *result = proposed;

  return VK_SUCCESS;
}
