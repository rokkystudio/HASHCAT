/**
 * Implement resource ownership for variable-size Vulkan 1.0 Compute graphs.
 *
 * Every VkBuffer, VkDeviceMemory, CPU map, descriptor, shader, Compute
 * pipeline, command buffer and fence belongs to one graph instance.
 * All memory indices and limits are queried from the real physical GPU.
 *
 * This module contains no GPU workload implementation, shader reflection
 * or manufacturer-dependent settings. The caller records actual commands.
 */

#include "vulkan-core10-graph.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/**
 * Track a genuine successfully acquired Vulkan handle and optional fault.
 */
static VkBool32 graph_acquired (
  vulkan_core10_graph_t *graph,
  uint32_t fail_after,
  const char *resource)
{
  graph->acquisitions++;

  if (fail_after == 0u || graph->acquisitions != fail_after)
  {
    return VK_FALSE;
  }

  fprintf (
    graph->diagnostics,
    "GRAPH_FAULT acquisition=%" PRIu32
    " resource=%s\n",
    graph->acquisitions, resource
  );

  return VK_TRUE;
}

/**
 * Release owned Vulkan objects in dependency-safe order and clear the graph.
 *
 * vkDeviceWaitIdle completes any outstanding recorded work before pools,
 * buffers or memory become invalid. Command buffer and descriptor set
 * are owned by their pools; mapped buffers are unmapped before freeing.
 */
VkResult vulkan_core10_graph_destroy (
  vulkan_core10_graph_t *graph)
{
  if (graph == NULL) return VK_ERROR_INITIALIZATION_FAILED;

  VkResult idle = VK_SUCCESS;
  uint32_t destroyed_buffers = 0u;
  uint32_t destroyed_memories = 0u;
  uint32_t destroyed_maps = 0u;
  uint32_t destroyed_shaders = 0u;
  uint32_t destroyed_pipelines = 0u;

  if (graph->device != VK_NULL_HANDLE)
  {
    idle = vkDeviceWaitIdle (graph->device);

    if (graph->fence != VK_NULL_HANDLE)
    {
      vkDestroyFence (graph->device, graph->fence, NULL);
    }

    if (graph->command_pool != VK_NULL_HANDLE)
    {
      vkDestroyCommandPool (
        graph->device, graph->command_pool, NULL
      );
    }

    if (graph->descriptor_pool != VK_NULL_HANDLE)
    {
      vkDestroyDescriptorPool (
        graph->device, graph->descriptor_pool, NULL
      );
    }

    for (uint32_t stage = 0u; stage < graph->stage_count; stage++)
    {
      if (graph->pipelines != NULL
       && graph->pipelines[stage] != VK_NULL_HANDLE)
      {
        vkDestroyPipeline (
          graph->device, graph->pipelines[stage], NULL
        );

        destroyed_pipelines++;
      }

      if (graph->shader_modules != NULL
       && graph->shader_modules[stage] != VK_NULL_HANDLE)
      {
        vkDestroyShaderModule (
          graph->device, graph->shader_modules[stage], NULL
        );

        destroyed_shaders++;
      }
    }

    if (graph->pipeline_layout != VK_NULL_HANDLE)
    {
      vkDestroyPipelineLayout (
        graph->device, graph->pipeline_layout, NULL
      );
    }

    if (graph->descriptor_layout != VK_NULL_HANDLE)
    {
      vkDestroyDescriptorSetLayout (
        graph->device, graph->descriptor_layout, NULL
      );
    }

    for (uint32_t index = graph->buffer_count; index != 0u; index--)
    {
      vulkan_core10_graph_buffer_t *buffer = &graph->buffers[index - 1u];

      if (buffer->mapped != NULL)
      {
        vkUnmapMemory (graph->device, buffer->memory);
        destroyed_maps++;
      }

      if (buffer->buffer != VK_NULL_HANDLE)
      {
        vkDestroyBuffer (graph->device, buffer->buffer, NULL);
        destroyed_buffers++;
      }

      if (buffer->memory != VK_NULL_HANDLE)
      {
        vkFreeMemory (graph->device, buffer->memory, NULL);
        destroyed_memories++;
      }
    }

    if (graph->diagnostics != NULL)
    {
      fprintf (
        graph->diagnostics,
        "GRAPH_DESTROY acquisitions=%" PRIu32
        " buffers=%" PRIu32 " memories=%" PRIu32
        " maps=%" PRIu32 " shaders=%" PRIu32
        " pipelines=%" PRIu32
        " submissions=%" PRIu32 " idle_result=%d\n",
        graph->acquisitions,
        destroyed_buffers, destroyed_memories, destroyed_maps,
        destroyed_shaders, destroyed_pipelines,
        graph->completed_submissions, idle
      );
    }

    vkDestroyDevice (graph->device, NULL);
  }

  free (graph->buffers);
  free (graph->shader_modules);
  free (graph->pipelines);
  vulkan_core10_graph_memory_destroy (&graph->memory_plan);

  memset (graph, 0, sizeof (*graph));

  return idle;
}

