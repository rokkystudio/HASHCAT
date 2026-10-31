/**
 * Discover a Vulkan 1.0-compatible compute device and actual memory policy.
 *
 * The standalone Android ARM64 test reads device limits and storage buffer
 * memory requirements from Vulkan APIs, selects a compatible host-visible
 * memory type, and verifies four aligned suballocations with public synthetic
 * CPU data. It validates coherent and noncoherent policy in isolated tests.
 *
 * Shader topology comes from a fixed public test workload. The first public
 * stage-A shader is dispatched and compared with a scalar CPU reference.
 * No hashcat kernel is opened and no value depends on a GPU model name.
 */

#include "vulkan-auto-config.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUFFER_COUNT 4u
#define ELEMENT_COUNT 2048u
#define RESULT_WORDS 4u

#define VK_CHECK(call) \
  do \
  { \
    result = (call); \
    if (result != VK_SUCCESS) \
    { \
      fprintf (stderr, "VULKAN_ERROR %s vkresult=%d\n", #call, result); \
      goto cleanup; \
    } \
  } while (0)

/**
 * Load an exact Vulkan 1.0 SPIR-V stream from a standalone shader file.
 * The caller owns and frees the returned allocation after device use.
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

  if (length <= 0 || length > 1024 * 1024 || length % 4 != 0
   || fseek (file, 0, SEEK_SET) != 0)
  {
    fclose (file);

    fprintf (stderr, "INVALID_SPIRV_SIZE path=%s\n", path);

    return NULL;
  }

  uint32_t *code = (uint32_t *) malloc ((size_t) length);

  if (code == NULL)
  {
    fclose (file);

    return NULL;
  }

  const size_t read_bytes = fread (code, 1, (size_t) length, file);

  fclose (file);

  if (read_bytes != (size_t) length || code[0] != 0x07230203u)
  {
    fprintf (stderr, "INVALID_SPIRV_MAGIC_OR_CONTENT path=%s\n", path);

    free (code);

    return NULL;
  }

  *bytes = (size_t) length;

  return code;
}

/**
 * Verify policy choices from exact artificial Vulkan memory properties.
 *
 * Covers coherent first-priority, host-visible noncoherent fallback,
 * noncoherent-atom offset alignment, missing memory support, and invalid
 * storage requirements independently of any connected device.
 */
static int verify_memory_policy (void)
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

  uint32_t type = UINT32_MAX;
  VkBool32 coherent = VK_FALSE;

  VkResult result = vulkan_auto_select_memory_type (
    &properties, 0x7u, &type, &coherent
  );

  if (result != VK_SUCCESS || type != 2u || coherent != VK_TRUE)
  {
    fprintf (stderr, "FAIL_POLICY coherent memory priority\n");

    return 1;
  }

  printf ("PASS_POLICY preferred_coherent_type=%" PRIu32 "\n", type);

  result = vulkan_auto_select_memory_type (
    &properties, 0x2u, &type, &coherent
  );

  if (result != VK_SUCCESS || type != 1u || coherent != VK_FALSE)
  {
    fprintf (stderr, "FAIL_POLICY noncoherent memory fallback\n");

    return 1;
  }

  printf ("PASS_POLICY fallback_noncoherent_type=%" PRIu32 "\n", type);

  const VkMemoryRequirements two_buffers[2] =
  {
    { .size = 384u, .alignment = 64u, .memoryTypeBits = 0x2u },
    { .size = 257u, .alignment = 64u, .memoryTypeBits = 0x2u }
  };
  const VkDeviceSize sizes[2] = { 384u, 257u };
  VkDeviceSize offsets[2] = { 0u };
  vulkan_auto_memory_plan_t plan = { 0 };

  result = vulkan_auto_plan_shared_memory (
    &properties, 256u, two_buffers, sizes, 2u,
    offsets, &plan, stdout
  );

  if (result != VK_SUCCESS || offsets[0] != 0u || offsets[1] != 512u
   || plan.allocation_bytes != 769u || plan.memory_type_index != 1u
   || plan.coherent != VK_FALSE)
  {
    fprintf (stderr, "FAIL_POLICY noncoherent atom padding\n");

    return 1;
  }

  printf (
    "PASS_POLICY noncoherent_atom=256 offset_1=%" PRIu64
    " allocation=%" PRIu64 "\n",
    (uint64_t) offsets[1], (uint64_t) plan.allocation_bytes
  );

  const VkMemoryRequirements coherent_buffers[2] =
  {
    { .size = 384u, .alignment = 64u, .memoryTypeBits = 0x6u },
    { .size = 257u, .alignment = 64u, .memoryTypeBits = 0x6u }
  };

  result = vulkan_auto_plan_shared_memory (
    &properties, 256u, coherent_buffers, sizes, 2u,
    offsets, &plan, stdout
  );

  if (result != VK_SUCCESS || offsets[1] != 384u
   || plan.allocation_bytes != 641u || plan.memory_type_index != 2u
   || plan.coherent != VK_TRUE)
  {
    fprintf (stderr, "FAIL_POLICY coherent offset alignment\n");

    return 1;
  }

  printf ("PASS_POLICY coherent_offset_1=%" PRIu64 "\n",
    (uint64_t) offsets[1]);

  result = vulkan_auto_select_memory_type (
    &properties, 0x1u, &type, &coherent
  );

  if (result != VK_ERROR_FEATURE_NOT_PRESENT)
  {
    fprintf (stderr, "FAIL_POLICY unsupported memory type accepted\n");

    return 1;
  }

  printf ("PASS_POLICY unsupported_memory_error=%d\n", result);

  VkMemoryRequirements invalid[2] =
  {
    { .size = 383u, .alignment = 64u, .memoryTypeBits = 0x6u },
    { .size = 257u, .alignment = 64u, .memoryTypeBits = 0x6u }
  };

  result = vulkan_auto_plan_shared_memory (
    &properties, 256u, invalid, sizes, 2u,
    offsets, &plan, stdout
  );

  if (result != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (stderr, "FAIL_POLICY insufficient required byte count\n");

    return 1;
  }

  printf ("PASS_POLICY invalid_requirements_error=%d\n", result);

  const VkMemoryRequirements overflowing[2] =
  {
    {
      .size = UINT64_MAX - 63u,
      .alignment = 64u,
      .memoryTypeBits = 0x6u
    },
    {
      .size = 257u,
      .alignment = 64u,
      .memoryTypeBits = 0x6u
    }
  };
  const VkDeviceSize overflow_sizes[2] = { 384u, 257u };

  result = vulkan_auto_plan_shared_memory (
    &properties, 256u, overflowing, overflow_sizes, 2u,
    offsets, &plan, stdout
  );

  if (result != VK_ERROR_OUT_OF_DEVICE_MEMORY)
  {
    fprintf (stderr, "FAIL_POLICY overflowing allocation accepted\n");

    return 1;
  }

  printf ("PASS_POLICY overflow_error=%d\n", result);

  return 0;
}

