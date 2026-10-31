/**
 * Own Vulkan 1.0 Compute resources, reusable commands and complete teardown.
 *
 * A single owner creates six externally specified buffers, maps CPU staging,
 * jointly plans actual memory types and heaps, and constructs three Compute
 * pipelines and a descriptor set. The application records its public Compute
 * workload into the owned command buffer and submits it sequentially.
 *
 * Vulkan objects are destroyed in dependency-safe order after a device-idle
 * wait. Deterministic fault points exercise real partially created resources
 * and cleanup without invoking a cryptographic shader or hashcat.
 */

#include "vulkan-core10-runtime.h"

#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#define VULKAN_CORE10_RUNTIME_ACQUISITIONS 28u

/**
 * Count a successful Vulkan resource acquisition and test one fault point.
 *
 * The fault is injected after the real driver has created the resource,
 * enabling deterministic cleanup testing of partially initialized objects.
 */
static VkBool32 runtime_acquire (
  vulkan_core10_runtime_t *runtime,
  uint32_t fault_after,
  const char *name)
{
  runtime->acquired++;

  if (runtime->acquired != fault_after) return VK_FALSE;

  fprintf (
    runtime->diagnostics,
    "RUNTIME_INJECT_FAULT acquisition=%" PRIu32 " resource=%s\n",
    runtime->acquired, name
  );

  return VK_TRUE;
}

/**
 * Release every acquired Vulkan 1.0 object, including on partial creation.
 *
 * Command/descriptor pools own the command buffer and descriptor set.
 * All buffer objects are destroyed before freeing their backing allocations.
 * A subsequent call on the cleared owner is safe and has no Vulkan effects.
 */
VkResult vulkan_core10_runtime_destroy (
  vulkan_core10_runtime_t *runtime)
{
  if (runtime == NULL) return VK_ERROR_INITIALIZATION_FAILED;

  if (runtime->device == VK_NULL_HANDLE)
  {
    memset (runtime, 0, sizeof (*runtime));

    return VK_SUCCESS;
  }

  const VkResult idle = vkDeviceWaitIdle (runtime->device);
  uint32_t buffer_count = 0u;
  uint32_t allocation_count = 0u;
  uint32_t mapped_count = 0u;
  uint32_t shader_count = 0u;
  uint32_t pipeline_count = 0u;

  if (runtime->fence != VK_NULL_HANDLE)
  {
    vkDestroyFence (runtime->device, runtime->fence, NULL);
  }

  if (runtime->command_pool != VK_NULL_HANDLE)
  {
    vkDestroyCommandPool (runtime->device, runtime->command_pool, NULL);
  }

  if (runtime->descriptor_pool != VK_NULL_HANDLE)
  {
    vkDestroyDescriptorPool (
      runtime->device, runtime->descriptor_pool, NULL
    );
  }

  for (uint32_t index = 0u; index < VULKAN_CORE10_RUNTIME_STAGES; index++)
  {
    if (runtime->pipelines[index] != VK_NULL_HANDLE)
    {
      vkDestroyPipeline (
        runtime->device, runtime->pipelines[index], NULL
      );

      pipeline_count++;
    }

    if (runtime->shaders[index] != VK_NULL_HANDLE)
    {
      vkDestroyShaderModule (
        runtime->device, runtime->shaders[index], NULL
      );

      shader_count++;
    }
  }

  if (runtime->pipeline_layout != VK_NULL_HANDLE)
  {
    vkDestroyPipelineLayout (
      runtime->device, runtime->pipeline_layout, NULL
    );
  }

  if (runtime->descriptor_layout != VK_NULL_HANDLE)
  {
    vkDestroyDescriptorSetLayout (
      runtime->device, runtime->descriptor_layout, NULL
    );
  }

  for (uint32_t index = VULKAN_CORE10_RUNTIME_BUFFERS; index != 0u; index--)
  {
    const uint32_t i = index - 1u;

    if (runtime->mapped[i] != NULL)
    {
      vkUnmapMemory (runtime->device, runtime->memory[i]);

      mapped_count++;
    }

    if (runtime->buffers[i] != VK_NULL_HANDLE)
    {
      vkDestroyBuffer (runtime->device, runtime->buffers[i], NULL);

      buffer_count++;
    }

    if (runtime->memory[i] != VK_NULL_HANDLE)
    {
      vkFreeMemory (runtime->device, runtime->memory[i], NULL);

      allocation_count++;
    }
  }

  if (runtime->diagnostics != NULL)
  {
    fprintf (
      runtime->diagnostics,
      "RUNTIME_DESTROY acquired=%" PRIu32
      " buffers=%" PRIu32 " allocations=%" PRIu32
      " mapped=%" PRIu32 " shaders=%" PRIu32
      " pipelines=%" PRIu32
      " submissions=%" PRIu32 " idle_result=%d\n",
      runtime->acquired,
      buffer_count, allocation_count, mapped_count,
      shader_count, pipeline_count,
      runtime->completed_submissions, idle
    );
  }

  vkDestroyDevice (runtime->device, NULL);
  memset (runtime, 0, sizeof (*runtime));

  return idle;
}

