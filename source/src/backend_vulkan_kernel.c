/**
 * Author......: See docs/credits.txt
 * License.....: MIT
 */

/**
 * Execute validated Vulkan 1.0 Compute programs in HASHCAT -I.
 *
 * The backend catalogue supplies a real physical device and VkInstance.
 * Information mode validates explicit SPIR-V 1.0 programs against physical
 * GPU limits before opening a temporary VkDevice. Storage-buffer,
 * descriptor-set, shader-module and pipeline ownership arrays are allocated
 * from declared counts and exact uniform or per-buffer ranges. The public
 * arithmetic programs verify one, two or four ordered stages; MD5 executes a
 * real 64-round compression stage over fixed public one-block messages; and
 * SHA-1, SHA-224, SHA-256 and BLAKE2s-256 accept variable-length byte
 * batches with algorithm-specific compression through the same owner.
 *
 * Information-mode outputs are checked against host references before
 * temporary Vulkan and host resources are released. The cryptographic paths
 * do not consume HASHCAT password candidates, target digests, salts, attack
 * modes or normal hashing scheduler state.
 */

#include "backend_vulkan_kernel.h"

#include "backend_vulkan.h"
#include "backend_vulkan_program.h"
#include "backend_vulkan_md5_spirv.h"
#include "backend_vulkan_hash.h"
#include "backend_vulkan_sha256.h"
#include "backend_vulkan_blake2s_spirv.h"
#include "backend_vulkan_sha1_spirv.h"
#include "backend_vulkan_sha224_spirv.h"
#include "backend_vulkan_sha256_spirv.h"
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

#define BACKEND_VULKAN_MD5_VECTORS 4u
#define BACKEND_VULKAN_MD5_BLOCK_WORDS 16u
#define BACKEND_VULKAN_MD5_DIGEST_WORDS 4u
#define BACKEND_VULKAN_MD5_BUFFER_WORDS (BACKEND_VULKAN_MD5_VECTORS * BACKEND_VULKAN_MD5_BLOCK_WORDS)
#define BACKEND_VULKAN_MD5_BYTES (BACKEND_VULKAN_MD5_BUFFER_WORDS * sizeof (uint32_t))

#define BACKEND_VULKAN_HASH_LOCAL_SIZE 32u
#define BACKEND_VULKAN_SHA256_DIGEST_BYTES 32u
#define BACKEND_VULKAN_HASH_METADATA_BYTES 8u

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
 * Declare the first cryptographic SPIR-V stage owned by the Vulkan backend.
 *
 * One invocation consumes one complete pre-padded 512-bit MD5 block and writes
 * four 32-bit digest state words. Four invocations process four independent
 * public known-answer vectors. The shader uses the MD5 initial state and round
 * constants corresponding to source/OpenCL/inc_hash_md5.cl, but no HASHCAT
 * password or target buffer is exposed to this program.
 */
static const backend_vulkan_program_stage_t backend_vulkan_md5_stages[1] =
{
  {
    .name = "md5_one_block",
    .spirv = backend_vulkan_md5_spirv,
    .spirv_words = sizeof (backend_vulkan_md5_spirv) / sizeof (uint32_t),
    .source_buffer_index = 0u,
    .destination_buffer_index = 1u,
    .local_size = { BACKEND_VULKAN_MD5_VECTORS, 1u, 1u },
    .group_count = { 1u, 1u, 1u }
  }
};

/**
 * Describe four independent one-block MD5 known-answer vectors.
 *
 * Both storage buffers expose 256 bytes so the input holds four complete
 * 64-byte padded blocks. The output shader writes the first sixteen words,
 * one four-word digest per vector; the remaining words remain host sentinels.
 */
static const backend_vulkan_program_t backend_vulkan_md5_program =
{
  .stages = backend_vulkan_md5_stages,
  .stage_count = 1u,
  .buffer_count = 2u,
  .buffer_bytes = BACKEND_VULKAN_MD5_BYTES
};

/**
 * Select the explicit host-side execution contract for a validated program.
 *
 * PUBLIC_U32 uses the synthetic arithmetic reference and optional controlled
 * fault/timeout probes. MD5 uses fixed public known-answer vectors. The
 * hash batch accepts caller-owned messages for SHA-1, SHA-224, SHA-256 or
 * BLAKE2s-256 and returns canonical digest bytes. The execution contract
 * is selected explicitly, never inferred from shader or GPU identity.
 */
typedef enum backend_vulkan_execution_kind
{
  BACKEND_VULKAN_EXECUTION_PUBLIC_U32 = 0,
  BACKEND_VULKAN_EXECUTION_MD5 = 1,
  BACKEND_VULKAN_EXECUTION_HASH_BATCH = 2
} backend_vulkan_execution_kind_t;

/**
 * Borrow input and output memory for a selected SHA or BLAKE2s-256 batch.
 *
 * input_bytes and output_bytes are the validated descriptor ranges.
 * digest_bytes selects the exact 20-, 28- or 32-byte result; name provides
 * its stable diagnostic label, and little_endian selects the byte order
 * required for canonical output. Caller spans and digest bytes remain
 * outside the GPU job's ownership.
 */
typedef struct backend_vulkan_hash_batch_context
{
  const backend_vulkan_hash_message_t *messages;
  uint32_t message_count;
  uint32_t digest_bytes;
  VkBool32 little_endian;
  const char *name;
  uint8_t *digests;
  VkDeviceSize input_bytes;
  VkDeviceSize output_bytes;
} backend_vulkan_hash_batch_context_t;

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
  VkDeviceSize bytes;
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
  uint32_t submitted;
  backend_vulkan_public_buffer_t *buffers;
  VkDescriptorSetLayout descriptor_layout;
  VkPipelineLayout pipeline_layout;
  VkDescriptorPool descriptor_pool;
  VkDescriptorSet *descriptor_set;
  VkShaderModule *shader;
  VkPipeline *pipeline;
  const backend_vulkan_program_t *program;
  struct backend_vulkan_public_fault *fault;
} backend_vulkan_public_job_t;

/**
 * Track a controlled diagnostic failure at an exact resource checkpoint.
 *
 * A non-NULL fault pointer activates consecutive checkpoint accounting.
 * The test's point is a positive one-based checkpoint number. An injected
 * error returns VK_ERROR_INITIALIZATION_FAILED and is explicitly identified
 * as simulated, never as a driver result or physical device loss.
 */
typedef struct backend_vulkan_public_fault
{
  uint32_t point;
  uint32_t reached;
  uint32_t hit;
  uint32_t submitted;
  FILE *diagnostics;
} backend_vulkan_public_fault_t;

/**
 * Record exact resources owned at the entry to deterministic cleanup.
 *
 * Resource counts describe non-null Vulkan handles and mapped allocations
 * passed through the normal ownership cleanup. completed records completion
 * of all cleanup calls and host-array releases. wait_result is the actual
 * vkDeviceWaitIdle result for an acquired VkDevice.
 */
