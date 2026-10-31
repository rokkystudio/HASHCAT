/**
 * Record an explicit, reusable Vulkan 1.0 multi-stage Compute graph.
 *
 * The recorder works with the existing mixed STORAGE_BUFFER/UNIFORM_BUFFER
 * multi-set runtime and supports any supported number of sequential stages.
 * Every transfer, pipeline index and readback is specified by the caller.
 */

#include "vulkan-core10-multiset-graph.h"

#include <inttypes.h>

/**
 * Check one declared buffer's role, logical size and Vulkan usage flags.
 */
static VkBool32 graph_buffer_has (
  const vulkan_core10_multiset_t *runtime,
  const vulkan_core10_multiset_buffer_spec_t *specs,
  uint32_t index,
  VkBufferUsageFlags required,
  VkBool32 mapped,
  VkDeviceSize bytes)
{
  const vulkan_core10_multiset_buffer_t *buffer = &runtime->buffers[index];

  return buffer->buffer != VK_NULL_HANDLE
      && buffer->logical_size == specs[index].size
      && bytes > 0u && bytes <= specs[index].size
      && (specs[index].usage & required) == required
      && (mapped == VK_FALSE || buffer->mapped != NULL)
    ? VK_TRUE : VK_FALSE;
}

/**
 * Validate the complete declared multi-stage transfer and dependency graph.
 *
 * Each stage uses a distinct storage output, and every later stage consumes
 * exactly the preceding stage's output. Readback buffers are distinct from
 * all GPU storage buffers and the CPU upload/uniform allocations.
 */
