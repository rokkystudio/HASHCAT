/**
 * Validate Vulkan 1.1 resource creation, cleanup and error propagation.
 *
 * The program creates one synthetic compute pipeline and its supporting
 * descriptors, coherent memory, command buffer and fence without submitting
 * GPU work. Each supported --fail-after step injects a deterministic error
 * after the corresponding real Vulkan object creation, then verifies that
 * every acquired resource was released in reverse dependency order.
 *
 * No password, hash, hashcat module, or cryptographic shader is processed.
 */

#include <vulkan/vulkan.h>

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUFFER_BYTES 4096u
#define MAX_CYCLES 16u

static const char *STEPS[] =
{
  "instance",
  "device",
  "buffer",
  "device-memory",
  "memory-map",
  "descriptor-layout",
  "pipeline-layout",
  "shader-module",
  "compute-pipeline",
  "descriptor-pool",
  "descriptor-set",
  "command-pool",
  "command-buffer",
  "fence"
};

#define STEP_COUNT (sizeof (STEPS) / sizeof (STEPS[0]))

#define VK_CHECK(call) \
  do \
  { \
    const VkResult result = (call); \
    if (result != VK_SUCCESS) \
    { \
      fprintf (stderr, "VULKAN_ERROR call=%s result=%d\n", #call, result); \
      goto cleanup; \
    } \
  } while (0)

#define CHECK_INJECTION(name) \
  do \
  { \
    created++; \
    if (failure_point != NULL && strcmp (failure_point, (name)) == 0) \
    { \
      fprintf (stderr, "INJECTED_FAILURE stage=%s code=42\n", (name)); \
      status = 42; \
      goto cleanup; \
    } \
  } while (0)

/**
 * Load a standalone Vulkan SPIR-V shader and validate word alignment/magic.
 * The caller owns the returned allocation.
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
    fprintf (stderr, "Invalid Vulkan SPIR-V file size: %s\n", path);

    fclose (file);

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
    fprintf (stderr, "Invalid Vulkan SPIR-V input: %s\n", path);

    free (code);

    return NULL;
  }

  *bytes = (size_t) length;

  return code;
}

/**
 * Return a precise valid failure-injection stage or NULL for unknown input.
 */
static const char *resolve_failure_point (const char *name)
{
  for (size_t i = 0; i < STEP_COUNT; i++)
  {
    if (strcmp (STEPS[i], name) == 0) return STEPS[i];
  }

  return NULL;
}

/**
 * Create and release one complete Vulkan object graph without GPU dispatch.
 *
 * Every successfully acquired Vulkan object, allocated command/descriptor
 * set and memory mapping increments created. Cleanup releases exactly these
 * resources in dependency order and increments released. The return code is
 * 0 on success, 42 for the requested failure point, or 1 for an API/resource
 * error or invalid lifetime accounting.
 */
static int run_cycle (
  const uint32_t *shader,
  size_t shader_bytes,
  const char *failure_point,
  uint32_t cycle)
{
  int status = 1;
  uint32_t created = 0u;
  uint32_t released = 0u;

  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkPhysicalDevice *devices = NULL;
  VkQueueFamilyProperties *families = NULL;
  VkDevice device = VK_NULL_HANDLE;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void *mapped = NULL;
  VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkShaderModule shader_module = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
  VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
  VkCommandPool command_pool = VK_NULL_HANDLE;
  VkCommandBuffer command_buffer = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  uint32_t compute_family = UINT32_MAX;

  const VkApplicationInfo application_info =
  {
    .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
    .pApplicationName = "Vulkan Resource Lifetime Diagnostic",
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
  CHECK_INJECTION ("instance");

  uint32_t device_count = 0u;

  VK_CHECK (vkEnumeratePhysicalDevices (instance, &device_count, NULL));

  if (device_count == 0u)
  {
    fprintf (stderr, "No Vulkan physical devices\n");

    goto cleanup;
  }

  devices = (VkPhysicalDevice *) calloc (device_count, sizeof (*devices));

  if (devices == NULL) goto cleanup;

  VK_CHECK (vkEnumeratePhysicalDevices (instance, &device_count, devices));

  for (uint32_t d = 0u; d < device_count; d++)
  {
    uint32_t family_count = 0u;

    vkGetPhysicalDeviceQueueFamilyProperties (devices[d], &family_count, NULL);

    if (family_count == 0u) continue;

    families = (VkQueueFamilyProperties *) calloc (
      family_count, sizeof (*families)
    );

    if (families == NULL) goto cleanup;

    vkGetPhysicalDeviceQueueFamilyProperties (
      devices[d], &family_count, families
    );

    for (uint32_t f = 0u; f < family_count; f++)
    {
      if (families[f].queueCount != 0u
       && (families[f].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0)
      {
        physical = devices[d];
        compute_family = f;

        break;
      }
    }

    free (families);
    families = NULL;

    if (physical != VK_NULL_HANDLE) break;
  }

  if (physical == VK_NULL_HANDLE)
  {
    fprintf (stderr, "No Vulkan compute-capable queue family\n");

    goto cleanup;
  }

  VkPhysicalDeviceProperties properties = { 0 };

  vkGetPhysicalDeviceProperties (physical, &properties);

  if (VK_VERSION_MAJOR (properties.apiVersion) < 1u
   || (VK_VERSION_MAJOR (properties.apiVersion) == 1u
    && VK_VERSION_MINOR (properties.apiVersion) < 1u))
  {
    fprintf (stderr, "Vulkan 1.1 not supported by selected compute device\n");

    goto cleanup;
  }

  printf (
    "VULKAN_DEVICE cycle=%" PRIu32 " name=%s"
    " vendor=0x%04" PRIx32 " device=0x%04" PRIx32
    " driver=0x%08" PRIx32 " api=%" PRIu32 ".%" PRIu32 ".%" PRIu32
    " queue_family=%" PRIu32 "\n",
    cycle, properties.deviceName, properties.vendorID,
    properties.deviceID, properties.driverVersion,
    VK_VERSION_MAJOR (properties.apiVersion),
    VK_VERSION_MINOR (properties.apiVersion),
    VK_VERSION_PATCH (properties.apiVersion), compute_family
  );

  const float priority = 1.0f;
  const VkDeviceQueueCreateInfo queue_info =
  {
    .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
    .queueFamilyIndex = compute_family,
    .queueCount = 1,
    .pQueuePriorities = &priority
  };
  const VkDeviceCreateInfo device_info =
  {
    .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
    .queueCreateInfoCount = 1,
    .pQueueCreateInfos = &queue_info
  };

  VK_CHECK (vkCreateDevice (physical, &device_info, NULL, &device));
  CHECK_INJECTION ("device");

  const VkBufferCreateInfo buffer_info =
  {
    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
    .size = BUFFER_BYTES,
    .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
    .sharingMode = VK_SHARING_MODE_EXCLUSIVE
  };

  VK_CHECK (vkCreateBuffer (device, &buffer_info, NULL, &buffer));
  CHECK_INJECTION ("buffer");

  VkMemoryRequirements requirements = { 0 };

  vkGetBufferMemoryRequirements (device, buffer, &requirements);

  VkPhysicalDeviceMemoryProperties available_memory = { 0 };

  vkGetPhysicalDeviceMemoryProperties (physical, &available_memory);

  uint32_t memory_type = UINT32_MAX;

  for (uint32_t i = 0u; i < available_memory.memoryTypeCount; i++)
  {
    const uint32_t type_bit = 1u << i;
    const VkMemoryPropertyFlags desired =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
      | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const VkMemoryPropertyFlags flags =
      available_memory.memoryTypes[i].propertyFlags;

    if ((requirements.memoryTypeBits & type_bit) != 0u
     && (flags & desired) == desired)
    {
      memory_type = i;

      break;
    }
  }

  if (memory_type == UINT32_MAX)
  {
    fprintf (stderr, "No host-visible coherent memory for storage buffer\n");

    goto cleanup;
  }

  const VkMemoryAllocateInfo allocation_info =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
    .allocationSize = requirements.size,
    .memoryTypeIndex = memory_type
  };

  VK_CHECK (vkAllocateMemory (device, &allocation_info, NULL, &memory));
  CHECK_INJECTION ("device-memory");

  VK_CHECK (vkBindBufferMemory (device, buffer, memory, 0));
  VK_CHECK (vkMapMemory (device, memory, 0, BUFFER_BYTES, 0, &mapped));
  CHECK_INJECTION ("memory-map");

  uint32_t *words = (uint32_t *) mapped;

  for (uint32_t i = 0u; i < BUFFER_BYTES / sizeof (uint32_t); i++)
  {
    words[i] = i * 1664525u + cycle * 1013904223u;
  }

  for (uint32_t i = 0u; i < BUFFER_BYTES / sizeof (uint32_t); i++)
  {
    const uint32_t expected = i * 1664525u + cycle * 1013904223u;

    if (words[i] != expected)
    {
      fprintf (stderr, "Mapped storage buffer CPU validation mismatch\n");

      goto cleanup;
    }
  }

  const VkDescriptorSetLayoutBinding bindings[2] =
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
    }
  };
  const VkDescriptorSetLayoutCreateInfo layout_info =
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
    .bindingCount = 2,
    .pBindings = bindings
  };

  VK_CHECK (vkCreateDescriptorSetLayout (
    device, &layout_info, NULL, &descriptor_layout
  ));
  CHECK_INJECTION ("descriptor-layout");

  const VkPipelineLayoutCreateInfo pipeline_layout_info =
  {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    .setLayoutCount = 1,
    .pSetLayouts = &descriptor_layout
  };

  VK_CHECK (vkCreatePipelineLayout (
    device, &pipeline_layout_info, NULL, &pipeline_layout
  ));
  CHECK_INJECTION ("pipeline-layout");

  const VkShaderModuleCreateInfo shader_info =
  {
    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
    .codeSize = shader_bytes,
    .pCode = shader
  };

  VK_CHECK (vkCreateShaderModule (
    device, &shader_info, NULL, &shader_module
  ));
  CHECK_INJECTION ("shader-module");

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
  CHECK_INJECTION ("compute-pipeline");

  const VkDescriptorPoolSize pool_size =
  {
    .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
    .descriptorCount = 2
  };
  const VkDescriptorPoolCreateInfo pool_info =
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
    .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
    .maxSets = 1,
    .poolSizeCount = 1,
    .pPoolSizes = &pool_size
  };

  VK_CHECK (vkCreateDescriptorPool (
    device, &pool_info, NULL, &descriptor_pool
  ));
  CHECK_INJECTION ("descriptor-pool");

  const VkDescriptorSetAllocateInfo set_info =
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
    .descriptorPool = descriptor_pool,
    .descriptorSetCount = 1,
    .pSetLayouts = &descriptor_layout
  };

  VK_CHECK (vkAllocateDescriptorSets (device, &set_info, &descriptor_set));
  CHECK_INJECTION ("descriptor-set");

  const VkDescriptorBufferInfo descriptor_buffer =
  {
    .buffer = buffer,
    .offset = 0,
    .range = BUFFER_BYTES
  };
  VkWriteDescriptorSet descriptors[2] = { 0 };

  for (uint32_t i = 0u; i < 2u; i++)
  {
    descriptors[i] = (VkWriteDescriptorSet)
    {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = descriptor_set,
      .dstBinding = i,
      .descriptorCount = 1,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .pBufferInfo = &descriptor_buffer
    };
  }

  vkUpdateDescriptorSets (device, 2, descriptors, 0, NULL);

  const VkCommandPoolCreateInfo command_pool_info =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
    .queueFamilyIndex = compute_family,
    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT
  };

  VK_CHECK (vkCreateCommandPool (
    device, &command_pool_info, NULL, &command_pool
  ));
  CHECK_INJECTION ("command-pool");

  const VkCommandBufferAllocateInfo command_info =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
    .commandPool = command_pool,
    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
    .commandBufferCount = 1
  };

  VK_CHECK (vkAllocateCommandBuffers (
    device, &command_info, &command_buffer
  ));
  CHECK_INJECTION ("command-buffer");

  const VkCommandBufferBeginInfo begin_info =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO
  };

  VK_CHECK (vkBeginCommandBuffer (command_buffer, &begin_info));

  vkCmdBindPipeline (
    command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline
  );
  vkCmdBindDescriptorSets (
    command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
    pipeline_layout, 0, 1, &descriptor_set, 0, NULL
  );

  VK_CHECK (vkEndCommandBuffer (command_buffer));

  const VkFenceCreateInfo fence_info =
  {
    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
  };

  VK_CHECK (vkCreateFence (device, &fence_info, NULL, &fence));
  CHECK_INJECTION ("fence");

  const VkResult fence_status = vkGetFenceStatus (device, fence);

  if (fence_status != VK_NOT_READY)
  {
    fprintf (
      stderr, "Unexpected signal state for unsubmitted Vulkan fence: %d\n",
      fence_status
    );

    goto cleanup;
  }

  status = 0;

