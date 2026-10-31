#ifndef VULKAN_CORE10_MULTISET_GRAPH_H
#define VULKAN_CORE10_MULTISET_GRAPH_H

/**
 * Describe a sequential Vulkan 1.0 multi-stage, multi-set Compute graph.
 *
 * The caller identifies all buffers, pipeline indices, workgroup counts,
 * and per-stage readbacks. No stage dependencies, shader interfaces, or
 * buffer roles are inferred from device names or compiled SPIR-V.
 */

#include "vulkan-core10-multiset.h"

/**
 * Declare one Compute stage and its exact storage-buffer dependency.
 *
 * source_buffer_index is read by the selected pipeline and
 * destination_buffer_index is written. Consecutive stages must use
 * the previous destination as their source. Each stage writes a distinct
 * output buffer that remains available for CPU readback.
 */
typedef struct vulkan_core10_multiset_graph_stage
{
  uint32_t pipeline_index;
  uint32_t source_buffer_index;
  uint32_t destination_buffer_index;
  uint32_t groups[3];
} vulkan_core10_multiset_graph_stage_t;

/**
 * Describe the complete transfer, dispatch, and readback command sequence.
 *
 * The first stage consumes a GPU buffer populated from upload_buffer_index.
 * Each stage's storage destination is copied to its corresponding mapped
 * readback_buffer_indices entry after all dispatches finish.
 *
 * The std140 uniform buffer is CPU mapped and is accessed by the shaders.
 * Host code flushes its writes and upload staging with multiset_sync before
 * each submit, and invalidates each readback after the completed fence.
 */
typedef struct vulkan_core10_multiset_graph_spec
{
  const vulkan_core10_multiset_graph_stage_t *stages;
  const uint32_t *readback_buffer_indices;
  uint32_t stage_count;
  uint32_t upload_buffer_index;
  uint32_t uniform_buffer_index;
  VkDeviceSize transfer_bytes;
} vulkan_core10_multiset_graph_spec_t;

/**
 * Validate an exact graph against the owned Vulkan buffers and GPU limits.
 *
 * buffer_specs must be the immutable declarations supplied to
 * vulkan_core10_multiset_create. Each logical buffer size is compared to
 * the created object. Returns VK_ERROR_INITIALIZATION_FAILED for invalid
 * indices, usage, data hazards or sizes, and VK_ERROR_FEATURE_NOT_PRESENT
 * for unsupported dispatch group counts.
 *
 * No command is recorded and no Vulkan driver function is called.
 */
VkResult vulkan_core10_multiset_graph_validate (
  const vulkan_core10_multiset_t *runtime,
  const vulkan_core10_multiset_buffer_spec_t *buffer_specs,
  uint32_t buffer_count,
  const vulkan_core10_multiset_graph_spec_t *graph,
  FILE *diagnostics);

/**
 * Record host upload, sequential Compute dispatches and all stage readbacks.
 *
 * Uses one primary command buffer, the runtime's shared pipeline layout
 * and descriptor sets, and Vulkan 1.0 vkCmdPipelineBarrier dependencies.
 * One explicit shader-write-to-shader-read buffer barrier follows each
 * stage except the last. The command buffer is reusable after fence
 * completion with newly flushed upload and uniform data.
 *
 * The caller owns the graph declaration and performs each submit,
 * synchronization and CPU verification. Returns vkBeginCommandBuffer
 * or vkEndCommandBuffer errors without interpreting driver failures.
 */
VkResult vulkan_core10_multiset_graph_record (
  vulkan_core10_multiset_t *runtime,
  const vulkan_core10_multiset_buffer_spec_t *buffer_specs,
  uint32_t buffer_count,
  const vulkan_core10_multiset_graph_spec_t *graph,
  FILE *diagnostics);

#endif
