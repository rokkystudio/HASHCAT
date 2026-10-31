/**
 * Execute three public SPIR-V 1.0 Compute stages with a Vulkan 1.0 default.
 *
 * The standalone Android program uses four DEVICE_LOCAL-preferred GPU
 * buffers and two HOST_VISIBLE staging buffers with six jointly planned
 * allocations. Driver-reported limits, memoryTypeBits, memory heap sizes,
 * compute queues and coherency determine resource compatibility.
 *
 * The compute stages use 32-bit integer arithmetic and a 32-lane shared
 * workgroup. CPU upload and final readback use core Vulkan 1.0 transfer
 * commands and barriers, with real flush/invalidate on noncoherent memory.
 * The four-word GPU result exposes both intermediate stage results for
 * independent per-element CPU comparison and stage checksums.
 *
 * No hashcat module, password, cryptographic algorithm, device model
 * profile, optional Vulkan extension or optional device feature is used.
 */

#include "vulkan-auto-config.h"
#include "vulkan-core11-policy.h"
#include "vulkan-multistage-memory-plan.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ELEMENT_COUNT 2048u
#define LOCAL_SIZE 32u
#define OUTPUT_WORDS 4u
#define STAGE_COUNT 3u
#define GPU_BUFFER_COUNT 4u
#define BUFFER_COUNT VULKAN_MULTISTAGE_BUFFER_COUNT
#define WORD_BYTES (ELEMENT_COUNT * sizeof (uint32_t))
#define OUTPUT_BYTES (WORD_BYTES * OUTPUT_WORDS)

typedef struct multistage_buffer
{
  VkBuffer buffer;
  VkDeviceMemory memory;
  void *mapped;
} multistage_buffer_t;