cleanup:
  if (device != VK_NULL_HANDLE)
  {
    const VkResult idle = vkDeviceWaitIdle (device);

    if (idle != VK_SUCCESS)
    {
      fprintf (stderr, "VULKAN_ERROR vkDeviceWaitIdle result=%d\n", idle);

      status = 1;
    }

    if (fence != VK_NULL_HANDLE)
    {
      vkDestroyFence (device, fence, NULL);

      released++;
    }

    if (command_buffer != VK_NULL_HANDLE)
    {
      vkFreeCommandBuffers (device, command_pool, 1, &command_buffer);

      released++;
    }

    if (command_pool != VK_NULL_HANDLE)
    {
      vkDestroyCommandPool (device, command_pool, NULL);

      released++;
    }

    if (descriptor_set != VK_NULL_HANDLE)
    {
      const VkResult freed = vkFreeDescriptorSets (
        device, descriptor_pool, 1, &descriptor_set
      );

      if (freed != VK_SUCCESS)
      {
        fprintf (
          stderr, "VULKAN_ERROR vkFreeDescriptorSets result=%d\n", freed
        );

        status = 1;
      }
      else
      {
        released++;
      }
    }

    if (descriptor_pool != VK_NULL_HANDLE)
    {
      vkDestroyDescriptorPool (device, descriptor_pool, NULL);

      released++;
    }

    if (pipeline != VK_NULL_HANDLE)
    {
      vkDestroyPipeline (device, pipeline, NULL);

      released++;
    }

    if (shader_module != VK_NULL_HANDLE)
    {
      vkDestroyShaderModule (device, shader_module, NULL);

      released++;
    }

    if (pipeline_layout != VK_NULL_HANDLE)
    {
      vkDestroyPipelineLayout (device, pipeline_layout, NULL);

      released++;
    }

    if (descriptor_layout != VK_NULL_HANDLE)
    {
      vkDestroyDescriptorSetLayout (device, descriptor_layout, NULL);

      released++;
    }

    if (mapped != NULL)
    {
      vkUnmapMemory (device, memory);

      released++;
    }

    if (memory != VK_NULL_HANDLE)
    {
      vkFreeMemory (device, memory, NULL);

      released++;
    }

    if (buffer != VK_NULL_HANDLE)
    {
      vkDestroyBuffer (device, buffer, NULL);

      released++;
    }

    vkDestroyDevice (device, NULL);

    released++;
  }

  free (families);
  free (devices);

  if (instance != VK_NULL_HANDLE)
  {
    vkDestroyInstance (instance, NULL);

    released++;
  }

  if (created != released)
  {
    fprintf (
      stderr, "RESOURCE_LIFETIME_MISMATCH cycle=%" PRIu32
      " created=%" PRIu32 " released=%" PRIu32 "\n",
      cycle, created, released
    );

    return 3;
  }

  if (status == 0)
  {
    printf (
      "PASS Vulkan resource cycle=%" PRIu32
      " created=%" PRIu32 " released=%" PRIu32
      " shader_dispatched=0\n",
      cycle, created, released
    );
  }
  else if (status == 42)
  {
    printf (
      "CLEANUP_OK cycle=%" PRIu32
      " created=%" PRIu32 " released=%" PRIu32
      " fail_after=%s shader_dispatched=0\n",
      cycle, created, released, failure_point
    );
  }
  else
  {
    fprintf (
      stderr, "VULKAN_LIFECYCLE_ERROR cycle=%" PRIu32
      " created=%" PRIu32 " released=%" PRIu32 "\n",
      cycle, created, released
    );
  }

  return status;
}