/**
 * Require the runtime capability checker to reject three invalid workloads.
 *
 * The checks use the same actual Vulkan physical device with explicit,
 * deliberately unsupported shader requirements. They do not create a
 * logical device, allocate memory or dispatch GPU work.
 */
static int verify_capability_rejections (
  VkPhysicalDevice physical,
  uint32_t index,
  const vulkan_auto_compute_requirements_t *supported,
  const VkPhysicalDeviceProperties *properties)
{
  vulkan_auto_compute_requirements_t unsupported = *supported;
  vulkan_auto_device_t selected = { 0 };

  unsupported.local_size[0] = 0u;

  VkResult result = vulkan_auto_check_device (
    physical, index, &unsupported, &selected, stdout
  );

  if (result != VK_ERROR_FEATURE_NOT_PRESENT)
  {
    fprintf (
      stderr, "FAIL_CAPABILITY_REJECTION zero_local_size vkresult=%d\n",
      result
    );

    return 1;
  }

  printf ("PASS_CAPABILITY_REJECTION zero_local_size vkresult=%d\n", result);

  unsupported = *supported;
  unsupported.group_count[0] = 0u;

  result = vulkan_auto_check_device (
    physical, index, &unsupported, &selected, stdout
  );

  if (result != VK_ERROR_FEATURE_NOT_PRESENT)
  {
    fprintf (
      stderr, "FAIL_CAPABILITY_REJECTION zero_workgroups vkresult=%d\n",
      result
    );

    return 1;
  }

  printf ("PASS_CAPABILITY_REJECTION zero_workgroups vkresult=%d\n", result);

  unsupported = *supported;
  unsupported.storage_buffer_count = 0u;

  result = vulkan_auto_check_device (
    physical, index, &unsupported, &selected, stdout
  );

  if (result != VK_ERROR_FEATURE_NOT_PRESENT)
  {
    fprintf (
      stderr, "FAIL_CAPABILITY_REJECTION missing_descriptors vkresult=%d\n",
      result
    );

    return 1;
  }

  printf ("PASS_CAPABILITY_REJECTION missing_descriptors vkresult=%d\n", result);

  if (properties->limits.maxComputeSharedMemorySize < UINT32_MAX)
  {
    unsupported = *supported;
    unsupported.shared_memory_bytes =
      properties->limits.maxComputeSharedMemorySize + 1u;

    result = vulkan_auto_check_device (
      physical, index, &unsupported, &selected, stdout
    );

    if (result != VK_ERROR_FEATURE_NOT_PRESENT)
    {
      fprintf (
        stderr, "FAIL_CAPABILITY_REJECTION shared_memory_limit vkresult=%d\n",
        result
      );

      return 1;
    }

    printf (
      "PASS_CAPABILITY_REJECTION shared_memory_limit vkresult=%d\n",
      result
    );
  }

  return 0;
}

