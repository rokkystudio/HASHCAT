/**
 * Author......: See docs/credits.txt
 * License.....: MIT
 */

/**
 * Verify one or two ordered public Vulkan 1.0 Compute stages in HASHCAT.
 *
 * The backend catalogue supplies a real physical device and VkInstance.
 * Information mode opens a temporary VkDevice and executes one public
 * SPIR-V 1.0 module or a dependent pair of modules in one command buffer.
 * A shader-write-to-shader-read barrier transfers 64 intermediate GPU
 * integers to the second Compute pipeline. Both stages' 64-word outputs
 * are compared to independent unsigned CPU arithmetic. All temporary
 * Vulkan resources are destroyed before returning.
 *
 * This source never reads or modifies HASHCAT password candidates,
 * digests, salts, attack modes or the normal hashing scheduler.
 */

#include "backend_vulkan_kernel.h"

#include "backend_vulkan.h"
#include "backend_vulkan_public_spirv.h"
#include "backend_vulkan_public_stage2_spirv.h"

#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#define BACKEND_VULKAN_PUBLIC_WORDS 64u
#define BACKEND_VULKAN_PUBLIC_LOCAL_SIZE 32u
#define BACKEND_VULKAN_PUBLIC_GROUPS 2u
#define BACKEND_VULKAN_PUBLIC_BYTES (BACKEND_VULKAN_PUBLIC_WORDS * sizeof (uint32_t))

/**
 * Own one host-visible storage buffer and the exact Vulkan memory plan.
 *
 * Each memory allocation uses the complete driver-provided size.
 * Noncoherent mappings are flushed or invalidated over VK_WHOLE_SIZE
 * using the physical device's nonCoherentAtomSize.
 */
typedef struct backend_vulkan_public_buffer
{
  VkBuffer buffer;
  VkDeviceMemory memory;
  void *mapped;
  vulkan_auto_memory_plan_t plan;
} backend_vulkan_public_buffer_t;

/**
 * Own one or two consecutive public Compute stages and their resources.
 *
 * VkInstance and VkPhysicalDevice remain backend_ctx catalogue-owned.
 * This job owns two or three storage buffers, a descriptor set and
 * pipeline per stage, a single command buffer, fence and VkDevice.
 */
typedef struct backend_vulkan_public_job
{
  VkDevice device;
  VkQueue queue;
  VkCommandPool command_pool;
  VkCommandBuffer command;
  VkFence fence;
  backend_vulkan_public_buffer_t buffers[3];
  VkDescriptorSetLayout descriptor_layout;
  VkPipelineLayout pipeline_layout;
  VkDescriptorPool descriptor_pool;
  VkDescriptorSet descriptor_set[2];
  VkShaderModule shader[2];
  VkPipeline pipeline[2];
  uint32_t stages;
} backend_vulkan_public_job_t;

/**
 * Release every acquired Vulkan object without touching the VkInstance.
 *
 * Waits for VkDevice idle before releasing a possibly submitted command.
 * Unmaps each allocation before destroying the buffer and freeing memory.
 * Destroys pipelines and descriptors before the logical device. A null
 * or incompletely initialized job is safe to release. The first
 * vkDeviceWaitIdle result is reported after all resources are freed.
 */
