/**
 * Author......: See docs/credits.txt
 * License.....: MIT
 */

/**
 * Execute validated public Vulkan 1.0 Compute programs in HASHCAT -I.
 *
 * The backend catalogue supplies a real physical device and VkInstance.
 * Information mode validates explicit one-, two- or four-stage SPIR-V 1.0
 * programs against the physical GPU limits before opening a temporary
 * VkDevice. Storage-buffer, descriptor-set, shader-module and pipeline
 * ownership arrays are dynamically allocated from declared counts.
 * Every stage reads its predecessor after an explicit Compute barrier;
 * every output is compared to independent unsigned 32-bit CPU arithmetic.
 * All temporary Vulkan and host resources are released before returning.
 *
 * This source never reads or modifies HASHCAT password candidates,
 * digests, salts, attack modes or the normal hashing scheduler.
 */

#include "backend_vulkan_kernel.h"

#include "backend_vulkan.h"
#include "backend_vulkan_program.h"
#include "backend_vulkan_public_spirv.h"
#include "backend_vulkan_public_stage2_spirv.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BACKEND_VULKAN_PUBLIC_WORDS 64u
#define BACKEND_VULKAN_PUBLIC_LOCAL_SIZE 32u
#define BACKEND_VULKAN_PUBLIC_GROUPS 2u
#define BACKEND_VULKAN_PUBLIC_BYTES (BACKEND_VULKAN_PUBLIC_WORDS * sizeof (uint32_t))

/**
 * Declare the exact embedded public shader stages consumed by the executor.
 *
 * Stage zero maps buffer 0 to 1; the next three stages map 1->2,
 * 2->3 and 3->4. Each stage declares immutable SPIR-V, exact Compute
 * topology and its own storage-buffer indices. The same declarations
 * control validation, resource allocation and GPU command recording;
 * no runtime device-name heuristic rewrites these values.
 */
static const backend_vulkan_program_stage_t backend_vulkan_public_stages[4] =
{
  {
    .name = "public_u32_transform",
    .spirv = backend_vulkan_public_spirv,
    .spirv_words = sizeof (backend_vulkan_public_spirv) / sizeof (uint32_t),
    .source_buffer_index = 0u,
    .destination_buffer_index = 1u,
    .local_size = { BACKEND_VULKAN_PUBLIC_LOCAL_SIZE, 1u, 1u },
    .group_count = { BACKEND_VULKAN_PUBLIC_GROUPS, 1u, 1u }
  },
  {
    .name = "public_u32_rotate",
    .spirv = backend_vulkan_public_stage2_spirv,
    .spirv_words =
      sizeof (backend_vulkan_public_stage2_spirv) / sizeof (uint32_t),
    .source_buffer_index = 1u,
    .destination_buffer_index = 2u,
    .local_size = { BACKEND_VULKAN_PUBLIC_LOCAL_SIZE, 1u, 1u },
    .group_count = { BACKEND_VULKAN_PUBLIC_GROUPS, 1u, 1u }
  },
  {
    .name = "public_u32_rotate_repeat_2",
    .spirv = backend_vulkan_public_stage2_spirv,
    .spirv_words =
      sizeof (backend_vulkan_public_stage2_spirv) / sizeof (uint32_t),
    .source_buffer_index = 2u,
    .destination_buffer_index = 3u,
    .local_size = { BACKEND_VULKAN_PUBLIC_LOCAL_SIZE, 1u, 1u },
    .group_count = { BACKEND_VULKAN_PUBLIC_GROUPS, 1u, 1u }
  },
  {
    .name = "public_u32_rotate_repeat_3",
    .spirv = backend_vulkan_public_stage2_spirv,
    .spirv_words =
      sizeof (backend_vulkan_public_stage2_spirv) / sizeof (uint32_t),
    .source_buffer_index = 3u,
    .destination_buffer_index = 4u,
    .local_size = { BACKEND_VULKAN_PUBLIC_LOCAL_SIZE, 1u, 1u },
    .group_count = { BACKEND_VULKAN_PUBLIC_GROUPS, 1u, 1u }
  }
};

/**
 * Describe the original one-stage public verification program.
 */
