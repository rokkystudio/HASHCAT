/**
 * Execute public synthetic GPU Compute with Vulkan 1.0 by default.
 *
 * The loader and physical-device versions are negotiated independently.
 * Instance API 1.1 is selected only for an explicit workload requirement.
 * The diagnostic plans four storage/transfer buffer allocations from actual
 * memory types, heap capacities and per-buffer memory requirements.
 *
 * An explicit noncoherent staging trial forbids HOST_COHERENT memory for
 * upload/download, so the actual driver flush/invalidate paths are exercised
 * when supported. Incompatible devices return the precise Vulkan error.
 * All GPU results are independently compared with CPU arithmetic.
 *
 * No optional device features, extensions, hashcat modules or cryptographic
 * workloads are loaded or executed.
 */

#include "vulkan-auto-config.h"
#include "vulkan-core11-policy.h"
#include "vulkan-memory-heap-policy.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ELEMENT_COUNT 2048u
#define BUFFER_BYTES (ELEMENT_COUNT * sizeof (uint32_t))
#define BUFFER_COUNT 4u

typedef struct core11_buffer
{
  VkBuffer buffer;
  VkDeviceMemory memory;
  void *mapped;
} core11_buffer_t;

#define VK_RUN(call) \
  do \
  { \
    result = (call); \
    if (result != VK_SUCCESS) \
    { \
      fprintf (stderr, "REJECT_VULKAN_CALL name=%s result=%d\n", \
        #call, result); \
      goto cleanup; \
    } \
  } while (0)

/**
 * Load and validate a standalone SPIR-V 1.0 shader module from disk.
 *
 * The shader must target SPIR-V 1.0 and have a four-byte-aligned file size.
 * This test never accepts higher SPIR-V versions or encrypted inputs.
 * The returned heap allocation belongs to the caller and must be freed.
 */
static uint32_t *load_shader (const char *path, size_t *bytes)
{
  FILE *file = fopen (path, "rb");

  if (file == NULL)
  {
    perror ("SHADER_FILE_OPEN_FAILED");

    return NULL;
  }

  if (fseek (file, 0, SEEK_END) != 0)
  {
    fclose (file);

    return NULL;
  }

  const long length = ftell (file);

  if (length < 20L || length > 1024L * 1024L
   || length % 4L != 0L
   || fseek (file, 0, SEEK_SET) != 0)
  {
    fprintf (stderr, "REJECT_SPIRV_SIZE length=%ld\n", length);
    fclose (file);

    return NULL;
  }

  uint32_t *code = (uint32_t *) malloc ((size_t) length);

  if (code == NULL)
  {
    fclose (file);

    return NULL;
  }

  const size_t read = fread (code, 1u, (size_t) length, file);

  fclose (file);

  if (read != (size_t) length
   || code[0] != UINT32_C (0x07230203)
   || code[1] != UINT32_C (0x00010000))
  {
    fprintf (stderr, "REJECT_SPIRV_VERSION require=1.0\n");
    free (code);

    return NULL;
  }

  *bytes = (size_t) length;

  return code;
}

/**
 * Synchronize one entirely mapped staging allocation for CPU/GPU transfer.
 *
 * With noncoherent memory, calls the Vulkan 1.0 flush before GPU work or
 * invalidate after a completed GPU fence, always using offset zero and
 * VK_WHOLE_SIZE. Coherent allocations need neither cache-management call.
 * Records the actual VkResult and whether a driver operation was required.
 */
static VkResult synchronize_staging (
  VkDevice device,
  const core11_buffer_t *buffer,
  const vulkan_heap_allocation_plan_t *plan,
  const vulkan_auto_device_t *selected,
  const vulkan_heap_allocation_request_t *request,
  uint32_t index,
  VkBool32 invalidate)
{
  const vulkan_auto_memory_plan_t memory_plan =
  {
    .allocation_bytes = request->requirements.size,
    .memory_type_index = plan->memory_type_indices[index],
    .coherent = plan->coherent[index],
    .non_coherent_atom_size =
      selected->properties.limits.nonCoherentAtomSize
  };

  const VkResult result = vulkan_auto_sync_mapped_memory (
    device, buffer->memory, &memory_plan, invalidate
  );

  fprintf (
    stdout,
    "CORE10_STAGING_SYNC index=%" PRIu32
    " operation=%s coherent=%u driver_call=%u"
    " atom=%" PRIu64 " result=%d\n",
    index, invalidate == VK_TRUE ? "invalidate" : "flush",
    plan->coherent[index],
    plan->coherent[index] == VK_FALSE ? 1u : 0u,
    (uint64_t) memory_plan.non_coherent_atom_size,
    result
  );

  return result;
}

/**
 * Execute a Vulkan 1.0-compatible shader with independently bound buffers.
 *
 * GPU storage prefers DEVICE_LOCAL and ordinary staging prefers coherent
 * HOST_VISIBLE memory. An explicit test request excludes HOST_COHERENT for
 * both staging buffers, so the noncoherent memory path must run or reject
 * the device with a concrete incompatibility result. The joint allocator
 * checks the actual Vulkan buffer memory requirements and heap capacities.
 */
static int run_compute (
  const vulkan_auto_device_t *selected,
  const uint32_t *shader,
  size_t shader_bytes,
  VkBool32 require_noncoherent_staging)
{
  enum
  {
    GPU_INPUT = 0,
    GPU_OUTPUT = 1,
    STAGING_UPLOAD = 2,
    STAGING_DOWNLOAD = 3
  };

  int status = 1;
  VkResult result = VK_SUCCESS;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  core11_buffer_t buffers[BUFFER_COUNT] = { 0 };
  vulkan_heap_allocation_request_t requests[BUFFER_COUNT] = { 0 };
  vulkan_heap_allocation_plan_t plan = { 0 };
  VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkShaderModule shader_module = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
  VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
  VkCommandPool command_pool = VK_NULL_HANDLE;
  VkCommandBuffer command = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;

  static const char *names[BUFFER_COUNT] =
  {
    "gpu-input", "gpu-output", "staging-upload", "staging-download"
  };
  static const VkBufferUsageFlags usages[BUFFER_COUNT] =
  {
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    VK_BUFFER_USAGE_TRANSFER_DST_BIT
  };

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

  VK_RUN (vkCreateDevice (
    selected->physical, &device_info, NULL, &device
  ));

  vkGetDeviceQueue (
    device, selected->compute_queue_family, 0u, &queue
  );

  for (uint32_t index = 0u; index < BUFFER_COUNT; index++)
  {
    const VkBufferCreateInfo buffer_info =
    {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = BUFFER_BYTES,
      .usage = usages[index],
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };

    VK_RUN (vkCreateBuffer (
      device, &buffer_info, NULL, &buffers[index].buffer
    ));

    requests[index].label = names[index];
    requests[index].role = index < STAGING_UPLOAD
      ? VULKAN_HEAP_BUFFER_GPU_STORAGE
      : VULKAN_HEAP_BUFFER_HOST_TRANSFER;

    /*
     * These memory flags belong to optional features not enabled above.
     * The synthetic Vulkan 1.1 core path cannot allocate those types.
     */
    requests[index].forbidden_flags =
      VK_MEMORY_PROPERTY_PROTECTED_BIT
      | VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD;

    if (require_noncoherent_staging == VK_TRUE
     && index >= STAGING_UPLOAD)
    {
      requests[index].forbidden_flags |=
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }

    vkGetBufferMemoryRequirements (
      device, buffers[index].buffer, &requests[index].requirements
    );

    fprintf (
      stdout,
      "CORE10_BUFFER_REQUIREMENTS index=%" PRIu32
      " role=%s size=%" PRIu64
      " alignment=%" PRIu64 " bits=0x%08" PRIx32 "\n",
      index, names[index],
      (uint64_t) requests[index].requirements.size,
      (uint64_t) requests[index].requirements.alignment,
      requests[index].requirements.memoryTypeBits
    );

    if (requests[index].requirements.size < BUFFER_BYTES)
    {
      fprintf (
        stderr, "REJECT_BUFFER_REQUIREMENTS index=%" PRIu32 "\n", index
      );

      goto cleanup;
    }
  }

  fprintf (
    stdout,
    "CORE10_STAGING_REQUEST mode=%s"
    " input_bits=0x%08" PRIx32
    " output_bits=0x%08" PRIx32 "\n",
    require_noncoherent_staging == VK_TRUE
    ? "noncoherent" : "auto",
    requests[STAGING_UPLOAD].requirements.memoryTypeBits,
    requests[STAGING_DOWNLOAD].requirements.memoryTypeBits
  );

  VK_RUN (vulkan_heap_plan_allocations (
    &selected->memory_properties,
    selected->properties.limits.maxMemoryAllocationCount,
    0u, requests, BUFFER_COUNT, &plan, stdout
  ));

  for (uint32_t index = 0u; index < BUFFER_COUNT; index++)
  {
    const VkMemoryAllocateInfo allocation =
    {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requests[index].requirements.size,
      .memoryTypeIndex = plan.memory_type_indices[index]
    };

    VK_RUN (vkAllocateMemory (
      device, &allocation, NULL, &buffers[index].memory
    ));

    VK_RUN (vkBindBufferMemory (
      device, buffers[index].buffer, buffers[index].memory, 0u
    ));
  }

  VK_RUN (vkMapMemory (
    device, buffers[STAGING_UPLOAD].memory, 0u,
    VK_WHOLE_SIZE, 0u, &buffers[STAGING_UPLOAD].mapped
  ));
  VK_RUN (vkMapMemory (
    device, buffers[STAGING_DOWNLOAD].memory, 0u,
    VK_WHOLE_SIZE, 0u, &buffers[STAGING_DOWNLOAD].mapped
  ));

  fprintf (
    stdout,
    "CORE10_STAGING_MAPPED input_type=%" PRIu32
    " output_type=%" PRIu32
    " input_coherent=%u output_coherent=%u\n",
    plan.memory_type_indices[STAGING_UPLOAD],
    plan.memory_type_indices[STAGING_DOWNLOAD],
    plan.coherent[STAGING_UPLOAD],
    plan.coherent[STAGING_DOWNLOAD]
  );

  uint32_t *upload = (uint32_t *) buffers[STAGING_UPLOAD].mapped;
  const uint32_t *download =
    (const uint32_t *) buffers[STAGING_DOWNLOAD].mapped;

  for (uint32_t index = 0u; index < ELEMENT_COUNT; index++)
  {
    upload[index] = index * UINT32_C (1664525);
  }

  VK_RUN (synchronize_staging (
    device, &buffers[STAGING_UPLOAD], &plan, selected,
    &requests[STAGING_UPLOAD], STAGING_UPLOAD, VK_FALSE
  ));

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

  const VkDescriptorSetLayoutCreateInfo layout_info =
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
    .bindingCount = 2u,
    .pBindings = bindings
  };

  VK_RUN (vkCreateDescriptorSetLayout (
    device, &layout_info, NULL, &descriptor_layout
  ));

  const VkPipelineLayoutCreateInfo pipeline_layout_info =
  {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    .setLayoutCount = 1u,
    .pSetLayouts = &descriptor_layout
  };

  VK_RUN (vkCreatePipelineLayout (
    device, &pipeline_layout_info, NULL, &pipeline_layout
  ));

  const VkShaderModuleCreateInfo shader_info =
  {
    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
    .codeSize = shader_bytes,
    .pCode = shader
  };

  VK_RUN (vkCreateShaderModule (
    device, &shader_info, NULL, &shader_module
  ));

  const VkComputePipelineCreateInfo pipeline_info =
  {
    .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
    .stage =
    {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
      .module = shader_module,
      .pName = "main"
    },
    .layout = pipeline_layout
  };

  VK_RUN (vkCreateComputePipelines (
    device, VK_NULL_HANDLE, 1u, &pipeline_info, NULL, &pipeline
  ));

  const VkDescriptorPoolSize pool_size =
  {
    .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
    .descriptorCount = 2u
  };
  const VkDescriptorPoolCreateInfo descriptor_pool_info =
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
    .maxSets = 1u,
    .poolSizeCount = 1u,
    .pPoolSizes = &pool_size
  };

  VK_RUN (vkCreateDescriptorPool (
    device, &descriptor_pool_info, NULL, &descriptor_pool
  ));

  const VkDescriptorSetAllocateInfo allocate_descriptors =
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
    .descriptorPool = descriptor_pool,
    .descriptorSetCount = 1u,
    .pSetLayouts = &descriptor_layout
  };

  VK_RUN (vkAllocateDescriptorSets (
    device, &allocate_descriptors, &descriptor_set
  ));

  const VkDescriptorBufferInfo buffer_info[2] =
  {
    {
      .buffer = buffers[GPU_INPUT].buffer,
      .offset = 0u,
      .range = BUFFER_BYTES
    },
    {
      .buffer = buffers[GPU_OUTPUT].buffer,
      .offset = 0u,
      .range = BUFFER_BYTES
    }
  };
  VkWriteDescriptorSet writes[2] = { 0 };

  for (uint32_t index = 0u; index < 2u; index++)
  {
    writes[index] = (VkWriteDescriptorSet)
    {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = descriptor_set,
      .dstBinding = index,
      .descriptorCount = 1u,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .pBufferInfo = &buffer_info[index]
    };
  }

  vkUpdateDescriptorSets (device, 2u, writes, 0u, NULL);

  const VkCommandPoolCreateInfo command_pool_info =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
    .queueFamilyIndex = selected->compute_queue_family
  };

  VK_RUN (vkCreateCommandPool (
    device, &command_pool_info, NULL, &command_pool
  ));

  const VkCommandBufferAllocateInfo allocate_command =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
    .commandPool = command_pool,
    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
    .commandBufferCount = 1u
  };

  VK_RUN (vkAllocateCommandBuffers (
    device, &allocate_command, &command
  ));

  const VkCommandBufferBeginInfo begin =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
  };

  VK_RUN (vkBeginCommandBuffer (command, &begin));

  const VkMemoryBarrier host_to_transfer =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT
  };

  vkCmdPipelineBarrier (
    command, VK_PIPELINE_STAGE_HOST_BIT,
    VK_PIPELINE_STAGE_TRANSFER_BIT,
    0u, 1u, &host_to_transfer, 0u, NULL, 0u, NULL
  );

  const VkBufferCopy copy =
  {
    .srcOffset = 0u,
    .dstOffset = 0u,
    .size = BUFFER_BYTES
  };

  vkCmdCopyBuffer (
    command, buffers[STAGING_UPLOAD].buffer,
    buffers[GPU_INPUT].buffer, 1u, &copy
  );

  const VkMemoryBarrier transfer_to_compute =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT
  };

  vkCmdPipelineBarrier (
    command, VK_PIPELINE_STAGE_TRANSFER_BIT,
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
    0u, 1u, &transfer_to_compute, 0u, NULL, 0u, NULL
  );

  vkCmdBindPipeline (
    command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline
  );
  vkCmdBindDescriptorSets (
    command, VK_PIPELINE_BIND_POINT_COMPUTE,
    pipeline_layout, 0u, 1u, &descriptor_set, 0u, NULL
  );
  vkCmdDispatch (command, ELEMENT_COUNT / 32u, 1u, 1u);

  const VkMemoryBarrier compute_to_transfer =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT
  };

  vkCmdPipelineBarrier (
    command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
    VK_PIPELINE_STAGE_TRANSFER_BIT,
    0u, 1u, &compute_to_transfer, 0u, NULL, 0u, NULL
  );

  vkCmdCopyBuffer (
    command, buffers[GPU_OUTPUT].buffer,
    buffers[STAGING_DOWNLOAD].buffer, 1u, &copy
  );

  const VkMemoryBarrier transfer_to_host =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_HOST_READ_BIT
  };

  vkCmdPipelineBarrier (
    command, VK_PIPELINE_STAGE_TRANSFER_BIT,
    VK_PIPELINE_STAGE_HOST_BIT,
    0u, 1u, &transfer_to_host, 0u, NULL, 0u, NULL
  );

  VK_RUN (vkEndCommandBuffer (command));

  const VkFenceCreateInfo fence_info =
  {
    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
  };

  VK_RUN (vkCreateFence (device, &fence_info, NULL, &fence));

  const VkSubmitInfo submit =
  {
    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    .commandBufferCount = 1u,
    .pCommandBuffers = &command
  };

  VK_RUN (vkQueueSubmit (queue, 1u, &submit, fence));

  VK_RUN (vkWaitForFences (
    device, 1u, &fence, VK_TRUE, UINT64_C (30000000000)
  ));

  VK_RUN (synchronize_staging (
    device, &buffers[STAGING_DOWNLOAD], &plan, selected,
    &requests[STAGING_DOWNLOAD], STAGING_DOWNLOAD, VK_TRUE
  ));

  uint64_t checksum = UINT64_C (14695981039346656037);

  for (uint32_t index = 0u; index < ELEMENT_COUNT; index++)
  {
    const uint32_t input = index * UINT32_C (1664525);
    const uint32_t affine =
      input * UINT32_C (1664525) + UINT32_C (1013904223);
    const uint32_t rotate = (input << 7u) | (input >> 25u);
    const uint32_t expected =
      (affine ^ rotate) + index * UINT32_C (2654435761);

    if (download[index] != expected)
    {
      fprintf (
        stderr,
        "REJECT_COMPUTE_OUTPUT index=%" PRIu32
        " expected=%08" PRIx32 " received=%08" PRIx32 "\n",
        index, expected, download[index]
      );

      goto cleanup;
    }

    for (uint32_t byte = 0u; byte < 4u; byte++)
    {
      checksum ^= (download[index] >> (8u * byte)) & UINT32_C (0xff);
      checksum *= UINT64_C (1099511628211);
    }
  }

  fprintf (
    stdout,
    "PASS_CORE11_COMPUTE device=%" PRIu32
    " queue=%" PRIu32 " words=%u checksum=%016" PRIx64
    " gpu_buffers=2 staging_buffers=2 copies=2"
    " optional_features=0 extensions=0\n",
    selected->index, selected->compute_queue_family,
    ELEMENT_COUNT, checksum
  );

  status = 0;

