/**
 * Validate Vulkan 1.1 shared memory suballocations and memory error handling.
 *
 * Four standalone synthetic compute storage buffers share one VkDeviceMemory
 * allocation at distinct alignment-correct offsets. Three GPU stages execute
 * using the existing public, noncryptographic Vulkan shaders. Per-stage GPU
 * results are checked against a scalar CPU oracle.
 *
 * Controlled --simulate failures test application error handling for missing
 * memory types, failed allocation, and VK_ERROR_DEVICE_LOST after completion.
 * No real device loss, oversized allocation, password or hash is involved.
 */

#include <vulkan/vulkan.h>

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ELEMENT_COUNT 2048u
#define LOCAL_SIZE 32u
#define RESULT_WORDS 4u
#define STAGE_COUNT 3u
#define BUFFER_COUNT 4u
#define ALLOCATION_LIMIT_BYTES (1024u * 1024u)

typedef struct host_buffer
{
  VkBuffer buffer;
  VkMemoryRequirements requirements;
  VkDeviceSize offset;
  VkDeviceSize bytes;
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
 * Create one storage-buffer handle and query its exact driver requirements.
 * No memory is allocated or mapped by this function.
 */
static VkResult create_storage_buffer (
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

  destination->bytes = bytes;

  vkGetBufferMemoryRequirements (
    device, destination->buffer, &destination->requirements
  );

  return VK_SUCCESS;
}

/**
 * Align an offset to an explicit positive power-of-two Vulkan alignment.
 * Reject invalid alignment and arithmetic overflow before Vulkan calls.
 */
static int align_offset (
  VkDeviceSize start,
  VkDeviceSize alignment,
  VkDeviceSize *aligned)
{
  if (alignment == 0u || (alignment & (alignment - 1u)) != 0u)
  {
    fprintf (
      stderr, "INVALID_MEMORY_ALIGNMENT alignment=%" PRIu64 "\n",
      (uint64_t) alignment
    );

    return 1;
  }

  if (start > UINT64_MAX - (alignment - 1u))
  {
    fprintf (stderr, "Vulkan memory offset alignment would overflow\n");

    return 1;
  }

  *aligned = (start + alignment - 1u) & ~(alignment - 1u);

  return 0;
}

/**
 * Compute four disjoint Vulkan memory ranges and a common allowed memory
 * type without allocating. Verify every alignment, range and type bit.
 */
static int plan_allocation (
  VkPhysicalDevice physical,
  host_buffer_t *buffers,
  VkDeviceSize *total_bytes,
  uint32_t *memory_type)
{
  static const char *names[BUFFER_COUNT] =
  {
    "input", "stage-a", "stage-b", "output"
  };

  VkPhysicalDeviceMemoryProperties memory = { 0 };

  vkGetPhysicalDeviceMemoryProperties (physical, &memory);

  uint32_t allowed_types = UINT32_MAX;
  VkDeviceSize end = 0u;

  for (uint32_t i = 0; i < BUFFER_COUNT; i++)
  {
    const VkMemoryRequirements *requirements = &buffers[i].requirements;
    VkDeviceSize start = 0u;

    if (requirements->size < buffers[i].bytes)
    {
      fprintf (stderr, "GPU buffer requires fewer bytes than its size\n");

      return 1;
    }

    if (align_offset (end, requirements->alignment, &start) != 0) return 1;

    if (requirements->size > UINT64_MAX - start)
    {
      fprintf (stderr, "Vulkan allocation size would overflow\n");

      return 1;
    }

    const VkDeviceSize finish = start + requirements->size;

    if (finish > ALLOCATION_LIMIT_BYTES)
    {
      fprintf (
        stderr,
        "VULKAN_MEMORY_LIMIT requested=%" PRIu64 " cap=%" PRIu32 "\n",
        (uint64_t) finish, ALLOCATION_LIMIT_BYTES
      );

      return 1;
    }

    buffers[i].offset = start;
    allowed_types &= requirements->memoryTypeBits;
    end = finish;

    printf (
      "BUFFER_LAYOUT name=%s bytes=%" PRIu64
      " required=%" PRIu64 " alignment=%" PRIu64
      " offset=%" PRIu64 " end=%" PRIu64
      " memory_type_bits=0x%08" PRIx32 "\n",
      names[i], (uint64_t) buffers[i].bytes,
      (uint64_t) requirements->size,
      (uint64_t) requirements->alignment,
      (uint64_t) start, (uint64_t) finish,
      requirements->memoryTypeBits
    );
  }

  uint32_t selected = UINT32_MAX;

  for (uint32_t i = 0; i < memory.memoryTypeCount; i++)
  {
    const VkMemoryPropertyFlags properties =
      memory.memoryTypes[i].propertyFlags;
    const VkMemoryPropertyFlags required =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
      | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    if ((allowed_types & (1u << i)) != 0u
     && (properties & required) == required)
    {
      selected = i;

      break;
    }
  }

  if (selected == UINT32_MAX)
  {
    fprintf (
      stderr, "No common host-visible, host-coherent Vulkan memory type\n"
    );

    return 1;
  }

  *total_bytes = end;
  *memory_type = selected;

  printf (
    "MEMORY_PLAN bytes=%" PRIu64
    " memory_type=%" PRIu32 " buffers=%u compatible_bits=0x%08" PRIx32 "\n",
    (uint64_t) end, selected, BUFFER_COUNT, allowed_types
  );

  return 0;
}

/**
 * Unbind and destroy a Vulkan storage buffer handle after all queue work.
 * Its associated allocation and host mapping are owned by the caller.
 */
static void destroy_storage_buffer (VkDevice device, host_buffer_t *buffer)
{
  if (buffer->buffer != VK_NULL_HANDLE)
  {
    vkDestroyBuffer (device, buffer->buffer, NULL);

    buffer->buffer = VK_NULL_HANDLE;
  }

  buffer->mapped = NULL;
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
 * Calculate stage A from one deterministic public synthetic input word.
 * All 32-bit integer operations are unsigned and wrap modulo 2^32.
 */
static uint32_t stage_a_reference (uint32_t index)
{
  const uint32_t value = synthetic_input (index);
  const uint32_t affine = value * 1664525u + 1013904223u;
  const uint32_t rotated = (value << 7u) | (value >> 25u);

  return (affine ^ rotated) + index * 2654435761u;
}

/**
 * Calculate stage B from current and seven-lanes-forward stage A values.
 * The neighbor index wraps within a 32-element workgroup.
 */
static uint32_t stage_b_reference (uint32_t index)
{
  const uint32_t lane = index & (LOCAL_SIZE - 1u);
  const uint32_t group_start = index & ~(LOCAL_SIZE - 1u);
  const uint32_t neighbor_index =
    group_start + ((lane + 7u) & (LOCAL_SIZE - 1u));
  const uint32_t value = stage_a_reference (index);
  const uint32_t neighbor = stage_a_reference (neighbor_index);
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
 * Check every intermediate and final result against distinct CPU formulas.
 * Print the first incorrect stage and exact word if the GPU result differs.
 */
static int verify_pipeline (
  const uint32_t *stage_a,
  const uint32_t *stage_b,
  const uint32_t *output)
{
  uint32_t mismatches = 0u;

  for (uint32_t index = 0; index < ELEMENT_COUNT; index++)
  {
    const uint32_t a = stage_a_reference (index);
    const uint32_t b = stage_b_reference (index);
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
            "PIPELINE_MISMATCH stage=%s index=%" PRIu32
            " word=%" PRIu32 " expected=%08" PRIx32
            " actual=%08" PRIx32 "\n",
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
      "FAIL Vulkan 3-stage pipeline: %" PRIu32 " incorrect words\n",
      mismatches
    );

    return 1;
  }

  const uint64_t checksum_a = words_checksum (stage_a, ELEMENT_COUNT);
  const uint64_t checksum_b = words_checksum (stage_b, ELEMENT_COUNT);
  const uint64_t checksum_c = words_checksum (
    output, ELEMENT_COUNT * RESULT_WORDS
  );

  printf (
    "PASS Vulkan 3-stage pipeline: %" PRIu32
    " elements, %" PRIu32 " correct 32-bit words; "
    "stageA=%016" PRIx64 " stageB=%016" PRIx64
    " output=%016" PRIx64 "\n",
    ELEMENT_COUNT, ELEMENT_COUNT * (RESULT_WORDS + 2u),
    checksum_a, checksum_b, checksum_c
  );

  return 0;
}

/**
 * Compile three synthetic compute pipelines, share four memory subranges,
 * and exercise error paths without inducing real Vulkan device loss.
 */
int main (int argc, char **argv)
{
  if ((argc != 4 && argc != 6)
   || (argc == 6 && strcmp (argv[4], "--simulate") != 0))
  {
    fprintf (
      stderr, "Usage: %s <stage-A.spv> <stage-B.spv> <stage-C.spv>"
      " [--simulate <memory-type|allocation|device-lost>]\n",
      argv[0]
    );

    return 2;
  }

  const char *simulate = (argc == 6) ? argv[5] : NULL;

  if (simulate != NULL
   && strcmp (simulate, "memory-type") != 0
   && strcmp (simulate, "allocation") != 0
   && strcmp (simulate, "device-lost") != 0)
  {
    fprintf (stderr, "Unknown simulated Vulkan failure: %s\n", simulate);

    return 2;
  }

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
  host_buffer_t buffers[BUFFER_COUNT] = { 0 };
  VkDeviceMemory shared_memory = VK_NULL_HANDLE;
  void *mapped_base = NULL;
  VkDeviceSize allocation_bytes = 0u;
  uint32_t selected_memory_type = UINT32_MAX;
  uint32_t bound_buffers = 0u;
  uint32_t freed_allocations = 0u;
  int simulated_device_lost = 0;
  uint32_t queue_family = UINT32_MAX;
  VkQueue queue = VK_NULL_HANDLE;

  const VkApplicationInfo application_info =
  {
    .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
    .pApplicationName = "Synthetic Vulkan Shared Allocation Diagnostic",
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

  const VkDeviceSize buffer_sizes[BUFFER_COUNT] =
  {
    ELEMENT_COUNT * sizeof (uint32_t),
    ELEMENT_COUNT * sizeof (uint32_t),
    ELEMENT_COUNT * sizeof (uint32_t),
    ELEMENT_COUNT * RESULT_WORDS * sizeof (uint32_t)
  };

  for (uint32_t i = 0; i < BUFFER_COUNT; i++)
  {
    VK_CHECK (create_storage_buffer (
      device, buffer_sizes[i], &buffers[i]
    ));
  }

  if (plan_allocation (
    physical, buffers, &allocation_bytes, &selected_memory_type
  ) != 0)
  {
    goto cleanup;
  }

  if (simulate != NULL && strcmp (simulate, "memory-type") == 0)
  {
    fprintf (
      stderr, "SIMULATED_VULKAN_FAILURE name=memory-type"
      " vkresult=%d\n", VK_ERROR_FEATURE_NOT_PRESENT
    );
    status = 42;

    goto cleanup;
  }

  if (simulate != NULL && strcmp (simulate, "allocation") == 0)
  {
    fprintf (
      stderr, "SIMULATED_VULKAN_FAILURE name=allocation"
      " vkresult=%d\n", VK_ERROR_OUT_OF_DEVICE_MEMORY
    );
    status = 43;

    goto cleanup;
  }

  const VkMemoryAllocateInfo memory_info =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
    .allocationSize = allocation_bytes,
    .memoryTypeIndex = selected_memory_type
  };

  VK_CHECK (vkAllocateMemory (device, &memory_info, NULL, &shared_memory));

  for (uint32_t i = 0; i < BUFFER_COUNT; i++)
  {
    VK_CHECK (vkBindBufferMemory (
      device, buffers[i].buffer, shared_memory, buffers[i].offset
    ));

    bound_buffers++;
  }

  VK_CHECK (vkMapMemory (
    device, shared_memory, 0, allocation_bytes, 0, &mapped_base
  ));

  for (uint32_t i = 0; i < BUFFER_COUNT; i++)
  {
    buffers[i].mapped = (uint8_t *) mapped_base + buffers[i].offset;
  }

  printf (
    "MEMORY_ALLOCATION allocations=1 bytes=%" PRIu64
    " bound_buffers=%" PRIu32 " memory_type=%" PRIu32 "\n",
    (uint64_t) allocation_bytes, bound_buffers, selected_memory_type
  );

  uint32_t *input_words = (uint32_t *) buffers[0].mapped;

  for (uint32_t i = 0; i < ELEMENT_COUNT; i++)
  {
    input_words[i] = synthetic_input (i);
  }

  memset (buffers[1].mapped, 0xa5, ELEMENT_COUNT * sizeof (uint32_t));
  memset (buffers[2].mapped, 0x5a, ELEMENT_COUNT * sizeof (uint32_t));
  memset (
    buffers[3].mapped, 0xc3,
    ELEMENT_COUNT * RESULT_WORDS * sizeof (uint32_t)
  );

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
    { .buffer = buffers[0].buffer, .offset = 0, .range = VK_WHOLE_SIZE },
    { .buffer = buffers[1].buffer, .offset = 0, .range = VK_WHOLE_SIZE },
    { .buffer = buffers[2].buffer, .offset = 0, .range = VK_WHOLE_SIZE },
    { .buffer = buffers[3].buffer, .offset = 0, .range = VK_WHOLE_SIZE }
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
    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
  };

  VK_CHECK (vkBeginCommandBuffer (command, &begin_info));

  vkCmdBindDescriptorSets (
    command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout,
    0, 1, &descriptor_set, 0, NULL
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

  VK_CHECK (vkQueueSubmit (queue, 1, &submit, fence));

  const VkResult waited = vkWaitForFences (
    device, 1, &fence, VK_TRUE, UINT64_C (30000000000)
  );

  if (waited == VK_ERROR_DEVICE_LOST)
  {
    fprintf (stderr, "VULKAN_DEVICE_LOST_FROM_DRIVER vkresult=%d\n", waited);
    status = 1;

    goto cleanup;
  }

  if (waited != VK_SUCCESS)
  {
    fprintf (stderr, "VULKAN_ERROR vkWaitForFences vkresult=%d\n", waited);

    goto cleanup;
  }

  if (simulate != NULL && strcmp (simulate, "device-lost") == 0)
  {
    /*
     * The real GPU has finished before this application-only injection.
     * Neither the driver nor any queue is actually forced into device loss.
     */
    const VkResult simulated_result = VK_ERROR_DEVICE_LOST;

    fprintf (
      stderr,
      "SIMULATED_VULKAN_FAILURE name=device-lost"
      " vkresult=%d real_fence_wait=%d\n",
      simulated_result, waited
    );

    simulated_device_lost = 1;
    status = 44;

    goto cleanup;
  }

  status = verify_pipeline (
    (const uint32_t *) buffers[1].mapped,
    (const uint32_t *) buffers[2].mapped,
    (const uint32_t *) buffers[3].mapped
  );

cleanup:
  if (device != VK_NULL_HANDLE)
  {
    const VkResult idle = vkDeviceWaitIdle (device);

    if (idle != VK_SUCCESS)
    {
      fprintf (
        stderr,
        "VULKAN_CLEANUP_ERROR vkDeviceWaitIdle result=%d\n", idle
      );

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

    if (mapped_base != NULL)
    {
      vkUnmapMemory (device, shared_memory);

      mapped_base = NULL;
    }

    for (uint32_t i = 0; i < BUFFER_COUNT; i++)
    {
      destroy_storage_buffer (device, &buffers[i]);
    }

    if (shared_memory != VK_NULL_HANDLE)
    {
      vkFreeMemory (device, shared_memory, NULL);

      freed_allocations++;
    }

    vkDestroyDevice (device, NULL);
  }

  if (instance != VK_NULL_HANDLE) vkDestroyInstance (instance, NULL);

  free (queue_families);
  free (physical_devices);
  for (uint32_t stage = 0; stage < STAGE_COUNT; stage++) free (shader_code[stage]);

  printf (
    "CLEANUP_SHARED_MEMORY allocations_freed=%" PRIu32
    " buffers_bound=%" PRIu32 " simulated_device_lost=%d"
    " final_status=%d\n",
    freed_allocations, bound_buffers, simulated_device_lost, status
  );

  return status;
}
