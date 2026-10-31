/**
 * Manage actual Vulkan 1.0 Compute pipelines with multiple descriptor sets.
 *
 * All buffers, descriptor layouts/sets, shader modules and pipelines use
 * explicit caller requirements and real physical device limits. Mapped
 * noncoherent memory uses Vulkan cache synchronization; all submitted
 * commands are completed by one reusable fence.
 *
 * This is generic Vulkan resource ownership for public integer Compute
 * diagnostics, not a cryptographic or password-processing implementation.
 */

#include "vulkan-core10-multiset.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/**
 * Count a genuinely acquired Vulkan object and optionally inject an error.
 *
 * A fault occurs after its resource acquisition completes successfully.
 * The caller then destroys every partially owned Vulkan resource.
 */
static VkBool32 multiset_acquired (
  vulkan_core10_multiset_t *runtime,
  uint32_t fail_after,
  const char *name)
{
  runtime->acquisitions++;

  if (fail_after == 0u || runtime->acquisitions != fail_after)
  {
    return VK_FALSE;
  }

  fprintf (
    runtime->diagnostics,
    "MULTI_INJECT_FAULT acquisition=%" PRIu32 " resource=%s\n",
    runtime->acquisitions, name
  );

  return VK_TRUE;
}

/**
 * Destroy all owned Vulkan objects in dependency-safe order.
 *
 * After device idle, destroys command/fence and descriptor pools, Compute
 * pipelines and modules, all set layouts, mapped memory and Vulkan buffers.
 * Host arrays and the joint memory plan are released even on early failure.
 */
VkResult vulkan_core10_multiset_destroy (
  vulkan_core10_multiset_t *runtime)
{
  if (runtime == NULL) return VK_ERROR_INITIALIZATION_FAILED;

  VkResult idle = VK_SUCCESS;
  uint32_t released_buffers = 0u;
  uint32_t released_memories = 0u;
  uint32_t released_maps = 0u;
  uint32_t released_layouts = 0u;
  uint32_t released_sets = 0u;
  uint32_t released_shaders = 0u;
  uint32_t released_pipelines = 0u;

  if (runtime->device != VK_NULL_HANDLE)
  {
    idle = vkDeviceWaitIdle (runtime->device);

    if (runtime->fence != VK_NULL_HANDLE)
    {
      vkDestroyFence (runtime->device, runtime->fence, NULL);
    }

    if (runtime->command_pool != VK_NULL_HANDLE)
    {
      vkDestroyCommandPool (
        runtime->device, runtime->command_pool, NULL
      );
    }

    for (uint32_t set = 0u; set < runtime->set_count; set++)
    {
      if (runtime->sets != NULL
       && runtime->sets[set] != VK_NULL_HANDLE)
      {
        released_sets++;
      }
    }

    if (runtime->descriptor_pool != VK_NULL_HANDLE)
    {
      vkDestroyDescriptorPool (
        runtime->device, runtime->descriptor_pool, NULL
      );
    }

    for (uint32_t stage = 0u; stage < runtime->stage_count; stage++)
    {
      if (runtime->pipelines != NULL
       && runtime->pipelines[stage] != VK_NULL_HANDLE)
      {
        vkDestroyPipeline (
          runtime->device, runtime->pipelines[stage], NULL
        );

        released_pipelines++;
      }

      if (runtime->shaders != NULL
       && runtime->shaders[stage] != VK_NULL_HANDLE)
      {
        vkDestroyShaderModule (
          runtime->device, runtime->shaders[stage], NULL
        );

        released_shaders++;
      }
    }

    if (runtime->pipeline_layout != VK_NULL_HANDLE)
    {
      vkDestroyPipelineLayout (
        runtime->device, runtime->pipeline_layout, NULL
      );
    }

    for (uint32_t set = 0u; set < runtime->set_count; set++)
    {
      if (runtime->layouts != NULL
       && runtime->layouts[set] != VK_NULL_HANDLE)
      {
        vkDestroyDescriptorSetLayout (
          runtime->device, runtime->layouts[set], NULL
        );

        released_layouts++;
      }
    }

    for (uint32_t index = runtime->buffer_count; index != 0u; index--)
    {
      vulkan_core10_multiset_buffer_t *buffer =
        &runtime->buffers[index - 1u];

      if (buffer->mapped != NULL)
      {
        vkUnmapMemory (runtime->device, buffer->memory);
        released_maps++;
      }

      if (buffer->buffer != VK_NULL_HANDLE)
      {
        vkDestroyBuffer (
          runtime->device, buffer->buffer, NULL
        );

        released_buffers++;
      }

      if (buffer->memory != VK_NULL_HANDLE)
      {
        vkFreeMemory (
          runtime->device, buffer->memory, NULL
        );

        released_memories++;
      }
    }

    if (runtime->diagnostics != NULL)
    {
      fprintf (
        runtime->diagnostics,
        "MULTI_DESTROY acquisitions=%" PRIu32
        " buffers=%" PRIu32 " memories=%" PRIu32
        " maps=%" PRIu32 " layouts=%" PRIu32
        " sets=%" PRIu32 " shaders=%" PRIu32
        " pipelines=%" PRIu32
        " submissions=%" PRIu32 " idle_result=%d\n",
        runtime->acquisitions,
        released_buffers, released_memories, released_maps,
        released_layouts, released_sets, released_shaders,
        released_pipelines, runtime->completed_submissions, idle
      );
    }

    vkDestroyDevice (runtime->device, NULL);
  }

  free (runtime->buffers);
  free (runtime->layouts);
  free (runtime->sets);
  free (runtime->shaders);
  free (runtime->pipelines);
  vulkan_core10_graph_memory_destroy (&runtime->memory_plan);

  memset (runtime, 0, sizeof (*runtime));

  return idle;
}