#define VK_REQUIRE(call) \
  do \
  { \
    result = (call); \
    if (result != VK_SUCCESS) \
    { \
      fprintf (stderr, "MULTISTAGE_VULKAN_ERROR name=%s result=%d\n", \
        #call, result); \
      goto cleanup; \
    } \
  } while (0)

/**
 * Load one SPIR-V 1.0 shader binary with an exact Vulkan magic/version.
 *
 * The full file must be a nonempty, four-byte-aligned SPIR-V 1.0 module.
 * The caller owns the returned words and releases them with free.
 */
static uint32_t *load_shader (
  const char *path,
  size_t *size)
{
  FILE *file = fopen (path, "rb");

  if (file == NULL)
  {
    perror ("MULTISTAGE_SHADER_OPEN_FAILED");

    return NULL;
  }

  if (fseek (file, 0, SEEK_END) != 0)
  {
    fclose (file);

    return NULL;
  }

  const long length = ftell (file);

  if (length < 20L || length > 1024L * 1024L
   || (length % 4L) != 0L
   || fseek (file, 0, SEEK_SET) != 0)
  {
    fprintf (stderr, "REJECT_MULTISTAGE_SPIRV_SIZE length=%ld\n", length);
    fclose (file);

    return NULL;
  }

  uint32_t *words = (uint32_t *) malloc ((size_t) length);

  if (words == NULL)
  {
    fclose (file);

    return NULL;
  }

  const size_t count = fread (words, 1u, (size_t) length, file);

  fclose (file);

  if (count != (size_t) length
   || words[0] != UINT32_C (0x07230203)
   || words[1] != UINT32_C (0x00010000))
  {
    fprintf (stderr, "REJECT_MULTISTAGE_SPIRV_VERSION require=1.0\n");
    free (words);

    return NULL;
  }

  *size = (size_t) length;

  return words;
}

/**
 * Produce one public synthetic integer input word.
 *
 * All operations are unsigned and are evaluated modulo 2^32.
 */
static uint32_t synthetic_input (uint32_t index)
{
  return (index * UINT32_C (0x9e3779b9))
       ^ (index << 15u)
       ^ (index >> 3u)
       ^ UINT32_C (0xa5a5a5a5);
}

/**
 * Evaluate the first shader's unsigned affine/XOR/rotation expression.
 */
static uint32_t stage_a_reference (uint32_t index)
{
  const uint32_t input = synthetic_input (index);
  const uint32_t affine =
    input * UINT32_C (1664525) + UINT32_C (1013904223);
  const uint32_t rotated = (input << 7u) | (input >> 25u);

  return (affine ^ rotated) + index * UINT32_C (2654435761);
}

/**
 * Evaluate the second shader's 32-lane local-memory neighbor expression.
 *
 * A seven-lane-forward neighbor wraps within the same 32-element workgroup.
 */
static uint32_t stage_b_reference (uint32_t index)
{
  const uint32_t lane = index & (LOCAL_SIZE - 1u);
  const uint32_t start = index & ~(LOCAL_SIZE - 1u);
  const uint32_t neighbor_index =
    start + ((lane + 7u) & (LOCAL_SIZE - 1u));
  const uint32_t value = stage_a_reference (index);
  const uint32_t neighbor = stage_a_reference (neighbor_index);
  const uint32_t rotated = (value << 11u) | (value >> 21u);

  return (value + neighbor)
       ^ rotated
       ^ UINT32_C (0x6c8e9cf5);
}

/**
 * Update one FNV-1a 64-bit checksum with a little-endian 32-bit word.
 */
static uint64_t checksum_word (uint64_t checksum, uint32_t word)
{
  for (uint32_t byte = 0u; byte < 4u; byte++)
  {
    checksum ^= (word >> (8u * byte)) & UINT32_C (0xff);
    checksum *= UINT64_C (1099511628211);
  }

  return checksum;
}

/**
 * Independently verify all stage A/B and four-word stage C GPU results.
 *
 * Both intermediate values are embedded in the public stage-C output, so
 * all 12,288 logical 32-bit output words can be checked after one readback.
 * Prints independent FNV-1a checksums for both stage states and final output.
 */
static int verify_pipeline (const uint32_t *download)
{
  uint64_t checksum_a = UINT64_C (14695981039346656037);
  uint64_t checksum_b = UINT64_C (14695981039346656037);
  uint64_t checksum_c = UINT64_C (14695981039346656037);

  for (uint32_t index = 0u; index < ELEMENT_COUNT; index++)
  {
    const uint32_t a = stage_a_reference (index);
    const uint32_t b = stage_b_reference (index);
    const uint32_t rotated = (b << 3u) | (b >> 29u);
    const uint32_t expected[OUTPUT_WORDS] =
    {
      a,
      b,
      a ^ b,
      (a * UINT32_C (2246822519)
       + b * UINT32_C (3266489917)) ^ rotated
    };

    for (uint32_t word = 0u; word < OUTPUT_WORDS; word++)
    {
      const uint32_t actual = download[index * OUTPUT_WORDS + word];

      if (actual != expected[word])
      {
        fprintf (
          stderr,
          "REJECT_MULTISTAGE_RESULT index=%" PRIu32
          " word=%" PRIu32 " expected=%08" PRIx32
          " received=%08" PRIx32 "\n",
          index, word, expected[word], actual
        );

        return 1;
      }

      checksum_c = checksum_word (checksum_c, actual);
    }

    checksum_a = checksum_word (
      checksum_a, download[index * OUTPUT_WORDS]
    );
    checksum_b = checksum_word (
      checksum_b, download[index * OUTPUT_WORDS + 1u]
    );
  }

  fprintf (
    stdout,
    "PASS_CORE10_MULTISTAGE elements=%u verified_words=%u"
    " stageA=%016" PRIx64 " stageB=%016" PRIx64
    " output=%016" PRIx64
    " stages=3 gpu_buffers=4 staging_buffers=2 copies=2"
    " enabled_features=0 extensions=0\n",
    ELEMENT_COUNT, ELEMENT_COUNT * (OUTPUT_WORDS + 2u),
    checksum_a, checksum_b, checksum_c
  );

  return 0;
}

/**
 * Apply Vulkan 1.0 whole-allocation mapped-memory synchronization.
 *
 * Coherent staging skips the driver cache operation; noncoherent staging
 * calls vkFlushMappedMemoryRanges before submit or
 * vkInvalidateMappedMemoryRanges after fence completion.
 */
static VkResult synchronize_staging (
  VkDevice device,
  const multistage_buffer_t *buffer,
  const vulkan_multistage_memory_plan_t *plan,
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
    "MULTISTAGE_SYNC index=%" PRIu32
    " operation=%s coherent=%u driver_call=%u"
    " atom=%" PRIu64 " result=%d\n",
    index, invalidate == VK_TRUE ? "invalidate" : "flush",
    memory_plan.coherent,
    memory_plan.coherent == VK_FALSE ? 1u : 0u,
    (uint64_t) memory_plan.non_coherent_atom_size,
    result
  );

  return result;
}

