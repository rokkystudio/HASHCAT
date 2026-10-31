#ifndef VULKAN_MULTISTAGE_MEMORY_PLAN_H
#define VULKAN_MULTISTAGE_MEMORY_PLAN_H

/**
 * Plan six separate Vulkan 1.0 GPU storage and host staging allocations.
 *
 * All memory types, allocation requirements, heap indices, heap sizes and
 * allocation limits are supplied by Vulkan at runtime. No GPU manufacturer,
 * Android version, memory type index or heap size is hardcoded.
 *
 * Planning does not allocate GPU memory and does not estimate live budgets.
 */

#include "vulkan-memory-heap-policy.h"

#include <stdint.h>
#include <stdio.h>

#define VULKAN_MULTISTAGE_BUFFER_COUNT 6u

/**
 * Describe six fully planned independent Vulkan memory allocations.
 *
 * Each buffer consumes exactly VkMemoryRequirements.size bytes from the
 * declared VkMemoryHeap of the selected VkMemoryType. A noncoherent CPU
 * allocation requires explicit mapped-memory flush and invalidate calls.
 */
typedef struct vulkan_multistage_memory_plan
{
  uint32_t memory_type_indices[VULKAN_MULTISTAGE_BUFFER_COUNT];
  uint32_t heap_indices[VULKAN_MULTISTAGE_BUFFER_COUNT];
  VkBool32 coherent[VULKAN_MULTISTAGE_BUFFER_COUNT];
  VkDeviceSize reserved_per_heap[VK_MAX_MEMORY_HEAPS];
} vulkan_multistage_memory_plan_t;

/**
 * Select one compatible type for each of six real Vulkan buffer requests.
 *
 * Requests are assessed as a single allocation group with backtracking.
 * GPU storage prefers DEVICE_LOCAL, host transfer requires HOST_VISIBLE
 * and prefers HOST_COHERENT. Preference never overrides compatibility,
 * explicitly forbidden memory flags, heap capacity or allocation limits.
 *
 * Returns VK_ERROR_FEATURE_NOT_PRESENT when one buffer has no valid type,
 * VK_ERROR_OUT_OF_DEVICE_MEMORY when no joint heap placement fits,
 * VK_ERROR_TOO_MANY_OBJECTS for insufficient allocation slots, and
 * VK_ERROR_INITIALIZATION_FAILED for malformed requirements.
 *
 * On failure the caller's destination remains unchanged. A successful
 * plan is not a guarantee that vkAllocateMemory will succeed.
 */
VkResult vulkan_multistage_plan_memory (
  const VkPhysicalDeviceMemoryProperties *properties,
  uint32_t max_memory_allocations,
  const vulkan_heap_allocation_request_t requests[VULKAN_MULTISTAGE_BUFFER_COUNT],
  vulkan_multistage_memory_plan_t *result,
  FILE *diagnostics);

#endif
