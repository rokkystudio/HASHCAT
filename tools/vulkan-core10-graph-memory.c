/**
 * Plan dynamically sized Vulkan 1.0 memory groups without device profiles.
 *
 * Backtracking explores every compatible memory-type assignment in stable
 * preference order. The declared allocation size of every VkBuffer is
 * reserved against the reported VkMemoryHeap.size of the chosen type.
 * vkAllocateMemory may subsequently fail due to the device's live budget.
 */

#include "vulkan-core10-graph-memory.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/**
 * Decide whether one memory type satisfies the exact declared role/flags.
 */
static VkBool32 graph_memory_compatible (
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
 * Classify one compatible memory type by the role's Vulkan property preference.
 */
static VkBool32 graph_memory_preferred (
  const VkMemoryType *type,
  vulkan_heap_buffer_role_t role)
{
  const VkMemoryPropertyFlags wanted =
    role == VULKAN_HEAP_BUFFER_HOST_TRANSFER
    ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;

  return (type->propertyFlags & wanted) != 0u
    ? VK_TRUE : VK_FALSE;
}

/**
 * Explore all compatible heap placements for an exact number of buffers.
 *
 * Each successful recursive frame reserves its buffer's complete Vulkan
 * allocation size. On backtracking the reservation is restored exactly.
 */
static VkBool32 graph_memory_assign (
  const VkPhysicalDeviceMemoryProperties *properties,
  const vulkan_heap_allocation_request_t *requests,
  uint32_t count,
  uint32_t depth,
  vulkan_core10_graph_memory_t *plan)
{
  if (depth == count) return VK_TRUE;

  const vulkan_heap_allocation_request_t *request = &requests[depth];
  const VkDeviceSize allocation_bytes = request->requirements.size;

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

      if (graph_memory_compatible (type, request) == VK_FALSE)
      {
        continue;
      }

      const VkBool32 preferred = graph_memory_preferred (
        type, request->role
      );

      if (preferred != (preference == 0u ? VK_TRUE : VK_FALSE))
      {
        continue;
      }

      const uint32_t heap = type->heapIndex;
      const VkDeviceSize used = plan->reserved_per_heap[heap];
      const VkDeviceSize size = properties->memoryHeaps[heap].size;

      if (used > size || allocation_bytes > size - used)
      {
        continue;
      }

      plan->memory_type_indices[depth] = index;
      plan->heap_indices[depth] = heap;
      plan->coherent[depth] =
        (type->propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0u
        ? VK_TRUE : VK_FALSE;
      plan->reserved_per_heap[heap] = used + allocation_bytes;

      if (graph_memory_assign (
        properties, requests, count, depth + 1u, plan
      ) == VK_TRUE)
      {
        return VK_TRUE;
      }

      plan->reserved_per_heap[heap] = used;
      plan->memory_type_indices[depth] = UINT32_MAX;
      plan->heap_indices[depth] = UINT32_MAX;
      plan->coherent[depth] = VK_FALSE;
    }
  }

  return VK_FALSE;
}

/**
 * Free all dynamically sized Vulkan memory planning arrays.
 */
void vulkan_core10_graph_memory_destroy (
  vulkan_core10_graph_memory_t *plan)
{
  if (plan == NULL) return;

  free (plan->memory_type_indices);
  free (plan->heap_indices);
  free (plan->coherent);

  memset (plan, 0, sizeof (*plan));
}

/**
 * Plan an arbitrary caller-declared number of independent Vulkan allocations.
 *
 * Validates memory types, exact VkMemoryRequirements and allocation limits.
 * Distinguishes unsupported memory flags from jointly insufficient heaps.
 * All output arrays are assigned only after complete successful placement.
 */