/**
 * Build a six-buffer Vulkan 1.0 resource owner from real device properties.
 *
 * Every buffer's size, usage, role and forbidden flags are caller supplied.
 * A single joint allocation plan is used so overlapping GPU/host memory
 * heaps are checked before any vkAllocateMemory call. All descriptor,
 * pipeline, command pool and fence handles are owned by this runtime.
 *
 * Failure at any Vulkan API step or explicitly injected fault destroys every
 * resource already created. No fallback changes buffer or shader contracts.
 */
VkResult vulkan_core10_runtime_create (
  vulkan_core10_runtime_t *runtime,
  const vulkan_auto_device_t *selected,
  const vulkan_core10_runtime_buffer_spec_t
    specs[VULKAN_CORE10_RUNTIME_BUFFERS],
  const vulkan_core10_runtime_shader_t
    shader_sources[VULKAN_CORE10_RUNTIME_STAGES],
  uint32_t fault_after,
  FILE *diagnostics)
{
  if (runtime == NULL || selected == NULL || specs == NULL
   || shader_sources == NULL || diagnostics == NULL
   || selected->physical == VK_NULL_HANDLE
   || fault_after > VULKAN_CORE10_RUNTIME_ACQUISITIONS)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  if (runtime->device != VK_NULL_HANDLE)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  memset (runtime, 0, sizeof (*runtime));
  runtime->diagnostics = diagnostics;
  runtime->limits = selected->properties.limits;
  runtime->queue_family = selected->compute_queue_family;

  for (uint32_t index = 0u; index < VULKAN_CORE10_RUNTIME_BUFFERS; index++)
  {
    const vulkan_core10_runtime_buffer_spec_t *spec = &specs[index];

    if (spec->label == NULL || spec->label[0] == '\0'
     || spec->size == 0u || spec->usage == 0u
     || (index < VULKAN_CORE10_RUNTIME_GPU_BUFFERS
      && (spec->role != VULKAN_HEAP_BUFFER_GPU_STORAGE
       || (spec->usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) == 0u
       || spec->map == VK_TRUE))
     || (index >= VULKAN_CORE10_RUNTIME_GPU_BUFFERS
      && (spec->role != VULKAN_HEAP_BUFFER_HOST_TRANSFER
       || spec->map != VK_TRUE))
     || (index == 0u
      && (spec->usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) == 0u)
     || (index == 3u
      && (spec->usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT) == 0u)
     || (index == 4u
      && (spec->usage & VK_BUFFER_USAGE_TRANSFER_SRC_BIT) == 0u)
     || (index == 5u
      && (spec->usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) == 0u))
    {
      fprintf (
        diagnostics,
        "RUNTIME_REJECT_BUFFER_SPEC index=%" PRIu32 "\n", index
      );

      return VK_ERROR_INITIALIZATION_FAILED;
    }
  }

  for (uint32_t index = 0u; index < VULKAN_CORE10_RUNTIME_STAGES; index++)
  {
    const vulkan_core10_runtime_shader_t *source =
      &shader_sources[index];

    if (source->words == NULL || source->bytes < 20u
     || source->bytes % 4u != 0u
     || source->words[0] != UINT32_C (0x07230203)
     || source->words[1] != UINT32_C (0x00010000))
    {
      fprintf (
        diagnostics,
        "RUNTIME_REJECT_SHADER_SPIRV index=%" PRIu32 "\n", index
      );

      return VK_ERROR_INITIALIZATION_FAILED;
    }
  }

  VkResult result = VK_SUCCESS;

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

#define RUNTIME_TRY(call, name) \
  do \
  { \
    result = (call); \
    if (result != VK_SUCCESS) \
    { \
      fprintf (diagnostics, \
        "RUNTIME_VULKAN_ERROR resource=%s call=%s result=%d\n", \
        name, #call, result); \
      goto error; \
    } \
    if (runtime_acquire (runtime, fault_after, name) == VK_TRUE) \
    { \
      result = VK_ERROR_INITIALIZATION_FAILED; \
      goto error; \
    } \
  } while (0)

  RUNTIME_TRY (vkCreateDevice (
    selected->physical, &device_info, NULL, &runtime->device
  ), "device");

  vkGetDeviceQueue (
    runtime->device, selected->compute_queue_family,
    0u, &runtime->queue
  );

  vulkan_heap_allocation_request_t
    requests[VULKAN_CORE10_RUNTIME_BUFFERS] = { 0 };

  for (uint32_t index = 0u; index < VULKAN_CORE10_RUNTIME_BUFFERS; index++)
  {
    const VkBufferCreateInfo buffer_info =
    {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = specs[index].size,
      .usage = specs[index].usage,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };

    RUNTIME_TRY (vkCreateBuffer (
      runtime->device, &buffer_info, NULL, &runtime->buffers[index]
    ), "buffer");

    requests[index].label = specs[index].label;
    requests[index].role = specs[index].role;
    requests[index].forbidden_flags = specs[index].forbidden_flags;

    vkGetBufferMemoryRequirements (
      runtime->device,
      runtime->buffers[index],
      &runtime->requirements[index]
    );

    requests[index].requirements = runtime->requirements[index];

    fprintf (
      diagnostics,
      "RUNTIME_REQUIREMENTS index=%" PRIu32
      " label=%s size=%" PRIu64
      " alignment=%" PRIu64 " bits=0x%08" PRIx32 "\n",
      index, specs[index].label,
      (uint64_t) requests[index].requirements.size,
      (uint64_t) requests[index].requirements.alignment,
      requests[index].requirements.memoryTypeBits
    );

    if (runtime->requirements[index].size < specs[index].size)
    {
      fprintf (
        diagnostics, "RUNTIME_REJECT_BUFFER_SIZE index=%" PRIu32 "\n",
        index
      );

      result = VK_ERROR_INITIALIZATION_FAILED;

      goto error;
    }
  }

  result = vulkan_multistage_plan_memory (
    &selected->memory_properties,
    selected->properties.limits.maxMemoryAllocationCount,
    requests, &runtime->memory_plan, diagnostics
  );

  if (result != VK_SUCCESS) goto error;

  for (uint32_t index = 0u; index < VULKAN_CORE10_RUNTIME_BUFFERS; index++)
  {
    const VkMemoryAllocateInfo allocation =
    {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requests[index].requirements.size,
      .memoryTypeIndex =
        runtime->memory_plan.memory_type_indices[index]
    };

    RUNTIME_TRY (vkAllocateMemory (
      runtime->device, &allocation, NULL, &runtime->memory[index]
    ), "memory");

    result = vkBindBufferMemory (
      runtime->device,
      runtime->buffers[index], runtime->memory[index], 0u
    );

    if (result != VK_SUCCESS)
    {
      fprintf (
        diagnostics,
        "RUNTIME_BIND_FAILED index=%" PRIu32 " result=%d\n",
        index, result
      );

      goto error;
    }
  }

  for (uint32_t index = VULKAN_CORE10_RUNTIME_GPU_BUFFERS;
       index < VULKAN_CORE10_RUNTIME_BUFFERS; index++)
  {
    RUNTIME_TRY (vkMapMemory (
      runtime->device, runtime->memory[index],
      0u, VK_WHOLE_SIZE, 0u, &runtime->mapped[index]
    ), "mapped-staging");
  }

  fprintf (
    diagnostics,
    "RUNTIME_STAGING upload_type=%" PRIu32
    " download_type=%" PRIu32
    " upload_coherent=%u download_coherent=%u\n",
    runtime->memory_plan.memory_type_indices[4],
    runtime->memory_plan.memory_type_indices[5],
    runtime->memory_plan.coherent[4],
    runtime->memory_plan.coherent[5]
  );

  VkDescriptorSetLayoutBinding
    bindings[VULKAN_CORE10_RUNTIME_GPU_BUFFERS] = { 0 };

  for (uint32_t index = 0u;
       index < VULKAN_CORE10_RUNTIME_GPU_BUFFERS; index++)
  {
    bindings[index] = (VkDescriptorSetLayoutBinding)
    {
      .binding = index,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 1u,
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
    };
  }

  const VkDescriptorSetLayoutCreateInfo layout_info =
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
    .bindingCount = VULKAN_CORE10_RUNTIME_GPU_BUFFERS,
    .pBindings = bindings
  };

  RUNTIME_TRY (vkCreateDescriptorSetLayout (
    runtime->device, &layout_info, NULL, &runtime->descriptor_layout
  ), "descriptor-layout");

  const VkPipelineLayoutCreateInfo pipeline_layout_info =
  {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    .setLayoutCount = 1u,
    .pSetLayouts = &runtime->descriptor_layout
  };

  RUNTIME_TRY (vkCreatePipelineLayout (
    runtime->device, &pipeline_layout_info, NULL, &runtime->pipeline_layout
  ), "pipeline-layout");

  for (uint32_t stage = 0u; stage < VULKAN_CORE10_RUNTIME_STAGES; stage++)
  {
    const VkShaderModuleCreateInfo shader_info =
    {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = shader_sources[stage].bytes,
      .pCode = shader_sources[stage].words
    };

    RUNTIME_TRY (vkCreateShaderModule (
      runtime->device, &shader_info, NULL, &runtime->shaders[stage]
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

    RUNTIME_TRY (vkCreateComputePipelines (
      runtime->device, VK_NULL_HANDLE, 1u,
      &pipeline_info, NULL, &runtime->pipelines[stage]
    ), "compute-pipeline");
  }

  const VkDescriptorPoolSize pool_size =
  {
    .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
    .descriptorCount = VULKAN_CORE10_RUNTIME_GPU_BUFFERS
  };
  const VkDescriptorPoolCreateInfo pool_info =
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
    .maxSets = 1u,
    .poolSizeCount = 1u,
    .pPoolSizes = &pool_size
  };

  RUNTIME_TRY (vkCreateDescriptorPool (
    runtime->device, &pool_info, NULL, &runtime->descriptor_pool
  ), "descriptor-pool");

  const VkDescriptorSetAllocateInfo descriptor_allocation =
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
    .descriptorPool = runtime->descriptor_pool,
    .descriptorSetCount = 1u,
    .pSetLayouts = &runtime->descriptor_layout
  };

  RUNTIME_TRY (vkAllocateDescriptorSets (
    runtime->device, &descriptor_allocation, &runtime->descriptor_set
  ), "descriptor-set");

  VkDescriptorBufferInfo
    descriptor_buffers[VULKAN_CORE10_RUNTIME_GPU_BUFFERS] = { 0 };
  VkWriteDescriptorSet
    descriptor_writes[VULKAN_CORE10_RUNTIME_GPU_BUFFERS] = { 0 };

  for (uint32_t index = 0u;
       index < VULKAN_CORE10_RUNTIME_GPU_BUFFERS; index++)
  {
    descriptor_buffers[index] = (VkDescriptorBufferInfo)
    {
      .buffer = runtime->buffers[index],
      .offset = 0u,
      .range = specs[index].size
    };
    descriptor_writes[index] = (VkWriteDescriptorSet)
    {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = runtime->descriptor_set,
      .dstBinding = index,
      .descriptorCount = 1u,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .pBufferInfo = &descriptor_buffers[index]
    };
  }

  vkUpdateDescriptorSets (
    runtime->device,
    VULKAN_CORE10_RUNTIME_GPU_BUFFERS,
    descriptor_writes, 0u, NULL
  );

  const VkCommandPoolCreateInfo command_pool_info =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
    .queueFamilyIndex = selected->compute_queue_family
  };

  RUNTIME_TRY (vkCreateCommandPool (
    runtime->device, &command_pool_info,
    NULL, &runtime->command_pool
  ), "command-pool");

  const VkCommandBufferAllocateInfo command_allocation =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
    .commandPool = runtime->command_pool,
    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
    .commandBufferCount = 1u
  };

  RUNTIME_TRY (vkAllocateCommandBuffers (
    runtime->device, &command_allocation, &runtime->command
  ), "command-buffer");

  const VkFenceCreateInfo fence_info =
  {
    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
  };

  RUNTIME_TRY (vkCreateFence (
    runtime->device, &fence_info, NULL, &runtime->fence
  ), "fence");