static VkResult backend_vulkan_public_release (
  backend_vulkan_public_job_t *job)
{
  if (job == NULL) return VK_ERROR_INITIALIZATION_FAILED;

  if (job->device == VK_NULL_HANDLE) return VK_SUCCESS;

  const VkResult result = vkDeviceWaitIdle (job->device);

  for (uint32_t stage = 0u; stage < 2u; stage++)
  {
    if (job->pipeline[stage] != VK_NULL_HANDLE)
    {
      vkDestroyPipeline (job->device, job->pipeline[stage], NULL);
    }

    if (job->shader[stage] != VK_NULL_HANDLE)
    {
      vkDestroyShaderModule (job->device, job->shader[stage], NULL);
    }
  }

  if (job->descriptor_pool != VK_NULL_HANDLE)
  {
    vkDestroyDescriptorPool (
      job->device, job->descriptor_pool, NULL
    );
  }

  if (job->pipeline_layout != VK_NULL_HANDLE)
  {
    vkDestroyPipelineLayout (
      job->device, job->pipeline_layout, NULL
    );
  }

  if (job->descriptor_layout != VK_NULL_HANDLE)
  {
    vkDestroyDescriptorSetLayout (
      job->device, job->descriptor_layout, NULL
    );
  }

  if (job->fence != VK_NULL_HANDLE)
  {
    vkDestroyFence (job->device, job->fence, NULL);
  }

  if (job->command != VK_NULL_HANDLE)
  {
    vkFreeCommandBuffers (
      job->device, job->command_pool, 1u, &job->command
    );
  }

  if (job->command_pool != VK_NULL_HANDLE)
  {
    vkDestroyCommandPool (
      job->device, job->command_pool, NULL
    );
  }

  for (uint32_t i = 0u; i < 3u; i++)
  {
    backend_vulkan_public_buffer_t *buffer = &job->buffers[i];

    if (buffer->mapped != NULL)
    {
      vkUnmapMemory (job->device, buffer->memory);
    }

    if (buffer->buffer != VK_NULL_HANDLE)
    {
      vkDestroyBuffer (job->device, buffer->buffer, NULL);
    }

    if (buffer->memory != VK_NULL_HANDLE)
    {
      vkFreeMemory (job->device, buffer->memory, NULL);
    }
  }

  vkDestroyDevice (job->device, NULL);

  memset (job, 0, sizeof (*job));

  return result;
}

/**
 * Allocate one exact 256-byte host-visible Vulkan storage buffer.
 *
 * Uses the real vkGetBufferMemoryRequirements bitmask to select an
 * explicitly compatible memory type. Prefers HOST_COHERENT and accepts
 * HOST_VISIBLE noncoherent memory only with explicit cache sync.
 * Returns the driver VkResult on failure; the parent job owns all
 * partial resources and must release them on the error path.
 */
static VkResult backend_vulkan_public_buffer_open (
  backend_vulkan_public_job_t *job,
  const vulkan_auto_device_t *selected,
  backend_vulkan_public_buffer_t *buffer)
{
  const VkBufferCreateInfo info =
  {
    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
    .size = BACKEND_VULKAN_PUBLIC_BYTES,
    .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
    .sharingMode = VK_SHARING_MODE_EXCLUSIVE
  };

  VkResult result = vkCreateBuffer (
    job->device, &info, NULL, &buffer->buffer
  );

  if (result != VK_SUCCESS) return result;

  VkMemoryRequirements requirements = { 0 };

  vkGetBufferMemoryRequirements (
    job->device, buffer->buffer, &requirements
  );

  uint32_t memory_type = 0u;
  VkBool32 coherent = VK_FALSE;

  result = vulkan_auto_select_memory_type (
    &selected->memory_properties,
    requirements.memoryTypeBits,
    &memory_type,
    &coherent
  );

  if (result != VK_SUCCESS) return result;

  buffer->plan = (vulkan_auto_memory_plan_t)
  {
    .allocation_bytes = requirements.size,
    .memory_type_index = memory_type,
    .compatible_memory_type_bits = requirements.memoryTypeBits,
    .coherent = coherent,
    .non_coherent_atom_size =
      selected->properties.limits.nonCoherentAtomSize
  };

  const VkMemoryAllocateInfo allocation =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
    .allocationSize = requirements.size,
    .memoryTypeIndex = memory_type
  };

  result = vkAllocateMemory (
    job->device, &allocation, NULL, &buffer->memory
  );

  if (result != VK_SUCCESS) return result;

  result = vkBindBufferMemory (
    job->device, buffer->buffer, buffer->memory, 0u
  );

  if (result != VK_SUCCESS) return result;

  return vkMapMemory (
    job->device, buffer->memory, 0u,
    VK_WHOLE_SIZE, 0u, &buffer->mapped
  );
}

/**
 * Create one Vulkan 1.0 Compute pipeline and descriptor set per stage.
 *
 * Both stages use one two-binding STORAGE_BUFFER descriptor layout.
 * Descriptor set zero binds buffers 0/1 and descriptor set one binds
 * buffers 1/2. Loads distinct immutable SPIR-V 1.0 modules and creates
 * one pipeline per stage, without enabling optional features or
 * extensions. Any partial resources belong to the outer job cleanup.
 */
