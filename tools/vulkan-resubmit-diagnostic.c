/**
 * Exercise repeated submissions of one Vulkan 1.1 compute command buffer.
 *
 * The standalone Android program records three synthetic compute dispatches
 * and their buffer memory dependencies once, submits the same executable
 * command buffer multiple times, refreshes coherent input before each submit,
 * resets a completed fence, and checks all intermediate/final output words.
 * No hashcat GPU kernels, credentials, passwords or hashes are processed.
 */

#include <vulkan/vulkan.h>

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ELEMENT_COUNT 2048u
#define LOCAL_SIZE 32u
#define RESULT_WORDS 4u
#define STAGE_COUNT 3u

typedef struct host_buffer
{
  VkBuffer buffer;
  VkDeviceMemory memory;
  void *mapped;
} host_buffer_t;

#define VK_CHECK(call) \
  do \
  { \
    const VkResult checked = (call); \
    if (checked != VK_SUCCESS) \
    { \
      fprintf (stderr, "VULKAN_ERROR %s: VkResult=%d\n", #call, checked); \
      goto cleanup; \
    } \
  } while (0)

/**
 * Read one valid SPIR-V word stream from disk and report its exact byte size.
 * The caller owns the allocation returned by this function.
 */
static uint32_t *read_shader (const char *path, size_t *shader_bytes)
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

  if (length <= 0 || length > 1024 * 1024 || (length % 4) != 0
   || fseek (file, 0, SEEK_SET) != 0)
  {
    fprintf (stderr, "Invalid SPIR-V word stream: %s\n", path);

    fclose (file);

    return NULL;
  }

  uint32_t *result = (uint32_t *) malloc ((size_t) length);

  if (result == NULL)
  {
    fclose (file);

    return NULL;
  }

  if (fread (result, 1, (size_t) length, file) != (size_t) length)
  {
    perror ("shader fread");

    free (result);
    fclose (file);

    return NULL;
  }

  fclose (file);

  if (result[0] != 0x07230203u)
  {
    fprintf (stderr, "Invalid SPIR-V magic: %s\n", path);

    free (result);

    return NULL;
  }

  *shader_bytes = (size_t) length;

  return result;
}

/**
 * Create and map one coherent, host-visible Vulkan storage buffer.
 * The caller releases the mapped region, buffer, and allocation.
 */
