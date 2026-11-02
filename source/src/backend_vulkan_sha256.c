/**
 * Author......: See docs/credits.txt
 * License.....: MIT
 */

/**
 * Execute independent SHA-256 byte-message batches on Vulkan Compute.
 *
 * The caller supplies exact byte spans and an open Vulkan device catalogue.
 * The shader performs canonical SHA-256 padding and compression, including
 * multi-block messages. This source allocates one temporary compute device,
 * two host-visible storage buffers and one SPIR-V pipeline for each batch.
 * The output contains unconditionally computed digests in original order.
 *
 * No HASHCAT candidate generator, target-hash comparison or attack
 * scheduling state is accessed.
 */

#include "backend_vulkan_sha256.h"

#include "backend_vulkan.h"
#include "backend_vulkan_sha256_spirv.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BACKEND_VULKAN_SHA256_LOCAL_SIZE 32u
#define BACKEND_VULKAN_SHA256_DIGEST_BYTES 32u
#define BACKEND_VULKAN_SHA256_METADATA_BYTES 8u

/**
 * Own the storage buffer, mapped allocation and explicit coherence plan.
 *
 * bytes is the exact descriptor-visible size. The full Vulkan memory
 * requirement is allocated and mapped so noncoherent flush/invalidate
 * operates over the driver's complete aligned allocation.
 */
typedef struct backend_vulkan_sha256_buffer
{
  VkBuffer buffer;
  VkDeviceMemory memory;
  void *mapped;
  VkDeviceSize bytes;
  vulkan_auto_memory_plan_t plan;
} backend_vulkan_sha256_buffer_t;

/**
 * Own all temporary resources of one submitted SHA-256 Vulkan batch.
 *
 * The parent VkInstance and VkPhysicalDevice belong to backend_ctx_t.
 * Resources are destroyed only after vkDeviceWaitIdle if a VkDevice
 * exists, including partially created jobs and submitted workloads.
 */
typedef struct backend_vulkan_sha256_job
{
  VkDevice device;
  VkQueue queue;
  backend_vulkan_sha256_buffer_t buffers[2];
  VkDescriptorSetLayout descriptor_layout;
  VkPipelineLayout pipeline_layout;
  VkDescriptorPool descriptor_pool;
  VkDescriptorSet descriptor_set;
  VkShaderModule shader;
  VkPipeline pipeline;
  VkCommandPool command_pool;
  VkCommandBuffer command;
  VkFence fence;
} backend_vulkan_sha256_job_t;

/**
 * Release the complete or partially constructed SHA-256 Vulkan job.
 *
 * Waits for device idle before releasing all owned resources, unmaps
 * complete host-visible allocations, destroys the logical device and
 * clears all handles. The actual vkDeviceWaitIdle result is returned.
 */