/**
 * Validate a caller-declared GPU or mapped HOST_VISIBLE Vulkan buffer.
 *
 * Mapped uniform-buffer allocations need no transfer usage, while CPU
 * staging transfer allocations must declare the transfer direction.
 */
static VkBool32 multiset_valid_buffer (
  const vulkan_core10_multiset_buffer_spec_t *spec)
{
  if (spec->label == NULL || spec->label[0] == '\0'
   || spec->size == 0u || spec->usage == 0u)
  {
    return VK_FALSE;
  }

  if (spec->role == VULKAN_HEAP_BUFFER_GPU_STORAGE)
  {
    return spec->map == VK_FALSE
      && (spec->usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) != 0u
      ? VK_TRUE : VK_FALSE;
  }

  if (spec->role == VULKAN_HEAP_BUFFER_HOST_TRANSFER)
  {
    return spec->map == VK_TRUE
      && (spec->usage
       & (VK_BUFFER_USAGE_TRANSFER_SRC_BIT
        | VK_BUFFER_USAGE_TRANSFER_DST_BIT
        | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT)) != 0u
      ? VK_TRUE : VK_FALSE;
  }

  return VK_FALSE;
}

/**
 * Verify each host allocation count before allocating dynamic runtime arrays.
 */
static VkBool32 multiset_counts_fit (
  uint32_t buffer_count,
  uint32_t binding_count,
  uint32_t set_count,
  uint32_t stage_count)
{
#define MULTI_COUNT_FITS(count, type) \
  ((size_t) (count) <= SIZE_MAX / sizeof (type))

  return
    MULTI_COUNT_FITS (buffer_count, vulkan_core10_multiset_buffer_t)
    && MULTI_COUNT_FITS (buffer_count, vulkan_heap_allocation_request_t)
    && MULTI_COUNT_FITS (buffer_count, vulkan_core10_descriptor_buffer_t)
    && MULTI_COUNT_FITS (binding_count, VkDescriptorSetLayoutBinding)
    && MULTI_COUNT_FITS (binding_count, VkDescriptorBufferInfo)
    && MULTI_COUNT_FITS (binding_count, VkWriteDescriptorSet)
    && MULTI_COUNT_FITS (set_count, VkDescriptorSetLayout)
    && MULTI_COUNT_FITS (set_count, VkDescriptorSet)
    && MULTI_COUNT_FITS (stage_count, VkShaderModule)
    && MULTI_COUNT_FITS (stage_count, VkPipeline)
    ? VK_TRUE : VK_FALSE;

#undef MULTI_COUNT_FITS
}