cleanup:
  if (device != VK_NULL_HANDLE)
  {
    const VkResult idle = vkDeviceWaitIdle (device);

    if (idle != VK_SUCCESS)
    {
      fprintf (
        stderr, "VULKAN_CORE11_CLEANUP_WAIT_FAILED result=%d\n", idle
      );

      status = 1;
    }

    if (fence != VK_NULL_HANDLE)
    {
      vkDestroyFence (device, fence, NULL);
    }

    if (command_pool != VK_NULL_HANDLE)
    {
      vkDestroyCommandPool (device, command_pool, NULL);
    }

    if (descriptor_pool != VK_NULL_HANDLE)
    {
      vkDestroyDescriptorPool (device, descriptor_pool, NULL);
    }

    if (pipeline != VK_NULL_HANDLE)
    {
      vkDestroyPipeline (device, pipeline, NULL);
    }

    if (shader_module != VK_NULL_HANDLE)
    {
      vkDestroyShaderModule (device, shader_module, NULL);
    }

    if (pipeline_layout != VK_NULL_HANDLE)
    {
      vkDestroyPipelineLayout (device, pipeline_layout, NULL);
    }

    if (descriptor_layout != VK_NULL_HANDLE)
    {
      vkDestroyDescriptorSetLayout (device, descriptor_layout, NULL);
    }

    for (uint32_t index = BUFFER_COUNT; index != 0u; index--)
    {
      core11_buffer_t *buffer = &buffers[index - 1u];

      if (buffer->mapped != NULL)
      {
        vkUnmapMemory (device, buffer->memory);
      }

      if (buffer->buffer != VK_NULL_HANDLE)
      {
        vkDestroyBuffer (device, buffer->buffer, NULL);
      }

      if (buffer->memory != VK_NULL_HANDLE)
      {
        vkFreeMemory (device, buffer->memory, NULL);
      }
    }

    vkDestroyDevice (device, NULL);
  }

  return status;
}