typedef struct backend_vulkan_public_release_counts
{
  uint32_t device;
  uint32_t buffers;
  uint32_t memories;
  uint32_t maps;
  uint32_t descriptor_sets;
  uint32_t shaders;
  uint32_t pipelines;
  uint32_t descriptor_pool;
  uint32_t pipeline_layout;
  uint32_t descriptor_layout;
  uint32_t fence;
  uint32_t command;
  uint32_t command_pool;
  uint32_t host_arrays;
  uint32_t completed;
  VkResult wait_result;
} backend_vulkan_public_release_counts_t;

/**
 * Trigger one explicitly numbered test checkpoint after resource acquisition.
 *
 * Without an active diagnostic fault, the function is a no-op. With a
 * diagnostic fault, each checkpoint increments reached exactly once.
 * The selected point prints its resource label and whether a real queue
 * submission already occurred, then returns a simulated initialization
 * error without substituting any Vulkan driver function or VkResult.
 */
static VkResult backend_vulkan_public_checkpoint (
  backend_vulkan_public_job_t *job,
  const char *resource)
{
  if (job->fault == NULL) return VK_SUCCESS;

  backend_vulkan_public_fault_t *fault = job->fault;

  fault->reached++;

  if (fault->reached != fault->point) return VK_SUCCESS;

  fault->hit = 1u;

  fprintf (
    fault->diagnostics,
    "BACKEND_PUBLIC_FAULT_TRIGGER"
    " point=%" PRIu32
    " resource=%s"
    " simulated=1"
    " submitted=%" PRIu32
    " result=%d\n",
    fault->reached,
    resource,
    fault->submitted,
    VK_ERROR_INITIALIZATION_FAILED
  );

  return VK_ERROR_INITIALIZATION_FAILED;
}

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
 * using exact program counts. Host arrays are freed even without VkDevice.
 * An optional counts output records the exact resources owned on entry,
 * the actual device-idle result and completion of the entire cleanup.
 * A null or partially initialized job is safe to release. The first
 * vkDeviceWaitIdle result survives cleanup.
 */