/**
 * Create the complete Vulkan 1.0 resource graph with exact descriptor sets.
 *
 * Descriptor set layouts and descriptor sets are allocated one by one to
 * support deterministic per-object partial-cleanup tests. The pipeline
 * layout receives the complete ordered set-layout array. Every Vulkan
 * driver call returns its actual VkResult or aborts resource creation.
 */
VkResult vulkan_core10_multiset_create (
  vulkan_core10_multiset_t *runtime,
  const vulkan_auto_device_t *selected,
  const vulkan_core10_multiset_buffer_spec_t *specs,
  uint32_t buffer_count,
  const vulkan_core10_descriptor_binding_t *bindings,
  uint32_t binding_count,
  uint32_t set_count,
  const vulkan_core10_multiset_shader_spec_t *shaders,
  uint32_t stage_count,
  uint32_t fail_after,
  FILE *diagnostics)
{
  if (runtime == NULL || selected == NULL || specs == NULL
   || bindings == NULL || shaders == NULL || diagnostics == NULL
   || selected->physical == VK_NULL_HANDLE || buffer_count == 0u
   || binding_count == 0u || set_count == 0u || stage_count == 0u
   || runtime->device != VK_NULL_HANDLE
   || runtime->buffers != NULL || runtime->layouts != NULL)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  const VkPhysicalDeviceLimits *limits = &selected->properties.limits;
  uint32_t map_count = 0u;

  for (uint32_t index = 0u; index < buffer_count; index++)
  {
    if (multiset_valid_buffer (&specs[index]) != VK_TRUE)
    {
      fprintf (
        diagnostics,
        "MULTI_REJECT_BUFFER index=%" PRIu32 "\n", index
      );

      return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (specs[index].map == VK_TRUE) map_count++;
  }

  if (multiset_counts_fit (
    buffer_count, binding_count, set_count, stage_count
  ) != VK_TRUE)
  {
    fprintf (
      diagnostics,
      "MULTI_REJECT_HOST_ARRAY_COUNT buffers=%" PRIu32
      " bindings=%" PRIu32 " sets=%" PRIu32 " stages=%" PRIu32 "\n",
      buffer_count, binding_count, set_count, stage_count
    );

    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }

  const uint64_t maximum_acquisitions =
    UINT64_C (6) + UINT64_C (2) * buffer_count
    + map_count + UINT64_C (2) * set_count
    + UINT64_C (2) * stage_count;

  if (maximum_acquisitions > UINT32_MAX
   || fail_after > maximum_acquisitions)
  {
    fprintf (
      diagnostics,
      "MULTI_REJECT_FAULT requested=%" PRIu32
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
        "MULTI_REJECT_SPIRV index=%" PRIu32
        " expected_version=1.0\n", stage
      );

      return VK_ERROR_INITIALIZATION_FAILED;
    }
  }

  vulkan_core10_descriptor_buffer_t *policy_buffers =
    (vulkan_core10_descriptor_buffer_t *) calloc (
      buffer_count, sizeof (*policy_buffers)
    );

  if (policy_buffers == NULL) return VK_ERROR_OUT_OF_HOST_MEMORY;

  for (uint32_t index = 0u; index < buffer_count; index++)
  {
    policy_buffers[index] = (vulkan_core10_descriptor_buffer_t)
    {
      .size = specs[index].size,
      .usage = specs[index].usage
    };
  }

  vulkan_core10_descriptor_counts_t types = { 0 };
  VkResult result = vulkan_core10_descriptor_validate (
    limits, policy_buffers, buffer_count, bindings, binding_count,
    set_count, &types, diagnostics
  );

  free (policy_buffers);

  if (result != VK_SUCCESS) return result;

  memset (runtime, 0, sizeof (*runtime));
  runtime->buffer_count = buffer_count;
  runtime->map_count = map_count;
  runtime->set_count = set_count;
  runtime->binding_count = binding_count;
  runtime->stage_count = stage_count;
  runtime->limits = *limits;
  runtime->diagnostics = diagnostics;

  vulkan_heap_allocation_request_t *requests =
    (vulkan_heap_allocation_request_t *) calloc (
      buffer_count, sizeof (*requests)
    );
  VkDescriptorSetLayoutBinding *layout_bindings =
    (VkDescriptorSetLayoutBinding *) calloc (
      binding_count, sizeof (*layout_bindings)
    );
  VkDescriptorBufferInfo *buffer_infos =
    (VkDescriptorBufferInfo *) calloc (
      binding_count, sizeof (*buffer_infos)
    );
  VkWriteDescriptorSet *writes =
    (VkWriteDescriptorSet *) calloc (
      binding_count, sizeof (*writes)
    );

  runtime->buffers = (vulkan_core10_multiset_buffer_t *) calloc (
    buffer_count, sizeof (*runtime->buffers)
  );
  runtime->layouts = (VkDescriptorSetLayout *) calloc (
    set_count, sizeof (*runtime->layouts)
  );
  runtime->sets = (VkDescriptorSet *) calloc (
    set_count, sizeof (*runtime->sets)
  );
  runtime->shaders = (VkShaderModule *) calloc (
    stage_count, sizeof (*runtime->shaders)
  );
  runtime->pipelines = (VkPipeline *) calloc (
    stage_count, sizeof (*runtime->pipelines)
  );

  if (requests == NULL || layout_bindings == NULL
   || buffer_infos == NULL || writes == NULL
   || runtime->buffers == NULL || runtime->layouts == NULL
   || runtime->sets == NULL || runtime->shaders == NULL
   || runtime->pipelines == NULL)
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

#define MULTI_TRY(command, label) \
  do \
  { \
    result = (command); \
    if (result != VK_SUCCESS) \
    { \
      fprintf (diagnostics, \
        "MULTI_VULKAN_ERROR resource=%s call=%s result=%d\n", \
        label, #command, result); \
      goto cleanup; \
    } \
    if (multiset_acquired (runtime, fail_after, label) == VK_TRUE) \
    { \
      result = VK_ERROR_INITIALIZATION_FAILED; \
      goto cleanup; \
    } \
  } while (0)

  MULTI_TRY (vkCreateDevice (
    selected->physical, &device_info, NULL, &runtime->device
  ), "device");

  vkGetDeviceQueue (
    runtime->device, selected->compute_queue_family, 0u, &runtime->queue
  );

  for (uint32_t index = 0u; index < buffer_count; index++)
  {
    const VkBufferCreateInfo create_buffer =
    {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = specs[index].size,
      .usage = specs[index].usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };

    MULTI_TRY (vkCreateBuffer (
      runtime->device, &create_buffer,
      NULL, &runtime->buffers[index].buffer
    ), "buffer");

    runtime->buffers[index].logical_size = specs[index].size;

    vkGetBufferMemoryRequirements (
      runtime->device, runtime->buffers[index].buffer,
      &runtime->buffers[index].requirements
    );

    requests[index] = (vulkan_heap_allocation_request_t)
    {
      .label = specs[index].label,
      .requirements = runtime->buffers[index].requirements,
      .role = specs[index].role,
      .forbidden_flags = specs[index].forbidden_flags
    };

    if (requests[index].requirements.size < specs[index].size)
    {
      result = VK_ERROR_INITIALIZATION_FAILED;

      goto cleanup;
    }

    fprintf (
      diagnostics,
      "MULTI_REQUIREMENTS index=%" PRIu32
      " label=%s size=%" PRIu64
      " alignment=%" PRIu64 " bits=0x%08" PRIx32 "\n",
      index, specs[index].label,
      (uint64_t) requests[index].requirements.size,
      (uint64_t) requests[index].requirements.alignment,
      requests[index].requirements.memoryTypeBits
    );
  }

  result = vulkan_core10_graph_memory_plan (
    &selected->memory_properties,
    limits->maxMemoryAllocationCount,
    0u, requests, buffer_count,
    &runtime->memory_plan, diagnostics
  );

  if (result != VK_SUCCESS) goto cleanup;

  for (uint32_t index = 0u; index < buffer_count; index++)
  {
    const VkMemoryAllocateInfo allocation =
    {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requests[index].requirements.size,
      .memoryTypeIndex = runtime->memory_plan.memory_type_indices[index]
    };

    MULTI_TRY (vkAllocateMemory (
      runtime->device, &allocation,
      NULL, &runtime->buffers[index].memory
    ), "memory");

    result = vkBindBufferMemory (
      runtime->device, runtime->buffers[index].buffer,
      runtime->buffers[index].memory, 0u
    );

    if (result != VK_SUCCESS)
    {
      fprintf (
        diagnostics, "MULTI_BIND_FAILED index=%" PRIu32
        " result=%d\n", index, result
      );

      goto cleanup;
    }
  }

  for (uint32_t index = 0u; index < buffer_count; index++)
  {
    if (specs[index].map != VK_TRUE) continue;

    MULTI_TRY (vkMapMemory (
      runtime->device, runtime->buffers[index].memory,
      0u, VK_WHOLE_SIZE, 0u, &runtime->buffers[index].mapped
    ), "mapped-buffer");

    fprintf (
      diagnostics,
      "MULTI_MAPPED index=%" PRIu32
      " type=%" PRIu32 " coherent=%u\n",
      index, runtime->memory_plan.memory_type_indices[index],
      runtime->memory_plan.coherent[index]
    );
  }

  for (uint32_t set = 0u; set < set_count; set++)
  {
    uint32_t items = 0u;

    for (uint32_t index = 0u; index < binding_count; index++)
    {
      if (bindings[index].set != set) continue;

      layout_bindings[items++] = (VkDescriptorSetLayoutBinding)
      {
        .binding = bindings[index].binding,
        .descriptorType = bindings[index].type,
        .descriptorCount = 1u,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
      };
    }

    const VkDescriptorSetLayoutCreateInfo layout_info =
    {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = items,
      .pBindings = layout_bindings
    };

    MULTI_TRY (vkCreateDescriptorSetLayout (
      runtime->device, &layout_info, NULL, &runtime->layouts[set]
    ), "set-layout");

    fprintf (
      diagnostics,
      "MULTI_SET_LAYOUT index=%" PRIu32
      " bindings=%" PRIu32 "\n", set, items
    );
  }

  const VkPipelineLayoutCreateInfo pipeline_layout_info =
  {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    .setLayoutCount = set_count,
    .pSetLayouts = runtime->layouts
  };

  MULTI_TRY (vkCreatePipelineLayout (
    runtime->device, &pipeline_layout_info,
    NULL, &runtime->pipeline_layout
  ), "pipeline-layout");

  for (uint32_t stage = 0u; stage < stage_count; stage++)
  {
    const VkShaderModuleCreateInfo module_info =
    {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = shaders[stage].bytes,
      .pCode = shaders[stage].words
    };

    MULTI_TRY (vkCreateShaderModule (
      runtime->device, &module_info, NULL, &runtime->shaders[stage]
    ), "shader-module");

    const VkComputePipelineCreateInfo pipeline_info =
    {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage =
      {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = runtime->shaders[stage],
        .pName = "main"
      },
      .layout = runtime->pipeline_layout
    };

    MULTI_TRY (vkCreateComputePipelines (
      runtime->device, VK_NULL_HANDLE, 1u,
      &pipeline_info, NULL, &runtime->pipelines[stage]
    ), "compute-pipeline");
  }

  VkDescriptorPoolSize pool_sizes[2] = { 0 };
  uint32_t pool_count = 0u;

  if (types.storage != 0u)
  {
    pool_sizes[pool_count++] = (VkDescriptorPoolSize)
    {
      .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = types.storage
    };
  }

  if (types.uniform != 0u)
  {
    pool_sizes[pool_count++] = (VkDescriptorPoolSize)
    {
      .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
      .descriptorCount = types.uniform
    };
  }

  const VkDescriptorPoolCreateInfo pool_info =
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
    .maxSets = set_count,
    .poolSizeCount = pool_count,
    .pPoolSizes = pool_sizes
  };

  MULTI_TRY (vkCreateDescriptorPool (
    runtime->device, &pool_info, NULL, &runtime->descriptor_pool
  ), "descriptor-pool");

  for (uint32_t set = 0u; set < set_count; set++)
  {
    const VkDescriptorSetAllocateInfo allocate_set =
    {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = runtime->descriptor_pool,
      .descriptorSetCount = 1u,
      .pSetLayouts = &runtime->layouts[set]
    };

    MULTI_TRY (vkAllocateDescriptorSets (
      runtime->device, &allocate_set, &runtime->sets[set]
    ), "descriptor-set");

    fprintf (
      diagnostics,
      "MULTI_SET_ALLOCATED index=%" PRIu32 "\n", set
    );
  }

  for (uint32_t index = 0u; index < binding_count; index++)
  {
    const vulkan_core10_descriptor_binding_t *entry = &bindings[index];

    buffer_infos[index] = (VkDescriptorBufferInfo)
    {
      .buffer = runtime->buffers[entry->buffer_index].buffer,
      .offset = entry->offset,
      .range = entry->range
    };

    writes[index] = (VkWriteDescriptorSet)
    {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = runtime->sets[entry->set],
      .dstBinding = entry->binding,
      .descriptorCount = 1u,
      .descriptorType = entry->type,
      .pBufferInfo = &buffer_infos[index]
    };

    fprintf (
      diagnostics,
      "MULTI_DESCRIPTOR index=%" PRIu32
      " set=%" PRIu32 " binding=%" PRIu32
      " type=%s buffer=%" PRIu32 " range=%" PRIu64 "\n",
      index, entry->set, entry->binding,
      entry->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
        ? "uniform" : "storage",
      entry->buffer_index, (uint64_t) entry->range
    );
  }

  vkUpdateDescriptorSets (
    runtime->device, binding_count, writes, 0u, NULL
  );

  const VkCommandPoolCreateInfo command_pool_info =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
    .queueFamilyIndex = selected->compute_queue_family
  };

  MULTI_TRY (vkCreateCommandPool (
    runtime->device, &command_pool_info,
    NULL, &runtime->command_pool
  ), "command-pool");

  const VkCommandBufferAllocateInfo command_info =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
    .commandPool = runtime->command_pool,
    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
    .commandBufferCount = 1u
  };

  MULTI_TRY (vkAllocateCommandBuffers (
    runtime->device, &command_info, &runtime->command
  ), "command-buffer");

  const VkFenceCreateInfo fence_info =
  {
    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
  };

  MULTI_TRY (vkCreateFence (
    runtime->device, &fence_info, NULL, &runtime->fence
  ), "fence");