/**
 * Run a SPIR-V 1.0 synthetic Compute shader with an API 1.0 default.
 *
 * The optional --minimum argument explicitly requires Vulkan 1.0 or 1.1.
 * The optional trailing --require-noncoherent-staging flag tests only
 * HOST_VISIBLE types lacking HOST_COHERENT on both staging buffers.
 * Unsupported memory types fail explicitly, without enabling a fallback.
 * All device limits and memory requirements are obtained from Vulkan.
 */
int main (int argc, char **argv)
{
  const VkBool32 require_noncoherent_staging =
    (argc == 3 && strcmp (argv[2], "--require-noncoherent-staging") == 0)
    || (argc == 5
     && strcmp (argv[4], "--require-noncoherent-staging") == 0)
    ? VK_TRUE : VK_FALSE;

  const VkBool32 explicit_version =
    argc == 4 || argc == 5 ? VK_TRUE : VK_FALSE;

  if (argc != 2
   && !(argc == 3 && require_noncoherent_staging == VK_TRUE)
   && !(explicit_version == VK_TRUE
    && strcmp (argv[2], "--minimum") == 0
    && (strcmp (argv[3], "1.0") == 0
     || strcmp (argv[3], "1.1") == 0)
    && (argc == 4 || require_noncoherent_staging == VK_TRUE)))
  {
    fprintf (
      stderr,
      "Usage: %s <stage-a-vk1.0.spv>"
      " [--minimum <1.0|1.1>] [--require-noncoherent-staging]\n",
      argv[0]
    );

    return 2;
  }

  size_t shader_bytes = 0u;
  uint32_t *shader = load_shader (argv[1], &shader_bytes);

  if (shader == NULL) return 2;

  const uint32_t required =
    explicit_version == VK_TRUE && strcmp (argv[3], "1.1") == 0
    ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0;

  uint32_t loader_version = 0u;

  VkResult result = vulkan_core11_query_loader_version (
    vkGetInstanceProcAddr, &loader_version, stdout
  );

  if (result != VK_SUCCESS)
  {
    free (shader);

    return 1;
  }

  uint32_t instance_version = 0u;

  result = vulkan_core11_select_instance_version (
    loader_version, required, &instance_version, stdout
  );

  if (result != VK_SUCCESS)
  {
    free (shader);

    return 1;
  }

  const VkApplicationInfo application =
  {
    .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
    .pApplicationName = "Vulkan 1.0 Core Synthetic Compute",
    .applicationVersion = VK_MAKE_VERSION (1, 0, 0),
    .pEngineName = "None",
    .engineVersion = VK_MAKE_VERSION (1, 0, 0),
    .apiVersion = instance_version
  };

  const VkInstanceCreateInfo instance_info =
  {
    .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
    .pApplicationInfo = &application,
    .enabledLayerCount = 0u,
    .enabledExtensionCount = 0u
  };

  VkInstance instance = VK_NULL_HANDLE;

  result = vkCreateInstance (&instance_info, NULL, &instance);

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr, "REJECT_VULKAN_INSTANCE result=%d\n", result
    );
    free (shader);

    return 1;
  }

  int status = 1;
  uint32_t device_count = 0u;

  result = vkEnumeratePhysicalDevices (
    instance, &device_count, NULL
  );

  if (result != VK_SUCCESS || device_count == 0u)
  {
    fprintf (
      stderr,
      "REJECT_VULKAN_DEVICE_ENUMERATION count=%" PRIu32
      " result=%d\n",
      device_count, result
    );

    goto cleanup_instance;
  }

  VkPhysicalDevice *devices = (VkPhysicalDevice *) calloc (
    device_count, sizeof (*devices)
  );

  if (devices == NULL) goto cleanup_instance;

  result = vkEnumeratePhysicalDevices (
    instance, &device_count, devices
  );

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr, "REJECT_VULKAN_DEVICE_ENUMERATION result=%d\n", result
    );

    goto cleanup_devices;
  }

  const vulkan_auto_compute_requirements_t workload =
  {
    .local_size = { 32u, 1u, 1u },
    .group_count = { ELEMENT_COUNT / 32u, 1u, 1u },
    .storage_buffer_count = 2u,
    .largest_storage_buffer_bytes = BUFFER_BYTES,
    .shared_memory_bytes = 0u
  };

  for (uint32_t index = 0u; index < device_count; index++)
  {
    VkPhysicalDeviceProperties properties = { 0 };

    vkGetPhysicalDeviceProperties (devices[index], &properties);

    uint32_t effective = 0u;

    result = vulkan_core11_select_device_version (
      properties.apiVersion, instance_version,
      required, &effective, stdout
    );

    if (result != VK_SUCCESS)
    {
      fprintf (
        stdout,
        "SKIP_CORE11_DEVICE index=%" PRIu32
        " reason=insufficient_core_api\n", index
      );

      continue;
    }

    vulkan_auto_device_t selected = { 0 };

    result = vulkan_auto_check_device (
      devices[index], index, &workload, &selected, stdout
    );

    if (result != VK_SUCCESS)
    {
      fprintf (
        stdout,
        "SKIP_CORE11_DEVICE index=%" PRIu32
        " reason=compute_limits_or_queue\n", index
      );

      continue;
    }

    VkPhysicalDeviceFeatures optional = { 0 };

    vkGetPhysicalDeviceFeatures (devices[index], &optional);

    fprintf (
      stdout,
      "CORE11_OPTIONAL_FEATURES reported_shaderInt64=%u"
      " reported_shaderInt16=%u enabled=0 extensions=0"
      " effective_api=%" PRIu32 ".%" PRIu32 "\n",
      optional.shaderInt64, optional.shaderInt16,
      VK_API_VERSION_MAJOR (effective),
      VK_API_VERSION_MINOR (effective)
    );

    if (run_compute (
      &selected, shader, shader_bytes, require_noncoherent_staging
    ) == 0)
    {
      status = 0;

      break;
    }

    fprintf (
      stdout,
      "SKIP_CORE11_DEVICE index=%" PRIu32
      " reason=memory_or_compute_execution\n", index
    );
  }

  if (status != 0)
  {
    fprintf (
      stderr,
      "NO_CORE11_COMPATIBLE_SYNTHETIC_COMPUTE_CONFIGURATION\n"
    );
  }

cleanup_devices:
  free (devices);

cleanup_instance:
  vkDestroyInstance (instance, NULL);
  free (shader);

  return status;
}