static VkResult backend_vulkan_sha256_release (
  backend_vulkan_sha256_job_t *job)
{
  if (job == NULL) return VK_ERROR_INITIALIZATION_FAILED;

  VkResult result = VK_SUCCESS;

  if (job->device != VK_NULL_HANDLE)
  {
    result = vkDeviceWaitIdle (job->device);

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
      vkDestroyDescriptorPool (job->device, job->descriptor_pool, NULL);
    }

    if (job->pipeline_layout != VK_NULL_HANDLE)
    {
      vkDestroyPipelineLayout (job->device, job->pipeline_layout, NULL);
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
      vkDestroyCommandPool (job->device, job->command_pool, NULL);
    }

    for (uint32_t i = 0u; i < 2u; i++)
    {
      backend_vulkan_sha256_buffer_t *buffer = &job->buffers[i];

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

  memset (job, 0, sizeof (*job));

  return result;
}

/**
 * Create one host-visible storage buffer with the driver's memory layout.
 *
 * Selects a compatible HOST_VISIBLE memory type, preferring HOST_COHERENT.
 * Allocates the complete VkMemoryRequirements size, binds the buffer to
 * device memory and maps the entire allocation. Cleanup owns all partial
 * handles if Vulkan reports an error at any step.
 */
static VkResult backend_vulkan_sha256_buffer_open (
  backend_vulkan_sha256_job_t *job,
  const vulkan_auto_device_t *selected,
  uint32_t index,
  VkDeviceSize bytes)
{
  backend_vulkan_sha256_buffer_t *buffer = &job->buffers[index];

  buffer->bytes = bytes;

  const VkBufferCreateInfo info =
  {
    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
    .size = bytes,
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
 * Create the two-buffer descriptor set and SHA-256 Compute pipeline.
 *
 * Uses the immutable embedded SPIR-V 1.0 byte stream. Binding zero
 * exposes the packed message metadata and bytes; binding one exposes
 * exactly eight digest words per message. All allocations remain owned
 * by the job and are released through backend_vulkan_sha256_release.
 */
static VkResult backend_vulkan_sha256_pipeline_open (
  backend_vulkan_sha256_job_t *job)
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
    job->device, &pipeline_layout_info, NULL, &job->pipeline_layout
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
      .range = job->buffers[0].bytes
    },
    {
      .buffer = job->buffers[1].buffer,
      .offset = 0u,
      .range = job->buffers[1].bytes
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
    .codeSize = sizeof (backend_vulkan_sha256_spirv),
    .pCode = backend_vulkan_sha256_spirv
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
 * Fill one GPU input buffer with explicit byte offsets, lengths and payload.
 *
 * Every message uses two uint32 metadata words. The payload follows all
 * metadata records, preserving the caller's exact byte sequence. Four-byte
 * padding at the end of the allocation is initialized to zero.
 */
static void backend_vulkan_sha256_pack_input (
  backend_vulkan_sha256_job_t *job,
  const backend_vulkan_sha256_message_t *messages,
  uint32_t message_count)
{
  uint8_t *mapped = (uint8_t *) job->buffers[0].mapped;

  memset (mapped, 0, (size_t) job->buffers[0].bytes);

  uint32_t *metadata = (uint32_t *) mapped;
  uint8_t *payload =
    mapped + (size_t) message_count * BACKEND_VULKAN_SHA256_METADATA_BYTES;

  uint32_t offset = 0u;

  for (uint32_t i = 0u; i < message_count; i++)
  {
    metadata[2u * i] = offset;
    metadata[2u * i + 1u] = messages[i].size;

    if (messages[i].size != 0u)
    {
      memcpy (payload + offset, messages[i].data, messages[i].size);
    }

    offset += messages[i].size;
  }
}

/**
 * Record one SHA-256 Compute dispatch and copy completed digests to host.
 *
 * Flushes noncoherent input memory, establishes host-to-shader and
 * shader-to-host buffer dependencies, submits the true GPU workload and
 * waits for its completion fence. Invalidation follows a successful wait.
 * The eight SHA-256 words of each output are encoded in network byte order.
 */
static VkResult backend_vulkan_sha256_dispatch (
  backend_vulkan_sha256_job_t *job,
  uint32_t message_count,
  uint32_t group_count,
  uint8_t *digests)
{
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

  const VkBufferMemoryBarrier input_barrier =
  {
    .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .buffer = job->buffers[0].buffer,
    .offset = 0u,
    .size = job->buffers[0].bytes
  };

  vkCmdPipelineBarrier (
    job->command,
    VK_PIPELINE_STAGE_HOST_BIT,
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
    0u,
    0u, NULL,
    1u, &input_barrier,
    0u, NULL
  );

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
    job->command, group_count, 1u, 1u
  );

  const VkBufferMemoryBarrier output_barrier =
  {
    .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .buffer = job->buffers[1].buffer,
    .offset = 0u,
    .size = job->buffers[1].bytes
  };

  vkCmdPipelineBarrier (
    job->command,
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
    VK_PIPELINE_STAGE_HOST_BIT,
    0u,
    0u, NULL,
    1u, &output_barrier,
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

  for (uint32_t i = 0u; i < message_count; i++)
  {
    for (uint32_t word_index = 0u; word_index < 8u; word_index++)
    {
      const uint32_t word = output[(size_t) i * 8u + word_index];
      const size_t byte_index =
        (size_t) i * BACKEND_VULKAN_SHA256_DIGEST_BYTES
        + word_index * 4u;

      digests[byte_index + 0u] = (uint8_t) (word >> 24u);
      digests[byte_index + 1u] = (uint8_t) (word >> 16u);
      digests[byte_index + 2u] = (uint8_t) (word >> 8u);
      digests[byte_index + 3u] = (uint8_t) word;
    }
  }

  return VK_SUCCESS;
}

/**
 * Hash caller-owned byte messages with the Vulkan 1.0 SHA-256 Compute kernel.
 *
 * Validates input sizes and message offsets before selecting a compatible
 * physical GPU. Creates a temporary queue and two storage buffers, writes
 * the message table and bytes, dispatches the SHA-256 shader and transfers
 * canonical digest bytes to the caller after successful GPU completion.
 * Every partially acquired resource is released before returning.
 *
 * The caller's output is not used for target comparisons or password work.
 * The function does not modify HASHCAT scheduling or device registration.
 */
VkResult backend_vulkan_sha256_hash_batch (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  const backend_vulkan_sha256_message_t *messages,
  uint32_t message_count,
  uint8_t *digests,
  FILE *diagnostics)
{
  if (backend_ctx == NULL
   || backend_ctx->vulkan_catalog == NULL
   || messages == NULL
   || message_count == 0u
   || message_count > INT32_MAX / 8u
   || digests == NULL
   || diagnostics == NULL)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  uint64_t payload_bytes = 0u;

  for (uint32_t i = 0u; i < message_count; i++)
  {
    if (messages[i].size != 0u && messages[i].data == NULL)
    {
      return VK_ERROR_INITIALIZATION_FAILED;
    }

    payload_bytes += messages[i].size;
  }

  const uint64_t metadata_bytes =
    (uint64_t) message_count * BACKEND_VULKAN_SHA256_METADATA_BYTES;

  const uint64_t input_bytes =
    (metadata_bytes + payload_bytes + 3u) & ~UINT64_C (3);

  const uint64_t output_bytes =
    (uint64_t) message_count * BACKEND_VULKAN_SHA256_DIGEST_BYTES;

  if (input_bytes > UINT32_MAX - 72u
   || input_bytes > SIZE_MAX
   || output_bytes > SIZE_MAX)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  const uint32_t group_count =
    (message_count / BACKEND_VULKAN_SHA256_LOCAL_SIZE)
    + ((message_count % BACKEND_VULKAN_SHA256_LOCAL_SIZE) != 0u);

  const VkDeviceSize largest_bytes =
    input_bytes > output_bytes ? input_bytes : output_bytes;

  const vulkan_auto_compute_requirements_t requirements =
  {
    .local_size = { BACKEND_VULKAN_SHA256_LOCAL_SIZE, 1u, 1u },
    .group_count = { group_count, 1u, 1u },
    .storage_buffer_count = 2u,
    .largest_storage_buffer_bytes = largest_bytes,
    .shared_memory_bytes = 0u
  };

  vulkan_auto_device_t selected = { 0 };
  uint32_t effective_api = 0u;

  VkResult result = backend_vulkan_catalog_select (
    backend_ctx, device_index, &requirements,
    &selected, &effective_api
  );

  if (result != VK_SUCCESS) return result;

  backend_vulkan_sha256_job_t job = { 0 };

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

  result = backend_vulkan_sha256_buffer_open (
    &job, &selected, 0u, input_bytes
  );

  if (result != VK_SUCCESS) goto cleanup;

  result = backend_vulkan_sha256_buffer_open (
    &job, &selected, 1u, output_bytes
  );

  if (result != VK_SUCCESS) goto cleanup;

  result = backend_vulkan_sha256_pipeline_open (&job);

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

  backend_vulkan_sha256_pack_input (&job, messages, message_count);

  result = backend_vulkan_sha256_dispatch (
    &job, message_count, group_count, digests
  );

  if (result == VK_SUCCESS)
  {
    fprintf (
      diagnostics,
      "BACKEND_VULKAN_SHA256_BATCH"
      " messages=%" PRIu32
      " input_bytes=%" PRIu64
      " output_bytes=%" PRIu64
      " groups=%" PRIu32
      " api=%" PRIu32 ".%" PRIu32
      " digests=%" PRIu32
      " scheduled=0\n",
      message_count, input_bytes, output_bytes,
      group_count,
      VK_API_VERSION_MAJOR (effective_api),
      VK_API_VERSION_MINOR (effective_api),
      message_count
    );
  }

cleanup:
  {
    const VkResult released = backend_vulkan_sha256_release (&job);

    if (result == VK_SUCCESS) result = released;

    return result;
  }
}