/**
 * Create and execute the three-stage Vulkan 1.0 synthetic Compute pipeline.
 *
 * Allocates six buffers with a joint type/heap plan. Four GPU storage buffers
 * need no host mapping; upload and download have separate HOST_VISIBLE
 * allocations. Shader stage dependencies and transfers are synchronized in
 * one command buffer, and one fence protects CPU readback.
 *
 * Unsupported types and limits fail explicitly. Every partial allocation
 * and Vulkan handle is released, including on an intermediate failure.
 */
static int run_multistage (
  const vulkan_auto_device_t *selected,
  uint32_t *const shaders[STAGE_COUNT],
  const size_t shader_sizes[STAGE_COUNT],
  VkBool32 force_noncoherent)
{
  enum
  {
    GPU_INPUT = 0,
    STATE_A = 1,
    STATE_B = 2,
    GPU_OUTPUT = 3,
    STAGING_UPLOAD = 4,
    STAGING_DOWNLOAD = 5
  };

  int status = 1;
  VkResult result = VK_SUCCESS;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkShaderModule shader_modules[STAGE_COUNT] = { VK_NULL_HANDLE };
  VkPipeline pipelines[STAGE_COUNT] = { VK_NULL_HANDLE };
  VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
  VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
  VkCommandPool command_pool = VK_NULL_HANDLE;
  VkCommandBuffer command = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;

  multistage_buffer_t buffers[BUFFER_COUNT] = { 0 };
  vulkan_heap_allocation_request_t requests[BUFFER_COUNT] = { 0 };
  vulkan_multistage_memory_plan_t plan = { 0 };

  static const char *names[BUFFER_COUNT] =
  {
    "gpu-input", "stage-a", "stage-b", "gpu-output",
    "staging-upload", "staging-download"
  };
  static const VkDeviceSize sizes[BUFFER_COUNT] =
  {
    WORD_BYTES, WORD_BYTES, WORD_BYTES, OUTPUT_BYTES,
    WORD_BYTES, OUTPUT_BYTES
  };
  static const VkBufferUsageFlags usages[BUFFER_COUNT] =
  {
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
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

  VK_REQUIRE (vkCreateDevice (
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
      .size = sizes[index],
      .usage = usages[index],
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };

    VK_REQUIRE (vkCreateBuffer (
      device, &buffer_info, NULL, &buffers[index].buffer
    ));

    requests[index].label = names[index];
    requests[index].role = index < GPU_BUFFER_COUNT
      ? VULKAN_HEAP_BUFFER_GPU_STORAGE
      : VULKAN_HEAP_BUFFER_HOST_TRANSFER;
    requests[index].forbidden_flags =
      VK_MEMORY_PROPERTY_PROTECTED_BIT
      | VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD;

    if (force_noncoherent == VK_TRUE && index >= STAGING_UPLOAD)
    {
      requests[index].forbidden_flags |=
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }

    vkGetBufferMemoryRequirements (
      device, buffers[index].buffer, &requests[index].requirements
    );

    fprintf (
      stdout,
      "MULTISTAGE_REQUIREMENTS index=%" PRIu32
      " label=%s size=%" PRIu64
      " alignment=%" PRIu64 " bits=0x%08" PRIx32 "\n",
      index, names[index],
      (uint64_t) requests[index].requirements.size,
      (uint64_t) requests[index].requirements.alignment,
      requests[index].requirements.memoryTypeBits
    );

    if (requests[index].requirements.size < sizes[index])
    {
      fprintf (
        stderr,
        "REJECT_MULTISTAGE_BUFFER_SIZE index=%" PRIu32 "\n",
        index
      );

      goto cleanup;
    }
  }

  fprintf (
    stdout,
    "MULTISTAGE_STAGING_REQUEST mode=%s"
    " upload_bits=0x%08" PRIx32
    " download_bits=0x%08" PRIx32 "\n",
    force_noncoherent == VK_TRUE ? "noncoherent" : "auto",
    requests[STAGING_UPLOAD].requirements.memoryTypeBits,
    requests[STAGING_DOWNLOAD].requirements.memoryTypeBits
  );

  VK_REQUIRE (vulkan_multistage_plan_memory (
    &selected->memory_properties,
    selected->properties.limits.maxMemoryAllocationCount,
    requests, &plan, stdout
  ));

  for (uint32_t index = 0u; index < BUFFER_COUNT; index++)
  {
    const VkMemoryAllocateInfo allocation =
    {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requests[index].requirements.size,
      .memoryTypeIndex = plan.memory_type_indices[index]
    };

    VK_REQUIRE (vkAllocateMemory (
      device, &allocation, NULL, &buffers[index].memory
    ));

    VK_REQUIRE (vkBindBufferMemory (
      device, buffers[index].buffer, buffers[index].memory, 0u
    ));
  }

  VK_REQUIRE (vkMapMemory (
    device, buffers[STAGING_UPLOAD].memory,
    0u, VK_WHOLE_SIZE, 0u, &buffers[STAGING_UPLOAD].mapped
  ));
  VK_REQUIRE (vkMapMemory (
    device, buffers[STAGING_DOWNLOAD].memory,
    0u, VK_WHOLE_SIZE, 0u, &buffers[STAGING_DOWNLOAD].mapped
  ));

  fprintf (
    stdout,
    "MULTISTAGE_STAGING_MAPPED upload_type=%" PRIu32
    " download_type=%" PRIu32
    " upload_coherent=%u download_coherent=%u\n",
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
    upload[index] = synthetic_input (index);
  }

  VK_REQUIRE (synchronize_staging (
    device, &buffers[STAGING_UPLOAD], &plan, selected,
    &requests[STAGING_UPLOAD], STAGING_UPLOAD, VK_FALSE
  ));

  VkDescriptorSetLayoutBinding bindings[GPU_BUFFER_COUNT] = { 0 };

  for (uint32_t index = 0u; index < GPU_BUFFER_COUNT; index++)
  {
    bindings[index] = (VkDescriptorSetLayoutBinding)
    {
      .binding = index,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = 1u,
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
    };
  }

  const VkDescriptorSetLayoutCreateInfo layout_info =
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
    .bindingCount = GPU_BUFFER_COUNT,
    .pBindings = bindings
  };

  VK_REQUIRE (vkCreateDescriptorSetLayout (
    device, &layout_info, NULL, &descriptor_layout
  ));

  const VkPipelineLayoutCreateInfo pipeline_layout_info =
  {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    .setLayoutCount = 1u,
    .pSetLayouts = &descriptor_layout
  };

  VK_REQUIRE (vkCreatePipelineLayout (
    device, &pipeline_layout_info, NULL, &pipeline_layout
  ));

  for (uint32_t stage = 0u; stage < STAGE_COUNT; stage++)
  {
    const VkShaderModuleCreateInfo module_info =
    {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = shader_sizes[stage],
      .pCode = shaders[stage]
    };

    VK_REQUIRE (vkCreateShaderModule (
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

    VK_REQUIRE (vkCreateComputePipelines (
      device, VK_NULL_HANDLE, 1u,
      &pipeline_info, NULL, &pipelines[stage]
    ));
  }

  const VkDescriptorPoolSize pool_size =
  {
    .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
    .descriptorCount = GPU_BUFFER_COUNT
  };
  const VkDescriptorPoolCreateInfo pool_info =
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
    .maxSets = 1u,
    .poolSizeCount = 1u,
    .pPoolSizes = &pool_size
  };

  VK_REQUIRE (vkCreateDescriptorPool (
    device, &pool_info, NULL, &descriptor_pool
  ));

  const VkDescriptorSetAllocateInfo allocate_info =
  {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
    .descriptorPool = descriptor_pool,
    .descriptorSetCount = 1u,
    .pSetLayouts = &descriptor_layout
  };

  VK_REQUIRE (vkAllocateDescriptorSets (
    device, &allocate_info, &descriptor_set
  ));

  VkDescriptorBufferInfo buffer_infos[GPU_BUFFER_COUNT] = { 0 };
  VkWriteDescriptorSet writes[GPU_BUFFER_COUNT] = { 0 };

  for (uint32_t index = 0u; index < GPU_BUFFER_COUNT; index++)
  {
    buffer_infos[index] = (VkDescriptorBufferInfo)
    {
      .buffer = buffers[index].buffer,
      .offset = 0u,
      .range = sizes[index]
    };
    writes[index] = (VkWriteDescriptorSet)
    {
      .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
      .dstSet = descriptor_set,
      .dstBinding = index,
      .descriptorCount = 1u,
      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .pBufferInfo = &buffer_infos[index]
    };
  }

  vkUpdateDescriptorSets (
    device, GPU_BUFFER_COUNT, writes, 0u, NULL
  );

  const VkCommandPoolCreateInfo command_pool_info =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
    .queueFamilyIndex = selected->compute_queue_family
  };

  VK_REQUIRE (vkCreateCommandPool (
    device, &command_pool_info, NULL, &command_pool
  ));

  const VkCommandBufferAllocateInfo allocate_command =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
    .commandPool = command_pool,
    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
    .commandBufferCount = 1u
  };

  VK_REQUIRE (vkAllocateCommandBuffers (
    device, &allocate_command, &command
  ));

  const VkCommandBufferBeginInfo begin =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
  };

  VK_REQUIRE (vkBeginCommandBuffer (command, &begin));

  const VkMemoryBarrier host_to_transfer =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT
  };

  vkCmdPipelineBarrier (
    command,
    VK_PIPELINE_STAGE_HOST_BIT,
    VK_PIPELINE_STAGE_TRANSFER_BIT,
    0u, 1u, &host_to_transfer, 0u, NULL, 0u, NULL
  );

  const VkBufferCopy upload_copy =
  {
    .srcOffset = 0u,
    .dstOffset = 0u,
    .size = WORD_BYTES
  };

  vkCmdCopyBuffer (
    command, buffers[STAGING_UPLOAD].buffer,
    buffers[GPU_INPUT].buffer, 1u, &upload_copy
  );

  const VkMemoryBarrier transfer_to_compute =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT
  };

  vkCmdPipelineBarrier (
    command,
    VK_PIPELINE_STAGE_TRANSFER_BIT,
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
    0u, 1u, &transfer_to_compute, 0u, NULL, 0u, NULL
  );

  vkCmdBindDescriptorSets (
    command, VK_PIPELINE_BIND_POINT_COMPUTE,
    pipeline_layout, 0u, 1u, &descriptor_set, 0u, NULL
  );

  const VkMemoryBarrier stage_barrier =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT
  };

  for (uint32_t stage = 0u; stage < STAGE_COUNT; stage++)
  {
    vkCmdBindPipeline (
      command, VK_PIPELINE_BIND_POINT_COMPUTE, pipelines[stage]
    );
    vkCmdDispatch (
      command, ELEMENT_COUNT / LOCAL_SIZE, 1u, 1u
    );

    if (stage + 1u < STAGE_COUNT)
    {
      vkCmdPipelineBarrier (
        command,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0u, 1u, &stage_barrier, 0u, NULL, 0u, NULL
      );
    }
  }

  const VkMemoryBarrier compute_to_transfer =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT
  };

  vkCmdPipelineBarrier (
    command,
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
    VK_PIPELINE_STAGE_TRANSFER_BIT,
    0u, 1u, &compute_to_transfer, 0u, NULL, 0u, NULL
  );

  const VkBufferCopy download_copy =
  {
    .srcOffset = 0u,
    .dstOffset = 0u,
    .size = OUTPUT_BYTES
  };

  vkCmdCopyBuffer (
    command, buffers[GPU_OUTPUT].buffer,
    buffers[STAGING_DOWNLOAD].buffer, 1u, &download_copy
  );

  const VkMemoryBarrier transfer_to_host =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_HOST_READ_BIT
  };

  vkCmdPipelineBarrier (
    command,
    VK_PIPELINE_STAGE_TRANSFER_BIT,
    VK_PIPELINE_STAGE_HOST_BIT,
    0u, 1u, &transfer_to_host, 0u, NULL, 0u, NULL
  );

  VK_REQUIRE (vkEndCommandBuffer (command));

  const VkFenceCreateInfo fence_info =
  {
    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
  };

  VK_REQUIRE (vkCreateFence (
    device, &fence_info, NULL, &fence
  ));

  const VkSubmitInfo submit =
  {
    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    .commandBufferCount = 1u,
    .pCommandBuffers = &command
  };

  VK_REQUIRE (vkQueueSubmit (queue, 1u, &submit, fence));
  VK_REQUIRE (vkWaitForFences (
    device, 1u, &fence, VK_TRUE, UINT64_C (30000000000)
  ));

  VK_REQUIRE (synchronize_staging (
    device, &buffers[STAGING_DOWNLOAD], &plan, selected,
    &requests[STAGING_DOWNLOAD], STAGING_DOWNLOAD, VK_TRUE
  ));

  status = verify_pipeline (download);