/**
 * Validate exact buffer role, memory mapping and declared usage flags.
 */
static VkBool32 graph_valid_buffer_spec (
  const vulkan_core10_graph_buffer_spec_t *buffer)
{
  if (buffer->label == NULL || buffer->label[0] == '\0'
   || buffer->size == 0u || buffer->usage == 0u)
  {
    return VK_FALSE;
  }

  if (buffer->role == VULKAN_HEAP_BUFFER_GPU_STORAGE)
  {
    return (buffer->usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) != 0u
      && buffer->map == VK_FALSE ? VK_TRUE : VK_FALSE;
  }

  if (buffer->role == VULKAN_HEAP_BUFFER_HOST_TRANSFER)
  {
    return (buffer->usage
      & (VK_BUFFER_USAGE_TRANSFER_SRC_BIT
       | VK_BUFFER_USAGE_TRANSFER_DST_BIT)) != 0u
      && buffer->map == VK_TRUE ? VK_TRUE : VK_FALSE;
  }

  return VK_FALSE;
}

/**
 * Validate one explicit storage binding against the real Vulkan limits.
 *
 * Descriptor indices are not inferred from buffer order or shader names.
 * Duplicate bindings and a byte range extending past the requested buffer
 * are forbidden even if the device might accept some alternative layout.
 */
static VkResult graph_check_descriptors (
  const vulkan_core10_graph_buffer_spec_t *buffers,
  uint32_t buffer_count,
  const vulkan_core10_graph_descriptor_spec_t *descriptors,
  uint32_t descriptor_count,
  const VkPhysicalDeviceLimits *limits,
  FILE *diagnostics)
{
  if (descriptor_count > limits->maxDescriptorSetStorageBuffers
   || descriptor_count > limits->maxPerStageDescriptorStorageBuffers
   || descriptor_count > limits->maxPerStageResources)
  {
    fprintf (
      diagnostics,
      "GRAPH_REJECT_DESCRIPTOR_COUNT count=%" PRIu32
      " set_limit=%" PRIu32 " stage_limit=%" PRIu32
      " resource_limit=%" PRIu32 "\n",
      descriptor_count, limits->maxDescriptorSetStorageBuffers,
      limits->maxPerStageDescriptorStorageBuffers,
      limits->maxPerStageResources
    );

    return VK_ERROR_FEATURE_NOT_PRESENT;
  }

  for (uint32_t index = 0u; index < descriptor_count; index++)
  {
    const vulkan_core10_graph_descriptor_spec_t *entry =
      &descriptors[index];

    if (entry->buffer_index >= buffer_count
     || (buffers[entry->buffer_index].usage
       & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) == 0u
     || entry->range == 0u
     || entry->range > limits->maxStorageBufferRange
     || entry->offset > buffers[entry->buffer_index].size
     || entry->range >
       buffers[entry->buffer_index].size - entry->offset
     || limits->minStorageBufferOffsetAlignment == 0u
     || entry->offset % limits->minStorageBufferOffsetAlignment != 0u)
    {
      fprintf (
        diagnostics,
        "GRAPH_REJECT_DESCRIPTOR_RANGE index=%" PRIu32
        " binding=%" PRIu32 " buffer=%" PRIu32
        " offset=%" PRIu64 " range=%" PRIu64 "\n",
        index, entry->binding, entry->buffer_index,
        (uint64_t) entry->offset, (uint64_t) entry->range
      );

      return VK_ERROR_FEATURE_NOT_PRESENT;
    }

    for (uint32_t previous = 0u; previous < index; previous++)
    {
      if (descriptors[previous].binding == entry->binding)
      {
        fprintf (
          diagnostics,
          "GRAPH_REJECT_DUPLICATE_BINDING binding=%" PRIu32 "\n",
          entry->binding
        );

        return VK_ERROR_INITIALIZATION_FAILED;
      }
    }
  }

  return VK_SUCCESS;
}

