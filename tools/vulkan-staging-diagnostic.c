/**
 * Exercise portable Vulkan 1.0 direct mapping and staging-buffer transfers.
 *
 * The runtime detects the actual compute queue, storage buffer memory types,
 * host-visible transfer memory, and cache properties using Vulkan API data.
 * The AUTO mode uses direct host mapping when both storage buffers support
 * it and selects staging when neither or either storage buffer can be mapped.
 * An explicit staging mode verifies the full GPU copy path on test devices.
 *
 * The only GPU workload is one public synthetic integer compute stage.
 * No hashcat library, password material, or cryptographic shader is used.
 */

#include "vulkan-staging-memory.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ELEMENT_COUNT 2048u
#define BUFFER_BYTES (ELEMENT_COUNT * sizeof (uint32_t))
#define DIAGNOSTIC_ALLOCATION_CAP (1024u * 1024u)
#define BUFFER_COUNT 4u

typedef struct vulkan_test_buffer
{
  const char *role;
  VkBuffer buffer;
  VkDeviceMemory memory;
  VkMemoryRequirements requirements;
  uint32_t memory_type;
  VkBool32 coherent;
  void *mapped;
} vulkan_test_buffer_t;

#define VK_CHECK(call) \
  do \
  { \
    result = (call); \
    if (result != VK_SUCCESS) \
    { \
      fprintf (stderr, "VULKAN_ERROR call=%s result=%d\n", #call, result); \
      goto cleanup; \
    } \
  } while (0)

/**
 * Read a public standalone compute shader and validate its SPIR-V header.
 * On success the caller owns the returned executable shader-code allocation.
 */
static uint32_t *read_shader (const char *path, size_t *bytes)
{
  FILE *file = fopen (path, "rb");

  if (file == NULL)
  {
    perror ("shader fopen");

    return NULL;
  }

  if (fseek (file, 0, SEEK_END) != 0)
  {
    fclose (file);

    return NULL;
  }

  const long length = ftell (file);

  if (length <= 0 || length > 1024 * 1024
   || length % 4 != 0 || fseek (file, 0, SEEK_SET) != 0)
  {
    fprintf (stderr, "INVALID_SPIRV_SIZE path=%s\n", path);
    fclose (file);

    return NULL;
  }

  uint32_t *code = (uint32_t *) malloc ((size_t) length);

  if (code == NULL)
  {
    fclose (file);

    return NULL;
  }

  const size_t read_count = fread (code, 1, (size_t) length, file);

  fclose (file);

  if (read_count != (size_t) length || code[0] != 0x07230203u)
  {
    fprintf (stderr, "INVALID_SPIRV_DATA path=%s\n", path);
    free (code);

    return NULL;
  }

  *bytes = (size_t) length;

  return code;
}

/**
 * Confirm mode negotiation from synthetic memory properties independently of
 * the hardware vendor and without creating any Vulkan device objects.
 */
static int check_transfer_policy (void)
{
  VkPhysicalDeviceMemoryProperties properties = { 0 };

  properties.memoryTypeCount = 3u;
  properties.memoryTypes[0].propertyFlags =
    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
  properties.memoryTypes[1].propertyFlags =
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
  properties.memoryTypes[2].propertyFlags =
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
    | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

  uint32_t chosen = UINT32_MAX;

  VkResult result = vulkan_auto_select_gpu_memory_type (
    &properties, 0x7u, &chosen
  );

  if (result != VK_SUCCESS || chosen != 0u) return 1;

  printf ("PASS_POLICY gpu_local_preference=0\n");

  result = vulkan_auto_select_gpu_memory_type (
    &properties, 0x6u, &chosen
  );

  if (result != VK_SUCCESS || chosen != 1u) return 1;

  printf ("PASS_POLICY compatible_nonlocal_type=1\n");

  VkBool32 coherent = VK_TRUE;

  result = vulkan_auto_select_memory_type (
    &properties, 0x2u, &chosen, &coherent
  );

  if (result != VK_SUCCESS || chosen != 1u || coherent != VK_FALSE)
  {
    return 1;
  }

  printf ("PASS_POLICY noncoherent_staging_memory_type=1\n");

  result = vulkan_auto_select_memory_type (
    &properties, 0x1u, &chosen, &coherent
  );

  if (result != VK_ERROR_FEATURE_NOT_PRESENT) return 1;

  printf ("PASS_POLICY no_host_visible_staging_result=%d\n", result);

  result = vulkan_auto_select_gpu_memory_type (
    &properties, 0u, &chosen
  );

  if (result != VK_ERROR_FEATURE_NOT_PRESENT) return 1;

  printf ("PASS_POLICY incompatible_gpu_memory_result=%d\n", result);

  vulkan_auto_transfer_mode_t mode = VULKAN_AUTO_TRANSFER_DIRECT;

  result = vulkan_auto_choose_transfer_mode (
    &properties, 0x7u, 0x6u, VK_FALSE, &mode, stdout
  );

  if (result != VK_SUCCESS || mode != VULKAN_AUTO_TRANSFER_DIRECT) return 1;

  printf ("PASS_POLICY supported_direct_mapping=1\n");

  result = vulkan_auto_choose_transfer_mode (
    &properties, 0x1u, 0x1u, VK_FALSE, &mode, stdout
  );

  if (result != VK_SUCCESS || mode != VULKAN_AUTO_TRANSFER_STAGING) return 1;

  printf ("PASS_POLICY device_local_only_selects_staging=1\n");

  result = vulkan_auto_choose_transfer_mode (
    &properties, 0x7u, 0x6u, VK_TRUE, &mode, stdout
  );

  if (result != VK_SUCCESS || mode != VULKAN_AUTO_TRANSFER_STAGING) return 1;

  printf ("PASS_POLICY explicit_staging=1\n");

  result = vulkan_auto_choose_transfer_mode (
    &properties, 0u, 0x1u, VK_FALSE, &mode, stdout
  );

  if (result != VK_ERROR_FEATURE_NOT_PRESENT) return 1;

  printf ("PASS_POLICY no_gpu_storage_type_result=%d\n", result);

  return 0;
}

/**
 * Create one storage or transfer buffer and query its actual memory types.
 * The buffer is not bound to physical-device memory in this method.
 */
static VkResult create_test_buffer (
  VkDevice device,
  vulkan_test_buffer_t *buffer,
  const char *role,
  VkBufferUsageFlags usage)
{
  buffer->role = role;

  const VkBufferCreateInfo create_info =
  {
    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
    .size = BUFFER_BYTES,
    .usage = usage,
    .sharingMode = VK_SHARING_MODE_EXCLUSIVE
  };

  const VkResult result = vkCreateBuffer (
    device, &create_info, NULL, &buffer->buffer
  );

  if (result == VK_SUCCESS)
  {
    vkGetBufferMemoryRequirements (
      device, buffer->buffer, &buffer->requirements
    );

    fprintf (
      stdout,
      "BUFFER_REQUIREMENTS role=%s"
      " required=%" PRIu64 " alignment=%" PRIu64
      " memory_bits=0x%08" PRIx32 "\n",
      role, (uint64_t) buffer->requirements.size,
      (uint64_t) buffer->requirements.alignment,
      buffer->requirements.memoryTypeBits
    );
  }

  return result;
}

/**
 * Allocate and bind one buffer to a compatible device memory type.
 *
 * Storage and transfer GPU buffers prefer DEVICE_LOCAL, while CPU staging
 * buffers require HOST_VISIBLE and prefer coherent access. Mapped staging
 * buffers expose their whole allocation, including noncoherent padding.
 */
static VkResult allocate_test_buffer (
  VkDevice device,
  const vulkan_auto_device_t *selected,
  vulkan_test_buffer_t *buffer,
  VkBool32 cpu_visible)
{
  uint32_t type = UINT32_MAX;
  VkBool32 coherent = VK_FALSE;
  VkResult result = VK_SUCCESS;

  if (cpu_visible == VK_TRUE)
  {
    result = vulkan_auto_select_memory_type (
      &selected->memory_properties,
      buffer->requirements.memoryTypeBits,
      &type, &coherent
    );
  }
  else
  {
    result = vulkan_auto_select_gpu_memory_type (
      &selected->memory_properties,
      buffer->requirements.memoryTypeBits,
      &type
    );
  }

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr,
      "REJECT_MEMORY_TYPE role=%s requested_host_visible=%u"
      " bits=0x%08" PRIx32 " vkresult=%d\n",
      buffer->role, cpu_visible,
      buffer->requirements.memoryTypeBits, result
    );

    return result;
  }

  if (buffer->requirements.size < BUFFER_BYTES
   || buffer->requirements.size > DIAGNOSTIC_ALLOCATION_CAP)
  {
    fprintf (
      stderr,
      "REJECT_ALLOCATION_SIZE role=%s size=%" PRIu64
      " cap=%u\n",
      buffer->role, (uint64_t) buffer->requirements.size,
      DIAGNOSTIC_ALLOCATION_CAP
    );

    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  }

  const VkMemoryAllocateInfo allocation =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
    .allocationSize = buffer->requirements.size,
    .memoryTypeIndex = type
  };

  result = vkAllocateMemory (
    device, &allocation, NULL, &buffer->memory
  );

  if (result != VK_SUCCESS) return result;

  result = vkBindBufferMemory (
    device, buffer->buffer, buffer->memory, 0u
  );

  if (result != VK_SUCCESS) return result;

  buffer->memory_type = type;
  buffer->coherent = coherent;

  if (cpu_visible == VK_TRUE)
  {
    result = vkMapMemory (
      device, buffer->memory, 0u, VK_WHOLE_SIZE, 0u, &buffer->mapped
    );

    if (result != VK_SUCCESS) return result;
  }

  fprintf (
    stdout,
    "BUFFER_ALLOCATION role=%s memory_type=%" PRIu32
    " cpu_mapped=%u coherent=%u bytes=%" PRIu64 "\n",
    buffer->role, type, cpu_visible, coherent,
    (uint64_t) buffer->requirements.size
  );

  return VK_SUCCESS;
}