VkResult vulkan_core10_graph_memory_plan (
  const VkPhysicalDeviceMemoryProperties *properties,
  uint32_t max_memory_allocations,
  uint32_t live_memory_allocations,
  const vulkan_heap_allocation_request_t *requests,
  uint32_t count,
  vulkan_core10_graph_memory_t *result,
  FILE *diagnostics)
{
  if (properties == NULL || requests == NULL || result == NULL
   || diagnostics == NULL || count == 0u
   || properties->memoryTypeCount == 0u
   || properties->memoryTypeCount > VK_MAX_MEMORY_TYPES
   || properties->memoryHeapCount == 0u
   || properties->memoryHeapCount > VK_MAX_MEMORY_HEAPS
   || result->memory_type_indices != NULL
   || result->heap_indices != NULL || result->coherent != NULL
   || result->count != 0u)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  if (live_memory_allocations > max_memory_allocations
   || count > max_memory_allocations - live_memory_allocations)
  {
    fprintf (
      diagnostics,
      "GRAPH_REJECT_ALLOCATION_COUNT requested=%" PRIu32
      " existing=%" PRIu32 " limit=%" PRIu32 "\n",
      count, live_memory_allocations, max_memory_allocations
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
        "GRAPH_REJECT_HEAP_INDEX memory_type=%" PRIu32
        " heap=%" PRIu32 "\n",
        index, properties->memoryTypes[index].heapIndex
      );

      return VK_ERROR_INITIALIZATION_FAILED;
    }
  }

  for (uint32_t buffer = 0u; buffer < count; buffer++)
  {
    const vulkan_heap_allocation_request_t *request = &requests[buffer];
    const VkDeviceSize alignment = request->requirements.alignment;

    if (request->label == NULL || request->label[0] == '\0'
     || request->requirements.size == 0u
     || alignment == 0u || (alignment & (alignment - 1u)) != 0u
     || (request->role != VULKAN_HEAP_BUFFER_GPU_STORAGE
      && request->role != VULKAN_HEAP_BUFFER_HOST_TRANSFER))
    {
      fprintf (
        diagnostics,
        "GRAPH_REJECT_REQUIREMENTS index=%" PRIu32 "\n", buffer
      );

      return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkBool32 compatible = VK_FALSE;

    for (uint32_t type = 0u; type < properties->memoryTypeCount; type++)
    {
      if ((request->requirements.memoryTypeBits
        & (UINT32_C (1) << type)) == 0u)
      {
        continue;
      }

      if (graph_memory_compatible (
        &properties->memoryTypes[type], request
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
        "GRAPH_REJECT_MEMORY_TYPE index=%" PRIu32
        " role=%u bits=0x%08" PRIx32
        " forbidden=0x%08" PRIx32 "\n",
        buffer, (unsigned int) request->role,
        request->requirements.memoryTypeBits,
        request->forbidden_flags
      );

      return VK_ERROR_FEATURE_NOT_PRESENT;
    }
  }

  const size_t index_bytes = (size_t) count * sizeof (uint32_t);
  const size_t coherence_bytes = (size_t) count * sizeof (VkBool32);

  if (index_bytes / sizeof (uint32_t) != count
   || coherence_bytes / sizeof (VkBool32) != count)
  {
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }

  vulkan_core10_graph_memory_t proposed = { 0 };

  proposed.memory_type_indices = (uint32_t *) malloc (
    (size_t) count * sizeof (uint32_t)
  );
  proposed.heap_indices = (uint32_t *) malloc (
    (size_t) count * sizeof (uint32_t)
  );
  proposed.coherent = (VkBool32 *) calloc (
    (size_t) count, sizeof (VkBool32)
  );

  if (proposed.memory_type_indices == NULL
   || proposed.heap_indices == NULL || proposed.coherent == NULL)
  {
    vulkan_core10_graph_memory_destroy (&proposed);

    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }

  proposed.count = count;

  for (uint32_t index = 0u; index < count; index++)
  {
    proposed.memory_type_indices[index] = UINT32_MAX;
    proposed.heap_indices[index] = UINT32_MAX;
  }

  if (graph_memory_assign (
    properties, requests, count, 0u, &proposed
  ) == VK_FALSE)
  {
    fprintf (
      diagnostics,
      "GRAPH_REJECT_HEAP_CAPACITY requested=%" PRIu32
      " reason=no_joint_placement\n",
      count
    );

    vulkan_core10_graph_memory_destroy (&proposed);

    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  }

  for (uint32_t index = 0u; index < count; index++)
  {
    fprintf (
      diagnostics,
      "GRAPH_PLAN_BUFFER index=%" PRIu32
      " label=%s type=%" PRIu32 " heap=%" PRIu32
      " bytes=%" PRIu64 " coherent=%u\n",
      index, requests[index].label,
      proposed.memory_type_indices[index],
      proposed.heap_indices[index],
      (uint64_t) requests[index].requirements.size,
      proposed.coherent[index]
    );
  }

  for (uint32_t heap = 0u; heap < properties->memoryHeapCount; heap++)
  {
    fprintf (
      diagnostics,
      "GRAPH_PLAN_HEAP index=%" PRIu32
      " reserved=%" PRIu64 " declared=%" PRIu64 "\n",
      heap, (uint64_t) proposed.reserved_per_heap[heap],
      (uint64_t) properties->memoryHeaps[heap].size
    );
  }

  *result = proposed;

  return VK_SUCCESS;
}