static const backend_vulkan_program_t backend_vulkan_public_single =
{
  .stages = backend_vulkan_public_stages,
  .stage_count = 1u,
  .buffer_count = 2u,
  .buffer_bytes = BACKEND_VULKAN_PUBLIC_BYTES
};

/**
 * Describe the dependent two-stage public verification program.
 */
static const backend_vulkan_program_t backend_vulkan_public_chain =
{
  .stages = backend_vulkan_public_stages,
  .stage_count = 2u,
  .buffer_count = 3u,
  .buffer_bytes = BACKEND_VULKAN_PUBLIC_BYTES
};

/**
 * Describe four ordered public stages using five storage buffers.
 *
 * The first stage uses public_u32_transform. The following three stages
 * reuse the immutable public_u32_rotate SPIR-V module with explicit
 * 1->2, 2->3 and 3->4 dependencies. Reusing the shader keeps the
 * workload synthetic while exercising dynamic executor ownership.
 */
static const backend_vulkan_program_t backend_vulkan_public_chain4 =
{
  .stages = backend_vulkan_public_stages,
  .stage_count = 4u,
  .buffer_count = 5u,
  .buffer_bytes = BACKEND_VULKAN_PUBLIC_BYTES
};

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
 * Own one validated public Compute program and all temporary resources.
 *
 * VkInstance and VkPhysicalDevice remain backend_ctx catalogue-owned.
 * Host arrays are allocated from program->buffer_count and stage_count.
 * The job owns every temporary storage buffer, descriptor set, shader,
 * pipeline, command object, fence and VkDevice until release.
 */
typedef struct backend_vulkan_public_job
{
  VkDevice device;
  VkQueue queue;
  VkCommandPool command_pool;
  VkCommandBuffer command;
  VkFence fence;
  backend_vulkan_public_buffer_t *buffers;
  VkDescriptorSetLayout descriptor_layout;
  VkPipelineLayout pipeline_layout;
  VkDescriptorPool descriptor_pool;
  VkDescriptorSet *descriptor_set;
  VkShaderModule *shader;
  VkPipeline *pipeline;
  const backend_vulkan_program_t *program;
} backend_vulkan_public_job_t;

/**
 * Allocate host ownership arrays for one validated public program.
 *
 * Uses the program's exact stage and buffer counts. No Vulkan object is
 * created here. A partial host allocation is released immediately and
 * reported as VK_ERROR_OUT_OF_HOST_MEMORY. Count multiplication is checked
 * before calloc so oversized declarations fail without integer wrapping.
 */
static VkResult backend_vulkan_public_prepare (
  backend_vulkan_public_job_t *job,
  const backend_vulkan_program_t *program)
{
  if (job == NULL || program == NULL
   || program->stage_count == 0u
   || program->buffer_count == 0u
   || program->stage_count > UINT32_MAX / 2u)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  size_t allocation_bytes = 0u;

  if (__builtin_mul_overflow (
        (size_t) program->stage_count,
        sizeof (*job->descriptor_set),
        &allocation_bytes
      )
   || __builtin_mul_overflow (
        (size_t) program->stage_count,
        sizeof (*job->shader),
        &allocation_bytes
      )
   || __builtin_mul_overflow (
        (size_t) program->stage_count,
        sizeof (*job->pipeline),
        &allocation_bytes
      )
   || __builtin_mul_overflow (
        (size_t) program->buffer_count,
        sizeof (*job->buffers),
        &allocation_bytes
      ))
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  job->program = program;

  job->buffers = calloc (
    program->buffer_count, sizeof (*job->buffers)
  );
  job->descriptor_set = calloc (
    program->stage_count, sizeof (*job->descriptor_set)
  );
  job->shader = calloc (
    program->stage_count, sizeof (*job->shader)
  );
  job->pipeline = calloc (
    program->stage_count, sizeof (*job->pipeline)
  );

  if (job->buffers == NULL
   || job->descriptor_set == NULL
   || job->shader == NULL
   || job->pipeline == NULL)
  {
    free (job->pipeline);
    free (job->shader);
    free (job->descriptor_set);
    free (job->buffers);

    memset (job, 0, sizeof (*job));

    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }

  return VK_SUCCESS;
}