/**
 * Construct explicitly declared Vulkan 1.0 buffers, descriptors and shaders.
 *
 * The application supplies every count, buffer usage, descriptor binding
 * and shader program independently. The Vulkan driver supplies exact memory
 * bitmasks, limits and heap sizes. A failure at any stage runs full teardown.
 */
VkResult vulkan_core10_graph_create (
  vulkan_core10_graph_t *graph,
  const vulkan_auto_device_t *selected,
  const vulkan_core10_graph_buffer_spec_t *specs,
  uint32_t buffer_count,
  const vulkan_core10_graph_descriptor_spec_t *descriptors,
  uint32_t descriptor_count,
  const vulkan_core10_graph_shader_spec_t *shaders,
  uint32_t stage_count,
  uint32_t fail_after,
  FILE *diagnostics)
{
  if (graph == NULL || selected == NULL || specs == NULL
   || descriptors == NULL || shaders == NULL || diagnostics == NULL
   || selected->physical == VK_NULL_HANDLE || buffer_count == 0u
   || descriptor_count == 0u || stage_count == 0u
   || graph->device != VK_NULL_HANDLE || graph->buffers != NULL
   || graph->shader_modules != NULL || graph->pipelines != NULL)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  const VkPhysicalDeviceLimits *limits = &selected->properties.limits;

  if (limits->maxBoundDescriptorSets < 1u)
  {
    return VK_ERROR_FEATURE_NOT_PRESENT;
  }

  uint32_t mapped_count = 0u;

  for (uint32_t index = 0u; index < buffer_count; index++)
  {
    if (graph_valid_buffer_spec (&specs[index]) == VK_FALSE)
    {
      fprintf (
        diagnostics,
        "GRAPH_REJECT_BUFFER_SPEC index=%" PRIu32 "\n", index
      );

      return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (specs[index].map == VK_TRUE) mapped_count++;
  }

  const uint64_t maximum_acquisitions =
    UINT64_C (8) + UINT64_C (2) * buffer_count
    + UINT64_C (2) * stage_count + mapped_count;

  if (maximum_acquisitions > UINT32_MAX
   || fail_after > maximum_acquisitions)
  {
    fprintf (
      diagnostics,
      "GRAPH_REJECT_FAULT_POINT requested=%" PRIu32
      " maximum=%" PRIu64 "\n",
      fail_after, maximum_acquisitions
    );

    return VK_ERROR_INITIALIZATION_FAILED;
  }

  for (uint32_t stage = 0u; stage < stage_count; stage++)
  {
    if (shaders[stage].words == NULL || shaders[stage].bytes < 20u
     || (shaders[stage].bytes & 3u) != 0u
     || shaders[stage].words[0] != UINT32_C (0x07230203)
     || shaders[stage].words[1] != UINT32_C (0x00010000))
    {
      fprintf (
        diagnostics,
        "GRAPH_REJECT_SHADER index=%" PRIu32
        " spirv_required=1.0\n",
        stage
      );

      return VK_ERROR_INITIALIZATION_FAILED;
    }
  }

  VkResult result = graph_check_descriptors (
    specs, buffer_count, descriptors,
    descriptor_count, limits, diagnostics
  );

  if (result != VK_SUCCESS) return result;

  if ((size_t) buffer_count > SIZE_MAX / sizeof (*graph->buffers)
   || (size_t) stage_count > SIZE_MAX / sizeof (*graph->pipelines)
   || (size_t) stage_count > SIZE_MAX / sizeof (*graph->shader_modules)
   || (size_t) descriptor_count > SIZE_MAX / sizeof (VkWriteDescriptorSet)
   || (size_t) descriptor_count > SIZE_MAX / sizeof (VkDescriptorBufferInfo)
   || (size_t) descriptor_count > SIZE_MAX / sizeof (VkDescriptorSetLayoutBinding))
  {
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }

  memset (graph, 0, sizeof (*graph));
  graph->buffer_count = buffer_count;
  graph->descriptor_count = descriptor_count;
  graph->stage_count = stage_count;
  graph->mapped_count = mapped_count;
  graph->limits = *limits;
  graph->queue_family = selected->compute_queue_family;
  graph->diagnostics = diagnostics;

  vulkan_heap_allocation_request_t *requests =
    (vulkan_heap_allocation_request_t *) calloc (
      buffer_count, sizeof (*requests)
    );
  VkDescriptorSetLayoutBinding *bindings =
    (VkDescriptorSetLayoutBinding *) calloc (
      descriptor_count, sizeof (*bindings)
    );
  VkDescriptorBufferInfo *buffer_infos =
    (VkDescriptorBufferInfo *) calloc (
      descriptor_count, sizeof (*buffer_infos)
    );
  VkWriteDescriptorSet *writes =
    (VkWriteDescriptorSet *) calloc (
      descriptor_count, sizeof (*writes)
    );

  graph->buffers = (vulkan_core10_graph_buffer_t *) calloc (
    buffer_count, sizeof (*graph->buffers)
  );
  graph->shader_modules = (VkShaderModule *) calloc (
    stage_count, sizeof (*graph->shader_modules)
  );
  graph->pipelines = (VkPipeline *) calloc (
    stage_count, sizeof (*graph->pipelines)
  );

  if (requests == NULL || bindings == NULL
   || buffer_infos == NULL || writes == NULL
   || graph->buffers == NULL || graph->shader_modules == NULL
   || graph->pipelines == NULL)
  {
    result = VK_ERROR_OUT_OF_HOST_MEMORY;

    goto cleanup;
  }

  const float priority = 1.0f;
  const VkDeviceQueueCreateInfo queue_info =
  {
    .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
    .queueFamilyIndex = selected->compute_queue_family,
    .queueCount = 1u,
    .pQueuePriorities = &priority
  };
  const VkDeviceCreateInfo device_info =
  {
    .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
    .queueCreateInfoCount = 1u,
    .pQueueCreateInfos = &queue_info,
    .enabledExtensionCount = 0u,
    .pEnabledFeatures = NULL
  };

#define GRAPH_TRY(call, name) \
  do \
  { \
    result = (call); \
    if (result != VK_SUCCESS) \
    { \
      fprintf (diagnostics, \
        "GRAPH_VULKAN_ERROR resource=%s call=%s result=%d\n", \
        name, #call, result); \
      goto cleanup; \
    } \
    if (graph_acquired (graph, fail_after, name) == VK_TRUE) \
    { \
      result = VK_ERROR_INITIALIZATION_FAILED; \
      goto cleanup; \
    } \
  } while (0)

  GRAPH_TRY (vkCreateDevice (
    selected->physical, &device_info, NULL, &graph->device
  ), "device");

  vkGetDeviceQueue (
    graph->device, selected->compute_queue_family, 0u, &graph->queue
  );

  for (uint32_t index = 0u; index < buffer_count; index++)
  {
    const VkBufferCreateInfo buffer_info =
    {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = specs[index].size,
      .usage = specs[index].usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };

    GRAPH_TRY (vkCreateBuffer (
      graph->device, &buffer_info,
      NULL, &graph->buffers[index].buffer
    ), "buffer");

    graph->buffers[index].logical_size = specs[index].size;
    vkGetBufferMemoryRequirements (
      graph->device, graph->buffers[index].buffer,
      &graph->buffers[index].requirements
    );

    requests[index] = (vulkan_heap_allocation_request_t)
    {
      .label = specs[index].label,
      .requirements = graph->buffers[index].requirements,
      .role = specs[index].role,
      .forbidden_flags = specs[index].forbidden_flags
    };

    fprintf (
      diagnostics,
      "GRAPH_REQUIREMENTS index=%" PRIu32
      " label=%s size=%" PRIu64
      " alignment=%" PRIu64
      " bits=0x%08" PRIx32 "\n",
      index, specs[index].label,
      (uint64_t) requests[index].requirements.size,
      (uint64_t) requests[index].requirements.alignment,
      requests[index].requirements.memoryTypeBits
    );

    if (requests[index].requirements.size < specs[index].size)
    {
      result = VK_ERROR_INITIALIZATION_FAILED;

      goto cleanup;
    }
  }

  result = vulkan_core10_graph_memory_plan (
    &selected->memory_properties,
    limits->maxMemoryAllocationCount,
    0u, requests, buffer_count,
    &graph->memory_plan, diagnostics
  );

  if (result != VK_SUCCESS) goto cleanup;

  for (uint32_t index = 0u; index < buffer_count; index++)
  {
    const VkMemoryAllocateInfo allocate =
    {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requests[index].requirements.size,
      .memoryTypeIndex = graph->memory_plan.memory_type_indices[index]
    };

    GRAPH_TRY (vkAllocateMemory (
      graph->device, &allocate, NULL, &graph->buffers[index].memory
    ), "memory");

    result = vkBindBufferMemory (
      graph->device,
      graph->buffers[index].buffer,
      graph->buffers[index].memory, 0u
    );

    if (result != VK_SUCCESS)
    {
      fprintf (
        diagnostics,
        "GRAPH_BIND_FAILED index=%" PRIu32 " result=%d\n",
        index, result
      );

      goto cleanup;
    }
  }

  for (uint32_t index = 0u; index < buffer_count; index++)
  {
    if (specs[index].map == VK_FALSE) continue;

    GRAPH_TRY (vkMapMemory (
      graph->device, graph->buffers[index].memory,
      0u, VK_WHOLE_SIZE, 0u, &graph->buffers[index].mapped
    ), "mapped-buffer");

    fprintf (
      diagnostics,
      "GRAPH_MAPPED index=%" PRIu32
      " type=%" PRIu32 " coherent=%u\n",
      index, graph->memory_plan.memory_type_indices[index],
      graph->memory_plan.coherent[index]
    );
  }

  for (uint32_t index = 0u; index < descriptor_count; index++)
  {
    bindings[index] = (VkDescriptorSetLayoutBinding)
    {
      .binding = descriptors[index].binding,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 1u,
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
    };
  }

  const VkDescriptorSetLayoutCreateInfo descriptor_layout_info =
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
    .bindingCount = descriptor_count,
    .pBindings = bindings
  };

  GRAPH_TRY (vkCreateDescriptorSetLayout (
    graph->device, &descriptor_layout_info,
    NULL, &graph->descriptor_layout
  ), "descriptor-layout");

  const VkPipelineLayoutCreateInfo pipeline_layout_info =
  {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    .setLayoutCount = 1u,
    .pSetLayouts = &graph->descriptor_layout
  };

  GRAPH_TRY (vkCreatePipelineLayout (
    graph->device, &pipeline_layout_info,
    NULL, &graph->pipeline_layout
  ), "pipeline-layout");

  for (uint32_t stage = 0u; stage < stage_count; stage++)
  {
    const VkShaderModuleCreateInfo module =
    {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = shaders[stage].bytes,
      .pCode = shaders[stage].words
    };

    GRAPH_TRY (vkCreateShaderModule (
      graph->device, &module, NULL,
      &graph->shader_modules[stage]
    ), "shader-module");

    const VkComputePipelineCreateInfo pipeline =
    {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage =
      {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = graph->shader_modules[stage],
        .pName = "main"
      },
      .layout = graph->pipeline_layout
    };

    GRAPH_TRY (vkCreateComputePipelines (
      graph->device, VK_NULL_HANDLE,
      1u, &pipeline, NULL, &graph->pipelines[stage]
    ), "compute-pipeline");
  }

  const VkDescriptorPoolSize descriptor_pool_size =
  {
    .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
    .descriptorCount = descriptor_count
  };
  const VkDescriptorPoolCreateInfo pool_info =
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
    .maxSets = 1u,
    .poolSizeCount = 1u,
    .pPoolSizes = &descriptor_pool_size
  };

  GRAPH_TRY (vkCreateDescriptorPool (
    graph->device, &pool_info, NULL, &graph->descriptor_pool
  ), "descriptor-pool");

  const VkDescriptorSetAllocateInfo descriptor_allocation =
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
    .descriptorPool = graph->descriptor_pool,
    .descriptorSetCount = 1u,
    .pSetLayouts = &graph->descriptor_layout
  };

  GRAPH_TRY (vkAllocateDescriptorSets (
    graph->device, &descriptor_allocation, &graph->descriptor_set
  ), "descriptor-set");

  for (uint32_t index = 0u; index < descriptor_count; index++)
  {
    const uint32_t buffer_index = descriptors[index].buffer_index;

    buffer_infos[index] = (VkDescriptorBufferInfo)
    {
      .buffer = graph->buffers[buffer_index].buffer,
      .offset = descriptors[index].offset,
      .range = descriptors[index].range
    };
    writes[index] = (VkWriteDescriptorSet)
    {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = graph->descriptor_set,
      .dstBinding = descriptors[index].binding,
      .descriptorCount = 1u,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .pBufferInfo = &buffer_infos[index]
    };
  }

  vkUpdateDescriptorSets (
    graph->device, descriptor_count, writes, 0u, NULL
  );

  const VkCommandPoolCreateInfo command_pool_info =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
    .queueFamilyIndex = selected->compute_queue_family
  };

  GRAPH_TRY (vkCreateCommandPool (
    graph->device, &command_pool_info,
    NULL, &graph->command_pool
  ), "command-pool");

  const VkCommandBufferAllocateInfo command_info =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
    .commandPool = graph->command_pool,
    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
    .commandBufferCount = 1u
  };

  GRAPH_TRY (vkAllocateCommandBuffers (
    graph->device, &command_info, &graph->command
  ), "command-buffer");

  const VkFenceCreateInfo fence_info =
  {
    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
  };

  GRAPH_TRY (vkCreateFence (
    graph->device, &fence_info, NULL, &graph->fence
  ), "fence");