static VkResult backend_vulkan_public_pipeline_open (
  backend_vulkan_public_job_t *job)
{
  if (job->stages != 1u && job->stages != 2u)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  const VkDescriptorSetLayoutBinding bindings[2] =
  {
    {
      .binding = 0u,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 1u,
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
    },
    {
      .binding = 1u,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 1u,
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
    }
  };

  const VkDescriptorSetLayoutCreateInfo descriptor_info =
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
    .bindingCount = 2u,
    .pBindings = bindings
  };

  VkResult result = vkCreateDescriptorSetLayout (
    job->device, &descriptor_info, NULL, &job->descriptor_layout
  );

  if (result != VK_SUCCESS) return result;

  const VkPipelineLayoutCreateInfo pipeline_layout_info =
  {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    .setLayoutCount = 1u,
    .pSetLayouts = &job->descriptor_layout
  };

  result = vkCreatePipelineLayout (
    job->device, &pipeline_layout_info, NULL, &job->pipeline_layout
  );

  if (result != VK_SUCCESS) return result;

  const VkDescriptorPoolSize pool_size =
  {
    .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
    .descriptorCount = job->stages * 2u
  };

  const VkDescriptorPoolCreateInfo pool_info =
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
    .maxSets = job->stages,
    .poolSizeCount = 1u,
    .pPoolSizes = &pool_size
  };

  result = vkCreateDescriptorPool (
    job->device, &pool_info, NULL, &job->descriptor_pool
  );

  if (result != VK_SUCCESS) return result;

  const VkDescriptorSetLayout layouts[2] =
  {
    job->descriptor_layout,
    job->descriptor_layout
  };

  const VkDescriptorSetAllocateInfo set_info =
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
    .descriptorPool = job->descriptor_pool,
    .descriptorSetCount = job->stages,
    .pSetLayouts = layouts
  };

  result = vkAllocateDescriptorSets (
    job->device, &set_info, job->descriptor_set
  );

  if (result != VK_SUCCESS) return result;

  for (uint32_t stage = 0u; stage < job->stages; stage++)
  {
    const VkDescriptorBufferInfo buffer_info[2] =
    {
      {
        .buffer = job->buffers[stage].buffer,
        .offset = 0u,
        .range = BACKEND_VULKAN_PUBLIC_BYTES
      },
      {
        .buffer = job->buffers[stage + 1u].buffer,
        .offset = 0u,
        .range = BACKEND_VULKAN_PUBLIC_BYTES
      }
    };

    const VkWriteDescriptorSet writes[2] =
    {
      {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = job->descriptor_set[stage],
        .dstBinding = 0u,
        .descriptorCount = 1u,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &buffer_info[0]
      },
      {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = job->descriptor_set[stage],
        .dstBinding = 1u,
        .descriptorCount = 1u,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &buffer_info[1]
      }
    };

    vkUpdateDescriptorSets (
      job->device, 2u, writes, 0u, NULL
    );

    const VkShaderModuleCreateInfo shader_info =
    {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = stage == 0u
        ? sizeof (backend_vulkan_public_spirv)
        : sizeof (backend_vulkan_public_stage2_spirv),
      .pCode = stage == 0u
        ? backend_vulkan_public_spirv
        : backend_vulkan_public_stage2_spirv
    };

    result = vkCreateShaderModule (
      job->device, &shader_info, NULL, &job->shader[stage]
    );

    if (result != VK_SUCCESS) return result;

    const VkComputePipelineCreateInfo compute_info =
    {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage =
      {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = job->shader[stage],
        .pName = "main"
      },
      .layout = job->pipeline_layout
    };

    result = vkCreateComputePipelines (
      job->device, VK_NULL_HANDLE, 1u, &compute_info,
      NULL, &job->pipeline[stage]
    );

    if (result != VK_SUCCESS) return result;
  }

  return VK_SUCCESS;
}

/**
 * Dispatch one or two public kernels with explicit GPU buffer barriers.
 *
 * Flushes mapped CPU input, dispatches stage one, and when requested
 * inserts a COMPUTE_SHADER write-to-read barrier on its intermediate
 * buffer before dispatching stage two. Final COMPUTE-to-HOST barriers
 * cover both outputs. Waits for a real Vulkan fence, invalidates
 * noncoherent memory and compares every intermediate/final result
 * against independently computed public 32-bit CPU arithmetic.
 *
 * No passwords, salts, target digests or HASHCAT scheduler are touched.
 */