#undef MULTI_TRY

  fprintf (
    diagnostics,
    "MULTI_READY acquisitions=%" PRIu32
    " buffers=%" PRIu32 " sets=%" PRIu32
    " bindings=%" PRIu32 " storage=%" PRIu32
    " uniform=%" PRIu32 " stages=%" PRIu32
    " mapped=%" PRIu32 " api=1.0\n",
    runtime->acquisitions, buffer_count, set_count, binding_count,
    types.storage, types.uniform, stage_count, map_count
  );

cleanup:
  free (requests);
  free (layout_bindings);
  free (buffer_infos);
  free (writes);

  if (result != VK_SUCCESS)
  {
    const VkResult idle = vulkan_core10_multiset_destroy (runtime);

    if (idle != VK_SUCCESS)
    {
      fprintf (
        diagnostics,
        "MULTI_CLEANUP_ERROR result=%d idle=%d\n",
        result, idle
      );
    }
  }

  return result;
}

/**
 * Begin recording one Vulkan 1.0 primary command buffer for repeated submits.
 */
VkResult vulkan_core10_multiset_begin (
  vulkan_core10_multiset_t *runtime)
{
  if (runtime == NULL || runtime->device == VK_NULL_HANDLE
   || runtime->command == VK_NULL_HANDLE
   || runtime->recording == VK_TRUE || runtime->recorded == VK_TRUE)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  const VkCommandBufferBeginInfo begin =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags = 0u
  };

  const VkResult result = vkBeginCommandBuffer (
    runtime->command, &begin
  );

  if (result == VK_SUCCESS) runtime->recording = VK_TRUE;

  return result;
}