#undef GRAPH_TRY

  fprintf (
    diagnostics,
    "GRAPH_READY acquisitions=%" PRIu32
    " buffers=%" PRIu32 " descriptors=%" PRIu32
    " stages=%" PRIu32 " maps=%" PRIu32 " core=1.0\n",
    graph->acquisitions, buffer_count,
    descriptor_count, stage_count, mapped_count
  );

cleanup:
  free (requests);
  free (bindings);
  free (buffer_infos);
  free (writes);

  if (result != VK_SUCCESS)
  {
    const VkResult idle = vulkan_core10_graph_destroy (graph);

    if (idle != VK_SUCCESS)
    {
      fprintf (
        diagnostics,
        "GRAPH_CLEANUP_ERROR idle_result=%d error=%d\n",
        idle, result
      );
    }
  }

  return result;
}

/**
 * Begin Vulkan 1.0 command recording only when no recording is in progress.
 */
VkResult vulkan_core10_graph_begin (
  vulkan_core10_graph_t *graph)
{
  if (graph == NULL || graph->device == VK_NULL_HANDLE
   || graph->command == VK_NULL_HANDLE
   || graph->recording == VK_TRUE || graph->recorded == VK_TRUE)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  const VkCommandBufferBeginInfo begin =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags = 0u
  };

  const VkResult result = vkBeginCommandBuffer (
    graph->command, &begin
  );

  if (result == VK_SUCCESS) graph->recording = VK_TRUE;

  return result;
}