#undef RUNTIME_TRY

  fprintf (
    diagnostics,
    "RUNTIME_READY acquisitions=%" PRIu32
    " buffers=6 pipelines=3 staging=2 queue=%" PRIu32
    " core=1.0\n",
    runtime->acquired, runtime->queue_family
  );

  return VK_SUCCESS;

error:
  {
    const VkResult destroyed = vulkan_core10_runtime_destroy (runtime);

    if (destroyed != VK_SUCCESS)
    {
      fprintf (
        diagnostics,
        "RUNTIME_CLEANUP_ERROR result=%d original=%d\n",
        destroyed, result
      );
    }
  }

  return result;
}

/**
 * Begin recording an owned Vulkan primary command buffer exactly once.
 */
VkResult vulkan_core10_runtime_begin_commands (
  vulkan_core10_runtime_t *runtime)
{
  if (runtime == NULL || runtime->device == VK_NULL_HANDLE
   || runtime->command == VK_NULL_HANDLE
   || runtime->commands_recorded == VK_TRUE
   || runtime->commands_recording == VK_TRUE)
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

  if (result == VK_SUCCESS) runtime->commands_recording = VK_TRUE;

  return result;
}

/**
 * End primary command recording and make the result sequentially reusable.
 */
VkResult vulkan_core10_runtime_end_commands (
  vulkan_core10_runtime_t *runtime)
{
  if (runtime == NULL || runtime->command == VK_NULL_HANDLE
   || runtime->commands_recorded == VK_TRUE
   || runtime->commands_recording != VK_TRUE)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  const VkResult result = vkEndCommandBuffer (runtime->command);

  if (result == VK_SUCCESS)
  {
    runtime->commands_recording = VK_FALSE;
    runtime->commands_recorded = VK_TRUE;
  }

  return result;
}