static VkResult backend_vulkan_public_dispatch (
  backend_vulkan_public_job_t *job,
  FILE *diagnostics)
{
  uint32_t input[BACKEND_VULKAN_PUBLIC_WORDS] = { 0 };

  for (uint32_t i = 0u; i < BACKEND_VULKAN_PUBLIC_WORDS; i++)
  {
    input[i] =
      UINT32_C (0x10203040) ^ (i * UINT32_C (0x01010101));
  }

  memcpy (
    job->buffers[0].mapped, input, sizeof (input)
  );

  VkResult result = vulkan_auto_sync_mapped_memory (
    job->device, job->buffers[0].memory,
    &job->buffers[0].plan, VK_FALSE
  );

  if (result != VK_SUCCESS) return result;

  const VkCommandBufferBeginInfo begin =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
  };

  result = vkBeginCommandBuffer (job->command, &begin);

  if (result != VK_SUCCESS) return result;

  for (uint32_t stage = 0u; stage < job->stages; stage++)
  {
    if (stage != 0u)
    {
      const VkBufferMemoryBarrier intermediate =
      {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = job->buffers[stage].buffer,
        .offset = 0u,
        .size = BACKEND_VULKAN_PUBLIC_BYTES
      };

      vkCmdPipelineBarrier (
        job->command,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0u,
        0u, NULL,
        1u, &intermediate,
        0u, NULL
      );
    }

    vkCmdBindPipeline (
      job->command, VK_PIPELINE_BIND_POINT_COMPUTE,
      job->pipeline[stage]
    );

    vkCmdBindDescriptorSets (
      job->command, VK_PIPELINE_BIND_POINT_COMPUTE,
      job->pipeline_layout, 0u, 1u,
      &job->descriptor_set[stage], 0u, NULL
    );

    vkCmdDispatch (
      job->command, BACKEND_VULKAN_PUBLIC_GROUPS, 1u, 1u
    );
  }

  VkBufferMemoryBarrier host_barriers[2] = { 0 };

  for (uint32_t stage = 0u; stage < job->stages; stage++)
  {
    host_barriers[stage] = (VkBufferMemoryBarrier)
    {
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .buffer = job->buffers[stage + 1u].buffer,
      .offset = 0u,
      .size = BACKEND_VULKAN_PUBLIC_BYTES
    };
  }

  vkCmdPipelineBarrier (
    job->command,
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
    VK_PIPELINE_STAGE_HOST_BIT,
    0u,
    0u, NULL,
    job->stages, host_barriers,
    0u, NULL
  );

  result = vkEndCommandBuffer (job->command);

  if (result != VK_SUCCESS) return result;

  const VkSubmitInfo submit =
  {
    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    .commandBufferCount = 1u,
    .pCommandBuffers = &job->command
  };

  result = vkQueueSubmit (
    job->queue, 1u, &submit, job->fence
  );

  if (result != VK_SUCCESS) return result;

  result = vkWaitForFences (
    job->device, 1u, &job->fence, VK_TRUE, UINT64_MAX
  );

  if (result != VK_SUCCESS) return result;

  for (uint32_t stage = 0u; stage < job->stages; stage++)
  {
    result = vulkan_auto_sync_mapped_memory (
      job->device, job->buffers[stage + 1u].memory,
      &job->buffers[stage + 1u].plan, VK_TRUE
    );

    if (result != VK_SUCCESS) return result;
  }

  const uint32_t *first =
    (const uint32_t *) job->buffers[1].mapped;

  for (uint32_t i = 0u; i < BACKEND_VULKAN_PUBLIC_WORDS; i++)
  {
    const uint32_t expected =
      (input[i] * UINT32_C (1664525) + UINT32_C (1013904223))
      ^ (i * UINT32_C (2654435769));

    if (first[i] != expected)
    {
      fprintf (
        diagnostics,
        "BACKEND_VULKAN_KERNEL_MISMATCH index=%" PRIu32
        " actual=0x%08" PRIx32
        " expected=0x%08" PRIx32 "\n",
        i, first[i], expected
      );

      return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (job->stages == 2u)
    {
      const uint32_t *second =
        (const uint32_t *) job->buffers[2].mapped;

      const uint32_t rotated =
        (expected << 9u) | (expected >> 23u);

      const uint32_t expected_second =
        (rotated + i * UINT32_C (2246822519))
        ^ UINT32_C (0x6c8e9cf5);

      if (second[i] != expected_second)
      {
        fprintf (
          diagnostics,
          "BACKEND_VULKAN_CHAIN_MISMATCH stage=2"
          " index=%" PRIu32
          " actual=0x%08" PRIx32
          " expected=0x%08" PRIx32 "\n",
          i, second[i], expected_second
        );

        return VK_ERROR_INITIALIZATION_FAILED;
      }
    }
  }

  if (job->stages == 1u)
  {
    fprintf (
      diagnostics,
      "BACKEND_VULKAN_KERNEL_DISPATCH_PASS"
      " kernel=public_u32_transform"
      " words=64 local=32 groups=2"
      " verified=64"
      " coherent_input=%u coherent_output=%u"
      " shaders=1 password_inputs=0 scheduled=0\n",
      job->buffers[0].plan.coherent,
      job->buffers[1].plan.coherent
    );
  }
  else
  {
    fprintf (
      diagnostics,
      "BACKEND_VULKAN_CHAIN_DISPATCH_PASS"
      " stage1=public_u32_transform"
      " stage2=public_u32_rotate"
      " words=64 local=32 groups=2"
      " dispatches=2 verified_first=64 verified_second=64"
      " coherent_input=%u coherent_intermediate=%u coherent_output=%u"
      " barrier=shader_write_to_shader_read"
      " shaders=2 password_inputs=0 scheduled=0\n",
      job->buffers[0].plan.coherent,
      job->buffers[1].plan.coherent,
      job->buffers[2].plan.coherent
    );
  }

  return VK_SUCCESS;
}

