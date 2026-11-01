/**
 * Author......: See docs/credits.txt
 * License.....: MIT
 */

/**
 * Verify one production-owned Vulkan 1.0 public Compute kernel in hashcat.
 *
 * The hashcat backend catalogue supplies a real physical device and
 * VkInstance. This information-mode diagnostic creates a short-lived
 * VkDevice, two host-visible storage buffers, descriptors, one Compute
 * pipeline using an embedded SPIR-V 1.0 module, and a command buffer.
 * A real GPU dispatch transforms 64 unsigned 32-bit public integers.
 * All GPU values are compared against an independent CPU expression.
 *
 * This source never reads or modifies hashcat password candidates, hash
 * values, salt buffers, kernel modes or the normal hashing scheduler.
 */

#include "backend_vulkan_kernel.h"

#include "backend_vulkan.h"
#include "backend_vulkan_public_spirv.h"

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
 * Own all Vulkan objects of one short-lived public Compute dispatch.
 *
 * VkInstance and VkPhysicalDevice remain owned by the backend_ctx
 * catalogue; the two buffers and all logical-device child resources
 * belong to this object until backend_vulkan_public_release.
 */
typedef struct backend_vulkan_public_job
{
  VkDevice device;
  VkQueue queue;
  VkCommandPool command_pool;
  VkCommandBuffer command;
  VkFence fence;
  backend_vulkan_public_buffer_t buffers[2];
  VkDescriptorSetLayout descriptor_layout;
  VkPipelineLayout pipeline_layout;
  VkDescriptorPool descriptor_pool;
  VkDescriptorSet descriptor_set;
  VkShaderModule shader;
  VkPipeline pipeline;
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

  if (job->pipeline != VK_NULL_HANDLE)
  {
    vkDestroyPipeline (job->device, job->pipeline, NULL);
  }

  if (job->shader != VK_NULL_HANDLE)
  {
    vkDestroyShaderModule (job->device, job->shader, NULL);
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

  for (uint32_t i = 0u; i < 2u; i++)
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
 * Create a Vulkan 1.0 Compute pipeline from the embedded public shader.
 *
 * Installs exactly two STORAGE_BUFFER descriptor bindings at set zero,
 * bindings zero and one; creates a descriptor pool and one set, writes
 * both fixed-range buffer descriptors, and creates a one-stage Compute
 * pipeline with entry point "main". No optional device feature,
 * extension, push constants or specialization parameters are needed.
 */
static VkResult backend_vulkan_public_pipeline_open (
  backend_vulkan_public_job_t *job)
{
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
    job->device, &pipeline_layout_info, NULL,
    &job->pipeline_layout
  );

  if (result != VK_SUCCESS) return result;

  const VkDescriptorPoolSize pool_size =
  {
    .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
    .descriptorCount = 2u
  };

  const VkDescriptorPoolCreateInfo pool_info =
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
    .maxSets = 1u,
    .poolSizeCount = 1u,
    .pPoolSizes = &pool_size
  };

  result = vkCreateDescriptorPool (
    job->device, &pool_info, NULL, &job->descriptor_pool
  );

  if (result != VK_SUCCESS) return result;

  const VkDescriptorSetAllocateInfo set_info =
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
    .descriptorPool = job->descriptor_pool,
    .descriptorSetCount = 1u,
    .pSetLayouts = &job->descriptor_layout
  };

  result = vkAllocateDescriptorSets (
    job->device, &set_info, &job->descriptor_set
  );

  if (result != VK_SUCCESS) return result;

  const VkDescriptorBufferInfo buffer_info[2] =
  {
    {
      .buffer = job->buffers[0].buffer,
      .offset = 0u,
      .range = BACKEND_VULKAN_PUBLIC_BYTES
    },
    {
      .buffer = job->buffers[1].buffer,
      .offset = 0u,
      .range = BACKEND_VULKAN_PUBLIC_BYTES
    }
  };

  const VkWriteDescriptorSet writes[2] =
  {
    {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = job->descriptor_set,
      .dstBinding = 0u,
      .descriptorCount = 1u,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .pBufferInfo = &buffer_info[0]
    },
    {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = job->descriptor_set,
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
    .codeSize = sizeof (backend_vulkan_public_spirv),
    .pCode = backend_vulkan_public_spirv
  };

  result = vkCreateShaderModule (
    job->device, &shader_info, NULL, &job->shader
  );

  if (result != VK_SUCCESS) return result;

  const VkComputePipelineCreateInfo compute_info =
  {
    .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
    .stage =
    {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
      .module = job->shader,
      .pName = "main"
    },
    .layout = job->pipeline_layout
  };

  return vkCreateComputePipelines (
    job->device, VK_NULL_HANDLE, 1u, &compute_info,
    NULL, &job->pipeline
  );
}

