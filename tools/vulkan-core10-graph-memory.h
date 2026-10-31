#ifndef VULKAN_CORE10_GRAPH_MEMORY_H
#define VULKAN_CORE10_GRAPH_MEMORY_H

/**
 * Assign memory types for any explicitly declared number of Vulkan 1.0 buffers.
 *
 * All compatibility requirements are read from the current physical device.
 * The caller owns the Vulkan buffers and obtains their VkMemoryRequirements
 * independently. Each allocation uses its complete requirements.size.
 * Declared heap capacities are not live memory budgets.
 */

#include "vulkan-memory-heap-policy.h"

#include <stdint.h>
#include <stdio.h>

/**
 * Own a dynamically sized array of exact Vulkan heap-allocation decisions.
 *
 * The caller initializes the object to zero and calls
 * vulkan_core10_graph_memory_destroy after successful planning.
 */
typedef struct vulkan_core10_graph_memory
{
  uint32_t count;
  uint32_t *memory_type_indices;
  uint32_t *heap_indices;
  VkBool32 *coherent;
  VkDeviceSize reserved_per_heap[VK_MAX_MEMORY_HEAPS];
} vulkan_core10_graph_memory_t;

/**
 * Jointly place every buffer in compatible Vulkan-reported heaps.
 *
 * GPU_STORAGE requires a compatible type and prefers DEVICE_LOCAL.
 * HOST_TRANSFER additionally requires HOST_VISIBLE and prefers
 * HOST_COHERENT. Every forbidden memory-property bit is respected.
 *
 * Candidate order is deterministic: preference class, then Vulkan
 * memory-type index. A complete backtracking search resolves heap
 * competition without substituting workload requirements.
 *
 * Returns VK_ERROR_INITIALIZATION_FAILED for malformed requests;
 * VK_ERROR_TOO_MANY_OBJECTS for an insufficient allocation-count limit;
 * VK_ERROR_FEATURE_NOT_PRESENT for missing compatible types;
 * VK_ERROR_OUT_OF_DEVICE_MEMORY for impossible joint heap placement;
 * VK_ERROR_OUT_OF_HOST_MEMORY for host array allocation failure.
 *
 * The result must be zero initialized. On any failure the output remains
 * unowned and may be safely passed to graph_memory_destroy.
 */
VkResult vulkan_core10_graph_memory_plan (
  const VkPhysicalDeviceMemoryProperties *properties,
  uint32_t max_memory_allocations,
  uint32_t live_memory_allocations,
  const vulkan_heap_allocation_request_t *requests,
  uint32_t count,
  vulkan_core10_graph_memory_t *result,
  FILE *diagnostics);

/**
 * Release the dynamic plan arrays and reset the object to zero.
 */
void vulkan_core10_graph_memory_destroy (
  vulkan_core10_graph_memory_t *plan);

#endif