/**
 * Submit and wait for one sequential execution of the recorded command.
 *
 * Each successful iteration owns its fence completion. Submitting again
 * resets the previously completed fence before vkQueueSubmit, while keeping
 * the already recorded VkCommandBuffer unchanged.
 */
VkResult vulkan_core10_runtime_submit_and_wait (
  vulkan_core10_runtime_t *runtime)
{
  if (runtime == NULL || runtime->device == VK_NULL_HANDLE
   || runtime->fence == VK_NULL_HANDLE
   || runtime->commands_recorded != VK_TRUE
   || runtime->submission_pending == VK_TRUE)
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

  const VkSubmitInfo submit =
  {
    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    .commandBufferCount = 1u,
    .pCommandBuffers = &runtime->command
  };

  result = vkQueueSubmit (
    runtime->queue, 1u, &submit, runtime->fence
  );

  if (result != VK_SUCCESS) return result;

  runtime->submission_pending = VK_TRUE;

  result = vkWaitForFences (
    runtime->device, 1u, &runtime->fence,
    VK_TRUE, UINT64_C (30000000000)
  );

  if (result != VK_SUCCESS) return result;

  runtime->submission_pending = VK_FALSE;
  runtime->completed_submissions++;

  fprintf (
    runtime->diagnostics,
    "RUNTIME_SUBMIT complete=%" PRIu32 " command_reused=%u result=0\n",
    runtime->completed_submissions,
    runtime->completed_submissions > 1u ? 1u : 0u
  );

  return VK_SUCCESS;
}