/**
 * Execute one real Vulkan device trial with a single shared memory allocation.
 *
 * The trial creates four storage buffer handles, gets each driver's actual
 * requirements, selects the memory type and offsets, allocates once, binds
 * and maps all buffers, and checks public CPU test patterns. It flushes
 * noncoherent memory when necessary and dispatches one original synthetic
 * Vulkan 1.0 compute shader with exact GPU result verification.
 *
 * All buffers, mappings, memory and the logical device are destroyed before
 * this function returns. A failure is reported so another device may be tried.
 */
static int verify_device_memory (
  const vulkan_auto_device_t *selected,
  const uint32_t *shader,
  size_t shader_bytes)
{
  static const VkDeviceSize payloads[BUFFER_COUNT] =
  {
    ELEMENT_COUNT * sizeof (uint32_t),
    ELEMENT_COUNT * sizeof (uint32_t),
    ELEMENT_COUNT * sizeof (uint32_t),
    ELEMENT_COUNT * RESULT_WORDS * sizeof (uint32_t)
  };

  int status = 1;
  VkDevice device = VK_NULL_HANDLE;
  VkBuffer buffers[BUFFER_COUNT] = { VK_NULL_HANDLE };
  VkMemoryRequirements requirements[BUFFER_COUNT] = { 0 };
  VkDeviceSize offsets[BUFFER_COUNT] = { 0 };
  VkDeviceMemory allocation = VK_NULL_HANDLE;
  void *mapped = NULL;
  vulkan_auto_memory_plan_t plan = { 0 };
  VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkShaderModule shader_module = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
  VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
  VkCommandPool command_pool = VK_NULL_HANDLE;
  VkCommandBuffer command = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;

  const float queue_priority = 1.0f;
  const VkDeviceQueueCreateInfo queue_info =
  {
    .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
    .queueFamilyIndex = selected->compute_queue_family,
    .queueCount = 1,
    .pQueuePriorities = &queue_priority
  };
  const VkDeviceCreateInfo device_info =
  {
    .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
    .queueCreateInfoCount = 1,
    .pQueueCreateInfos = &queue_info
  };

  VkResult result = vkCreateDevice (
    selected->physical, &device_info, NULL, &device
  );

  if (result != VK_SUCCESS)
  {
    fprintf (stderr, "REJECT_DEVICE_CREATION vkresult=%d\n", result);

    goto cleanup;
  }

  for (uint32_t i = 0u; i < BUFFER_COUNT; i++)
  {
    const VkBufferCreateInfo create_info =
    {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = payloads[i],
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };

    result = vkCreateBuffer (device, &create_info, NULL, &buffers[i]);

    if (result != VK_SUCCESS)
    {
      fprintf (
        stderr, "REJECT_BUFFER index=%" PRIu32 " vkresult=%d\n", i, result
      );

      goto cleanup;
    }

    vkGetBufferMemoryRequirements (
      device, buffers[i], &requirements[i]
    );
  }

  result = vulkan_auto_plan_shared_memory (
    &selected->memory_properties,
    selected->properties.limits.nonCoherentAtomSize,
    requirements, payloads, BUFFER_COUNT, offsets, &plan, stdout
  );

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr,
      "REJECT_ALLOCATION_PLAN index=%" PRIu32
      " vkresult=%d\n",
      selected->index, result
    );

    goto cleanup;
  }

  const VkMemoryAllocateInfo memory_info =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
    .allocationSize = plan.allocation_bytes,
    .memoryTypeIndex = plan.memory_type_index
  };

  result = vkAllocateMemory (
    device, &memory_info, NULL, &allocation
  );

  if (result != VK_SUCCESS)
  {
    fprintf (stderr, "REJECT_VK_ALLOCATE_MEMORY vkresult=%d\n", result);

    goto cleanup;
  }

  for (uint32_t i = 0u; i < BUFFER_COUNT; i++)
  {
    result = vkBindBufferMemory (
      device, buffers[i], allocation, offsets[i]
    );

    if (result != VK_SUCCESS)
    {
      fprintf (
        stderr,
        "REJECT_VK_BIND_BUFFER_MEMORY index=%" PRIu32
        " vkresult=%d\n", i, result
      );

      goto cleanup;
    }
  }

  result = vkMapMemory (
    device, allocation, 0u, VK_WHOLE_SIZE, 0u, &mapped
  );

  if (result != VK_SUCCESS)
  {
    fprintf (stderr, "REJECT_VK_MAP_MEMORY vkresult=%d\n", result);

    goto cleanup;
  }

  for (uint32_t buffer = 0u; buffer < BUFFER_COUNT; buffer++)
  {
    uint32_t *words = (uint32_t *) (
      (uint8_t *) mapped + offsets[buffer]
    );

    for (uint32_t word = 0u;
         word < payloads[buffer] / sizeof (uint32_t);
         word++)
    {
      words[word] = word * 1664525u + buffer * 1013904223u;
    }
  }

  result = vulkan_auto_sync_mapped_memory (
    device, allocation, &plan, VK_FALSE
  );

  if (result != VK_SUCCESS)
  {
    fprintf (stderr, "REJECT_VK_FLUSH_MAPPED_MEMORY vkresult=%d\n", result);

    goto cleanup;
  }

  uint32_t verified_words = 0u;

  for (uint32_t buffer = 0u; buffer < BUFFER_COUNT; buffer++)
  {
    const uint32_t *words = (const uint32_t *) (
      (const uint8_t *) mapped + offsets[buffer]
    );

    for (uint32_t word = 0u;
         word < payloads[buffer] / sizeof (uint32_t);
         word++)
    {
      const uint32_t expected =
        word * 1664525u + buffer * 1013904223u;

      if (words[word] != expected)
      {
        fprintf (
          stderr,
          "REJECT_CPU_MAPPING buffer=%" PRIu32
          " word=%" PRIu32 " expected=%08" PRIx32
          " actual=%08" PRIx32 "\n",
          buffer, word, expected, words[word]
        );

        goto cleanup;
      }

      verified_words++;
    }
  }

  VkDescriptorSetLayoutBinding bindings[BUFFER_COUNT] = { 0 };

  for (uint32_t i = 0u; i < BUFFER_COUNT; i++)
  {
    bindings[i] = (VkDescriptorSetLayoutBinding)
    {
      .binding = i,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
    };
  }

  const VkDescriptorSetLayoutCreateInfo layout_info =
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
    .bindingCount = BUFFER_COUNT,
    .pBindings = bindings
  };

  VK_CHECK (vkCreateDescriptorSetLayout (
    device, &layout_info, NULL, &descriptor_layout
  ));

  const VkPipelineLayoutCreateInfo pipeline_layout_info =
  {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    .setLayoutCount = 1,
    .pSetLayouts = &descriptor_layout
  };

  VK_CHECK (vkCreatePipelineLayout (
    device, &pipeline_layout_info, NULL, &pipeline_layout
  ));

  const VkShaderModuleCreateInfo module_info =
  {
    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
    .codeSize = shader_bytes,
    .pCode = shader
  };

  VK_CHECK (vkCreateShaderModule (
    device, &module_info, NULL, &shader_module
  ));

  const VkComputePipelineCreateInfo compute_info =
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

  VK_CHECK (vkCreateComputePipelines (
    device, VK_NULL_HANDLE, 1, &compute_info, NULL, &pipeline
  ));

  const VkDescriptorPoolSize pool_size =
  {
    .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
    .descriptorCount = BUFFER_COUNT
  };
  const VkDescriptorPoolCreateInfo pool_info =
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
    .maxSets = 1,
    .poolSizeCount = 1,
    .pPoolSizes = &pool_size
  };

  VK_CHECK (vkCreateDescriptorPool (
    device, &pool_info, NULL, &descriptor_pool
  ));

  const VkDescriptorSetAllocateInfo set_info =
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
    .descriptorPool = descriptor_pool,
    .descriptorSetCount = 1,
    .pSetLayouts = &descriptor_layout
  };

  VK_CHECK (vkAllocateDescriptorSets (
    device, &set_info, &descriptor_set
  ));

  const VkDescriptorBufferInfo buffer_info[BUFFER_COUNT] =
  {
    { .buffer = buffers[0], .offset = 0, .range = VK_WHOLE_SIZE },
    { .buffer = buffers[1], .offset = 0, .range = VK_WHOLE_SIZE },
    { .buffer = buffers[2], .offset = 0, .range = VK_WHOLE_SIZE },
    { .buffer = buffers[3], .offset = 0, .range = VK_WHOLE_SIZE }
  };
  VkWriteDescriptorSet writes[BUFFER_COUNT] = { 0 };

  for (uint32_t i = 0u; i < BUFFER_COUNT; i++)
  {
    writes[i] = (VkWriteDescriptorSet)
    {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = descriptor_set,
      .dstBinding = i,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .pBufferInfo = &buffer_info[i]
    };
  }

  vkUpdateDescriptorSets (device, BUFFER_COUNT, writes, 0, NULL);

  const VkCommandPoolCreateInfo command_pool_info =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
    .queueFamilyIndex = selected->compute_queue_family
  };

  VK_CHECK (vkCreateCommandPool (
    device, &command_pool_info, NULL, &command_pool
  ));

  const VkCommandBufferAllocateInfo command_info =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
    .commandPool = command_pool,
    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
    .commandBufferCount = 1
  };

  VK_CHECK (vkAllocateCommandBuffers (
    device, &command_info, &command
  ));

  const VkCommandBufferBeginInfo begin_info =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
  };

  VK_CHECK (vkBeginCommandBuffer (command, &begin_info));

  const VkMemoryBarrier upload_barrier =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT
  };

  vkCmdPipelineBarrier (
    command,
    VK_PIPELINE_STAGE_HOST_BIT,
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
    0, 1, &upload_barrier, 0, NULL, 0, NULL
  );

  vkCmdBindPipeline (
    command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline
  );
  vkCmdBindDescriptorSets (
    command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
    0, 1, &descriptor_set, 0, NULL
  );
  vkCmdDispatch (command, ELEMENT_COUNT / 32u, 1, 1);

  const VkMemoryBarrier download_barrier =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_HOST_READ_BIT
  };

  vkCmdPipelineBarrier (
    command,
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
    VK_PIPELINE_STAGE_HOST_BIT,
    0, 1, &download_barrier, 0, NULL, 0, NULL
  );

  VK_CHECK (vkEndCommandBuffer (command));

  const VkFenceCreateInfo fence_info =
  {
    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
  };

  VK_CHECK (vkCreateFence (device, &fence_info, NULL, &fence));

  const VkSubmitInfo submit_info =
  {
    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    .commandBufferCount = 1,
    .pCommandBuffers = &command
  };

  vkGetDeviceQueue (
    device, selected->compute_queue_family, 0, &queue
  );

  VK_CHECK (vkQueueSubmit (queue, 1, &submit_info, fence));
  VK_CHECK (vkWaitForFences (
    device, 1, &fence, VK_TRUE, UINT64_C (30000000000)
  ));

  VK_CHECK (vulkan_auto_sync_mapped_memory (
    device, allocation, &plan, VK_TRUE
  ));

  const uint32_t *input = (const uint32_t *) (
    (const uint8_t *) mapped + offsets[0]
  );
  const uint32_t *gpu_output = (const uint32_t *) (
    (const uint8_t *) mapped + offsets[1]
  );
  uint64_t output_checksum = UINT64_C (14695981039346656037);

  for (uint32_t index = 0u; index < ELEMENT_COUNT; index++)
  {
    const uint32_t value = input[index];
    const uint32_t affine = value * 1664525u + 1013904223u;
    const uint32_t rotated = (value << 7u) | (value >> 25u);
    const uint32_t expected =
      (affine ^ rotated) + index * 2654435761u;

    if (gpu_output[index] != expected)
    {
      fprintf (
        stderr,
        "REJECT_SYNTHETIC_GPU_RESULT index=%" PRIu32
        " expected=%08" PRIx32 " actual=%08" PRIx32 "\n",
        index, expected, gpu_output[index]
      );

      goto cleanup;
    }

    for (uint32_t byte = 0u; byte < 4u; byte++)
    {
      const uint64_t value =
        (uint64_t) ((gpu_output[index] >> (byte * 8u)) & 0xffu);

      output_checksum =
        (output_checksum ^ value) * UINT64_C (1099511628211);
    }
  }

  printf (
    "PASS_VULKAN_GPU_STAGE device_index=%" PRIu32
    " local_size=32 groups=64 words=%u"
    " memory_type=%" PRIu32 " coherent=%u"
    " fnv1a64=%016" PRIx64 "\n",
    selected->index, ELEMENT_COUNT,
    plan.memory_type_index, plan.coherent,
    output_checksum
  );

  printf (
    "PASS_VULKAN_AUTO_CONFIG device_index=%" PRIu32
    " queue_family=%" PRIu32
    " memory_type=%" PRIu32 " coherent=%u"
    " allocation_bytes=%" PRIu64
    " buffers=%u verified_cpu_words=%" PRIu32
    " gpu_dispatches=1 verified_gpu_words=%u\n",
    selected->index,
    selected->compute_queue_family,
    plan.memory_type_index, plan.coherent,
    (uint64_t) plan.allocation_bytes,
    BUFFER_COUNT, verified_words, ELEMENT_COUNT
  );

  status = 0;