VkResult vulkan_core10_multiset_graph_validate (
  const vulkan_core10_multiset_t *runtime,
  const vulkan_core10_multiset_buffer_spec_t *buffer_specs,
  uint32_t buffer_count,
  const vulkan_core10_multiset_graph_spec_t *graph,
  FILE *diagnostics)
{
  if (runtime == NULL || runtime->device == VK_NULL_HANDLE
   || runtime->buffers == NULL || runtime->pipelines == NULL
   || runtime->sets == NULL || runtime->pipeline_layout == VK_NULL_HANDLE
   || buffer_specs == NULL || graph == NULL || diagnostics == NULL
   || graph->stages == NULL || graph->readback_buffer_indices == NULL
   || graph->stage_count == 0u
   || graph->stage_count > runtime->stage_count
   || buffer_count == 0u || buffer_count != runtime->buffer_count
   || graph->transfer_bytes == 0u
   || graph->upload_buffer_index >= buffer_count
   || graph->uniform_buffer_index >= buffer_count
   || graph->upload_buffer_index == graph->uniform_buffer_index)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  if (graph_buffer_has (
    runtime, buffer_specs, graph->upload_buffer_index,
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_TRUE,
    graph->transfer_bytes
  ) != VK_TRUE
   || graph_buffer_has (
    runtime, buffer_specs, graph->uniform_buffer_index,
    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_TRUE,
    buffer_specs[graph->uniform_buffer_index].size
  ) != VK_TRUE)
  {
    fprintf (diagnostics, "MULTIGRAPH_REJECT_HOST_BUFFERS\n");

    return VK_ERROR_INITIALIZATION_FAILED;
  }

  for (uint32_t stage = 0u; stage < graph->stage_count; stage++)
  {
    const vulkan_core10_multiset_graph_stage_t *step =
      &graph->stages[stage];
    const uint32_t readback = graph->readback_buffer_indices[stage];

    if (step->pipeline_index >= runtime->stage_count
     || step->source_buffer_index >= buffer_count
     || step->destination_buffer_index >= buffer_count
     || readback >= buffer_count
     || step->source_buffer_index == step->destination_buffer_index
     || step->source_buffer_index == graph->upload_buffer_index
     || step->destination_buffer_index == graph->upload_buffer_index
     || step->source_buffer_index == graph->uniform_buffer_index
     || step->destination_buffer_index == graph->uniform_buffer_index
     || readback == graph->upload_buffer_index
     || readback == graph->uniform_buffer_index
     || readback == step->source_buffer_index
     || readback == step->destination_buffer_index
     || runtime->pipelines[step->pipeline_index] == VK_NULL_HANDLE
     || graph_buffer_has (
        runtime, buffer_specs, step->source_buffer_index,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_FALSE,
        graph->transfer_bytes
      ) != VK_TRUE
     || graph_buffer_has (
        runtime, buffer_specs, step->destination_buffer_index,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
          | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_FALSE, graph->transfer_bytes
      ) != VK_TRUE
     || graph_buffer_has (
        runtime, buffer_specs, readback,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_TRUE,
        graph->transfer_bytes
      ) != VK_TRUE)
    {
      fprintf (
        diagnostics, "MULTIGRAPH_REJECT_STAGE stage=%" PRIu32 "\n", stage
      );

      return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (stage == 0u)
    {
      if (graph_buffer_has (
        runtime, buffer_specs, step->source_buffer_index,
        VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_FALSE,
        graph->transfer_bytes
      ) != VK_TRUE)
      {
        fprintf (diagnostics, "MULTIGRAPH_REJECT_INPUT\n");

        return VK_ERROR_INITIALIZATION_FAILED;
      }
    }
    else if (step->source_buffer_index
      != graph->stages[stage - 1u].destination_buffer_index)
    {
      fprintf (
        diagnostics, "MULTIGRAPH_REJECT_DEPENDENCY stage=%" PRIu32 "\n",
        stage
      );

      return VK_ERROR_INITIALIZATION_FAILED;
    }

    for (uint32_t axis = 0u; axis < 3u; axis++)
    {
      if (step->groups[axis] == 0u)
      {
        fprintf (
          diagnostics,
          "MULTIGRAPH_REJECT_DISPATCH stage=%" PRIu32
          " axis=%" PRIu32 " zero=1\n",
          stage, axis
        );

        return VK_ERROR_INITIALIZATION_FAILED;
      }

      if (step->groups[axis] > runtime->limits.maxComputeWorkGroupCount[axis])
      {
        fprintf (
          diagnostics,
          "MULTIGRAPH_REJECT_LIMIT stage=%" PRIu32
          " axis=%" PRIu32 " requested=%" PRIu32
          " limit=%" PRIu32 "\n",
          stage, axis, step->groups[axis],
          runtime->limits.maxComputeWorkGroupCount[axis]
        );

        return VK_ERROR_FEATURE_NOT_PRESENT;
      }
    }

    for (uint32_t prior = 0u; prior < stage; prior++)
    {
      if (step->destination_buffer_index
          == graph->stages[prior].source_buffer_index
       || step->destination_buffer_index
          == graph->stages[prior].destination_buffer_index
       || readback == graph->readback_buffer_indices[prior]
       || readback == graph->stages[prior].source_buffer_index
       || readback == graph->stages[prior].destination_buffer_index
       || graph->readback_buffer_indices[prior]
          == step->source_buffer_index
       || graph->readback_buffer_indices[prior]
          == step->destination_buffer_index)
      {
        fprintf (
          diagnostics, "MULTIGRAPH_REJECT_ALIAS stage=%" PRIu32
          " prior=%" PRIu32 "\n", stage, prior
        );

        return VK_ERROR_INITIALIZATION_FAILED;
      }
    }
  }

  fprintf (
    diagnostics,
    "MULTIGRAPH_ACCEPT stages=%" PRIu32 " sets=%" PRIu32
    " buffers=%" PRIu32 " bytes=%" PRIu64 "\n",
    graph->stage_count, runtime->set_count,
    runtime->buffer_count, (uint64_t) graph->transfer_bytes
  );

  return VK_SUCCESS;
}

/**
 * Record one Vulkan 1.0 upload, all dependent dispatches and readbacks.
 *
 * Host-to-transfer/uniform, transfer-to-shader, shader-to-shader,
 * shader-to-transfer and transfer-to-host barriers express all memory
 * dependencies. Stage outputs remain distinct while later stages execute.
 */
VkResult vulkan_core10_multiset_graph_record (
  vulkan_core10_multiset_t *runtime,
  const vulkan_core10_multiset_buffer_spec_t *buffer_specs,
  uint32_t buffer_count,
  const vulkan_core10_multiset_graph_spec_t *graph,
  FILE *diagnostics)
{
  VkResult result = vulkan_core10_multiset_graph_validate (
    runtime, buffer_specs, buffer_count, graph, diagnostics
  );

  if (result != VK_SUCCESS) return result;

  result = vulkan_core10_multiset_begin (runtime);

  if (result != VK_SUCCESS) return result;

  const VkMemoryBarrier host_to_device =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT
      | VK_ACCESS_UNIFORM_READ_BIT
  };

  vkCmdPipelineBarrier (
    runtime->command,
    VK_PIPELINE_STAGE_HOST_BIT,
    VK_PIPELINE_STAGE_TRANSFER_BIT
      | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
    0u, 1u, &host_to_device, 0u, NULL, 0u, NULL
  );

  const uint32_t first_input = graph->stages[0].source_buffer_index;
  const VkBufferCopy upload =
  {
    .srcOffset = 0u,
    .dstOffset = 0u,
    .size = graph->transfer_bytes
  };

  vkCmdCopyBuffer (
    runtime->command,
    runtime->buffers[graph->upload_buffer_index].buffer,
    runtime->buffers[first_input].buffer,
    1u, &upload
  );

  const VkBufferMemoryBarrier uploaded =
  {
    .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .buffer = runtime->buffers[first_input].buffer,
    .offset = 0u,
    .size = graph->transfer_bytes
  };

  vkCmdPipelineBarrier (
    runtime->command,
    VK_PIPELINE_STAGE_TRANSFER_BIT,
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
    0u, 0u, NULL, 1u, &uploaded, 0u, NULL
  );

  vkCmdBindDescriptorSets (
    runtime->command, VK_PIPELINE_BIND_POINT_COMPUTE,
    runtime->pipeline_layout, 0u,
    runtime->set_count, runtime->sets, 0u, NULL
  );

  for (uint32_t stage = 0u; stage < graph->stage_count; stage++)
  {
    const vulkan_core10_multiset_graph_stage_t *step =
      &graph->stages[stage];

    vkCmdBindPipeline (
      runtime->command, VK_PIPELINE_BIND_POINT_COMPUTE,
      runtime->pipelines[step->pipeline_index]
    );

    vkCmdDispatch (
      runtime->command, step->groups[0],
      step->groups[1], step->groups[2]
    );

    if (stage + 1u != graph->stage_count)
    {
      const VkBufferMemoryBarrier dependency =
      {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer =
          runtime->buffers[step->destination_buffer_index].buffer,
        .offset = 0u,
        .size = graph->transfer_bytes
      };

      vkCmdPipelineBarrier (
        runtime->command,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0u, 0u, NULL, 1u, &dependency, 0u, NULL
      );

      fprintf (
        diagnostics,
        "MULTIGRAPH_BARRIER from=%" PRIu32
        " to=%" PRIu32 " buffer=%" PRIu32 "\n",
        stage, stage + 1u, step->destination_buffer_index
      );
    }

    fprintf (
      diagnostics,
      "MULTIGRAPH_DISPATCH stage=%" PRIu32
      " pipeline=%" PRIu32 " input=%" PRIu32
      " output=%" PRIu32 " groups=%" PRIu32 ",%" PRIu32 ",%" PRIu32 "\n",
      stage, step->pipeline_index,
      step->source_buffer_index, step->destination_buffer_index,
      step->groups[0], step->groups[1], step->groups[2]
    );
  }

  const VkMemoryBarrier device_to_transfer =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT
  };

  vkCmdPipelineBarrier (
    runtime->command,
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
    VK_PIPELINE_STAGE_TRANSFER_BIT,
    0u, 1u, &device_to_transfer, 0u, NULL, 0u, NULL
  );

  for (uint32_t stage = 0u; stage < graph->stage_count; stage++)
  {
    const VkBufferCopy download =
    {
      .srcOffset = 0u, .dstOffset = 0u,
      .size = graph->transfer_bytes
    };

    vkCmdCopyBuffer (
      runtime->command,
      runtime->buffers[graph->stages[stage].destination_buffer_index].buffer,
      runtime->buffers[graph->readback_buffer_indices[stage]].buffer,
      1u, &download
    );

    fprintf (
      diagnostics,
      "MULTIGRAPH_READBACK stage=%" PRIu32
      " gpu=%" PRIu32 " host=%" PRIu32 "\n",
      stage, graph->stages[stage].destination_buffer_index,
      graph->readback_buffer_indices[stage]
    );
  }

  const VkMemoryBarrier transfer_to_host =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_HOST_READ_BIT
  };

  vkCmdPipelineBarrier (
    runtime->command, VK_PIPELINE_STAGE_TRANSFER_BIT,
    VK_PIPELINE_STAGE_HOST_BIT,
    0u, 1u, &transfer_to_host, 0u, NULL, 0u, NULL
  );

  return vulkan_core10_multiset_end (runtime);
}