/**
 * Finish the one reusable command recording and track its executable state.
 */
VkResult vulkan_core10_graph_end (
  vulkan_core10_graph_t *graph)
{
  if (graph == NULL || graph->command == VK_NULL_HANDLE
   || graph->recording != VK_TRUE || graph->recorded == VK_TRUE)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  const VkResult result = vkEndCommandBuffer (graph->command);

  if (result == VK_SUCCESS)
  {
    graph->recording = VK_FALSE;
    graph->recorded = VK_TRUE;
  }

  return result;
}

/**
 * Submit one previously recorded command buffer and wait for its GPU fence.
 *
 * The same command buffer may run repeatedly with updated mapped input only
 * after the last submission completed. Vulkan failures remain visible.
 */
VkResult vulkan_core10_graph_submit_and_wait (
  vulkan_core10_graph_t *graph)
{
  if (graph == NULL || graph->device == VK_NULL_HANDLE
   || graph->fence == VK_NULL_HANDLE || graph->recorded != VK_TRUE
   || graph->pending == VK_TRUE)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  VkResult result = VK_SUCCESS;

  if (graph->completed_submissions != 0u)
  {
    result = vkResetFences (
      graph->device, 1u, &graph->fence
    );

    if (result != VK_SUCCESS) return result;
  }

  const VkSubmitInfo submit =
  {
    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    .commandBufferCount = 1u,
    .pCommandBuffers = &graph->command
  };

  result = vkQueueSubmit (
    graph->queue, 1u, &submit, graph->fence
  );

  if (result != VK_SUCCESS) return result;

  graph->pending = VK_TRUE;

  result = vkWaitForFences (
    graph->device, 1u, &graph->fence,
    VK_TRUE, UINT64_C (30000000000)
  );

  if (result != VK_SUCCESS) return result;

  graph->pending = VK_FALSE;
  graph->completed_submissions++;

  fprintf (
    graph->diagnostics,
    "GRAPH_SUBMIT completed=%" PRIu32
    " reused=%u result=0\n",
    graph->completed_submissions,
    graph->completed_submissions > 1u ? 1u : 0u
  );

  return VK_SUCCESS;
}