cleanup:
  if (device != VK_NULL_HANDLE)
  {
    const VkResult idle = vkDeviceWaitIdle (device);

    if (idle != VK_SUCCESS)
    {
      fprintf (
        stderr, "MULTISTAGE_CLEANUP_WAIT_FAILED result=%d\n", idle
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

    for (uint32_t stage = 0u; stage < STAGE_COUNT; stage++)
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

    if (descriptor_layout != VK_NULL_HANDLE)
    {
      vkDestroyDescriptorSetLayout (device, descriptor_layout, NULL);
    }

    for (uint32_t index = BUFFER_COUNT; index != 0u; index--)
    {
      multistage_buffer_t *buffer = &buffers[index - 1u];

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
 * Run three SPIR-V 1.0 shaders with exact device-dependent Vulkan resources.
 *
 * The standard invocation requests an API 1.0 instance. --minimum 1.1
 * explicitly requires core 1.1; --require-noncoherent-staging rejects
 * coherent CPU staging memory instead of silently selecting another type.
 * Missing or incompatible shader inputs exit with status 2. Failure to
 * execute a compatible GPU pipeline exits with status 1.
 */
int main (int argc, char **argv)
{
  const VkBool32 explicit_version =
    argc == 6 || argc == 7 ? VK_TRUE : VK_FALSE;
  const VkBool32 force_noncoherent =
    (argc == 5
     && strcmp (argv[4], "--require-noncoherent-staging") == 0)
    || (argc == 7
     && strcmp (argv[6], "--require-noncoherent-staging") == 0)
    ? VK_TRUE : VK_FALSE;

  if (argc != 4
   && !(argc == 5 && force_noncoherent == VK_TRUE)
   && !(explicit_version == VK_TRUE
    && strcmp (argv[4], "--minimum") == 0
    && (strcmp (argv[5], "1.0") == 0
     || strcmp (argv[5], "1.1") == 0)
    && (argc == 6 || force_noncoherent == VK_TRUE)))
  {
    fprintf (
      stderr,
      "Usage: %s <stage-a-vk1.0.spv> <stage-b-vk1.0.spv>"
      " <stage-c-vk1.0.spv>"
      " [--minimum <1.0|1.1>] [--require-noncoherent-staging]\n",
      argv[0]
    );

    return 2;
  }

  uint32_t *shaders[STAGE_COUNT] = { NULL };
  size_t sizes[STAGE_COUNT] = { 0u };
  int status = 1;

  for (uint32_t stage = 0u; stage < STAGE_COUNT; stage++)
  {
    shaders[stage] = load_shader (argv[stage + 1u], &sizes[stage]);

    if (shaders[stage] == NULL)
    {
      status = 2;
      goto cleanup_shaders;
    }
  }

  const uint32_t required =
    explicit_version == VK_TRUE && strcmp (argv[5], "1.1") == 0
    ? VK_API_VERSION_1_1 : VK_API_VERSION_1_0;
  uint32_t loader_version = 0u;
  VkResult result = vulkan_core11_query_loader_version (
    vkGetInstanceProcAddr, &loader_version, stdout
  );

  if (result != VK_SUCCESS) goto cleanup_shaders;

  uint32_t instance_version = 0u;

  result = vulkan_core11_select_instance_version (
    loader_version, required, &instance_version, stdout
  );

  if (result != VK_SUCCESS) goto cleanup_shaders;

  const VkApplicationInfo application =
  {
    .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
    .pApplicationName = "Vulkan 1.0 Portable Three Stage Compute",
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

  result = vkCreateInstance (
    &instance_info, NULL, &instance
  );

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr, "REJECT_MULTISTAGE_INSTANCE result=%d\n", result
    );

    goto cleanup_shaders;
  }

  uint32_t device_count = 0u;
  result = vkEnumeratePhysicalDevices (
    instance, &device_count, NULL
  );

  if (result != VK_SUCCESS || device_count == 0u)
  {
    fprintf (
      stderr,
      "REJECT_MULTISTAGE_DEVICE_ENUMERATION count=%" PRIu32
      " result=%d\n", device_count, result
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
      stderr, "REJECT_MULTISTAGE_DEVICE_ENUMERATION result=%d\n",
      result
    );

    goto cleanup_devices;
  }

  const vulkan_auto_compute_requirements_t requirements =
  {
    .local_size = { LOCAL_SIZE, 1u, 1u },
    .group_count = { ELEMENT_COUNT / LOCAL_SIZE, 1u, 1u },
    .storage_buffer_count = GPU_BUFFER_COUNT,
    .largest_storage_buffer_bytes = OUTPUT_BYTES,
    .shared_memory_bytes = LOCAL_SIZE * sizeof (uint32_t)
  };

  for (uint32_t index = 0u; index < device_count; index++)
  {
    VkPhysicalDeviceProperties properties = { 0 };

    vkGetPhysicalDeviceProperties (
      devices[index], &properties
    );

    uint32_t effective = 0u;

    result = vulkan_core11_select_device_version (
      properties.apiVersion, instance_version,
      required, &effective, stdout
    );

    if (result != VK_SUCCESS)
    {
      fprintf (
        stdout,
        "SKIP_MULTISTAGE_DEVICE index=%" PRIu32
        " reason=unsupported_core_api\n", index
      );

      continue;
    }

    vulkan_auto_device_t selected = { 0 };

    result = vulkan_auto_check_device (
      devices[index], index,
      &requirements, &selected, stdout
    );

    if (result != VK_SUCCESS)
    {
      fprintf (
        stdout,
        "SKIP_MULTISTAGE_DEVICE index=%" PRIu32
        " reason=compute_limits_or_queue\n", index
      );

      continue;
    }

    fprintf (
      stdout,
      "MULTISTAGE_DEVICE_API effective=%" PRIu32 ".%" PRIu32
      " requested=%" PRIu32 ".%" PRIu32
      " shared_bytes=%u storage_bindings=%u"
      " enabled_features=0 extensions=0\n",
      VK_API_VERSION_MAJOR (effective),
      VK_API_VERSION_MINOR (effective),
      VK_API_VERSION_MAJOR (required),
      VK_API_VERSION_MINOR (required),
      (unsigned int) requirements.shared_memory_bytes,
      GPU_BUFFER_COUNT
    );

    if (run_multistage (
      &selected, shaders, sizes, force_noncoherent
    ) == 0)
    {
      status = 0;

      break;
    }

    fprintf (
      stdout,
      "SKIP_MULTISTAGE_DEVICE index=%" PRIu32
      " reason=memory_or_compute_execution\n", index
    );
  }

  if (status != 0)
  {
    fprintf (
      stderr,
      "NO_COMPATIBLE_VULKAN_MULTISTAGE_CONFIGURATION\n"
    );
  }

cleanup_devices:
  free (devices);

cleanup_instance:
  vkDestroyInstance (instance, NULL);

cleanup_shaders:
  for (uint32_t stage = 0u; stage < STAGE_COUNT; stage++)
  {
    free (shaders[stage]);
  }

  return status;
}