/**
 * Run one or two public Vulkan 1.0 stages using backend device discovery.
 *
 * Validates the exact Compute limits and required storage descriptors,
 * opens a temporary VkDevice, allocates the stage buffers and pipelines,
 * performs one ordered Compute command-buffer submission and verifies
 * each GPU output against the independent public-integer CPU reference.
 * The temporary device is closed before returning and the backend's
 * ordinary HASHCAT hashing device list is not touched.
 */
static VkResult backend_vulkan_kernel_verify_stages (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  FILE *diagnostics,
  uint32_t stages)
{
  if (backend_ctx == NULL || diagnostics == NULL
   || backend_ctx->vulkan_catalog == NULL
   || device_index >= backend_ctx->vulkan_devices_cnt
   || (stages != 1u && stages != 2u))
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  const vulkan_auto_compute_requirements_t requirements =
  {
    .local_size = { BACKEND_VULKAN_PUBLIC_LOCAL_SIZE, 1u, 1u },
    .group_count = { BACKEND_VULKAN_PUBLIC_GROUPS, 1u, 1u },
    .storage_buffer_count = stages * 2u,
    .largest_storage_buffer_bytes = BACKEND_VULKAN_PUBLIC_BYTES,
    .shared_memory_bytes = 0u
  };

  vulkan_auto_device_t selected = { 0 };
  uint32_t effective_api = 0u;

  VkResult result = backend_vulkan_catalog_select (
    backend_ctx, device_index, &requirements,
    &selected, &effective_api
  );

  if (result != VK_SUCCESS) return result;

  backend_vulkan_public_job_t job = { 0 };

  job.stages = stages;

  const float priority = 1.0f;

  const VkDeviceQueueCreateInfo queue_info =
  {
    .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
    .queueFamilyIndex = selected.compute_queue_family,
    .queueCount = 1u,
    .pQueuePriorities = &priority
  };

  const VkDeviceCreateInfo device_info =
  {
    .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
    .queueCreateInfoCount = 1u,
    .pQueueCreateInfos = &queue_info,
    .enabledLayerCount = 0u,
    .enabledExtensionCount = 0u,
    .pEnabledFeatures = NULL
  };

  result = vkCreateDevice (
    selected.physical, &device_info, NULL, &job.device
  );

  if (result != VK_SUCCESS) goto cleanup;

  vkGetDeviceQueue (
    job.device, selected.compute_queue_family, 0u, &job.queue
  );

  if (job.queue == VK_NULL_HANDLE)
  {
    result = VK_ERROR_INITIALIZATION_FAILED;

    goto cleanup;
  }

  for (uint32_t i = 0u; i < stages + 1u; i++)
  {
    result = backend_vulkan_public_buffer_open (
      &job, &selected, &job.buffers[i]
    );

    if (result != VK_SUCCESS) goto cleanup;
  }

  result = backend_vulkan_public_pipeline_open (&job);

  if (result != VK_SUCCESS) goto cleanup;

  const VkCommandPoolCreateInfo pool_info =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
    .queueFamilyIndex = selected.compute_queue_family,
    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT
  };

  result = vkCreateCommandPool (
    job.device, &pool_info, NULL, &job.command_pool
  );

  if (result != VK_SUCCESS) goto cleanup;

  const VkCommandBufferAllocateInfo command_info =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
    .commandPool = job.command_pool,
    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
    .commandBufferCount = 1u
  };

  result = vkAllocateCommandBuffers (
    job.device, &command_info, &job.command
  );

  if (result != VK_SUCCESS) goto cleanup;

  const VkFenceCreateInfo fence_info =
  {
    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
  };

  result = vkCreateFence (
    job.device, &fence_info, NULL, &job.fence
  );

  if (result != VK_SUCCESS) goto cleanup;

  if (stages == 1u)
  {
    fprintf (
      diagnostics,
      "BACKEND_VULKAN_KERNEL_READY"
      " index=%" PRIu32
      " queue_family=%" PRIu32
      " api=%" PRIu32 ".%" PRIu32
      " shader_words=%" PRIu64
      " descriptors=2"
      " local=32 groups=2"
      " features=0 extensions=0 scheduled=0\n",
      device_index, selected.compute_queue_family,
      VK_API_VERSION_MAJOR (effective_api),
      VK_API_VERSION_MINOR (effective_api),
      (uint64_t) (sizeof (backend_vulkan_public_spirv) / sizeof (uint32_t))
    );
  }
  else
  {
    fprintf (
      diagnostics,
      "BACKEND_VULKAN_CHAIN_READY"
      " index=%" PRIu32
      " queue_family=%" PRIu32
      " api=%" PRIu32 ".%" PRIu32
      " shader_words_first=%" PRIu64
      " shader_words_second=%" PRIu64
      " descriptors=4 sets=2 buffers=3"
      " local=32 groups=2"
      " features=0 extensions=0 scheduled=0\n",
      device_index, selected.compute_queue_family,
      VK_API_VERSION_MAJOR (effective_api),
      VK_API_VERSION_MINOR (effective_api),
      (uint64_t) (sizeof (backend_vulkan_public_spirv) / sizeof (uint32_t)),
      (uint64_t) (sizeof (backend_vulkan_public_stage2_spirv) / sizeof (uint32_t))
    );
  }

  result = backend_vulkan_public_dispatch (
    &job, diagnostics
  );