cleanup:
  if (device != VK_NULL_HANDLE)
  {
    const VkResult idle = vkDeviceWaitIdle (device);

    if (idle != VK_SUCCESS)
    {
      fprintf (stderr, "VULKAN_CLEANUP_IDLE_ERROR vkresult=%d\n", idle);

      status = 1;
    }

    if (fence != VK_NULL_HANDLE) vkDestroyFence (device, fence, NULL);

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

    if (mapped != NULL)
    {
      vkUnmapMemory (device, allocation);
    }

    for (uint32_t i = 0u; i < BUFFER_COUNT; i++)
    {
      if (buffers[i] != VK_NULL_HANDLE)
      {
        vkDestroyBuffer (device, buffers[i], NULL);
      }
    }

    if (allocation != VK_NULL_HANDLE)
    {
      vkFreeMemory (device, allocation, NULL);
    }

    vkDestroyDevice (device, NULL);
  }

  return status;
}

/**
 * Select the first API-compatible and memory-compatible Vulkan compute GPU.
 *
 * The diagnostic's fixed 32-lane public shader metadata determines limits
 * to check. All actual device and allocation parameters are obtained from
 * Vulkan APIs. Every rejected device yields a reason, and no selection uses
 * Android model, vendor ID or chipset name.
 */
int main (int argc, char **argv)
{
  if (argc != 2)
  {
    fprintf (
      stderr, "Usage: %s <stage-A-vulkan1.0.spv>\n", argv[0]
    );

    return 2;
  }

  size_t shader_bytes = 0u;
  uint32_t *shader = read_shader (argv[1], &shader_bytes);

  if (shader == NULL) return 2;

  if (verify_memory_policy () != 0)
  {
    free (shader);

    return 1;
  }

  const VkApplicationInfo application =
  {
    .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
    .pApplicationName = "Portable Vulkan Compute Configuration",
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
  VkResult result = vkCreateInstance (
    &instance_info, NULL, &instance
  );

  if (result != VK_SUCCESS)
  {
    fprintf (stderr, "VULKAN_INSTANCE_ERROR vkresult=%d\n", result);

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
      "NO_PHYSICAL_VULKAN_DEVICES vkresult=%d count=%" PRIu32 "\n",
      result, device_count
    );

    goto cleanup_instance;
  }

  VkPhysicalDevice *devices = (VkPhysicalDevice *) calloc (
    device_count, sizeof (*devices)
  );

  if (devices == NULL)
  {
    fprintf (stderr, "VULKAN_HOST_ALLOCATION_FAILED\n");

    goto cleanup_instance;
  }

  result = vkEnumeratePhysicalDevices (
    instance, &device_count, devices
  );

  if (result != VK_SUCCESS)
  {
    fprintf (stderr, "VULKAN_DEVICE_ENUMERATION_FAILED vkresult=%d\n", result);

    goto cleanup_devices;
  }

  /*
   * These are fixed, documented properties of the public example shader.
   * They are not guessed from any device model or driver characteristics.
   */
  const vulkan_auto_compute_requirements_t workload =
  {
    .local_size = { 32u, 1u, 1u },
    .group_count = { 64u, 1u, 1u },
    .storage_buffer_count = BUFFER_COUNT,
    .largest_storage_buffer_bytes =
      ELEMENT_COUNT * RESULT_WORDS * sizeof (uint32_t),
    .shared_memory_bytes = 32u * sizeof (uint32_t)
  };

  printf (
    "WORKLOAD_PROFILE vulkan_min=1.0"
    " local_x=%" PRIu32 " groups_x=%" PRIu32
    " descriptors=%" PRIu32 " largest_range=%" PRIu64
    " shared_bytes=%" PRIu32 "\n",
    workload.local_size[0],
    workload.group_count[0],
    workload.storage_buffer_count,
    (uint64_t) workload.largest_storage_buffer_bytes,
    workload.shared_memory_bytes
  );

  for (uint32_t index = 0u; index < device_count; index++)
  {
    vulkan_auto_device_t selected = { 0 };

    result = vulkan_auto_check_device (
      devices[index], index, &workload, &selected, stdout
    );

    if (result != VK_SUCCESS)
    {
      printf (
        "SKIP_VULKAN_DEVICE index=%" PRIu32 " vkresult=%d\n",
        index, result
      );

      continue;
    }

    if (verify_capability_rejections (
      devices[index], index, &workload, &selected.properties
    ) != 0)
    {
      status = 1;

      break;
    }

    if (verify_device_memory (&selected, shader, shader_bytes) == 0)
    {
      status = 0;

      break;
    }

    printf (
      "SKIP_VULKAN_DEVICE index=%" PRIu32
      " reason=memory_or_resource_error\n", index
    );
  }

  if (status != 0)
  {
    fprintf (
      stderr,
      "NO_COMPATIBLE_VULKAN_COMPUTE_CONFIGURATION"
      " source=runtime_capabilities\n"
    );
  }

cleanup_devices:
  free (devices);

cleanup_instance:
  vkDestroyInstance (instance, NULL);
  free (shader);

  return status;
}