/**
 * Release every acquired Vulkan and host object without touching VkInstance.
 *
 * Waits for VkDevice idle when a logical device exists, then releases every
 * pipeline, shader, descriptor object, command object and storage allocation
 * using the exact program counts. Host ownership arrays are also freed when
 * VkDevice creation never succeeded. A null or partially initialized job is
 * safe to release. The first vkDeviceWaitIdle result survives cleanup.
 */
static VkResult backend_vulkan_public_release (
  backend_vulkan_public_job_t *job)
{
  if (job == NULL) return VK_ERROR_INITIALIZATION_FAILED;

  VkResult result = VK_SUCCESS;

  const uint32_t stage_count =
    job->program != NULL ? job->program->stage_count : 0u;

  const uint32_t buffer_count =
    job->program != NULL ? job->program->buffer_count : 0u;

  if (job->device != VK_NULL_HANDLE)
  {
    result = vkDeviceWaitIdle (job->device);

    for (uint32_t stage = 0u; stage < stage_count; stage++)
    {
      if (job->pipeline != NULL
       && job->pipeline[stage] != VK_NULL_HANDLE)
      {
        vkDestroyPipeline (
          job->device, job->pipeline[stage], NULL
        );
      }

      if (job->shader != NULL
       && job->shader[stage] != VK_NULL_HANDLE)
      {
        vkDestroyShaderModule (
          job->device, job->shader[stage], NULL
        );
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

    if (job->command != VK_NULL_HANDLE
     && job->command_pool != VK_NULL_HANDLE)
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

    for (uint32_t i = 0u; i < buffer_count; i++)
    {
      if (job->buffers == NULL) break;

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
  }

  free (job->pipeline);
  free (job->shader);
  free (job->descriptor_set);
  free (job->buffers);

  memset (job, 0, sizeof (*job));

  return result;
}

/**
 * Allocate one host-visible Vulkan storage buffer for the active program.
 *
 * Uses program->buffer_bytes as the logical size and the real
 * vkGetBufferMemoryRequirements bitmask to select an
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
    .size = job->program->buffer_bytes,
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
 * Create one descriptor set and Vulkan 1.0 Compute pipeline per program stage.
 *
 * The validated program supplies immutable SPIR-V bytes, exact source and
 * destination buffer indices and stage count. Every stage uses one common
 * two-binding STORAGE_BUFFER layout. Descriptor sets are allocated one at a
 * time into host arrays sized by backend_vulkan_public_prepare, so no fixed
 * stage capacity remains in this executor.
 */
static VkResult backend_vulkan_public_pipeline_open (
  backend_vulkan_public_job_t *job)
{
  if (job == NULL || job->program == NULL
   || job->program->stage_count == 0u
   || job->descriptor_set == NULL
   || job->shader == NULL
   || job->pipeline == NULL
   || job->buffers == NULL)
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
    .descriptorCount = job->program->stage_count * 2u
  };

  const VkDescriptorPoolCreateInfo pool_info =
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
    .maxSets = job->program->stage_count,
    .poolSizeCount = 1u,
    .pPoolSizes = &pool_size
  };

  result = vkCreateDescriptorPool (
    job->device, &pool_info, NULL, &job->descriptor_pool
  );

  if (result != VK_SUCCESS) return result;

  for (uint32_t stage_index = 0u;
       stage_index < job->program->stage_count;
       stage_index++)
  {
    const VkDescriptorSetAllocateInfo set_info =
    {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = job->descriptor_pool,
      .descriptorSetCount = 1u,
      .pSetLayouts = &job->descriptor_layout
    };

    result = vkAllocateDescriptorSets (
      job->device, &set_info, &job->descriptor_set[stage_index]
    );

    if (result != VK_SUCCESS) return result;

    const backend_vulkan_program_stage_t *stage =
      &job->program->stages[stage_index];

    const VkDescriptorBufferInfo buffer_info[2] =
    {
      {
        .buffer = job->buffers[stage->source_buffer_index].buffer,
        .offset = 0u,
        .range = job->program->buffer_bytes
      },
      {
        .buffer = job->buffers[stage->destination_buffer_index].buffer,
        .offset = 0u,
        .range = job->program->buffer_bytes
      }
    };

    const VkWriteDescriptorSet writes[2] =
    {
      {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = job->descriptor_set[stage_index],
        .dstBinding = 0u,
        .descriptorCount = 1u,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &buffer_info[0]
      },
      {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = job->descriptor_set[stage_index],
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
      .codeSize = (size_t) stage->spirv_words * sizeof (uint32_t),
      .pCode = stage->spirv
    };

    result = vkCreateShaderModule (
      job->device, &shader_info, NULL, &job->shader[stage_index]
    );

    if (result != VK_SUCCESS) return result;

    const VkComputePipelineCreateInfo compute_info =
    {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage =
      {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = job->shader[stage_index],
        .pName = "main"
      },
      .layout = job->pipeline_layout
    };

    result = vkCreateComputePipelines (
      job->device, VK_NULL_HANDLE, 1u, &compute_info,
      NULL, &job->pipeline[stage_index]
    );

    if (result != VK_SUCCESS) return result;
  }

  return VK_SUCCESS;
}

/**
 * Dispatch any validated public program with explicit declared dependencies.
 *
 * Writes the public CPU input to the first stage's declared source buffer.
 * Every later stage receives a COMPUTE_SHADER write-to-read barrier on its
 * declared source. Each pipeline/set and three-dimensional dispatch count
 * comes from the program declaration. Final shader-write-to-host-read
 * barriers cover every distinct destination buffer. A real fence completes
 * before noncoherent invalidation and CPU comparison of every stage output.
 *
 * Stage zero uses the public_u32_transform CPU reference. Every following
 * stage uses the public_u32_rotate reference, matching the immutable public
 * SPIR-V module reused by the four-stage diagnostic program. No HASHCAT
 * candidate, digest, salt or scheduling data participates in verification.
 */
static VkResult backend_vulkan_public_dispatch (
  backend_vulkan_public_job_t *job,
  FILE *diagnostics)
{
  if (job == NULL || job->program == NULL
   || job->buffers == NULL
   || job->descriptor_set == NULL
   || job->pipeline == NULL)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  const backend_vulkan_program_stage_t *first_stage =
    &job->program->stages[0];

  uint32_t input[BACKEND_VULKAN_PUBLIC_WORDS] = { 0 };

  for (uint32_t i = 0u; i < BACKEND_VULKAN_PUBLIC_WORDS; i++)
  {
    input[i] =
      UINT32_C (0x10203040) ^ (i * UINT32_C (0x01010101));
  }

  backend_vulkan_public_buffer_t *input_buffer =
    &job->buffers[first_stage->source_buffer_index];

  memcpy (
    input_buffer->mapped, input, sizeof (input)
  );

  VkResult result = vulkan_auto_sync_mapped_memory (
    job->device, input_buffer->memory,
    &input_buffer->plan, VK_FALSE
  );

  if (result != VK_SUCCESS) return result;

  const VkCommandBufferBeginInfo begin =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
  };

  result = vkBeginCommandBuffer (job->command, &begin);

  if (result != VK_SUCCESS) return result;

  for (uint32_t stage_index = 0u;
       stage_index < job->program->stage_count;
       stage_index++)
  {
    const backend_vulkan_program_stage_t *stage =
      &job->program->stages[stage_index];

    if (stage_index != 0u)
    {
      const VkBufferMemoryBarrier intermediate =
      {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = job->buffers[stage->source_buffer_index].buffer,
        .offset = 0u,
        .size = job->program->buffer_bytes
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
      job->pipeline[stage_index]
    );

    vkCmdBindDescriptorSets (
      job->command, VK_PIPELINE_BIND_POINT_COMPUTE,
      job->pipeline_layout, 0u, 1u,
      &job->descriptor_set[stage_index], 0u, NULL
    );

    vkCmdDispatch (
      job->command,
      stage->group_count[0],
      stage->group_count[1],
      stage->group_count[2]
    );
  }

  for (uint32_t stage_index = 0u;
       stage_index < job->program->stage_count;
       stage_index++)
  {
    const backend_vulkan_program_stage_t *stage =
      &job->program->stages[stage_index];

    const VkBufferMemoryBarrier host_barrier =
    {
      .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
      .buffer = job->buffers[stage->destination_buffer_index].buffer,
      .offset = 0u,
      .size = job->program->buffer_bytes
    };

    vkCmdPipelineBarrier (
      job->command,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_HOST_BIT,
      0u,
      0u, NULL,
      1u, &host_barrier,
      0u, NULL
    );
  }

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

  for (uint32_t stage_index = 0u;
       stage_index < job->program->stage_count;
       stage_index++)
  {
    const uint32_t output_index =
      job->program->stages[stage_index].destination_buffer_index;

    result = vulkan_auto_sync_mapped_memory (
      job->device, job->buffers[output_index].memory,
      &job->buffers[output_index].plan, VK_TRUE
    );

    if (result != VK_SUCCESS) return result;
  }

  uint32_t expected[BACKEND_VULKAN_PUBLIC_WORDS] = { 0 };

  memcpy (expected, input, sizeof (expected));

  for (uint32_t stage_index = 0u;
       stage_index < job->program->stage_count;
       stage_index++)
  {
    const backend_vulkan_program_stage_t *stage =
      &job->program->stages[stage_index];

    const uint32_t *actual =
      (const uint32_t *)
      job->buffers[stage->destination_buffer_index].mapped;

    for (uint32_t i = 0u; i < BACKEND_VULKAN_PUBLIC_WORDS; i++)
    {
      uint32_t next = 0u;

      if (stage_index == 0u)
      {
        next =
          (expected[i] * UINT32_C (1664525) + UINT32_C (1013904223))
          ^ (i * UINT32_C (2654435769));
      }
      else
      {
        const uint32_t rotated =
          (expected[i] << 9u) | (expected[i] >> 23u);

        next =
          (rotated + i * UINT32_C (2246822519))
          ^ UINT32_C (0x6c8e9cf5);
      }

      if (actual[i] != next)
      {
        fprintf (
          diagnostics,
          "BACKEND_VULKAN_PROGRAM_MISMATCH"
          " stage=%" PRIu32
          " name=%s"
          " index=%" PRIu32
          " actual=0x%08" PRIx32
          " expected=0x%08" PRIx32 "\n",
          stage_index, stage->name, i, actual[i], next
        );

        return VK_ERROR_INITIALIZATION_FAILED;
      }

      expected[i] = next;
    }
  }

  const uint32_t first_output_index =
    job->program->stages[0].destination_buffer_index;

  if (job->program->stage_count == 1u)
  {
    fprintf (
      diagnostics,
      "BACKEND_VULKAN_KERNEL_DISPATCH_PASS"
      " kernel=%s"
      " words=64 local=32 groups=2"
      " verified=64"
      " coherent_input=%u coherent_output=%u"
      " shaders=1 password_inputs=0 scheduled=0\n",
      job->program->stages[0].name,
      input_buffer->plan.coherent,
      job->buffers[first_output_index].plan.coherent
    );
  }
  else if (job->program->stage_count == 2u)
  {
    const uint32_t second_output_index =
      job->program->stages[1].destination_buffer_index;

    fprintf (
      diagnostics,
      "BACKEND_VULKAN_CHAIN_DISPATCH_PASS"
      " stage1=%s"
      " stage2=%s"
      " words=64 local=32 groups=2"
      " dispatches=2 verified_first=64 verified_second=64"
      " coherent_input=%u coherent_intermediate=%u coherent_output=%u"
      " barrier=shader_write_to_shader_read"
      " shaders=2 password_inputs=0 scheduled=0\n",
      job->program->stages[0].name,
      job->program->stages[1].name,
      input_buffer->plan.coherent,
      job->buffers[first_output_index].plan.coherent,
      job->buffers[second_output_index].plan.coherent
    );
  }
  else
  {
    uint32_t noncoherent_outputs = 0u;

    for (uint32_t stage_index = 0u;
         stage_index < job->program->stage_count;
         stage_index++)
    {
      const uint32_t output_index =
        job->program->stages[stage_index].destination_buffer_index;

      if (job->buffers[output_index].plan.coherent == VK_FALSE)
      {
        noncoherent_outputs++;
      }
    }

    fprintf (
      diagnostics,
      "BACKEND_VULKAN_PROGRAM_DISPATCH_PASS"
      " stages=%" PRIu32
      " buffers=%" PRIu32
      " words=64"
      " dispatches=%" PRIu32
      " verified_words=%" PRIu32
      " dependency_barriers=%" PRIu32
      " descriptors=%" PRIu32
      " coherent_input=%u"
      " noncoherent_outputs=%" PRIu32
      " password_inputs=0 scheduled=0\n",
      job->program->stage_count,
      job->program->buffer_count,
      job->program->stage_count,
      job->program->stage_count * BACKEND_VULKAN_PUBLIC_WORDS,
      job->program->stage_count - 1u,
      job->program->stage_count * 2u,
      input_buffer->plan.coherent,
      noncoherent_outputs
    );
  }

  return VK_SUCCESS;
}

/**
 * Run one declared public Vulkan 1.0 program using backend device discovery.
 *
 * Obtains the real physical-device properties from the backend catalogue,
 * validates the complete graph against those limits, allocates host ownership
 * arrays from program counts, then creates exactly one temporary VkDevice.
 * Buffers, descriptor sets, shaders, pipelines and command recording all
 * consume the same declaration. No Vulkan HASHCAT hashing device is created.
 */
static VkResult backend_vulkan_kernel_verify_program (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  FILE *diagnostics,
  const backend_vulkan_program_t *program)
{
  if (backend_ctx == NULL || diagnostics == NULL
   || backend_ctx->vulkan_catalog == NULL
   || device_index >= backend_ctx->vulkan_devices_cnt
   || program == NULL || program->stage_count == 0u)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  const backend_vulkan_program_stage_t *first_stage =
    &program->stages[0];

  const vulkan_auto_compute_requirements_t requirements =
  {
    .local_size =
    {
      first_stage->local_size[0],
      first_stage->local_size[1],
      first_stage->local_size[2]
    },
    .group_count =
    {
      first_stage->group_count[0],
      first_stage->group_count[1],
      first_stage->group_count[2]
    },
    .storage_buffer_count = 2u,
    .largest_storage_buffer_bytes = program->buffer_bytes,
    .shared_memory_bytes = 0u
  };

  vulkan_auto_device_t selected = { 0 };
  uint32_t effective_api = 0u;

  VkResult result = backend_vulkan_catalog_select (
    backend_ctx, device_index, &requirements,
    &selected, &effective_api
  );

  if (result != VK_SUCCESS) return result;

  result = backend_vulkan_program_validate (
    program, &selected.properties.limits, diagnostics
  );

  if (result != VK_SUCCESS) return result;

  backend_vulkan_public_job_t job = { 0 };

  result = backend_vulkan_public_prepare (&job, program);

  if (result != VK_SUCCESS) return result;

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

  for (uint32_t i = 0u; i < program->buffer_count; i++)
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

  if (program->stage_count == 1u)
  {
    fprintf (
      diagnostics,
      "BACKEND_VULKAN_KERNEL_READY"
      " index=%" PRIu32
      " queue_family=%" PRIu32
      " api=%" PRIu32 ".%" PRIu32
      " shader_words=%" PRIu32
      " descriptors=2"
      " local=%" PRIu32 " groups=%" PRIu32
      " program_validated=1 dynamic_resources=1"
      " features=0 extensions=0 scheduled=0\n",
      device_index, selected.compute_queue_family,
      VK_API_VERSION_MAJOR (effective_api),
      VK_API_VERSION_MINOR (effective_api),
      program->stages[0].spirv_words,
      program->stages[0].local_size[0],
      program->stages[0].group_count[0]
    );
  }
  else if (program->stage_count == 2u)
  {
    fprintf (
      diagnostics,
      "BACKEND_VULKAN_CHAIN_READY"
      " index=%" PRIu32
      " queue_family=%" PRIu32
      " api=%" PRIu32 ".%" PRIu32
      " shader_words_first=%" PRIu32
      " shader_words_second=%" PRIu32
      " descriptors=4 sets=2 buffers=3"
      " local=%" PRIu32 " groups=%" PRIu32
      " program_validated=1 dynamic_resources=1"
      " features=0 extensions=0 scheduled=0\n",
      device_index, selected.compute_queue_family,
      VK_API_VERSION_MAJOR (effective_api),
      VK_API_VERSION_MINOR (effective_api),
      program->stages[0].spirv_words,
      program->stages[1].spirv_words,
      program->stages[0].local_size[0],
      program->stages[0].group_count[0]
    );
  }
  else
  {
    fprintf (
      diagnostics,
      "BACKEND_VULKAN_PROGRAM_READY"
      " index=%" PRIu32
      " queue_family=%" PRIu32
      " api=%" PRIu32 ".%" PRIu32
      " stages=%" PRIu32
      " buffers=%" PRIu32
      " descriptor_sets=%" PRIu32
      " descriptors=%" PRIu32
      " shader_modules=%" PRIu32
      " pipelines=%" PRIu32
      " local=%" PRIu32 " groups=%" PRIu32
      " program_validated=1 dynamic_resources=1"
      " features=0 extensions=0 scheduled=0\n",
      device_index, selected.compute_queue_family,
      VK_API_VERSION_MAJOR (effective_api),
      VK_API_VERSION_MINOR (effective_api),
      program->stage_count,
      program->buffer_count,
      program->stage_count,
      program->stage_count * 2u,
      program->stage_count,
      program->stage_count,
      program->stages[0].local_size[0],
      program->stages[0].group_count[0]
    );
  }

  result = backend_vulkan_public_dispatch (
    &job, diagnostics
  );

cleanup:
  {
    const VkResult released = backend_vulkan_public_release (&job);

    if (result == VK_SUCCESS) result = released;

    if (program->stage_count == 1u)
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
    else if (program->stage_count == 2u)
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
    else
    {
      fprintf (
        diagnostics,
        "BACKEND_VULKAN_PROGRAM_CLOSE"
        " index=%" PRIu32
        " stages=%" PRIu32
        " result=%d"
        " released=1 scheduled=0\n",
        device_index, program->stage_count, result
      );
    }

    return result;
  }
}

/**
 * Verify one public SPIR-V 1.0 Compute stage with dynamic ownership.
 *
 * Validates the exact one-stage program against physical GPU limits,
 * allocates two storage-buffer records and one descriptor/shader/pipeline
 * entry, submits one declared Compute dispatch, verifies 64 public words
 * and frees all Vulkan and host resources without scheduling HASHCAT.
 */
VkResult backend_vulkan_kernel_verify (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  FILE *diagnostics)
{
  return backend_vulkan_kernel_verify_program (
    backend_ctx, device_index, diagnostics,
    &backend_vulkan_public_single
  );
}

/**
 * Verify two dependent public SPIR-V 1.0 stages with dynamic ownership.
 *
 * Validates the program against real limits, allocates three storage
 * buffers and two descriptor/shader/pipeline entries and submits two
 * ordered Compute dispatches. A write-to-read buffer barrier connects
 * the declared stages, while a real fence precedes CPU comparison of
 * 128 public values. No HASHCAT candidate, digest or scheduler is used.
 */
VkResult backend_vulkan_kernel_verify_chain (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  FILE *diagnostics)
{
  return backend_vulkan_kernel_verify_program (
    backend_ctx, device_index, diagnostics,
    &backend_vulkan_public_chain
  );
}

/**
 * Verify four ordered public SPIR-V 1.0 Compute stages with dynamic ownership.
 *
 * Uses five storage buffers and four descriptor/shader/pipeline entries
 * allocated from the program declaration. Stage zero runs the public
 * transform shader; the following three stages reuse the public rotate
 * shader through explicit 1->2->3->4 dependencies. A completed Vulkan
 * fence precedes CPU verification of all 256 stage-output words.
 *
 * This method is information-mode diagnostics only. It does not register
 * Vulkan with the HASHCAT hashing scheduler or process password material.
 */
VkResult backend_vulkan_kernel_verify_chain4 (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  FILE *diagnostics)
{
  return backend_vulkan_kernel_verify_program (
    backend_ctx, device_index, diagnostics,
    &backend_vulkan_public_chain4
  );
}