/**
 * Finish recording and mark the owned primary command buffer executable.
 */
VkResult vulkan_core10_multiset_end (
  vulkan_core10_multiset_t *runtime)
{
  if (runtime == NULL || runtime->command == VK_NULL_HANDLE
   || runtime->recording != VK_TRUE
   || runtime->recorded == VK_TRUE)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  const VkResult result = vkEndCommandBuffer (
    runtime->command
  );

  if (result == VK_SUCCESS)
  {
    runtime->recording = VK_FALSE;
    runtime->recorded = VK_TRUE;
  }

  return result;
}

/**
 * Submit and await exactly one previously recorded Vulkan 1.0 Compute run.
 *
 * Reuses the same command buffer after a previous completed fence, with no
 * shader/pipeline/descriptors rebuilt between sequential submissions.
 */
VkResult vulkan_core10_multiset_submit_and_wait (
  vulkan_core10_multiset_t *runtime)
{
  if (runtime == NULL || runtime->device == VK_NULL_HANDLE
   || runtime->queue == VK_NULL_HANDLE
   || runtime->fence == VK_NULL_HANDLE || runtime->recorded != VK_TRUE
   || runtime->pending == VK_TRUE)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  VkResult result = VK_SUCCESS;

  if (runtime->completed_submissions != 0u)
  {
    result = vkResetFences (
      runtime->device, 1u, &runtime->fence
    );

    if (result != VK_SUCCESS) return result;
  }

  const VkSubmitInfo submission =
  {
    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    .commandBufferCount = 1u,
    .pCommandBuffers = &runtime->command
  };

  result = vkQueueSubmit (
    runtime->queue, 1u, &submission, runtime->fence
  );

  if (result != VK_SUCCESS) return result;

  runtime->pending = VK_TRUE;

  result = vkWaitForFences (
    runtime->device, 1u, &runtime->fence,
    VK_TRUE, UINT64_C (30000000000)
  );

  if (result != VK_SUCCESS) return result;

  runtime->pending = VK_FALSE;
  runtime->completed_submissions++;

  fprintf (
    runtime->diagnostics,
    "MULTI_SUBMIT completed=%" PRIu32 " reused=%u result=0\n",
    runtime->completed_submissions,
    runtime->completed_submissions > 1u ? 1u : 0u
  );

  return VK_SUCCESS;
}

