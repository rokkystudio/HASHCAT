#ifndef VULKAN_MEMORY_HEAP_POLICY_H
#define VULKAN_MEMORY_HEAP_POLICY_H

/**
 * Plan up to four independent Vulkan 1.0 buffer memory allocations.
 *
 * The planner uses exact VkMemoryRequirements, VkMemoryType.heapIndex,
 * VkMemoryHeap.size and VkPhysicalDeviceLimits.maxMemoryAllocationCount.
 * It searches feasible assignments without matching a GPU model or vendor.
 *
 * The declared heap size is not the live available memory budget. A
 * successful plan is a necessary compatibility check, not a promise that
 * vkAllocateMemory will succeed.
 */

#include <vulkan/vulkan.h>

#include <stdint.h>
#include <stdio.h>

#define VULKAN_HEAP_PLAN_MAX_BUFFERS 4u

/**
 * Define functional access requirements for an independently bound buffer.
 *
 * GPU_STORAGE accepts any compatible type and prioritizes DEVICE_LOCAL.
 * HOST_TRANSFER requires HOST_VISIBLE and prioritizes HOST_COHERENT.
 * The preferences only order candidate types; all compatible alternatives
 * are searched when earlier assignments cannot fit the requested buffers.
 */
typedef enum vulkan_heap_buffer_role
{
  VULKAN_HEAP_BUFFER_GPU_STORAGE = 0,
  VULKAN_HEAP_BUFFER_HOST_TRANSFER = 1
} vulkan_heap_buffer_role_t;

/**
 * Describe an actual Vulkan buffer before allocating its device memory.
 *
 * The caller supplies vkGetBufferMemoryRequirements output, its functional
 * memory role, any explicitly unsupported memory-property flags, and a
 * nonempty diagnostic label. The entire requirements.size is allocated.
 */
typedef struct vulkan_heap_allocation_request
{
  const char *label;
  VkMemoryRequirements requirements;
  vulkan_heap_buffer_role_t role;
  VkMemoryPropertyFlags forbidden_flags;
} vulkan_heap_allocation_request_t;

/**
 * Return a complete per-buffer and per-heap memory assignment.
 *
 * The planner reserves each requested allocation's full requirements.size
 * from its selected declared memory heap. It does not create Vulkan objects,
 * allocate physical memory, or modify the result on failure.
 */
typedef struct vulkan_heap_allocation_plan
{
  uint32_t buffer_count;
  uint32_t memory_type_indices[VULKAN_HEAP_PLAN_MAX_BUFFERS];
  uint32_t heap_indices[VULKAN_HEAP_PLAN_MAX_BUFFERS];
  VkBool32 coherent[VULKAN_HEAP_PLAN_MAX_BUFFERS];
  VkDeviceSize reserved_per_heap[VK_MAX_MEMORY_HEAPS];
} vulkan_heap_allocation_plan_t;

/**
 * Select a feasible placement for one to four Vulkan 1.0 buffer allocations.
 *
 * Checks the driver's memory-type/heap counts, each heap index and declared
 * heap size, the Vulkan memory requirements and the remaining allocation
 * count before searching all compatible combinations. GPU storage prefers
 * DEVICE_LOCAL; host staging prefers HOST_COHERENT. The search proceeds in
 * original buffer order, then preference class, then memory-type index.
 *
 * A request with no compatible memory type returns
 * VK_ERROR_FEATURE_NOT_PRESENT. An incompatible heap capacity returns
 * VK_ERROR_OUT_OF_DEVICE_MEMORY. Exhausted allocation handles return
 * VK_ERROR_TOO_MANY_OBJECTS, and structurally invalid input returns
 * VK_ERROR_INITIALIZATION_FAILED. Diagnostics identify the exact condition.
 *
 * The caller must still handle vkAllocateMemory failures, since heap.size
 * is declared capacity rather than current free memory or memory budget.
 */
VkResult vulkan_heap_plan_allocations (
  const VkPhysicalDeviceMemoryProperties *properties,
  uint32_t max_memory_allocations,
  uint32_t live_allocation_count,
  const vulkan_heap_allocation_request_t *requests,
  uint32_t request_count,
  vulkan_heap_allocation_plan_t *result,
  FILE *diagnostics);

#endif
