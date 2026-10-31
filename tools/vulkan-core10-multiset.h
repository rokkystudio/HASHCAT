#ifndef VULKAN_CORE10_MULTISET_H
#define VULKAN_CORE10_MULTISET_H

/**
 * Own one Vulkan 1.0 Compute graph with multiple descriptor set layouts.
 *
 * The caller provides exact Vulkan buffer requirements, descriptor types,
 * set/binding positions and SPIR-V 1.0 shader interfaces. The runtime
 * supports STORAGE_BUFFER and UNIFORM_BUFFER descriptors with any
 * nonzero set count satisfying the current physical device's limits.
 *
 * All GPU resources, descriptor sets, pipeline layouts, command objects
 * and CPU mappings belong to the runtime. No device-specific preset,
 * shader reflection, password algorithm or implicit fallback is used.
 */

#include "vulkan-auto-config.h"
#include "vulkan-core10-graph-memory.h"
#include "vulkan-core10-descriptor-policy.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/**
 * Specify an exact Vulkan buffer and required memory accessibility.
 *
 * GPU_STORAGE buffers are not mapped; HOST_TRANSFER requires HOST_VISIBLE
 * and is mapped for its entire allocation. A mapped buffer may carry
 * TRANSFER_SRC, TRANSFER_DST or UNIFORM_BUFFER usage as declared.
 */
typedef struct vulkan_core10_multiset_buffer_spec
{
  const char *label;
  VkDeviceSize size;
  VkBufferUsageFlags usage;
  vulkan_heap_buffer_role_t role;
  VkMemoryPropertyFlags forbidden_flags;
  VkBool32 map;
} vulkan_core10_multiset_buffer_spec_t;

/**
 * Hold a complete caller-owned SPIR-V 1.0 Compute shader module.
 *
 * Each shader module declares an entry point named "main" and the same
 * explicitly specified sequence of descriptor set layouts.
 */
typedef struct vulkan_core10_multiset_shader_spec
{
  const uint32_t *words;
  size_t bytes;
} vulkan_core10_multiset_shader_spec_t;

/**
 * Own one complete GPU buffer, allocated memory and optional host map.
 */
typedef struct vulkan_core10_multiset_buffer
{
  VkBuffer buffer;
  VkDeviceMemory memory;
  VkMemoryRequirements requirements;
  VkDeviceSize logical_size;
  void *mapped;
} vulkan_core10_multiset_buffer_t;

/**
 * Maintain dynamic Vulkan 1.0 buffer, descriptor and Compute object state.
 *
 * VkInstance and VkPhysicalDevice are owned by the application. A
 * VkDescriptorSet is released with its owning VkDescriptorPool, not by a
 * separate vkFreeDescriptorSets call. The caller zero initializes this
 * owner and calls multiset_destroy after successful creation.
 */
typedef struct vulkan_core10_multiset
{
  VkDevice device;
  VkQueue queue;
  vulkan_core10_multiset_buffer_t *buffers;
  uint32_t buffer_count;
  uint32_t map_count;
  uint32_t set_count;
  uint32_t binding_count;
  uint32_t stage_count;
  VkDescriptorSetLayout *layouts;
  VkDescriptorSet *sets;
  VkShaderModule *shaders;
  VkPipeline *pipelines;
  VkPipelineLayout pipeline_layout;
  VkDescriptorPool descriptor_pool;
  VkCommandPool command_pool;
  VkCommandBuffer command;
  VkFence fence;
  VkPhysicalDeviceLimits limits;
  vulkan_core10_graph_memory_t memory_plan;
  uint32_t acquisitions;
  uint32_t completed_submissions;
  VkBool32 recording;
  VkBool32 recorded;
  VkBool32 pending;
  FILE *diagnostics;
} vulkan_core10_multiset_t;

/**
 * Create one capability-validated Vulkan 1.0 multiset Compute graph.
 *
 * set_count declares the complete contiguous set indices 0..set_count-1.
 * Each set receives at least one binding and a distinct VkDescriptorSet.
 * bindings include explicit STORAGE_BUFFER/UNIFORM_BUFFER types and
 * exactly mapped buffer indices, offsets and ranges. All buffers use
 * current driver memory requirements and a joint heap-capacity plan.
 *
 * Uniform and storage descriptors are validated independently against
 * maxDescriptorSet*, maxPerStage*, maxUniformBufferRange,
 * maxStorageBufferRange, and both descriptor-offset alignments.
 *
 * fail_after=0 creates the complete runtime. A nonzero index injects an
 * application-level error after exactly that many successful Vulkan
 * acquisitions, returning VK_ERROR_INITIALIZATION_FAILED with complete
 * partial object cleanup. Real VkResult driver failures are propagated.
 *
 * On failure, any owned resources are destroyed and the owner becomes
 * empty. A successful call requires multiset_destroy by the caller.
 */
VkResult vulkan_core10_multiset_create (
  vulkan_core10_multiset_t *runtime,
  const vulkan_auto_device_t *selected,
  const vulkan_core10_multiset_buffer_spec_t *buffers,
  uint32_t buffer_count,
  const vulkan_core10_descriptor_binding_t *bindings,
  uint32_t binding_count,
  uint32_t set_count,
  const vulkan_core10_multiset_shader_spec_t *shaders,
  uint32_t stage_count,
  uint32_t fail_after,
  FILE *diagnostics);

/**
 * Begin recording one primary command buffer for repeated sequential submits.
 *
 * The application records all shader/transfer/barrier operations explicitly.
 * The command buffer does not use VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT.
 */
VkResult vulkan_core10_multiset_begin (
  vulkan_core10_multiset_t *runtime);

/**
 * Finish the one reusable primary command buffer recording.
 */
VkResult vulkan_core10_multiset_end (
  vulkan_core10_multiset_t *runtime);

/**
 * Submit the existing command buffer and wait for Vulkan 1.0 fence completion.
 *
 * Resets a completed fence before subsequent submissions. A pending GPU
 * submission is never submitted again before the previous wait completes.
 */
VkResult vulkan_core10_multiset_submit_and_wait (
  vulkan_core10_multiset_t *runtime);

/**
 * Synchronize the entire mapped HOST_VISIBLE allocation in one buffer.
 *
 * For HOST_COHERENT memory this returns VK_SUCCESS without driver calls.
 * For noncoherent memory it flushes CPU writes before GPU execution or
 * invalidates GPU writes after a completed fence, using VK_WHOLE_SIZE.
 */
VkResult vulkan_core10_multiset_sync (
  vulkan_core10_multiset_t *runtime,
  uint32_t buffer_index,
  VkBool32 invalidate);

/**
 * Release all Vulkan objects, dynamic arrays and memory plan in safe order.
 *
 * Includes ownership of all descriptor set layouts and their allocated
 * sets, GPU buffers/memory, mapped CPU allocations, shaders, pipelines,
 * queue command objects and fence. The owner becomes zero initialized.
 */
VkResult vulkan_core10_multiset_destroy (
  vulkan_core10_multiset_t *runtime);

#endif