cleanup:
  {
    const VkResult released = backend_vulkan_public_release (&job);

    if (result == VK_SUCCESS) result = released;

    if (stages == 1u)
    {
      fprintf (
        diagnostics,
        "BACKEND_VULKAN_KERNEL_CLOSE"
        " index=%" PRIu32
        " result=%d"
        " released=1 scheduled=0\n",
        device_index, result
      );
    }
    else
    {
      fprintf (
        diagnostics,
        "BACKEND_VULKAN_CHAIN_CLOSE"
        " index=%" PRIu32
        " result=%d"
        " released=1 scheduled=0\n",
        device_index, result
      );
    }

    return result;
  }
}

/**
 * Verify one embedded public SPIR-V 1.0 Compute module on a Vulkan GPU.
 *
 * Uses a temporary VkDevice with two storage buffers, one descriptor
 * set and one Compute pipeline. Verifies 64 output words against CPU
 * arithmetic, cleans up resources and leaves HASHCAT hashing disabled.
 */
VkResult backend_vulkan_kernel_verify (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  FILE *diagnostics)
{
  return backend_vulkan_kernel_verify_stages (
    backend_ctx, device_index, diagnostics, 1u
  );
}

/**
 * Verify two ordered public SPIR-V 1.0 Compute modules on a Vulkan GPU.
 *
 * Uses one temporary VkDevice with three storage buffers, two descriptor
 * sets and two Compute pipelines. A shader-write-to-shader-read buffer
 * barrier makes the first stage's output available to the second.
 * A completed Vulkan fence precedes CPU checks of all 128 stage words.
 * No HASHCAT candidate, digest or hashing-scheduler work is accepted.
 */
VkResult backend_vulkan_kernel_verify_chain (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  FILE *diagnostics)
{
  return backend_vulkan_kernel_verify_stages (
    backend_ctx, device_index, diagnostics, 2u
  );
}
