#ifndef VULKAN_CORE10_GRAPH_H
#define VULKAN_CORE10_GRAPH_H

/**
 * Own an explicitly specified Vulkan 1.0 Compute resource graph.
 *
 * Buffer, storage-descriptor and Compute shader counts are input data,
 * not device-specific profiles. One VkDevice, one queue, one descriptor
 * set, one command buffer and one fence belong to each graph instance.
 *
 * The application describes the exact buffer and descriptor requirements
 * and validates shader-specific workgroup limits through the capability
 * checker before creating the graph. No SPIR-V reflection or inference is
 * performed; unsupported requirements return explicit errors.
 */

#include "vulkan-auto-config.h"
#include "vulkan-core10-graph-memory.h"

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

/**
 * Describe one independently allocated Vulkan buffer.
 *
 * GPU_STORAGE buffers must include STORAGE_BUFFER usage and are not mapped.
 * HOST_TRANSFER buffers must be host mapped and use the exact declared
 * transfer flags. The map requirement selects HOST_VISIBLE memory.
 */
typedef struct vulkan_core10_graph_buffer_spec
{
  const char *label;
  VkDeviceSize size;
  VkBufferUsageFlags usage;
  vulkan_heap_buffer_role_t role;
  VkMemoryPropertyFlags forbidden_flags;
  VkBool32 map;
} vulkan_core10_graph_buffer_spec_t;

/**
 * Map an explicit STORAGE_BUFFER descriptor binding to one Vulkan buffer.
 *
 * Binding numbers are supplied by the shader's known interface and may
 * be nonconsecutive. A binding appears exactly once; offset and range
 * obey the actual Vulkan minStorageBufferOffsetAlignment and range limits.
 */
typedef struct vulkan_core10_graph_descriptor_spec
{
  uint32_t binding;
  uint32_t buffer_index;
  VkDeviceSize offset;
  VkDeviceSize range;
} vulkan_core10_graph_descriptor_spec_t;

/**
 * Reference a caller-owned SPIR-V 1.0 Compute module for one pipeline.
 *
 * The caller owns shader words throughout creation. All pipelines use
 * the shader's "main" entry point and the explicitly declared set layout.
 */
typedef struct vulkan_core10_graph_shader_spec
{
  const uint32_t *words;
  size_t bytes;
} vulkan_core10_graph_shader_spec_t;

/**
 * Own one actual Vulkan buffer handle, device allocation and CPU map.
 */
typedef struct vulkan_core10_graph_buffer
{
  VkBuffer buffer;
  VkDeviceMemory memory;
  VkMemoryRequirements requirements;
  VkDeviceSize logical_size;
  void *mapped;
} vulkan_core10_graph_buffer_t;

/**
 * Store runtime-owned dynamic Vulkan objects and command completion state.
 *
 * Callers zero initialize this struct and explicitly invoke graph_destroy
 * after a successful graph_create. VkInstance and VkPhysicalDevice remain
 * owned by the caller. VkDescriptorSet belongs to the runtime's pool.
 */
typedef struct vulkan_core10_graph
{
  VkDevice device;
  VkQueue queue;
  uint32_t queue_family;
  vulkan_core10_graph_buffer_t *buffers;
  uint32_t buffer_count;
  uint32_t mapped_count;
  uint32_t descriptor_count;
  uint32_t stage_count;
  vulkan_core10_graph_memory_t memory_plan;
  VkDescriptorSetLayout descriptor_layout;
  VkPipelineLayout pipeline_layout;
  VkShaderModule *shader_modules;
  VkPipeline *pipelines;
  VkDescriptorPool descriptor_pool;
  VkDescriptorSet descriptor_set;
  VkCommandPool command_pool;
  VkCommandBuffer command;
  VkFence fence;
  VkPhysicalDeviceLimits limits;
  uint32_t acquisitions;
  uint32_t completed_submissions;
  VkBool32 recording;
  VkBool32 recorded;
  VkBool32 pending;
  FILE *diagnostics;
} vulkan_core10_graph_t;

/**
 * Create a device-capability-driven Vulkan 1.0 graph of arbitrary size.
 *
 * buffer_count, descriptor_count and stage_count are explicit nonzero
 * counts; each input array contains exactly that many records. All
 * buffers are independently allocated using real VkMemoryRequirements
 * and one joint heap plan. Every descriptor binding is explicitly mapped
 * to a storage buffer. Each shader creates one Compute pipeline.
 *
 * Counts are checked against Vulkan device limits and host allocation
 * ranges. The caller supplies the exact shader interface and guarantees
 * it matches the declared descriptor binding and workload metadata.
 *
 * fail_after=0 performs normal creation. A nonzero one-based number
 * deliberately interrupts immediately after that many successful Vulkan
 * acquisitions, testing complete partial-resource cleanup. The number of
 * acquisitions depends on the declared graph and is reported on success.
 *
 * All Vulkan error codes are propagated. On failure, graph_destroy runs;
 * the owner is empty. On success, the caller owns the created graph.
 */
VkResult vulkan_core10_graph_create (
  vulkan_core10_graph_t *graph,
  const vulkan_auto_device_t *selected,
  const vulkan_core10_graph_buffer_spec_t *buffers,
  uint32_t buffer_count,
  const vulkan_core10_graph_descriptor_spec_t *descriptors,
  uint32_t descriptor_count,
  const vulkan_core10_graph_shader_spec_t *shaders,
  uint32_t stage_count,
  uint32_t fail_after,
  FILE *diagnostics);

/**
 * Begin recording the owned primary command buffer exactly once.
 *
 * Callers record the required vkCmdCopyBuffer, vkCmdPipelineBarrier,
 * vkCmdBindDescriptorSets and vkCmdDispatch commands. No implicit
 * algorithm or barrier is inserted by this resource owner.
 */
VkResult vulkan_core10_graph_begin (vulkan_core10_graph_t *graph);

/**
 * Complete the primary command buffer for sequential Vulkan submissions.
 *
 * The command buffer has no ONE_TIME_SUBMIT flag and may be resubmitted
 * only after the previous fence has completed.
 */
VkResult vulkan_core10_graph_end (vulkan_core10_graph_t *graph);

/**
 * Submit the recorded Vulkan 1.0 command buffer and wait for its fence.
 *
 * A completed previous fence is reset before resubmitting the same
 * command buffer. A pending GPU submission blocks further submissions
 * until completion. Driver errors and timeouts are returned without
 * silently switching queues or altering the workload.
 */
VkResult vulkan_core10_graph_submit_and_wait (
  vulkan_core10_graph_t *graph);

/**
 * Flush or invalidate the full host-visible mapped Vulkan memory allocation.
 *
 * An explicitly mapped HOST_COHERENT buffer needs no cache driver call;
 * noncoherent memory uses vkFlushMappedMemoryRanges before GPU access
 * or vkInvalidateMappedMemoryRanges after a completed GPU fence.
 * Offset 0 with VK_WHOLE_SIZE satisfies nonCoherentAtomSize alignment.
 */
VkResult vulkan_core10_graph_sync (
  vulkan_core10_graph_t *graph,
  uint32_t buffer_index,
  VkBool32 invalidate);

/**
 * Destroy all graph-owned resources including partial failed creations.
 *
 * Waits for device idle when a VkDevice exists; releases fence, command
 * pool, descriptor objects, pipelines, shaders, all mapped CPU memory,
 * GPU buffers, Vulkan memory and finally the logical device. Also frees
 * all host arrays and the memory plan. A second destroy is harmless.
 */
VkResult vulkan_core10_graph_destroy (vulkan_core10_graph_t *graph);

#endif