static VkResult create_host_buffer (
  VkPhysicalDevice physical,
  VkDevice device,
  VkDeviceSize bytes,
  host_buffer_t *destination)
{
  const VkBufferCreateInfo create_info =
  {
    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
    .size = bytes,
    .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
    .sharingMode = VK_SHARING_MODE_EXCLUSIVE
  };

  VkResult result = vkCreateBuffer (
    device, &create_info, NULL, &destination->buffer
  );

  if (result != VK_SUCCESS) return result;

  VkMemoryRequirements requirements = { 0 };

  vkGetBufferMemoryRequirements (device, destination->buffer, &requirements);

  VkPhysicalDeviceMemoryProperties memory = { 0 };

  vkGetPhysicalDeviceMemoryProperties (physical, &memory);

  uint32_t memory_type = UINT32_MAX;

  for (uint32_t i = 0; i < memory.memoryTypeCount; i++)
  {
    const uint32_t bit = 1u << i;
    const VkMemoryPropertyFlags flags = memory.memoryTypes[i].propertyFlags;

    if ((requirements.memoryTypeBits & bit) != 0
     && (flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
       == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
         | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
    {
      memory_type = i;

      break;
    }
  }

  if (memory_type == UINT32_MAX)
  {
    fprintf (stderr, "No host-visible and host-coherent Vulkan memory type\n");

    return VK_ERROR_FEATURE_NOT_PRESENT;
  }

  const VkMemoryAllocateInfo allocate_info =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
    .allocationSize = requirements.size,
    .memoryTypeIndex = memory_type
  };

  result = vkAllocateMemory (device, &allocate_info, NULL, &destination->memory);

  if (result != VK_SUCCESS) return result;

  result = vkBindBufferMemory (device, destination->buffer, destination->memory, 0);

  if (result != VK_SUCCESS) return result;

  return vkMapMemory (device, destination->memory, 0, bytes, 0, &destination->mapped);
}

/**
 * Unmap and release the Vulkan resources of one host-visible storage buffer.
 */
static void destroy_host_buffer (VkDevice device, host_buffer_t *buffer)
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
 * Derive the exact synthetic input word for a fixed element index.
 * Unsigned arithmetic is performed modulo 2^32.
 */
static uint32_t synthetic_input (uint32_t index)
{
  return (index * 0x9e3779b9u)
       ^ (index << 15u)
       ^ (index >> 3u)
       ^ 0xa5a5a5a5u;
}

/**
 * Derive a submission-specific public input while preserving 32-bit wrapping.
 * Different submissions use different input words in the same mapped buffer.
 */
static uint32_t submitted_input (uint32_t index, uint32_t submission)
{
  return synthetic_input (index) ^ (submission * 0x9e3779b9u);
}

/**
 * Calculate stage A from the exact input words written for one submission.
 */
static uint32_t stage_a_reference (uint32_t index, uint32_t submission)
{
  const uint32_t value = submitted_input (index, submission);
  const uint32_t affine = value * 1664525u + 1013904223u;
  const uint32_t rotated = (value << 7u) | (value >> 25u);

  return (affine ^ rotated) + index * 2654435761u;
}

/**
 * Calculate stage B from stage-A values of the same submission.
 * The seven-lane neighbor always wraps inside its own 32-lane workgroup.
 */
static uint32_t stage_b_reference (uint32_t index, uint32_t submission)
{
  const uint32_t lane = index & (LOCAL_SIZE - 1u);
  const uint32_t group_start = index & ~(LOCAL_SIZE - 1u);
  const uint32_t neighbor_index =
    group_start + ((lane + 7u) & (LOCAL_SIZE - 1u));
  const uint32_t value = stage_a_reference (index, submission);
  const uint32_t neighbor = stage_a_reference (neighbor_index, submission);
  const uint32_t rotated = (value << 11u) | (value >> 21u);

  return (value + neighbor) ^ rotated ^ 0x6c8e9cf5u;
}

/**
 * Calculate the FNV-1a checksum of 32-bit words in device byte order.
 * A consistent little-endian Android AArch64 CPU is used for verification.
 */
static uint64_t words_checksum (const uint32_t *words, uint32_t count)
{
  const uint8_t *bytes = (const uint8_t *) words;
  uint64_t checksum = UINT64_C (14695981039346656037);

  for (size_t i = 0; i < (size_t) count * sizeof (uint32_t); i++)
  {
    checksum = (checksum ^ bytes[i]) * UINT64_C (1099511628211);
  }

  return checksum;
}

/**
 * Check every intermediate and final GPU word for a specific submission.
 * Print the first incorrect stage, submission, index and 32-bit value.
 */
static int verify_pipeline (
  const uint32_t *stage_a,
  const uint32_t *stage_b,
  const uint32_t *output,
  uint32_t submission)
{
  uint32_t mismatches = 0u;

  for (uint32_t index = 0; index < ELEMENT_COUNT; index++)
  {
    const uint32_t a = stage_a_reference (index, submission);
    const uint32_t b = stage_b_reference (index, submission);
    const uint32_t rotated = (b << 3u) | (b >> 29u);

    const uint32_t expected[6] =
    {
      a,
      b,
      a,
      b,
      a ^ b,
      (a * 2246822519u + b * 3266489917u) ^ rotated
    };

    const uint32_t actual[6] =
    {
      stage_a[index],
      stage_b[index],
      output[index * RESULT_WORDS + 0u],
      output[index * RESULT_WORDS + 1u],
      output[index * RESULT_WORDS + 2u],
      output[index * RESULT_WORDS + 3u]
    };

    for (uint32_t word = 0; word < 6u; word++)
    {
      if (actual[word] != expected[word])
      {
        if (mismatches < 8u)
        {
          fprintf (
            stderr,
            "PIPELINE_MISMATCH submission=%" PRIu32
            " stage=%s index=%" PRIu32
            " word=%" PRIu32 " expected=%08" PRIx32
            " actual=%08" PRIx32 "\n",
            submission,
            word == 0u ? "A" : word == 1u ? "B" : "C",
            index, word, expected[word], actual[word]
          );
        }

        mismatches++;
      }
    }
  }

  if (mismatches != 0u)
  {
    fprintf (
      stderr,
      "FAIL Vulkan 3-stage replay submission=%" PRIu32
      " incorrect words=%" PRIu32 "\n",
      submission, mismatches
    );

    return 1;
  }

  const uint64_t checksum_a = words_checksum (stage_a, ELEMENT_COUNT);
  const uint64_t checksum_b = words_checksum (stage_b, ELEMENT_COUNT);
  const uint64_t checksum_c = words_checksum (
    output, ELEMENT_COUNT * RESULT_WORDS
  );

  printf (
    "PASS Vulkan replay submission=%" PRIu32 ": %" PRIu32
    " elements, %" PRIu32 " correct 32-bit words; "
    "stageA=%016" PRIx64 " stageB=%016" PRIx64
    " output=%016" PRIx64 "\n",
    submission, ELEMENT_COUNT, ELEMENT_COUNT * (RESULT_WORDS + 2u),
    checksum_a, checksum_b, checksum_c
  );

  return 0;
}

/**
 * Record a reusable three-stage compute command buffer and resubmit it
 * serially with distinct input generations, a reset fence and CPU checks.
 */
int main (int argc, char **argv)
{
  if (argc != 6 || strcmp (argv[4], "--submissions") != 0)
  {
    fprintf (
      stderr,
      "Usage: %s <stage-A.spv> <stage-B.spv> <stage-C.spv>"
      " --submissions <2..16>\n", argv[0]
    );

    return 2;
  }

  errno = 0;
  char *end = NULL;
  const long requested = strtol (argv[5], &end, 10);

  if (errno != 0 || end == argv[5] || *end != 0
   || requested < 2 || requested > 16)
  {
    fprintf (stderr, "Invalid submissions count (expected integer 2..16)\n");

    return 2;
  }

  const uint32_t submissions = (uint32_t) requested;
  int status = 1;
  size_t shader_bytes[STAGE_COUNT] = { 0 };
  uint32_t *shader_code[STAGE_COUNT] = { NULL };

  for (uint32_t stage = 0; stage < STAGE_COUNT; stage++)
  {
    shader_code[stage] = read_shader (argv[stage + 1u], &shader_bytes[stage]);

    if (shader_code[stage] == NULL)
    {
      for (uint32_t prior = 0; prior < stage; prior++) free (shader_code[prior]);

      return 2;
    }
  }

  VkInstance instance = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkPhysicalDevice *physical_devices = NULL;
  VkQueueFamilyProperties *queue_families = NULL;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkShaderModule shader_modules[STAGE_COUNT] = { VK_NULL_HANDLE };
  VkPipeline pipelines[STAGE_COUNT] = { VK_NULL_HANDLE };
  VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
  VkCommandPool command_pool = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  host_buffer_t input = { 0 };
  host_buffer_t state_a = { 0 };
  host_buffer_t state_b = { 0 };
  host_buffer_t output = { 0 };
  uint32_t queue_family = UINT32_MAX;
  VkQueue queue = VK_NULL_HANDLE;

  const VkApplicationInfo application_info =
  {
    .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
    .pApplicationName = "Synthetic Vulkan Command Reuse Diagnostic",
    .applicationVersion = VK_MAKE_VERSION (1, 0, 0),
    .pEngineName = "None",
    .engineVersion = VK_MAKE_VERSION (1, 0, 0),
    .apiVersion = VK_API_VERSION_1_1
  };

  const VkInstanceCreateInfo instance_info =
  {
    .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
    .pApplicationInfo = &application_info
  };

  VK_CHECK (vkCreateInstance (&instance_info, NULL, &instance));

  uint32_t physical_count = 0;

  VK_CHECK (vkEnumeratePhysicalDevices (instance, &physical_count, NULL));

  if (physical_count == 0u)
  {
    fprintf (stderr, "No Vulkan physical devices\n");

    goto cleanup;
  }

  physical_devices = (VkPhysicalDevice *) calloc (
    physical_count, sizeof (*physical_devices)
  );

  if (physical_devices == NULL) goto cleanup;

  VK_CHECK (vkEnumeratePhysicalDevices (
    instance, &physical_count, physical_devices
  ));

  for (uint32_t i = 0; i < physical_count; i++)
  {
    uint32_t family_count = 0;

    vkGetPhysicalDeviceQueueFamilyProperties (
      physical_devices[i], &family_count, NULL
    );

    if (family_count == 0u) continue;

    queue_families = (VkQueueFamilyProperties *) calloc (
      family_count, sizeof (*queue_families)
    );

    if (queue_families == NULL) goto cleanup;

    vkGetPhysicalDeviceQueueFamilyProperties (
      physical_devices[i], &family_count, queue_families
    );

    for (uint32_t j = 0; j < family_count; j++)
    {
      if (queue_families[j].queueCount != 0u
       && (queue_families[j].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0)
      {
        physical = physical_devices[i];
        queue_family = j;

        break;
      }
    }

    free (queue_families);
    queue_families = NULL;

    if (physical != VK_NULL_HANDLE) break;
  }

  if (physical == VK_NULL_HANDLE)
  {
    fprintf (stderr, "No Vulkan compute queue found\n");

    goto cleanup;
  }

  VkPhysicalDeviceProperties properties = { 0 };

  vkGetPhysicalDeviceProperties (physical, &properties);

  printf (
    "VULKAN_DEVICE name=%s vendor=0x%04" PRIx32
    " device=0x%04" PRIx32 " driver=0x%08" PRIx32
    " api=%" PRIu32 ".%" PRIu32 ".%" PRIu32
    " queue_family=%" PRIu32 "\n",
    properties.deviceName, properties.vendorID, properties.deviceID,
    properties.driverVersion, VK_VERSION_MAJOR (properties.apiVersion),
    VK_VERSION_MINOR (properties.apiVersion),
    VK_VERSION_PATCH (properties.apiVersion), queue_family
  );

  printf (
    "VULKAN_LIMITS workgroup_x=%" PRIu32
    " invocations=%" PRIu32 " shared_bytes=%" PRIu32 "\n",
    properties.limits.maxComputeWorkGroupSize[0],
    properties.limits.maxComputeWorkGroupInvocations,
    properties.limits.maxComputeSharedMemorySize
  );

  if (VK_VERSION_MAJOR (properties.apiVersion) < 1u
   || (VK_VERSION_MAJOR (properties.apiVersion) == 1u
    && VK_VERSION_MINOR (properties.apiVersion) < 1u)
   || properties.limits.maxComputeWorkGroupSize[0] < LOCAL_SIZE
   || properties.limits.maxComputeWorkGroupInvocations < LOCAL_SIZE
   || properties.limits.maxComputeSharedMemorySize
      < LOCAL_SIZE * sizeof (uint32_t))
  {
    fprintf (stderr, "Required Vulkan 1.1 / compute limits are not supported\n");

    goto cleanup;
  }

  const float queue_priority = 1.0f;
  const VkDeviceQueueCreateInfo queue_info =
  {
    .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
    .queueFamilyIndex = queue_family,
    .queueCount = 1,
    .pQueuePriorities = &queue_priority
  };
  const VkDeviceCreateInfo device_info =
  {
    .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
    .queueCreateInfoCount = 1,
    .pQueueCreateInfos = &queue_info
  };

  VK_CHECK (vkCreateDevice (physical, &device_info, NULL, &device));

  vkGetDeviceQueue (device, queue_family, 0, &queue);

  VK_CHECK (create_host_buffer (
    physical, device, ELEMENT_COUNT * sizeof (uint32_t), &input
  ));
  VK_CHECK (create_host_buffer (
    physical, device, ELEMENT_COUNT * sizeof (uint32_t), &state_a
  ));
  VK_CHECK (create_host_buffer (
    physical, device, ELEMENT_COUNT * sizeof (uint32_t), &state_b
  ));
  VK_CHECK (create_host_buffer (
    physical, device, ELEMENT_COUNT * RESULT_WORDS * sizeof (uint32_t), &output
  ));

  uint32_t *input_words = (uint32_t *) input.mapped;

  /* Input and all output buffers are filled after the previous fence wait. */

  const VkDescriptorSetLayoutBinding bindings[4] =
  {
    {
      .binding = 0,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
    },
    {
      .binding = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
    },
    {
      .binding = 2,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
    },
    {
      .binding = 3,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
    }
  };
  const VkDescriptorSetLayoutCreateInfo set_info =
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
    .bindingCount = 4,
    .pBindings = bindings
  };

  VK_CHECK (vkCreateDescriptorSetLayout (
    device, &set_info, NULL, &set_layout
  ));

  const VkPipelineLayoutCreateInfo pipeline_layout_info =
  {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    .setLayoutCount = 1,
    .pSetLayouts = &set_layout
  };

  VK_CHECK (vkCreatePipelineLayout (
    device, &pipeline_layout_info, NULL, &pipeline_layout
  ));

  for (uint32_t stage = 0; stage < STAGE_COUNT; stage++)
  {
    const VkShaderModuleCreateInfo module_info =
    {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = shader_bytes[stage],
      .pCode = shader_code[stage]
    };

    VK_CHECK (vkCreateShaderModule (
      device, &module_info, NULL, &shader_modules[stage]
    ));

    const VkComputePipelineCreateInfo pipeline_info =
    {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage =
      {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
        .module = shader_modules[stage],
        .pName = "main"
      },
      .layout = pipeline_layout
    };

    VK_CHECK (vkCreateComputePipelines (
      device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipelines[stage]
    ));
  }

  const VkDescriptorPoolSize pool_size =
  {
    .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
    .descriptorCount = 4
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

  const VkDescriptorSetAllocateInfo set_alloc =
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
    .descriptorPool = descriptor_pool,
    .descriptorSetCount = 1,
    .pSetLayouts = &set_layout
  };
  VkDescriptorSet descriptor_set = VK_NULL_HANDLE;

  VK_CHECK (vkAllocateDescriptorSets (device, &set_alloc, &descriptor_set));

  const VkDescriptorBufferInfo buffer_infos[4] =
  {
    { .buffer = input.buffer,   .offset = 0, .range = VK_WHOLE_SIZE },
    { .buffer = state_a.buffer, .offset = 0, .range = VK_WHOLE_SIZE },
    { .buffer = state_b.buffer, .offset = 0, .range = VK_WHOLE_SIZE },
    { .buffer = output.buffer,  .offset = 0, .range = VK_WHOLE_SIZE }
  };
  VkWriteDescriptorSet descriptors[4] = { 0 };

  for (uint32_t binding = 0; binding < 4u; binding++)
  {
    descriptors[binding] = (VkWriteDescriptorSet)
    {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = descriptor_set,
      .dstBinding = binding,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .pBufferInfo = &buffer_infos[binding]
    };
  }

  vkUpdateDescriptorSets (device, 4, descriptors, 0, NULL);

  const VkCommandPoolCreateInfo command_pool_info =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
    .queueFamilyIndex = queue_family
  };

  VK_CHECK (vkCreateCommandPool (
    device, &command_pool_info, NULL, &command_pool
  ));

  const VkCommandBufferAllocateInfo command_alloc =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
    .commandPool = command_pool,
    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
    .commandBufferCount = 1
  };
  VkCommandBuffer command = VK_NULL_HANDLE;

  VK_CHECK (vkAllocateCommandBuffers (device, &command_alloc, &command));

  const VkCommandBufferBeginInfo begin_info =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags = 0
  };

  VK_CHECK (vkBeginCommandBuffer (command, &begin_info));

  vkCmdBindDescriptorSets (
    command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
    0, 1, &descriptor_set, 0, NULL
  );

  const VkMemoryBarrier input_barrier =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT
  };

  vkCmdPipelineBarrier (
    command,
    VK_PIPELINE_STAGE_HOST_BIT,
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
    0, 1, &input_barrier, 0, NULL, 0, NULL
  );

  const VkMemoryBarrier stage_barrier =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT
  };

  for (uint32_t stage = 0; stage < STAGE_COUNT; stage++)
  {
    vkCmdBindPipeline (
      command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[stage]
    );
    vkCmdDispatch (command, ELEMENT_COUNT / LOCAL_SIZE, 1, 1);

    if (stage + 1u < STAGE_COUNT)
    {
      vkCmdPipelineBarrier (
        command,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 1, &stage_barrier, 0, NULL, 0, NULL
      );
    }
  }

  const VkMemoryBarrier readback_barrier =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_HOST_READ_BIT
  };

  vkCmdPipelineBarrier (
    command,
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
    VK_PIPELINE_STAGE_HOST_BIT,
    0, 1, &readback_barrier, 0, NULL, 0, NULL
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
    .commandBufferCount = 1,
    .pCommandBuffers = &command
  };

  for (uint32_t submission = 0; submission < submissions; submission++)
  {
    /*
     * Each prior submission completed before the host touches these mapped
     * buffers. Host-coherent memory requires no explicit flush/invalidate.
     */
    for (uint32_t index = 0; index < ELEMENT_COUNT; index++)
    {
      input_words[index] = submitted_input (index, submission);
    }

    memset (state_a.mapped, 0xa5, ELEMENT_COUNT * sizeof (uint32_t));
    memset (state_b.mapped, 0x5a, ELEMENT_COUNT * sizeof (uint32_t));
    memset (
      output.mapped, 0xc3,
      ELEMENT_COUNT * RESULT_WORDS * sizeof (uint32_t)
    );

    if (submission > 0u)
    {
      VK_CHECK (vkResetFences (device, 1, &fence));
    }

    VK_CHECK (vkQueueSubmit (queue, 1, &submit, fence));
    VK_CHECK (vkWaitForFences (
      device, 1, &fence, VK_TRUE, UINT64_C (30000000000)
    ));

    status = verify_pipeline (
      (const uint32_t *) state_a.mapped,
      (const uint32_t *) state_b.mapped,
      (const uint32_t *) output.mapped,
      submission
    );

    if (status != 0) goto cleanup;
  }

  printf (
    "PASS Vulkan command reuse: %" PRIu32
    " sequential submissions, 3 dispatches/submission, "
    "12288 verified words/submission\n",
    submissions
  );