/**
 * Apply whole-mapping Vulkan 1.0 flush or invalidate for one staging buffer.
 *
 * Invokes actual Vulkan driver operations only for noncoherent memory.
 */
VkResult vulkan_core10_runtime_sync_staging (
  vulkan_core10_runtime_t *runtime,
  uint32_t index,
  VkBool32 invalidate)
{
  if (runtime == NULL || runtime->device == VK_NULL_HANDLE
   || index < VULKAN_CORE10_RUNTIME_GPU_BUFFERS
   || index >= VULKAN_CORE10_RUNTIME_BUFFERS
   || runtime->mapped[index] == NULL
   || runtime->memory[index] == VK_NULL_HANDLE)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  const vulkan_auto_memory_plan_t memory_plan =
  {
    .allocation_bytes = runtime->requirements[index].size,
    .memory_type_index = runtime->memory_plan.memory_type_indices[index],
    .coherent = runtime->memory_plan.coherent[index],
    .non_coherent_atom_size = runtime->limits.nonCoherentAtomSize
  };

  const VkResult result = vulkan_auto_sync_mapped_memory (
    runtime->device, runtime->memory[index], &memory_plan,
    invalidate
  );

  fprintf (
    runtime->diagnostics,
    "RUNTIME_SYNC index=%" PRIu32
    " operation=%s coherent=%u driver_call=%u"
    " atom=%" PRIu64 " result=%d\n",
    index, invalidate == VK_TRUE ? "invalidate" : "flush",
    memory_plan.coherent,
    memory_plan.coherent == VK_TRUE ? 0u : 1u,
    (uint64_t) memory_plan.non_coherent_atom_size,
    result
  );

  return result;
}