/**
 * Select an exact lifecycle test with 1-16 cycles and an optional known
 * failure point; reject invalid arguments before creating Vulkan objects.
 */
int main (int argc, char **argv)
{
  if ((argc != 4 && argc != 6)
   || strcmp (argv[2], "--cycles") != 0
   || (argc == 6 && strcmp (argv[4], "--fail-after") != 0))
  {
    fprintf (
      stderr,
      "Usage: %s <synthetic-stage-A.spv> --cycles <1..16>"
      " [--fail-after <known-resource-step>]\n", argv[0]
    );

    return 2;
  }

  errno = 0;
  char *end = NULL;
  const long count = strtol (argv[3], &end, 10);

  if (errno != 0 || end == argv[3] || *end != 0
   || count < 1 || count > MAX_CYCLES)
  {
    fprintf (stderr, "Invalid cycles count (expected integer 1..16)\n");

    return 2;
  }

  const char *failure_point = NULL;

  if (argc == 6)
  {
    failure_point = resolve_failure_point (argv[5]);

    if (failure_point == NULL)
    {
      fprintf (stderr, "Unknown Vulkan failure injection step: %s\n", argv[5]);

      return 2;
    }
  }

  size_t shader_bytes = 0u;
  uint32_t *shader = read_shader (argv[1], &shader_bytes);

  if (shader == NULL) return 2;

  int result = 0;

  for (uint32_t cycle = 0u; cycle < (uint32_t) count; cycle++)
  {
    result = run_cycle (shader, shader_bytes, failure_point, cycle);

    if (result != 0) break;
  }

  free (shader);

  if (result == 0)
  {
    printf (
      "PASS Vulkan resource lifecycle: %ld cycles, 14 resources/cycle, "
      "all resources released; GPU submissions=0\n", count
    );
  }

  return result;
}