cleanup:
  if (device != VK_NULL_HANDLE)
  {
    (void) vkDeviceWaitIdle (device);

    if (fence != VK_NULL_HANDLE) vkDestroyFence (device, fence, NULL);
    if (command_pool != VK_NULL_HANDLE)
    {
      vkDestroyCommandPool (device, command_pool, NULL);
    }
    if (descriptor_pool != VK_NULL_HANDLE)
    {
      vkDestroyDescriptorPool (device, descriptor_pool, NULL);
    }

    for (uint32_t stage = 0; stage < STAGE_COUNT; stage++)
    {
      if (pipelines[stage] != VK_NULL_HANDLE)
      {
        vkDestroyPipeline (device, pipelines[stage], NULL);
      }

      if (shader_modules[stage] != VK_NULL_HANDLE)
      {
        vkDestroyShaderModule (device, shader_modules[stage], NULL);
      }
    }

    if (pipeline_layout != VK_NULL_HANDLE)
    {
      vkDestroyPipelineLayout (device, pipeline_layout, NULL);
    }
    if (set_layout != VK_NULL_HANDLE)
    {
      vkDestroyDescriptorSetLayout (device, set_layout, NULL);
    }

    destroy_host_buffer (device, &output);
    destroy_host_buffer (device, &state_b);
    destroy_host_buffer (device, &state_a);
    destroy_host_buffer (device, &input);
    vkDestroyDevice (device, NULL);
  }

  if (instance != VK_NULL_HANDLE) vkDestroyInstance (instance, NULL);

  free (queue_families);
  free (physical_devices);
  for (uint32_t stage = 0; stage < STAGE_COUNT; stage++) free (shader_code[stage]);

  return status;
}