/**
 * Flush CPU uploads or invalidate GPU readback only when memory is noncoherent.
 * Whole-allocation mapping permits VK_WHOLE_SIZE without manually rounding
 * ranges to the GPU's nonCoherentAtomSize.
 */
static VkResult sync_test_buffer (
  VkDevice device,
  const vulkan_auto_device_t *selected,
  const vulkan_test_buffer_t *buffer,
  VkBool32 invalidate)
{
  vulkan_auto_memory_plan_t plan =
  {
    .allocation_bytes = buffer->requirements.size,
    .memory_type_index = buffer->memory_type,
    .coherent = buffer->coherent,
    .non_coherent_atom_size =
      selected->properties.limits.nonCoherentAtomSize
  };

  return vulkan_auto_sync_mapped_memory (
    device, buffer->memory, &plan, invalidate
  );
}

/**
 * Destroy mapped memory and its buffer in a valid Vulkan ownership order.
 * The caller waits for completion of any previously submitted GPU work.
 */
static void destroy_test_buffer (
  VkDevice device,
  vulkan_test_buffer_t *buffer)
{
  if (buffer->mapped != NULL)
  {
    vkUnmapMemory (device, buffer->memory);
    buffer->mapped = NULL;
  }

  if (buffer->buffer != VK_NULL_HANDLE)
  {
    vkDestroyBuffer (device, buffer->buffer, NULL);
    buffer->buffer = VK_NULL_HANDLE;
  }

  if (buffer->memory != VK_NULL_HANDLE)
  {
    vkFreeMemory (device, buffer->memory, NULL);
    buffer->memory = VK_NULL_HANDLE;
  }
}