/**
 * Flush or invalidate one full Vulkan host-mapped buffer allocation.
 *
 * The runtime uses VkMemoryRequirements and the actual memory type's
 * coherent property; noncoherent memory invokes the Vulkan cache API.
 */
VkResult vulkan_core10_multiset_sync (
  vulkan_core10_multiset_t *runtime,
  uint32_t buffer_index,
  VkBool32 invalidate)
{
  if (runtime == NULL || runtime->device == VK_NULL_HANDLE
   || buffer_index >= runtime->buffer_count
   || runtime->buffers[buffer_index].mapped == NULL
   || runtime->buffers[buffer_index].memory == VK_NULL_HANDLE)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  const vulkan_core10_multiset_buffer_t *buffer =
    &runtime->buffers[buffer_index];

  const vulkan_auto_memory_plan_t plan =
  {
    .allocation_bytes = buffer->requirements.size,
    .memory_type_index =
      runtime->memory_plan.memory_type_indices[buffer_index],
    .coherent = runtime->memory_plan.coherent[buffer_index],
    .non_coherent_atom_size = runtime->limits.nonCoherentAtomSize
  };

  const VkResult result = vulkan_auto_sync_mapped_memory (
    runtime->device, buffer->memory, &plan, invalidate
  );

  fprintf (
    runtime->diagnostics,
    "MULTI_SYNC index=%" PRIu32
    " operation=%s coherent=%u driver_call=%u"
    " atom=%" PRIu64 " result=%d\n",
    buffer_index,
    invalidate == VK_TRUE ? "invalidate" : "flush",
    plan.coherent, plan.coherent == VK_TRUE ? 0u : 1u,
    (uint64_t) plan.non_coherent_atom_size, result
  );

  return result;
}