/**
 * Record and execute two workgroups of the public unsigned-integer shader.
 *
 * Flushes the mapped input allocation if noncoherent, records a single
 * vkCmdDispatch and a SHADER_WRITE-to-HOST_READ buffer barrier, submits
 * the command on a genuine Compute queue, waits for the Vulkan fence,
 * invalidates the output allocation if noncoherent and compares all
 * 64 results against independent scalar CPU arithmetic.
 *
 * This method never receives a hashcat candidate, salt or hash value.
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

  vkCmdBindPipeline (
    job->command, VK_PIPELINE_BIND_POINT_COMPUTE,
    job->pipeline
  );

  vkCmdBindDescriptorSets (
    job->command, VK_PIPELINE_BIND_POINT_COMPUTE,
    job->pipeline_layout, 0u, 1u,
    &job->descriptor_set, 0u, NULL
  );

  vkCmdDispatch (
    job->command, BACKEND_VULKAN_PUBLIC_GROUPS, 1u, 1u
  );

  const VkBufferMemoryBarrier barrier =
  {
    .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .buffer = job->buffers[1].buffer,
    .offset = 0u,
    .size = BACKEND_VULKAN_PUBLIC_BYTES
  };

  vkCmdPipelineBarrier (
    job->command,
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
    VK_PIPELINE_STAGE_HOST_BIT,
    0u,
    0u, NULL,
    1u, &barrier,
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

  result = vulkan_auto_sync_mapped_memory (
    job->device, job->buffers[1].memory,
    &job->buffers[1].plan, VK_TRUE
  );

  if (result != VK_SUCCESS) return result;

  const uint32_t *output =
    (const uint32_t *) job->buffers[1].mapped;

  for (uint32_t i = 0u; i < BACKEND_VULKAN_PUBLIC_WORDS; i++)
  {
    const uint32_t expected =
      (input[i] * UINT32_C (1664525) + UINT32_C (1013904223))
      ^ (i * UINT32_C (2654435769));

    if (output[i] != expected)
    {
      fprintf (
        diagnostics,
        "BACKEND_VULKAN_KERNEL_MISMATCH index=%" PRIu32
        " actual=0x%08" PRIx32
        " expected=0x%08" PRIx32 "\n",
        i, output[i], expected
      );

      return VK_ERROR_INITIALIZATION_FAILED;
    }
  }

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

  return VK_SUCCESS;
}

/**
 * Execute the embedded Vulkan SPIR-V 1.0 kernel using backend discovery.
 *
 * Resolves an exact physical device using the live backend catalogue
 * and the declared shader topology. Owns a temporary VkDevice and all
 * descriptor, shader, pipeline, command, fence and storage resources.
 * Runs one real Compute dispatch, checks 64 public CPU reference
 * values and releases every resource before returning.
 *
 * No hashcat hashing device is registered and no scheduled work
 * is permitted through this diagnostic entry point.
 */
VkResult backend_vulkan_kernel_verify (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  FILE *diagnostics)
{
  if (backend_ctx == NULL || diagnostics == NULL
   || backend_ctx->vulkan_catalog == NULL
   || device_index >= backend_ctx->vulkan_devices_cnt)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  const vulkan_auto_compute_requirements_t requirements =
  {
    .local_size = { BACKEND_VULKAN_PUBLIC_LOCAL_SIZE, 1u, 1u },
    .group_count = { BACKEND_VULKAN_PUBLIC_GROUPS, 1u, 1u },
    .storage_buffer_count = 2u,
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

  for (uint32_t i = 0u; i < 2u; i++)
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

  result = backend_vulkan_public_dispatch (
    &job, diagnostics
  );

cleanup:
  {
    const VkResult released = backend_vulkan_public_release (&job);

    if (result == VK_SUCCESS) result = released;

    fprintf (
      diagnostics,
      "BACKEND_VULKAN_KERNEL_CLOSE"
      " index=%" PRIu32
      " result=%d"
      " released=1 scheduled=0\n",
      device_index, result
    );

    return result;
  }
}