static VkResult backend_vulkan_public_release (
  backend_vulkan_public_job_t *job,
  backend_vulkan_public_release_counts_t *counts)
{
  if (job == NULL) return VK_ERROR_INITIALIZATION_FAILED;

  VkResult result = VK_SUCCESS;

  const uint32_t stage_count =
    job->program != NULL ? job->program->stage_count : 0u;

  const uint32_t buffer_count =
    job->program != NULL ? job->program->buffer_count : 0u;

  if (counts != NULL)
  {
    memset (counts, 0, sizeof (*counts));

    counts->device = job->device != VK_NULL_HANDLE;
    counts->descriptor_pool = job->descriptor_pool != VK_NULL_HANDLE;
    counts->pipeline_layout = job->pipeline_layout != VK_NULL_HANDLE;
    counts->descriptor_layout = job->descriptor_layout != VK_NULL_HANDLE;
    counts->fence = job->fence != VK_NULL_HANDLE;
    counts->command = job->command != VK_NULL_HANDLE;
    counts->command_pool = job->command_pool != VK_NULL_HANDLE;
    counts->host_arrays =
      (job->buffers != NULL)
      + (job->descriptor_set != NULL)
      + (job->shader != NULL)
      + (job->pipeline != NULL);

    for (uint32_t stage = 0u; stage < stage_count; stage++)
    {
      if (job->descriptor_set != NULL)
      {
        counts->descriptor_sets +=
          job->descriptor_set[stage] != VK_NULL_HANDLE;
      }

      if (job->shader != NULL)
      {
        counts->shaders += job->shader[stage] != VK_NULL_HANDLE;
      }

      if (job->pipeline != NULL)
      {
        counts->pipelines += job->pipeline[stage] != VK_NULL_HANDLE;
      }
    }

    for (uint32_t i = 0u; i < buffer_count; i++)
    {
      if (job->buffers == NULL) break;

      counts->buffers +=
        job->buffers[i].buffer != VK_NULL_HANDLE;
      counts->memories +=
        job->buffers[i].memory != VK_NULL_HANDLE;
      counts->maps += job->buffers[i].mapped != NULL;
    }
  }

  if (job->device != VK_NULL_HANDLE)
  {
    result = vkDeviceWaitIdle (job->device);

    if (counts != NULL) counts->wait_result = result;

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

  if (counts != NULL) counts->completed = 1u;

  return result;
}

/**
 * Allocate one host-visible Vulkan storage buffer for the active program.
 *
 * Resolves the exact logical size declared for buffer_index and uses the real
 * vkGetBufferMemoryRequirements bitmask to select an explicitly compatible
 * memory type. Uniform programs therefore retain their shared range, while
 * heterogeneous batch programs allocate only their declared input/output
 * ranges. HOST_COHERENT is preferred; HOST_VISIBLE noncoherent memory uses
 * explicit cache synchronization.
 *
 * After each successful buffer, memory and mapping acquisition, an optional
 * numbered diagnostic checkpoint can return a simulated initialization error.
 * Otherwise real driver VkResults propagate unchanged and the parent job owns
 * every partial resource.
 */
static VkResult backend_vulkan_public_buffer_open (
  backend_vulkan_public_job_t *job,
  const vulkan_auto_device_t *selected,
  uint32_t buffer_index)
{
  if (job == NULL || job->program == NULL || job->buffers == NULL
   || buffer_index >= job->program->buffer_count)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  backend_vulkan_public_buffer_t *buffer =
    &job->buffers[buffer_index];

  buffer->bytes = backend_vulkan_program_buffer_bytes (
    job->program, buffer_index
  );

  if (buffer->bytes == 0u)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  const VkBufferCreateInfo info =
  {
    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
    .size = buffer->bytes,
    .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
    .sharingMode = VK_SHARING_MODE_EXCLUSIVE
  };

  VkResult result = vkCreateBuffer (
    job->device, &info, NULL, &buffer->buffer
  );

  if (result != VK_SUCCESS) return result;

  result = backend_vulkan_public_checkpoint (job, "buffer");

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

  result = backend_vulkan_public_checkpoint (job, "memory");

  if (result != VK_SUCCESS) return result;

  result = vkBindBufferMemory (
    job->device, buffer->buffer, buffer->memory, 0u
  );

  if (result != VK_SUCCESS) return result;

  result = vkMapMemory (
    job->device, buffer->memory, 0u,
    VK_WHOLE_SIZE, 0u, &buffer->mapped
  );

  if (result != VK_SUCCESS) return result;

  return backend_vulkan_public_checkpoint (job, "map");
}

/**
 * Create one descriptor set and Vulkan 1.0 Compute pipeline per program stage.
 *
 * The validated program supplies immutable SPIR-V bytes, exact source and
 * destination buffer indices and stage count. Every stage uses one common
 * two-binding STORAGE_BUFFER layout. Descriptor sets are allocated one at a
 * time into host arrays sized by backend_vulkan_public_prepare.
 * Explicit diagnostic checkpoints follow layout, pool and per-stage
 * descriptor/shader/pipeline acquisitions. An injected initialization
 * error leaves all acquired objects owned by the standard job cleanup.
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

  result = backend_vulkan_public_checkpoint (job, "descriptor_layout");

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

  result = backend_vulkan_public_checkpoint (job, "pipeline_layout");

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

  result = backend_vulkan_public_checkpoint (job, "descriptor_pool");

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

    result = backend_vulkan_public_checkpoint (job, "descriptor_set");

    if (result != VK_SUCCESS) return result;

    const backend_vulkan_program_stage_t *stage =
      &job->program->stages[stage_index];

    const VkDescriptorBufferInfo buffer_info[2] =
    {
      {
        .buffer = job->buffers[stage->source_buffer_index].buffer,
        .offset = 0u,
        .range = job->buffers[stage->source_buffer_index].bytes
      },
      {
        .buffer = job->buffers[stage->destination_buffer_index].buffer,
        .offset = 0u,
        .range = job->buffers[stage->destination_buffer_index].bytes
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

    result = backend_vulkan_public_checkpoint (job, "shader");

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

    result = backend_vulkan_public_checkpoint (job, "pipeline");

    if (result != VK_SUCCESS) return result;
  }

  return VK_SUCCESS;
}

/**
 * Build four canonical one-block MD5 messages in native little-endian words.
 *
 * The public vectors are "", "a", "abc" and "message digest". Every message
 * fits one MD5 block. Padding is encoded explicitly into uint32 words so the
 * host representation does not depend on pointer casts or host byte order.
 * Word 14 receives the original bit length and word 15 remains zero.
 */
static void backend_vulkan_md5_prepare_blocks (
  uint32_t blocks[BACKEND_VULKAN_MD5_BUFFER_WORDS])
{
  static const char *messages[BACKEND_VULKAN_MD5_VECTORS] =
  {
    "",
    "a",
    "abc",
    "message digest"
  };

  memset (
    blocks, 0,
    BACKEND_VULKAN_MD5_BUFFER_WORDS * sizeof (uint32_t)
  );

  for (uint32_t vector_index = 0u;
       vector_index < BACKEND_VULKAN_MD5_VECTORS;
       vector_index++)
  {
    const uint8_t *message =
      (const uint8_t *) messages[vector_index];

    const size_t length = strlen (messages[vector_index]);
    const uint32_t base =
      vector_index * BACKEND_VULKAN_MD5_BLOCK_WORDS;

    for (size_t i = 0u; i < length; i++)
    {
      blocks[base + (uint32_t) (i >> 2)]
        |= (uint32_t) message[i] << ((uint32_t) (i & 3u) * 8u);
    }

    blocks[base + (uint32_t) (length >> 2)]
      |= UINT32_C (0x80) << ((uint32_t) (length & 3u) * 8u);

    blocks[base + 14u] = (uint32_t) length * 8u;
  }
}

/**
 * Dispatch the fixed-vector MD5 program through the common dynamic resources.
 *
 * The source buffer receives four complete padded MD5 blocks. The destination
 * buffer is initialized to a sentinel before submission so writes outside the
 * sixteen declared digest words are detectable. One real Compute dispatch
 * executes four invocations, followed by a shader-write-to-host-read barrier,
 * real queue submission and fence wait. The first sixteen output words are
 * compared with standard MD5 known-answer digests and every remaining word
 * must preserve the sentinel.
 *
 * Returns VK_SUCCESS only for all four exact 128-bit digests, unchanged
 * out-of-range words and successful Vulkan synchronization. No HASHCAT
 * candidate, target digest, salt or scheduler state is read or modified.
 */
static VkResult backend_vulkan_md5_dispatch (
  backend_vulkan_public_job_t *job,
  FILE *diagnostics)
{
  if (job == NULL || diagnostics == NULL || job->program == NULL
   || job->program->stage_count != 1u
   || job->program->buffer_count != 2u
   || job->program->buffer_bytes != BACKEND_VULKAN_MD5_BYTES
   || job->buffers == NULL
   || job->descriptor_set == NULL
   || job->pipeline == NULL)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  static const uint32_t expected[
    BACKEND_VULKAN_MD5_VECTORS * BACKEND_VULKAN_MD5_DIGEST_WORDS] =
  {
    UINT32_C (0xd98c1dd4), UINT32_C (0x04b2008f),
    UINT32_C (0x980980e9), UINT32_C (0x7e42f8ec),
    UINT32_C (0xb975c10c), UINT32_C (0xa8b6f1c0),
    UINT32_C (0xe299c331), UINT32_C (0x61267769),
    UINT32_C (0x98500190), UINT32_C (0xb04fd23c),
    UINT32_C (0x7d3f96d6), UINT32_C (0x727fe128),
    UINT32_C (0x7d696bf9), UINT32_C (0x8d93b77c),
    UINT32_C (0x312f5a52), UINT32_C (0xd061f1aa)
  };

  const backend_vulkan_program_stage_t *stage =
    &job->program->stages[0];

  backend_vulkan_public_buffer_t *input_buffer =
    &job->buffers[stage->source_buffer_index];

  backend_vulkan_public_buffer_t *output_buffer =
    &job->buffers[stage->destination_buffer_index];

  uint32_t blocks[BACKEND_VULKAN_MD5_BUFFER_WORDS];

  backend_vulkan_md5_prepare_blocks (blocks);

  memcpy (
    input_buffer->mapped, blocks, sizeof (blocks)
  );

  uint32_t *output_words = (uint32_t *) output_buffer->mapped;

  for (uint32_t i = 0u; i < BACKEND_VULKAN_MD5_BUFFER_WORDS; i++)
  {
    output_words[i] = UINT32_C (0xa5a5a5a5);
  }

  VkResult result = vulkan_auto_sync_mapped_memory (
    job->device, input_buffer->memory,
    &input_buffer->plan, VK_FALSE
  );

  if (result != VK_SUCCESS) return result;

  result = vulkan_auto_sync_mapped_memory (
    job->device, output_buffer->memory,
    &output_buffer->plan, VK_FALSE
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
    job->pipeline[0]
  );

  vkCmdBindDescriptorSets (
    job->command, VK_PIPELINE_BIND_POINT_COMPUTE,
    job->pipeline_layout, 0u, 1u,
    &job->descriptor_set[0], 0u, NULL
  );

  vkCmdDispatch (
    job->command,
    stage->group_count[0],
    stage->group_count[1],
    stage->group_count[2]
  );

  const VkBufferMemoryBarrier host_barrier =
  {
    .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .buffer = output_buffer->buffer,
    .offset = 0u,
    .size = output_buffer->bytes
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

  job->submitted = 1u;

  result = vkWaitForFences (
    job->device, 1u, &job->fence, VK_TRUE, UINT64_MAX
  );

  if (result != VK_SUCCESS) return result;

  result = vulkan_auto_sync_mapped_memory (
    job->device, output_buffer->memory,
    &output_buffer->plan, VK_TRUE
  );

  if (result != VK_SUCCESS) return result;

  const uint32_t digest_words =
    BACKEND_VULKAN_MD5_VECTORS * BACKEND_VULKAN_MD5_DIGEST_WORDS;

  for (uint32_t i = 0u; i < digest_words; i++)
  {
    if (output_words[i] != expected[i])
    {
      fprintf (
        diagnostics,
        "BACKEND_VULKAN_MD5_MISMATCH"
        " vector=%" PRIu32
        " word=%" PRIu32
        " actual=0x%08" PRIx32
        " expected=0x%08" PRIx32 "\n",
        i / BACKEND_VULKAN_MD5_DIGEST_WORDS,
        i % BACKEND_VULKAN_MD5_DIGEST_WORDS,
        output_words[i], expected[i]
      );

      return VK_ERROR_INITIALIZATION_FAILED;
    }
  }

  for (uint32_t i = digest_words;
       i < BACKEND_VULKAN_MD5_BUFFER_WORDS;
       i++)
  {
    if (output_words[i] != UINT32_C (0xa5a5a5a5))
    {
      fprintf (
        diagnostics,
        "BACKEND_VULKAN_MD5_RANGE_MISMATCH"
        " word=%" PRIu32
        " actual=0x%08" PRIx32
        " expected=0xa5a5a5a5\n",
        i, output_words[i]
      );

      return VK_ERROR_INITIALIZATION_FAILED;
    }
  }

  fprintf (
    diagnostics,
    "BACKEND_VULKAN_MD5_DISPATCH_PASS"
    " vectors=4"
    " blocks=4"
    " rounds=64"
    " dispatches=1"
    " verified_digest_words=16"
    " sentinel_words=48"
    " coherent_input=%u"
    " coherent_output=%u"
    " password_inputs=0"
    " target_digests=0"
    " scheduled=0\n",
    input_buffer->plan.coherent,
    output_buffer->plan.coherent
  );

  return VK_SUCCESS;
}

/**
 * Pack caller-owned SHA or BLAKE2s messages into the input storage buffer.
 *
 * The buffer begins with two uint32 words per message: byte offset relative to
 * the payload start and byte length. Payload bytes follow the complete metadata
 * table without gaps; the final allocation padding is zeroed. The caller's
 * message spans are borrowed only for the duration of this copy.
 */
static VkResult backend_vulkan_hash_batch_pack_input (
  backend_vulkan_public_job_t *job,
  const backend_vulkan_hash_batch_context_t *context)
{
  if (job == NULL || context == NULL || job->buffers == NULL
   || context->messages == NULL || context->message_count == 0u
   || job->buffers[0].mapped == NULL
   || job->buffers[0].bytes != context->input_bytes)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  if (context->input_bytes > SIZE_MAX)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  uint8_t *mapped = (uint8_t *) job->buffers[0].mapped;

  memset (mapped, 0, (size_t) context->input_bytes);

  uint32_t *metadata = (uint32_t *) mapped;
  uint8_t *payload =
    mapped
    + (size_t) context->message_count
      * BACKEND_VULKAN_HASH_METADATA_BYTES;

  uint32_t offset = 0u;

  for (uint32_t i = 0u; i < context->message_count; i++)
  {
    metadata[2u * i] = offset;
    metadata[2u * i + 1u] = context->messages[i].size;

    if (context->messages[i].size != 0u)
    {
      memcpy (
        payload + offset,
        context->messages[i].data,
        context->messages[i].size
      );
    }

    offset += context->messages[i].size;
  }

  return VK_SUCCESS;
}

/**
 * Dispatch one caller-owned SHA-family or BLAKE2s-256 byte-message batch.
 *
 * The exact heterogeneous input/output ranges come from the validated program.
 * Input metadata and bytes are flushed before a host-write-to-shader-read
 * barrier. One real Compute dispatch hashes all messages, followed by a
 * shader-write-to-host-read barrier, queue submission, fence completion and
 * output invalidation. The digest word count is taken from the selected
 * algorithm, then serialized in its canonical big- or little-endian byte
 * order into caller-owned output memory.
 *
 * Returns VK_SUCCESS only after every requested message has a complete digest.
 * No target comparison, password generation or HASHCAT scheduler state is
 * accessed.
 */
static VkResult backend_vulkan_hash_batch_dispatch (
  backend_vulkan_public_job_t *job,
  const backend_vulkan_hash_batch_context_t *context,
  FILE *diagnostics)
{
  if (job == NULL || context == NULL || diagnostics == NULL
   || job->program == NULL
   || job->program->stage_count != 1u
   || job->program->buffer_count != 2u
   || job->buffers == NULL
   || job->descriptor_set == NULL
   || job->pipeline == NULL
   || context->messages == NULL
   || context->message_count == 0u
   || context->digests == NULL
   || job->buffers[0].bytes != context->input_bytes
   || job->buffers[1].bytes != context->output_bytes)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  VkResult result = backend_vulkan_hash_batch_pack_input (
    job, context
  );

  if (result != VK_SUCCESS) return result;

  result = vulkan_auto_sync_mapped_memory (
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

  const backend_vulkan_program_stage_t *stage =
    &job->program->stages[0];

  vkCmdBindPipeline (
    job->command, VK_PIPELINE_BIND_POINT_COMPUTE,
    job->pipeline[0]
  );

  vkCmdBindDescriptorSets (
    job->command, VK_PIPELINE_BIND_POINT_COMPUTE,
    job->pipeline_layout, 0u, 1u,
    &job->descriptor_set[0], 0u, NULL
  );

  vkCmdDispatch (
    job->command,
    stage->group_count[0],
    stage->group_count[1],
    stage->group_count[2]
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

  job->submitted = 1u;

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

  const uint32_t digest_words =
    context->digest_bytes / sizeof (uint32_t);

  for (uint32_t i = 0u; i < context->message_count; i++)
  {
    for (uint32_t word_index = 0u; word_index < digest_words; word_index++)
    {
      const uint32_t word =
        output[(size_t) i * digest_words + word_index];

      const size_t byte_index =
        (size_t) i * context->digest_bytes
        + (size_t) word_index * sizeof (uint32_t);

      if (context->little_endian == VK_TRUE)
      {
        context->digests[byte_index + 0u] = (uint8_t) word;
        context->digests[byte_index + 1u] = (uint8_t) (word >> 8u);
        context->digests[byte_index + 2u] = (uint8_t) (word >> 16u);
        context->digests[byte_index + 3u] = (uint8_t) (word >> 24u);
      }
      else
      {
        context->digests[byte_index + 0u] = (uint8_t) (word >> 24u);
        context->digests[byte_index + 1u] = (uint8_t) (word >> 16u);
        context->digests[byte_index + 2u] = (uint8_t) (word >> 8u);
        context->digests[byte_index + 3u] = (uint8_t) word;
      }
    }
  }

  fprintf (
    diagnostics,
    "BACKEND_VULKAN_%s_DISPATCH_PASS"
    " messages=%" PRIu32
    " input_bytes=%" PRIu64
    " output_bytes=%" PRIu64
    " groups=%" PRIu32
    " dispatches=1"
    " digests=%" PRIu32
    " password_generation=0"
    " target_digests=0"
    " scheduled=0\n",
    context->name,
    context->message_count,
    (uint64_t) context->input_bytes,
    (uint64_t) context->output_bytes,
    stage->group_count[0],
    context->message_count
  );

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
 * Controlled diagnostics can abort immediately before vkQueueSubmit or
 * immediately after a successful real submission; the latter is completed
 * by vkDeviceWaitIdle in the ordinary job cleanup. An explicit fence probe
 * submits the same commands without associating the fresh unsignaled fence,
 * then reads a real VK_TIMEOUT from a zero-duration vkWaitForFences call.
 * No failed Vulkan driver result or device-loss condition is synthesized.
 *
 * Stage zero uses the public_u32_transform CPU reference. Every following
 * stage uses the public_u32_rotate reference, matching the immutable public
 * SPIR-V module reused by the four-stage diagnostic program. No HASHCAT
 * candidate, digest, salt or scheduling data participates in verification.
 */
static VkResult backend_vulkan_public_dispatch (
  backend_vulkan_public_job_t *job,
  FILE *diagnostics,
  VkBool32 probe_unsignaled_fence)
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

  result = backend_vulkan_public_checkpoint (job, "before_submit");

  if (result != VK_SUCCESS) return result;

  const VkSubmitInfo submit =
  {
    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    .commandBufferCount = 1u,
    .pCommandBuffers = &job->command
  };

  result = vkQueueSubmit (
    job->queue, 1u, &submit,
    probe_unsignaled_fence == VK_TRUE ? VK_NULL_HANDLE : job->fence
  );

  if (result != VK_SUCCESS) return result;

  job->submitted = 1u;

  if (job->fault != NULL) job->fault->submitted = 1u;

  if (probe_unsignaled_fence == VK_TRUE)
  {
    result = vkWaitForFences (
      job->device, 1u, &job->fence, VK_TRUE, 0u
    );

    fprintf (
      diagnostics,
      "BACKEND_PUBLIC_FENCE_WAIT"
      " submitted=%" PRIu32
      " fence_submitted=0"
      " timeout_ns=0"
      " driver_call=1"
      " result=%d"
      " scheduled=0\n",
      job->submitted, result
    );

    return result == VK_SUCCESS
      ? VK_ERROR_INITIALIZATION_FAILED : result;
  }

  result = backend_vulkan_public_checkpoint (job, "after_submit");

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
 * Run one declared Vulkan 1.0 program using backend device discovery.
 *
 * Obtains real physical-device properties from the backend catalogue,
 * validates the complete graph, allocates ownership arrays from program
 * counts and creates one temporary VkDevice. execution_kind explicitly
 * selects public arithmetic, fixed-vector MD5 or a caller-owned hash batch.
 * execution_context is required only by the hash batch path. No shader
 * identity or device-name inference is used.
 *
 * controlled_fault_point and probe_unsignaled_fence are valid only for the
 * public arithmetic execution kind. Cryptographic executions use the same
 * dynamic allocation, descriptor, pipeline, command and cleanup path without
 * enabling those arithmetic diagnostics. No Vulkan HASHCAT hashing device is
 * registered and scheduler state is not modified.
 */
static VkResult backend_vulkan_kernel_verify_program (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  FILE *diagnostics,
  const backend_vulkan_program_t *program,
  backend_vulkan_execution_kind_t execution_kind,
  void *execution_context,
  uint32_t controlled_fault_point,
  VkBool32 probe_unsignaled_fence)
{
  if (backend_ctx == NULL || diagnostics == NULL
   || backend_ctx->vulkan_catalog == NULL
   || device_index >= backend_ctx->vulkan_devices_cnt
   || program == NULL || program->stage_count == 0u
   || execution_kind > BACKEND_VULKAN_EXECUTION_HASH_BATCH
   || controlled_fault_point > 37u
   || (execution_kind != BACKEND_VULKAN_EXECUTION_HASH_BATCH
    && execution_context != NULL)
   || (execution_kind == BACKEND_VULKAN_EXECUTION_HASH_BATCH
    && (execution_context == NULL
     || controlled_fault_point != 0u || probe_unsignaled_fence == VK_TRUE))
   || (execution_kind == BACKEND_VULKAN_EXECUTION_MD5
    && (controlled_fault_point != 0u || probe_unsignaled_fence == VK_TRUE))
   || (probe_unsignaled_fence == VK_TRUE
    && (execution_kind != BACKEND_VULKAN_EXECUTION_PUBLIC_U32
     || controlled_fault_point != 0u || program->stage_count != 4u)))
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
    .largest_storage_buffer_bytes =
      backend_vulkan_program_largest_buffer_bytes (program),
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

  backend_vulkan_public_fault_t fault =
  {
    .point = controlled_fault_point,
    .diagnostics = diagnostics
  };

  result = backend_vulkan_public_prepare (&job, program);

  if (result != VK_SUCCESS) return result;

  if (controlled_fault_point != 0u) job.fault = &fault;

  result = backend_vulkan_public_checkpoint (&job, "host_arrays");

  if (result != VK_SUCCESS) goto cleanup;

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

  result = backend_vulkan_public_checkpoint (&job, "device");

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
      &job, &selected, i
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

  result = backend_vulkan_public_checkpoint (&job, "command_pool");

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

  result = backend_vulkan_public_checkpoint (&job, "command_buffer");

  if (result != VK_SUCCESS) goto cleanup;

  const VkFenceCreateInfo fence_info =
  {
    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
  };

  result = vkCreateFence (
    job.device, &fence_info, NULL, &job.fence
  );

  if (result != VK_SUCCESS) goto cleanup;

  result = backend_vulkan_public_checkpoint (&job, "fence");

  if (result != VK_SUCCESS) goto cleanup;

  if (execution_kind == BACKEND_VULKAN_EXECUTION_HASH_BATCH)
  {
    const backend_vulkan_hash_batch_context_t *context =
      (const backend_vulkan_hash_batch_context_t *) execution_context;

    fprintf (
      diagnostics,
      "BACKEND_VULKAN_%s_READY"
      " index=%" PRIu32
      " queue_family=%" PRIu32
      " api=%" PRIu32 ".%" PRIu32
      " shader_words=%" PRIu32
      " descriptors=2"
      " buffers=2"
      " input_bytes=%" PRIu64
      " output_bytes=%" PRIu64
      " messages=%" PRIu32
      " local=%" PRIu32 " groups=%" PRIu32
      " program_validated=1 dynamic_resources=1"
      " features=0 extensions=0"
      " password_generation=0 target_digests=0 scheduled=0\n",
      context->name,
      device_index, selected.compute_queue_family,
      VK_API_VERSION_MAJOR (effective_api),
      VK_API_VERSION_MINOR (effective_api),
      program->stages[0].spirv_words,
      (uint64_t) context->input_bytes,
      (uint64_t) context->output_bytes,
      context->message_count,
      program->stages[0].local_size[0],
      program->stages[0].group_count[0]
    );
  }
  else if (execution_kind == BACKEND_VULKAN_EXECUTION_MD5)
  {
    fprintf (
      diagnostics,
      "BACKEND_VULKAN_MD5_READY"
      " index=%" PRIu32
      " queue_family=%" PRIu32
      " api=%" PRIu32 ".%" PRIu32
      " shader_words=%" PRIu32
      " descriptors=2"
      " buffers=2"
      " vectors=4"
      " block_bytes=64"
      " local=%" PRIu32 " groups=%" PRIu32
      " program_validated=1 dynamic_resources=1"
      " features=0 extensions=0"
      " password_inputs=0 target_digests=0 scheduled=0\n",
      device_index, selected.compute_queue_family,
      VK_API_VERSION_MAJOR (effective_api),
      VK_API_VERSION_MINOR (effective_api),
      program->stages[0].spirv_words,
      program->stages[0].local_size[0],
      program->stages[0].group_count[0]
    );
  }
  else if (program->stage_count == 1u)
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

  if (execution_kind == BACKEND_VULKAN_EXECUTION_HASH_BATCH)
  {
    result = backend_vulkan_hash_batch_dispatch (
      &job,
      (const backend_vulkan_hash_batch_context_t *) execution_context,
      diagnostics
    );
  }
  else if (execution_kind == BACKEND_VULKAN_EXECUTION_MD5)
  {
    result = backend_vulkan_md5_dispatch (
      &job, diagnostics
    );
  }
  else
  {
    result = backend_vulkan_public_dispatch (
      &job, diagnostics, probe_unsignaled_fence
    );
  }

cleanup:
  {
    backend_vulkan_public_release_counts_t counts = { 0 };

    const uint32_t submitted = job.submitted;

    const VkResult released = backend_vulkan_public_release (
      &job,
      controlled_fault_point != 0u || probe_unsignaled_fence == VK_TRUE
        ? &counts : NULL
    );

    if (result == VK_SUCCESS || (probe_unsignaled_fence == VK_TRUE
      && released != VK_SUCCESS)) result = released;

    if (probe_unsignaled_fence == VK_TRUE)
    {
      fprintf (
        diagnostics,
        "BACKEND_PUBLIC_FENCE_RELEASE"
        " submitted=%" PRIu32
        " device=%" PRIu32
        " fence=%" PRIu32
        " command_buffer=%" PRIu32
        " pipelines=%" PRIu32
        " buffers=%" PRIu32
        " wait_idle=%d"
        " completed=%" PRIu32
        " cleanup_result=%d"
        " scheduled=0\n",
        submitted,
        counts.device,
        counts.fence,
        counts.command,
        counts.pipelines,
        counts.buffers,
        counts.wait_result,
        counts.completed,
        released
      );

      if (submitted == 1u
       && (counts.device != 1u
        || counts.fence != 1u
        || counts.command != 1u
        || counts.pipelines != program->stage_count
        || counts.buffers != program->buffer_count
        || counts.completed != 1u))
      {
        result = VK_ERROR_INITIALIZATION_FAILED;
      }
    }

    if (controlled_fault_point != 0u)
    {
      fprintf (
        diagnostics,
        "BACKEND_PUBLIC_FAULT_RELEASE"
        " point=%" PRIu32
        " reached=%" PRIu32
        " hit=%" PRIu32
        " submitted=%" PRIu32
        " device=%" PRIu32
        " buffers=%" PRIu32
        " memories=%" PRIu32
        " maps=%" PRIu32
        " descriptor_sets=%" PRIu32
        " shaders=%" PRIu32
        " pipelines=%" PRIu32
        " descriptor_pool=%" PRIu32
        " pipeline_layout=%" PRIu32
        " descriptor_layout=%" PRIu32
        " fence=%" PRIu32
        " command_buffer=%" PRIu32
        " command_pool=%" PRIu32
        " host_arrays=%" PRIu32
        " wait_idle=%d"
        " completed=%" PRIu32
        " cleanup_result=%d"
        " simulated=1\n",
        controlled_fault_point,
        fault.reached,
        fault.hit,
        fault.submitted,
        counts.device,
        counts.buffers,
        counts.memories,
        counts.maps,
        counts.descriptor_sets,
        counts.shaders,
        counts.pipelines,
        counts.descriptor_pool,
        counts.pipeline_layout,
        counts.descriptor_layout,
        counts.fence,
        counts.command,
        counts.command_pool,
        counts.host_arrays,
        counts.wait_result,
        counts.completed,
        released
      );

      if (fault.hit != 1u
       || fault.reached != controlled_fault_point
       || counts.completed != 1u
       || released != VK_SUCCESS)
      {
        result = VK_ERROR_INITIALIZATION_FAILED;
      }
    }

    if (execution_kind == BACKEND_VULKAN_EXECUTION_HASH_BATCH)
    {
      fprintf (
        diagnostics,
        "BACKEND_VULKAN_%s_CLOSE"
        " index=%" PRIu32
        " result=%d"
        " released=1"
        " password_generation=0 target_digests=0 scheduled=0\n",
        ((const backend_vulkan_hash_batch_context_t *) execution_context)->name,
        device_index, result
      );
    }
    else if (execution_kind == BACKEND_VULKAN_EXECUTION_MD5)
    {
      fprintf (
        diagnostics,
        "BACKEND_VULKAN_MD5_CLOSE"
        " index=%" PRIu32
        " result=%d"
        " released=1"
        " password_inputs=0 target_digests=0 scheduled=0\n",
        device_index, result
      );
    }
    else if (program->stage_count == 1u)
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
    &backend_vulkan_public_single,
    BACKEND_VULKAN_EXECUTION_PUBLIC_U32, NULL,
    0u, VK_FALSE
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
    &backend_vulkan_public_chain,
    BACKEND_VULKAN_EXECUTION_PUBLIC_U32, NULL,
    0u, VK_FALSE
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
    &backend_vulkan_public_chain4,
    BACKEND_VULKAN_EXECUTION_PUBLIC_U32, NULL,
    0u, VK_FALSE
  );
}

/**
 * Verify the first cryptographic Vulkan stage with public MD5 vectors.
 *
 * Executes four independent one-block MD5 messages through one declared
 * SPIR-V Compute stage using the same dynamic storage-buffer, descriptor,
 * pipeline, command and cleanup ownership path as the public programs.
 * The GPU digest words must match the standard known answers for "", "a",
 * "abc" and "message digest"; unwritten destination words must preserve
 * their host sentinel.
 *
 * This method is information-mode validation only. Its inputs are immutable
 * public test messages and it does not read HASHCAT candidates, target
 * digests, salts, attack modes or scheduler state.
 */
VkResult backend_vulkan_kernel_verify_md5 (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  FILE *diagnostics)
{
  return backend_vulkan_kernel_verify_program (
    backend_ctx, device_index, diagnostics,
    &backend_vulkan_md5_program,
    BACKEND_VULKAN_EXECUTION_MD5, NULL,
    0u, VK_FALSE
  );
}

/**
 * Hash independent byte messages with SHA-1, SHA-224, SHA-256 or BLAKE2s.
 *
 * Uses dedicated SPIR-V 1.0 modules with standard SHA padding or BLAKE2s
 * byte counters/finalization. Validates exact byte spans, packed offsets
 * and output sizes before creating a one-stage heterogeneous-buffer program.
 * The selected algorithm produces 20, 28 or 32 canonical digest bytes.
 *
 * The same dynamic backend_vulkan_program_t executor owns VkDevice,
 * descriptors, pipeline, command submission, fence and cleanup for all three
 * algorithms. Message bytes and digest storage remain caller-owned; no
 * HASHCAT candidate generation, target comparison or scheduling occurs.
 */
VkResult backend_vulkan_hash_batch (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  backend_vulkan_hash_algorithm_t algorithm,
  const backend_vulkan_hash_message_t *messages,
  uint32_t message_count,
  uint8_t *digests,
  FILE *diagnostics)
{
  if (backend_ctx == NULL
   || backend_ctx->vulkan_catalog == NULL
   || device_index >= backend_ctx->vulkan_devices_cnt
   || messages == NULL
   || message_count == 0u
   || message_count > INT32_MAX / 8u
   || digests == NULL
   || diagnostics == NULL)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  const uint32_t *spirv = NULL;
  uint32_t spirv_words = 0u;
  uint32_t digest_bytes = 0u;
  VkBool32 little_endian = VK_FALSE;
  const char *algorithm_name = NULL;
  const char *stage_name = NULL;

  switch (algorithm)
  {
    case BACKEND_VULKAN_HASH_SHA1:
      spirv = backend_vulkan_sha1_spirv;
      spirv_words =
        sizeof (backend_vulkan_sha1_spirv) / sizeof (uint32_t);
      digest_bytes = 20u;
      algorithm_name = "SHA1";
      stage_name = "sha1_byte_batch";
      break;

    case BACKEND_VULKAN_HASH_SHA224:
      spirv = backend_vulkan_sha224_spirv;
      spirv_words =
        sizeof (backend_vulkan_sha224_spirv) / sizeof (uint32_t);
      digest_bytes = 28u;
      algorithm_name = "SHA224";
      stage_name = "sha224_byte_batch";
      break;

    case BACKEND_VULKAN_HASH_SHA256:
      spirv = backend_vulkan_sha256_spirv;
      spirv_words =
        sizeof (backend_vulkan_sha256_spirv) / sizeof (uint32_t);
      digest_bytes = BACKEND_VULKAN_SHA256_DIGEST_BYTES;
      algorithm_name = "SHA256";
      stage_name = "sha256_byte_batch";
      break;

    case BACKEND_VULKAN_HASH_BLAKE2S:
      spirv = backend_vulkan_blake2s_spirv;
      spirv_words =
        sizeof (backend_vulkan_blake2s_spirv) / sizeof (uint32_t);
      digest_bytes = 32u;
      little_endian = VK_TRUE;
      algorithm_name = "BLAKE2S";
      stage_name = "blake2s_byte_batch";
      break;

    default:
      return VK_ERROR_INITIALIZATION_FAILED;
  }

  uint64_t payload_bytes = 0u;

  for (uint32_t i = 0u; i < message_count; i++)
  {
    if ((messages[i].size != 0u && messages[i].data == NULL)
     || messages[i].size > UINT32_MAX - 72u)
    {
      return VK_ERROR_INITIALIZATION_FAILED;
    }

    payload_bytes += messages[i].size;
  }

  const uint64_t metadata_bytes =
    (uint64_t) message_count * BACKEND_VULKAN_HASH_METADATA_BYTES;

  if (metadata_bytes > UINT32_MAX
   || payload_bytes > UINT32_MAX
   || metadata_bytes + payload_bytes > UINT32_MAX - 3u)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  const uint64_t input_bytes =
    (metadata_bytes + payload_bytes + 3u) & ~UINT64_C (3);

  const uint64_t output_bytes =
    (uint64_t) message_count * digest_bytes;

  if (input_bytes > SIZE_MAX || output_bytes > SIZE_MAX)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  const uint32_t group_count =
    (message_count / BACKEND_VULKAN_HASH_LOCAL_SIZE)
    + ((message_count % BACKEND_VULKAN_HASH_LOCAL_SIZE) != 0u);

  backend_vulkan_program_stage_t stage =
  {
    .name = stage_name,
    .spirv = spirv,
    .spirv_words = spirv_words,
    .source_buffer_index = 0u,
    .destination_buffer_index = 1u,
    .local_size =
    {
      BACKEND_VULKAN_HASH_LOCAL_SIZE, 1u, 1u
    },
    .group_count = { group_count, 1u, 1u }
  };

  const VkDeviceSize buffer_sizes[2] =
  {
    (VkDeviceSize) input_bytes,
    (VkDeviceSize) output_bytes
  };

  const backend_vulkan_program_t program =
  {
    .stages = &stage,
    .stage_count = 1u,
    .buffer_count = 2u,
    .buffer_bytes = 0u,
    .buffer_sizes = buffer_sizes
  };

  backend_vulkan_hash_batch_context_t context =
  {
    .messages = messages,
    .message_count = message_count,
    .digest_bytes = digest_bytes,
    .little_endian = little_endian,
    .name = algorithm_name,
    .digests = digests,
    .input_bytes = (VkDeviceSize) input_bytes,
    .output_bytes = (VkDeviceSize) output_bytes
  };

  return backend_vulkan_kernel_verify_program (
    backend_ctx, device_index, diagnostics,
    &program,
    BACKEND_VULKAN_EXECUTION_HASH_BATCH, &context,
    0u, VK_FALSE
  );
}

/**
 * Compute canonical SHA-256 digests through the common Vulkan hash API.
 *
 * Preserves the public SHA-256 entry point and input type while executing
 * the same dynamically owned SHA-256 SPIR-V program as the generic batch
 * API. Each input produces one complete 32-byte SHA-256 digest. All GPU
 * resources are owned by the common executor and released on return.
 */
VkResult backend_vulkan_sha256_hash_batch (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  const backend_vulkan_sha256_message_t *messages,
  uint32_t message_count,
  uint8_t *digests,
  FILE *diagnostics)
{
  return backend_vulkan_hash_batch (
    backend_ctx, device_index,
    BACKEND_VULKAN_HASH_SHA256,
    messages, message_count, digests, diagnostics
  );
}

/**
 * Verify variable-length SHA-256 execution with four public known answers.
 *
 * Builds two short one-block messages and two messages that require two padded
 * blocks, calls the arbitrary byte-message batch API once and compares all
 * 128 returned digest bytes with independent canonical SHA-256 values.
 * The 56-byte vector exercises the padding boundary where a second block is
 * mandatory; the 100-byte vector exercises ordinary multi-block compression.
 *
 * No HASHCAT candidate source or target digest buffer participates.
 */
VkResult backend_vulkan_sha256_verify_known_answers (
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

  static const uint8_t empty[1] = { 0u };
  static const uint8_t abc[3] = { 'a', 'b', 'c' };

  uint8_t a56[56];
  uint8_t a100[100];

  memset (a56, 'a', sizeof (a56));
  memset (a100, 'a', sizeof (a100));

  const backend_vulkan_sha256_message_t messages[4] =
  {
    { .data = empty, .size = 0u },
    { .data = abc, .size = sizeof (abc) },
    { .data = a56, .size = sizeof (a56) },
    { .data = a100, .size = sizeof (a100) }
  };

  static const uint8_t expected[4][BACKEND_VULKAN_SHA256_DIGEST_BYTES] =
  {
    {
      0xe3u, 0xb0u, 0xc4u, 0x42u, 0x98u, 0xfcu, 0x1cu, 0x14u,
      0x9au, 0xfbu, 0xf4u, 0xc8u, 0x99u, 0x6fu, 0xb9u, 0x24u,
      0x27u, 0xaeu, 0x41u, 0xe4u, 0x64u, 0x9bu, 0x93u, 0x4cu,
      0xa4u, 0x95u, 0x99u, 0x1bu, 0x78u, 0x52u, 0xb8u, 0x55u
    },
    {
      0xbau, 0x78u, 0x16u, 0xbfu, 0x8fu, 0x01u, 0xcfu, 0xeau,
      0x41u, 0x41u, 0x40u, 0xdeu, 0x5du, 0xaeu, 0x22u, 0x23u,
      0xb0u, 0x03u, 0x61u, 0xa3u, 0x96u, 0x17u, 0x7au, 0x9cu,
      0xb4u, 0x10u, 0xffu, 0x61u, 0xf2u, 0x00u, 0x15u, 0xadu
    },
    {
      0xb3u, 0x54u, 0x39u, 0xa4u, 0xacu, 0x6fu, 0x09u, 0x48u,
      0xb6u, 0xd6u, 0xf9u, 0xe3u, 0xc6u, 0xafu, 0x0fu, 0x5fu,
      0x59u, 0x0cu, 0xe2u, 0x0fu, 0x1bu, 0xdeu, 0x70u, 0x90u,
      0xefu, 0x79u, 0x70u, 0x68u, 0x6eu, 0xc6u, 0x73u, 0x8au
    },
    {
      0x28u, 0x16u, 0x59u, 0x78u, 0x88u, 0xe4u, 0xa0u, 0xd3u,
      0xa3u, 0x6bu, 0x82u, 0xb8u, 0x33u, 0x16u, 0xabu, 0x32u,
      0x68u, 0x0eu, 0xb8u, 0xf0u, 0x0fu, 0x8cu, 0xd3u, 0xb9u,
      0x04u, 0xd6u, 0x81u, 0x24u, 0x6du, 0x28u, 0x5au, 0x0eu
    }
  };

  uint8_t digests[sizeof (expected)];

  const VkResult result = backend_vulkan_sha256_hash_batch (
    backend_ctx, device_index, messages, 4u, digests, diagnostics
  );

  if (result != VK_SUCCESS) return result;

  for (uint32_t message_index = 0u; message_index < 4u; message_index++)
  {
    for (uint32_t byte_index = 0u;
         byte_index < BACKEND_VULKAN_SHA256_DIGEST_BYTES;
         byte_index++)
    {
      const uint8_t actual =
        digests[
          (size_t) message_index * BACKEND_VULKAN_SHA256_DIGEST_BYTES
          + byte_index
        ];

      if (actual != expected[message_index][byte_index])
      {
        fprintf (
          diagnostics,
          "BACKEND_VULKAN_SHA256_MISMATCH"
          " message=%" PRIu32
          " byte=%" PRIu32
          " actual=0x%02x"
          " expected=0x%02x\n",
          message_index, byte_index,
          (unsigned int) actual,
          (unsigned int) expected[message_index][byte_index]
        );

        return VK_ERROR_INITIALIZATION_FAILED;
      }
    }
  }

  fprintf (
    diagnostics,
    "BACKEND_VULKAN_SHA256_VERIFY_PASS"
    " messages=4"
    " one_block=2"
    " two_block=2"
    " verified_digest_bytes=128"
    " password_generation=0"
    " target_digests=0"
    " scheduled=0\n"
  );

  return VK_SUCCESS;
}

/**
 * Exercise one controlled ownership checkpoint of a four-stage public program.
 *
 * Requires an active Vulkan backend catalogue and point in the inclusive
 * range 1..37. Each point aborts after exactly one successful acquisition
 * or at a declared command-submission boundary. A real driver function is
 * never replaced. The test destroys all acquired resources, reports their
 * exact ownership counts and returns VK_ERROR_INITIALIZATION_FAILED.
 *
 * Points 1..35 cover host arrays, VkDevice, five storage buffers and their
 * memory/mappings, four descriptor/shader/pipeline stages, command pool,
 * command buffer and fence. Point 36 aborts before vkQueueSubmit; point 37
 * aborts after a real successful vkQueueSubmit but before fence wait.
 * Cleanup then performs a real vkDeviceWaitIdle before destroying the job.
 *
 * No input from HASHCAT candidates, hashes, salts or attack scheduling is
 * accepted. A new working four-stage device can be created under the same
 * live catalogue after this method returns.
 */
VkResult backend_vulkan_kernel_verify_chain4_fault (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  FILE *diagnostics,
  uint32_t controlled_fault_point)
{
  if (controlled_fault_point == 0u
   || controlled_fault_point > 37u)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  return backend_vulkan_kernel_verify_program (
    backend_ctx, device_index, diagnostics,
    &backend_vulkan_public_chain4,
    BACKEND_VULKAN_EXECUTION_PUBLIC_U32, NULL,
    controlled_fault_point, VK_FALSE
  );
}

/**
 * Observe a real unsignaled-fence timeout after four public GPU dispatches.
 *
 * Runs the validated four-stage program on a real Vulkan queue, but submits
 * its command buffer with VK_NULL_HANDLE instead of the owned unsignaled
 * fence. A zero-duration vkWaitForFences call on that fence returns the
 * driver's VK_TIMEOUT status. The normal release path waits for device
 * idle before destroying the resources, without reading incomplete output.
 * No queue or wait driver function is replaced, and no HASHCAT workload
 * or scheduler is accessed.
 *
 * Returns VK_TIMEOUT only when the real fence wait times out and all
 * resources are released successfully. Driver and cleanup errors are
 * propagated as Vulkan results.
 */
VkResult backend_vulkan_kernel_verify_chain4_fence_timeout (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  FILE *diagnostics)
{
  return backend_vulkan_kernel_verify_program (
    backend_ctx, device_index, diagnostics,
    &backend_vulkan_public_chain4,
    BACKEND_VULKAN_EXECUTION_PUBLIC_U32, NULL,
    0u, VK_TRUE
  );
}