/**
 * Execute a public integer shader using the requested automatic transfer
 * policy. Both direct and staging modes use exact Vulkan device/queue and
 * memory requirements. Staging uses upload and readback vkCmdCopyBuffer
 * commands separated from compute work by Vulkan 1.0 memory barriers.
 */
static int run_device (
  const vulkan_auto_device_t *selected,
  const uint32_t *shader,
  size_t shader_bytes,
  VkBool32 force_staging)
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
  vulkan_test_buffer_t buffers[BUFFER_COUNT] = { 0 };
  VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkShaderModule module = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
  VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
  VkCommandPool command_pool = VK_NULL_HANDLE;
  VkCommandBuffer command = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  vulkan_auto_transfer_mode_t mode = VULKAN_AUTO_TRANSFER_DIRECT;

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
    .pQueueCreateInfos = &queue_info
  };

  VK_CHECK (vkCreateDevice (
    selected->physical, &device_info, NULL, &device
  ));

  vkGetDeviceQueue (
    device, selected->compute_queue_family, 0u, &queue
  );

  VK_CHECK (create_test_buffer (
    device, &buffers[GPU_INPUT], "gpu-input",
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT
  ));
  VK_CHECK (create_test_buffer (
    device, &buffers[GPU_OUTPUT], "gpu-output",
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
  ));

  VK_CHECK (vulkan_auto_choose_transfer_mode (
    &selected->memory_properties,
    buffers[GPU_INPUT].requirements.memoryTypeBits,
    buffers[GPU_OUTPUT].requirements.memoryTypeBits,
    force_staging, &mode, stdout
  ));

  if (mode == VULKAN_AUTO_TRANSFER_STAGING)
  {
    VK_CHECK (create_test_buffer (
      device, &buffers[STAGING_UPLOAD], "staging-upload",
      VK_BUFFER_USAGE_TRANSFER_SRC_BIT
    ));
    VK_CHECK (create_test_buffer (
      device, &buffers[STAGING_DOWNLOAD], "staging-download",
      VK_BUFFER_USAGE_TRANSFER_DST_BIT
    ));
  }

  const VkBool32 map_compute =
    mode == VULKAN_AUTO_TRANSFER_DIRECT ? VK_TRUE : VK_FALSE;

  VK_CHECK (allocate_test_buffer (
    device, selected, &buffers[GPU_INPUT], map_compute
  ));
  VK_CHECK (allocate_test_buffer (
    device, selected, &buffers[GPU_OUTPUT], map_compute
  ));

  if (mode == VULKAN_AUTO_TRANSFER_STAGING)
  {
    VK_CHECK (allocate_test_buffer (
      device, selected, &buffers[STAGING_UPLOAD], VK_TRUE
    ));
    VK_CHECK (allocate_test_buffer (
      device, selected, &buffers[STAGING_DOWNLOAD], VK_TRUE
    ));
  }

  vulkan_test_buffer_t *upload =
    (mode == VULKAN_AUTO_TRANSFER_DIRECT)
      ? &buffers[GPU_INPUT] : &buffers[STAGING_UPLOAD];
  vulkan_test_buffer_t *download =
    (mode == VULKAN_AUTO_TRANSFER_DIRECT)
      ? &buffers[GPU_OUTPUT] : &buffers[STAGING_DOWNLOAD];

  uint32_t *input = (uint32_t *) upload->mapped;
  uint32_t *output = (uint32_t *) download->mapped;

  for (uint32_t index = 0u; index < ELEMENT_COUNT; index++)
  {
    input[index] = index * 1664525u;
    output[index] = UINT32_C (0xa5a5a5a5);
  }

  VK_CHECK (sync_test_buffer (
    device, selected, upload, VK_FALSE
  ));

  VK_CHECK (sync_test_buffer (
    device, selected, download, VK_FALSE
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
  const VkDescriptorSetLayoutCreateInfo descriptor_info =
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
    .bindingCount = 2u,
    .pBindings = bindings
  };

  VK_CHECK (vkCreateDescriptorSetLayout (
    device, &descriptor_info, NULL, &descriptor_layout
  ));

  const VkPipelineLayoutCreateInfo pipeline_layout_info =
  {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    .setLayoutCount = 1u,
    .pSetLayouts = &descriptor_layout
  };

  VK_CHECK (vkCreatePipelineLayout (
    device, &pipeline_layout_info, NULL, &pipeline_layout
  ));

  const VkShaderModuleCreateInfo shader_info =
  {
    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
    .codeSize = shader_bytes,
    .pCode = shader
  };

  VK_CHECK (vkCreateShaderModule (
    device, &shader_info, NULL, &module
  ));

  const VkComputePipelineCreateInfo compute_info =
  {
    .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
    .stage =
    {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
      .module = module,
      .pName = "main"
    },
    .layout = pipeline_layout
  };

  VK_CHECK (vkCreateComputePipelines (
    device, VK_NULL_HANDLE, 1u, &compute_info, NULL, &pipeline
  ));

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

  VK_CHECK (vkCreateDescriptorPool (
    device, &pool_info, NULL, &descriptor_pool
  ));

  const VkDescriptorSetAllocateInfo descriptor_alloc =
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
    .descriptorPool = descriptor_pool,
    .descriptorSetCount = 1u,
    .pSetLayouts = &descriptor_layout
  };

  VK_CHECK (vkAllocateDescriptorSets (
    device, &descriptor_alloc, &descriptor_set
  ));

  const VkDescriptorBufferInfo descriptor_buffers[2] =
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

  for (uint32_t i = 0u; i < 2u; i++)
  {
    writes[i] = (VkWriteDescriptorSet)
    {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = descriptor_set,
      .dstBinding = i,
      .descriptorCount = 1u,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .pBufferInfo = &descriptor_buffers[i]
    };
  }

  vkUpdateDescriptorSets (device, 2u, writes, 0u, NULL);

  const VkCommandPoolCreateInfo command_pool_info =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
    .queueFamilyIndex = selected->compute_queue_family
  };

  VK_CHECK (vkCreateCommandPool (
    device, &command_pool_info, NULL, &command_pool
  ));

  const VkCommandBufferAllocateInfo command_alloc =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
    .commandPool = command_pool,
    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
    .commandBufferCount = 1u
  };

  VK_CHECK (vkAllocateCommandBuffers (
    device, &command_alloc, &command
  ));

  const VkCommandBufferBeginInfo begin_info =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
  };

  VK_CHECK (vkBeginCommandBuffer (command, &begin_info));

  const VkMemoryBarrier host_to_gpu =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
    .dstAccessMask = (mode == VULKAN_AUTO_TRANSFER_STAGING)
      ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_SHADER_READ_BIT
  };

  vkCmdPipelineBarrier (
    command,
    VK_PIPELINE_STAGE_HOST_BIT,
    (mode == VULKAN_AUTO_TRANSFER_STAGING)
      ? VK_PIPELINE_STAGE_TRANSFER_BIT
      : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
    0u, 1u, &host_to_gpu, 0u, NULL, 0u, NULL
  );

  const VkBufferCopy region =
  {
    .srcOffset = 0u,
    .dstOffset = 0u,
    .size = BUFFER_BYTES
  };

  if (mode == VULKAN_AUTO_TRANSFER_STAGING)
  {
    vkCmdCopyBuffer (
      command, buffers[STAGING_UPLOAD].buffer,
      buffers[GPU_INPUT].buffer, 1u, &region
    );

    const VkMemoryBarrier transfer_to_shader =
    {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_SHADER_READ_BIT
    };

    vkCmdPipelineBarrier (
      command,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      0u, 1u, &transfer_to_shader, 0u, NULL, 0u, NULL
    );
  }

  vkCmdBindPipeline (
    command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline
  );
  vkCmdBindDescriptorSets (
    command, VK_PIPELINE_BIND_POINT_COMPUTE,
    pipeline_layout, 0u, 1u, &descriptor_set, 0u, NULL
  );
  vkCmdDispatch (command, ELEMENT_COUNT / 32u, 1u, 1u);

  if (mode == VULKAN_AUTO_TRANSFER_STAGING)
  {
    const VkMemoryBarrier shader_to_transfer =
    {
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT
    };

    vkCmdPipelineBarrier (
      command,
      VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      0u, 1u, &shader_to_transfer, 0u, NULL, 0u, NULL
    );

    vkCmdCopyBuffer (
      command, buffers[GPU_OUTPUT].buffer,
      buffers[STAGING_DOWNLOAD].buffer, 1u, &region
    );
  }

  const VkMemoryBarrier gpu_to_host =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .srcAccessMask = (mode == VULKAN_AUTO_TRANSFER_STAGING)
      ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_SHADER_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_HOST_READ_BIT
  };

  vkCmdPipelineBarrier (
    command,
    (mode == VULKAN_AUTO_TRANSFER_STAGING)
      ? VK_PIPELINE_STAGE_TRANSFER_BIT
      : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
    VK_PIPELINE_STAGE_HOST_BIT,
    0u, 1u, &gpu_to_host, 0u, NULL, 0u, NULL
  );

  VK_CHECK (vkEndCommandBuffer (command));

  const VkFenceCreateInfo fence_info =
  {
    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
  };

  VK_CHECK (vkCreateFence (device, &fence_info, NULL, &fence));

  const VkSubmitInfo submit =
  {
    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    .commandBufferCount = 1u,
    .pCommandBuffers = &command
  };

  VK_CHECK (vkQueueSubmit (queue, 1u, &submit, fence));
  VK_CHECK (vkWaitForFences (
    device, 1u, &fence, VK_TRUE, UINT64_C (30000000000)
  ));

  VK_CHECK (sync_test_buffer (
    device, selected, download, VK_TRUE
  ));

  uint64_t checksum = UINT64_C (14695981039346656037);

  for (uint32_t index = 0u; index < ELEMENT_COUNT; index++)
  {
    const uint32_t value = index * 1664525u;
    const uint32_t affine = value * 1664525u + 1013904223u;
    const uint32_t rotated = (value << 7u) | (value >> 25u);
    const uint32_t expected =
      (affine ^ rotated) + index * 2654435761u;

    if (input[index] != value || output[index] != expected)
    {
      fprintf (
        stderr,
        "REJECT_GPU_WORD index=%" PRIu32 " input=%08" PRIx32
        " expected=%08" PRIx32 " actual=%08" PRIx32 "\n",
        index, input[index], expected, output[index]
      );

      goto cleanup;
    }

    for (uint32_t byte = 0u; byte < 4u; byte++)
    {
      const uint64_t next =
        (uint64_t) ((output[index] >> (byte * 8u)) & 0xffu);

      checksum = (checksum ^ next) * UINT64_C (1099511628211);
    }
  }

  printf (
    "PASS_VULKAN_TRANSFER mode=%s device=%" PRIu32
    " queue=%" PRIu32 " words=%u checksum=%016" PRIx64
    " uploads=%u downloads=%u gpu_dispatches=1\n",
    (mode == VULKAN_AUTO_TRANSFER_STAGING) ? "staging" : "direct",
    selected->index, selected->compute_queue_family, ELEMENT_COUNT,
    checksum,
    (mode == VULKAN_AUTO_TRANSFER_STAGING) ? 1u : 0u,
    (mode == VULKAN_AUTO_TRANSFER_STAGING) ? 1u : 0u
  );

  status = 0;