/**
 * Synchronize one exactly mapped HOST_VISIBLE Vulkan allocation.
 *
 * Coherent allocations require no driver cache call. Noncoherent buffers
 * flush on host-to-device uploads or invalidate after device-to-host work.
 */
VkResult vulkan_core10_graph_sync (
  vulkan_core10_graph_t *graph,
  uint32_t buffer_index,
  VkBool32 invalidate)
{
  if (graph == NULL || graph->device == VK_NULL_HANDLE
   || buffer_index >= graph->buffer_count
   || graph->buffers[buffer_index].mapped == NULL
   || graph->buffers[buffer_index].memory == VK_NULL_HANDLE)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  const vulkan_core10_graph_buffer_t *buffer =
    &graph->buffers[buffer_index];
  const vulkan_auto_memory_plan_t plan =
  {
    .allocation_bytes = buffer->requirements.size,
    .memory_type_index =
      graph->memory_plan.memory_type_indices[buffer_index],
    .coherent = graph->memory_plan.coherent[buffer_index],
    .non_coherent_atom_size = graph->limits.nonCoherentAtomSize
  };

  const VkResult result = vulkan_auto_sync_mapped_memory (
    graph->device, buffer->memory, &plan, invalidate
  );

  fprintf (
    graph->diagnostics,
    "GRAPH_SYNC index=%" PRIu32
    " operation=%s coherent=%u"
    " driver_call=%u atom=%" PRIu64 " result=%d\n",
    buffer_index,
    invalidate == VK_TRUE ? "invalidate" : "flush",
    plan.coherent, plan.coherent == VK_TRUE ? 0u : 1u,
    (uint64_t) plan.non_coherent_atom_size, result
  );

  return result;
}