cleanup:
  if (device != VK_NULL_HANDLE)
  {
    const VkResult idle = vkDeviceWaitIdle (device);

    if (idle != VK_SUCCESS)
    {
      fprintf (
        stderr, "VULKAN_CLEANUP_WAIT_ERROR vkresult=%d\n", idle
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

    if (module != VK_NULL_HANDLE)
    {
      vkDestroyShaderModule (device, module, NULL);
    }

    if (pipeline_layout != VK_NULL_HANDLE)
    {
      vkDestroyPipelineLayout (device, pipeline_layout, NULL);
    }

    if (descriptor_layout != VK_NULL_HANDLE)
    {
      vkDestroyDescriptorSetLayout (device, descriptor_layout, NULL);
    }

    for (uint32_t i = BUFFER_COUNT; i != 0u; i--)
    {
      destroy_test_buffer (device, &buffers[i - 1u]);
    }

    vkDestroyDevice (device, NULL);
  }

  return status;
}

/**
 * Run independent transfer-mode tests and then execute the public compute
 * shader on the first Vulkan device matching its explicitly declared limits.
 */
int main (int argc, char **argv)
{
  if (argc != 4 || strcmp (argv[2], "--mode") != 0
   || (strcmp (argv[3], "auto") != 0
    && strcmp (argv[3], "staging") != 0))
  {
    fprintf (
      stderr,
      "Usage: %s <stage-A-vulkan1.0.spv> --mode <auto|staging>\n",
      argv[0]
    );

    return 2;
  }

  size_t shader_bytes = 0u;
  uint32_t *shader = read_shader (argv[1], &shader_bytes);

  if (shader == NULL) return 2;

  if (check_transfer_policy () != 0)
  {
    fprintf (stderr, "INVALID_VULKAN_TRANSFER_POLICY\n");
    free (shader);

    return 1;
  }

  const VkApplicationInfo application =
  {
    .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
    .pApplicationName = "Portable Synthetic Vulkan Staging",
    .applicationVersion = VK_MAKE_VERSION (1, 0, 0),
    .pEngineName = "None",
    .engineVersion = VK_MAKE_VERSION (1, 0, 0),
    .apiVersion = VK_API_VERSION_1_0
  };
  const VkInstanceCreateInfo instance_info =
  {
    .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
    .pApplicationInfo = &application
  };

  VkInstance instance = VK_NULL_HANDLE;
  const VkResult instance_result = vkCreateInstance (
    &instance_info, NULL, &instance
  );

  if (instance_result != VK_SUCCESS)
  {
    fprintf (
      stderr, "VULKAN_INSTANCE_ERROR vkresult=%d\n", instance_result
    );
    free (shader);

    return 1;
  }

  int status = 1;
  uint32_t device_count = 0u;
  VkResult result = vkEnumeratePhysicalDevices (
    instance, &device_count, NULL
  );

  if (result != VK_SUCCESS || device_count == 0u)
  {
    fprintf (
      stderr,
      "NO_PHYSICAL_DEVICES count=%" PRIu32 " vkresult=%d\n",
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
    fprintf (stderr, "DEVICE_ENUMERATION_ERROR vkresult=%d\n", result);

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

  const VkBool32 forced =
    strcmp (argv[3], "staging") == 0 ? VK_TRUE : VK_FALSE;

  for (uint32_t index = 0u; index < device_count; index++)
  {
    vulkan_auto_device_t selected = { 0 };

    result = vulkan_auto_check_device (
      devices[index], index, &workload, &selected, stdout
    );

    if (result != VK_SUCCESS)
    {
      fprintf (
        stdout,
        "SKIP_VULKAN_DEVICE index=%" PRIu32 " reason=compute_limits\n",
        index
      );

      continue;
    }

    if (run_device (&selected, shader, shader_bytes, forced) == 0)
    {
      status = 0;

      break;
    }

    fprintf (
      stdout,
      "SKIP_VULKAN_DEVICE index=%" PRIu32 " reason=memory_or_gpu_error\n",
      index
    );
  }

  if (status != 0)
  {
    fprintf (
      stderr,
      "NO_SUPPORTED_VULKAN_TRANSFER_CONFIGURATION mode=%s\n",
      argv[3]
    );
  }

cleanup_devices:
  free (devices);

cleanup_instance:
  vkDestroyInstance (instance, NULL);
  free (shader);

  return status;
}
