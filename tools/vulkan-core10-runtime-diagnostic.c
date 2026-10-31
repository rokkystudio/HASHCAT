/**
 * Verify a reusable Vulkan 1.0 runtime with public three-stage GPU Compute.
 *
 * The application checks exact shader requirements and physical GPU limits,
 * creates a portable runtime owner, and records one command buffer containing
 * two transfers and three synthetic integer Compute stages.
 *
 * Multiple submissions change public input data, flush noncoherent CPU
 * writes, wait for the GPU fence, invalidate noncoherent readback and compare
 * every reported intermediate/final integer against independent CPU formulas.
 *
 * Deterministic resource-acquisition faults verify that the runtime releases
 * all existing Vulkan resources. This file does not load a hashcat backend,
 * a credential, a password or any cryptographic GPU shader.
 */

#include "vulkan-auto-config.h"
#include "vulkan-core10-runtime.h"
#include "vulkan-core11-policy.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ELEMENT_COUNT 2048u
#define LOCAL_SIZE 32u
#define OUTPUT_WORDS 4u
#define INPUT_BYTES (ELEMENT_COUNT * sizeof (uint32_t))
#define OUTPUT_BYTES (INPUT_BYTES * OUTPUT_WORDS)

/**
 * Read one public SPIR-V 1.0 shader module from an exact supplied path.
 *
 * Accepts at most 1 MiB of complete, 32-bit-aligned shader words and
 * explicitly requires the SPIR-V 1.0 header. The caller owns the data.
 */
static uint32_t *read_shader (
  const char *path,
  size_t *bytes)
{
  FILE *file = fopen (path, "rb");

  if (file == NULL)
  {
    perror ("RUNTIME_SHADER_OPEN_FAILED");

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
    fprintf (
      stderr, "RUNTIME_SHADER_INVALID_SIZE bytes=%ld\n", length
    );

    fclose (file);

    return NULL;
  }

  uint32_t *code = (uint32_t *) malloc ((size_t) length);

  if (code == NULL)
  {
    fclose (file);

    return NULL;
  }

  const size_t read_bytes = fread (code, 1u, (size_t) length, file);

  fclose (file);

  if (read_bytes != (size_t) length
   || code[0] != UINT32_C (0x07230203)
   || code[1] != UINT32_C (0x00010000))
  {
    fprintf (
      stderr, "RUNTIME_SHADER_INVALID_SPIRV expected=1.0\n"
    );

    free (code);

    return NULL;
  }

  *bytes = (size_t) length;

  return code;
}

/**
 * Convert an unsigned decimal CLI token into an exact 32-bit integer.
 *
 * Signs, whitespace, nondecimal bytes and values above UINT32_MAX are
 * rejected. Required argument ranges are validated by the caller.
 */
static VkBool32 parse_decimal (
  const char *text,
  uint32_t *value)
{
  if (text == NULL || value == NULL || *text == '\0')
  {
    return VK_FALSE;
  }

  uint32_t result = 0u;

  for (const char *digit = text; *digit != '\0'; digit++)
  {
    if (*digit < '0' || *digit > '9') return VK_FALSE;

    const uint32_t number = (uint32_t) (*digit - '0');

    if (result > (UINT32_MAX - number) / 10u)
    {
      return VK_FALSE;
    }

    result = result * 10u + number;
  }

  *value = result;

  return VK_TRUE;
}

/**
 * Generate one deterministic public integer word for one GPU submission.
 *
 * Submission zero matches the independent original three-stage reference.
 * Later generations XOR a submission-dependent 32-bit constant into input.
 */
static uint32_t synthetic_input (
  uint32_t index,
  uint32_t submission)
{
  const uint32_t value =
    (index * UINT32_C (0x9e3779b9))
    ^ (index << 15u)
    ^ (index >> 3u)
    ^ UINT32_C (0xa5a5a5a5);

  return value ^ (submission * UINT32_C (0x9e3779b9));
}

/**
 * Calculate the first synthetic shader stage from one public input word.
 */
static uint32_t stage_a_reference (
  uint32_t index,
  uint32_t submission)
{
  const uint32_t value = synthetic_input (index, submission);
  const uint32_t affine =
    value * UINT32_C (1664525) + UINT32_C (1013904223);
  const uint32_t rotated = (value << 7u) | (value >> 25u);

  return (affine ^ rotated) + index * UINT32_C (2654435761);
}

/**
 * Calculate stage B using the seven-lanes-forward neighbor from stage A.
 *
 * All values wrap modulo 2^32. Neighbor lookup stays in the current group
 * of 32 invocations, matching the shader's shared-memory barrier semantics.
 */
static uint32_t stage_b_reference (
  uint32_t index,
  uint32_t submission)
{
  const uint32_t lane = index & (LOCAL_SIZE - 1u);
  const uint32_t group = index & ~(LOCAL_SIZE - 1u);
  const uint32_t neighbor_index =
    group + ((lane + 7u) & (LOCAL_SIZE - 1u));
  const uint32_t value = stage_a_reference (index, submission);
  const uint32_t neighbor = stage_a_reference (
    neighbor_index, submission
  );
  const uint32_t rotated = (value << 11u) | (value >> 21u);

  return (value + neighbor)
       ^ rotated
       ^ UINT32_C (0x6c8e9cf5);
}

/**
 * Append one little-endian 32-bit word to a FNV-1a 64-bit checksum.
 */
static uint64_t checksum_word (
  uint64_t digest,
  uint32_t word)
{
  for (uint32_t byte = 0u; byte < 4u; byte++)
  {
    digest ^= (word >> (byte * 8u)) & UINT32_C (0xff);
    digest *= UINT64_C (1099511628211);
  }

  return digest;
}

/**
 * Compare a completed GPU readback to independently evaluated shader stages.
 *
 * The first two words of each GPU uvec4 expose the actual stage-A/B output.
 * Verifies all 12,288 logical stage words and reports independent per-stage
 * FNV-1a digests. Every new submission uses a distinct reference generation.
 */
static int verify_submission (
  const uint32_t *output,
  uint32_t submission)
{
  uint64_t digest_a = UINT64_C (14695981039346656037);
  uint64_t digest_b = UINT64_C (14695981039346656037);
  uint64_t digest_output = UINT64_C (14695981039346656037);

  for (uint32_t index = 0u; index < ELEMENT_COUNT; index++)
  {
    const uint32_t a = stage_a_reference (index, submission);
    const uint32_t b = stage_b_reference (index, submission);
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
      const uint32_t actual = output[index * OUTPUT_WORDS + word];

      if (actual != expected[word])
      {
        fprintf (
          stderr,
          "RUNTIME_OUTPUT_MISMATCH submission=%" PRIu32
          " index=%" PRIu32 " word=%" PRIu32
          " expected=%08" PRIx32 " actual=%08" PRIx32 "\n",
          submission, index, word, expected[word], actual
        );

        return 1;
      }

      digest_output = checksum_word (digest_output, actual);
    }

    digest_a = checksum_word (
      digest_a, output[index * OUTPUT_WORDS]
    );
    digest_b = checksum_word (
      digest_b, output[index * OUTPUT_WORDS + 1u]
    );
  }

  fprintf (
    stdout,
    "RUNTIME_PASS submission=%" PRIu32
    " verified_words=%u stageA=%016" PRIx64
    " stageB=%016" PRIx64
    " output=%016" PRIx64 "\n",
    submission, ELEMENT_COUNT * (OUTPUT_WORDS + 2u),
    digest_a, digest_b, digest_output
  );

  return 0;
}

/**
 * Record one Vulkan 1.0 command buffer with three public Compute stages.
 *
 * The command buffer contains host→transfer, transfer→Compute,
 * Compute→Compute, Compute→transfer and transfer→host barriers around
 * exactly two copies. It has no ONE_TIME_SUBMIT flag and is reused after
 * each completed GPU fence without recording it again.
 */
static VkResult record_pipeline (
  vulkan_core10_runtime_t *runtime)
{
  VkResult result = vulkan_core10_runtime_begin_commands (runtime);

  if (result != VK_SUCCESS) return result;

  const VkCommandBuffer command = runtime->command;
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

  const VkBufferCopy upload =
  {
    .srcOffset = 0u,
    .dstOffset = 0u,
    .size = INPUT_BYTES
  };

  vkCmdCopyBuffer (
    command,
    runtime->buffers[4], runtime->buffers[0],
    1u, &upload
  );

  const VkMemoryBarrier upload_to_compute =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT
  };

  vkCmdPipelineBarrier (
    command,
    VK_PIPELINE_STAGE_TRANSFER_BIT,
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
    0u, 1u, &upload_to_compute, 0u, NULL, 0u, NULL
  );

  vkCmdBindDescriptorSets (
    command,
    VK_PIPELINE_BIND_POINT_COMPUTE,
    runtime->pipeline_layout,
    0u, 1u, &runtime->descriptor_set, 0u, NULL
  );

  const VkMemoryBarrier stage_barrier =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT
  };

  for (uint32_t stage = 0u;
       stage < VULKAN_CORE10_RUNTIME_STAGES; stage++)
  {
    vkCmdBindPipeline (
      command, VK_PIPELINE_BIND_POINT_COMPUTE,
      runtime->pipelines[stage]
    );

    vkCmdDispatch (
      command, ELEMENT_COUNT / LOCAL_SIZE, 1u, 1u
    );

    if (stage + 1u < VULKAN_CORE10_RUNTIME_STAGES)
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

  const VkBufferCopy download =
  {
    .srcOffset = 0u,
    .dstOffset = 0u,
    .size = OUTPUT_BYTES
  };

  vkCmdCopyBuffer (
    command,
    runtime->buffers[3], runtime->buffers[5],
    1u, &download
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

  return vulkan_core10_runtime_end_commands (runtime);
}

/**
 * Execute a real GPU synthetic workload or a selected cleanup fault test.
 *
 * The runtime creates all six memory resources, descriptors and commands.
 * On normal execution, changes the public input before each of 2..16
 * submissions, flushes CPU staging, waits for a fence and independently
 * verifies the readback after optional noncoherent invalidate.
 *
 * fault_after deliberately fails on a one-based successful Vulkan resource
 * acquisition, producing an explicit error with complete runtime teardown.
 */
static int run_device (
  const vulkan_auto_device_t *selected,
  const vulkan_core10_runtime_shader_t
    shaders[VULKAN_CORE10_RUNTIME_STAGES],
  uint32_t submissions,
  VkBool32 force_noncoherent,
  uint32_t fault_after)
{
  static const char *labels[VULKAN_CORE10_RUNTIME_BUFFERS] =
  {
    "gpu-input", "stage-a", "stage-b", "gpu-output",
    "staging-upload", "staging-download"
  };
  static const VkDeviceSize sizes[VULKAN_CORE10_RUNTIME_BUFFERS] =
  {
    INPUT_BYTES, INPUT_BYTES, INPUT_BYTES,
    OUTPUT_BYTES, INPUT_BYTES, OUTPUT_BYTES
  };
  static const VkBufferUsageFlags usages[VULKAN_CORE10_RUNTIME_BUFFERS] =
  {
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    VK_BUFFER_USAGE_TRANSFER_DST_BIT
  };

  vulkan_core10_runtime_buffer_spec_t
    buffers[VULKAN_CORE10_RUNTIME_BUFFERS] = { 0 };

  for (uint32_t index = 0u; index < VULKAN_CORE10_RUNTIME_BUFFERS; index++)
  {
    buffers[index] = (vulkan_core10_runtime_buffer_spec_t)
    {
      .label = labels[index],
      .size = sizes[index],
      .usage = usages[index],
      .role = index < 4u
        ? VULKAN_HEAP_BUFFER_GPU_STORAGE
        : VULKAN_HEAP_BUFFER_HOST_TRANSFER,
      .forbidden_flags =
        VK_MEMORY_PROPERTY_PROTECTED_BIT
        | VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD,
      .map = index >= 4u ? VK_TRUE : VK_FALSE
    };

    if (force_noncoherent == VK_TRUE && index >= 4u)
    {
      buffers[index].forbidden_flags |=
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    }
  }

  vulkan_core10_runtime_t runtime = { 0 };
  VkResult result = vulkan_core10_runtime_create (
    &runtime, selected, buffers, shaders,
    fault_after, stdout
  );

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr,
      "RUNTIME_CREATE_FAILED result=%d fault_after=%" PRIu32 "\n",
      result, fault_after
    );

    return 1;
  }

  int status = 1;

  if (fault_after != 0u)
  {
    fprintf (
      stderr, "RUNTIME_REJECT_UNREACHED_FAULT acquisition=%" PRIu32 "\n",
      fault_after
    );

    goto cleanup;
  }

  result = record_pipeline (&runtime);

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr, "RUNTIME_RECORD_FAILED result=%d\n", result
    );

    goto cleanup;
  }

  uint32_t *upload = (uint32_t *) runtime.mapped[4];
  const uint32_t *download = (const uint32_t *) runtime.mapped[5];

  fprintf (
    stdout,
    "RUNTIME_STAGING_REQUEST mode=%s"
    " upload_type=%" PRIu32
    " download_type=%" PRIu32
    " upload_coherent=%u download_coherent=%u\n",
    force_noncoherent == VK_TRUE ? "noncoherent" : "auto",
    runtime.memory_plan.memory_type_indices[4],
    runtime.memory_plan.memory_type_indices[5],
    runtime.memory_plan.coherent[4],
    runtime.memory_plan.coherent[5]
  );

  for (uint32_t submission = 0u; submission < submissions; submission++)
  {
    for (uint32_t index = 0u; index < ELEMENT_COUNT; index++)
    {
      upload[index] = synthetic_input (index, submission);
    }

    result = vulkan_core10_runtime_sync_staging (
      &runtime, 4u, VK_FALSE
    );

    if (result != VK_SUCCESS)
    {
      fprintf (
        stderr, "RUNTIME_FLUSH_FAILED result=%d\n", result
      );

      goto cleanup;
    }

    result = vulkan_core10_runtime_submit_and_wait (&runtime);

    if (result != VK_SUCCESS)
    {
      fprintf (
        stderr, "RUNTIME_SUBMISSION_FAILED result=%d\n", result
      );

      goto cleanup;
    }

    result = vulkan_core10_runtime_sync_staging (
      &runtime, 5u, VK_TRUE
    );

    if (result != VK_SUCCESS)
    {
      fprintf (
        stderr, "RUNTIME_INVALIDATE_FAILED result=%d\n", result
      );

      goto cleanup;
    }

    if (verify_submission (download, submission) != 0)
    {
      goto cleanup;
    }
  }

  status = 0;

cleanup:
  result = vulkan_core10_runtime_destroy (&runtime);

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr, "RUNTIME_DEVICE_CLEANUP_FAILED result=%d\n", result
    );

    status = 1;
  }

  return status;
}

/**
 * Select a compatible Vulkan 1.0 physical device and run the synthetic test.
 *
 * Supports optional explicit Vulkan 1.1, 2..16 sequential GPU submissions,
 * forced noncoherent staging, and one deterministic acquisition-failure point
 * from 1 through 28. CLI errors and unreadable shader files return 2.
 *
 * Physical device suitability depends only on version, Compute queue,
 * exact descriptor/dispatch/shared-memory limits and real buffer memory
 * requirements. Each application owns and destroys its VkInstance.
 */
int main (int argc, char **argv)
{
  if (argc < 4)
  {
    fprintf (
      stderr,
      "Usage: %s <stage-a.spv> <stage-b.spv> <stage-c.spv>"
      " [--minimum <1.0|1.1>] [--submissions <2..16>]"
      " [--require-noncoherent-staging] [--fail-after <1..28>]\n",
      argv[0]
    );

    return 2;
  }

  uint32_t submissions = 4u;
  uint32_t fault_after = 0u;
  uint32_t required_api = VK_API_VERSION_1_0;
  VkBool32 explicit_version = VK_FALSE;
  VkBool32 explicit_submissions = VK_FALSE;
  VkBool32 explicit_fault = VK_FALSE;
  VkBool32 force_noncoherent = VK_FALSE;

  for (int argument = 4; argument < argc; argument++)
  {
    if (strcmp (argv[argument], "--minimum") == 0
     && explicit_version == VK_FALSE && argument + 1 < argc)
    {
      explicit_version = VK_TRUE;
      argument++;

      if (strcmp (argv[argument], "1.0") == 0)
      {
        required_api = VK_API_VERSION_1_0;
      }
      else if (strcmp (argv[argument], "1.1") == 0)
      {
        required_api = VK_API_VERSION_1_1;
      }
      else
      {
        fprintf (stderr, "RUNTIME_REJECT_API_REQUIREMENT\n");

        return 2;
      }
    }
    else if (strcmp (argv[argument], "--submissions") == 0
      && explicit_submissions == VK_FALSE && argument + 1 < argc)
    {
      explicit_submissions = VK_TRUE;

      if (parse_decimal (argv[++argument], &submissions) == VK_FALSE
       || submissions < 2u || submissions > 16u)
      {
        fprintf (stderr, "RUNTIME_REJECT_SUBMISSIONS allowed=2..16\n");

        return 2;
      }
    }
    else if (strcmp (argv[argument], "--fail-after") == 0
      && explicit_fault == VK_FALSE && argument + 1 < argc)
    {
      explicit_fault = VK_TRUE;

      if (parse_decimal (argv[++argument], &fault_after) == VK_FALSE
       || fault_after == 0u || fault_after > 28u)
      {
        fprintf (stderr, "RUNTIME_REJECT_FAULT allowed=1..28\n");

        return 2;
      }
    }
    else if (strcmp (
        argv[argument], "--require-noncoherent-staging"
      ) == 0)
    {
      if (force_noncoherent == VK_TRUE) return 2;

      force_noncoherent = VK_TRUE;
    }
    else
    {
      fprintf (
        stderr, "RUNTIME_REJECT_ARGUMENT argument=%s\n",
        argv[argument]
      );

      return 2;
    }
  }

  vulkan_core10_runtime_shader_t
    shaders[VULKAN_CORE10_RUNTIME_STAGES] = { 0 };
  int status = 1;

  for (uint32_t index = 0u;
       index < VULKAN_CORE10_RUNTIME_STAGES; index++)
  {
    size_t bytes = 0u;
    uint32_t *code = read_shader (argv[index + 1u], &bytes);

    if (code == NULL)
    {
      status = 2;

      goto cleanup_shaders;
    }

    shaders[index].words = code;
    shaders[index].bytes = bytes;
  }

  uint32_t loader_version = 0u;
  VkResult result = vulkan_core11_query_loader_version (
    vkGetInstanceProcAddr, &loader_version, stdout
  );

  if (result != VK_SUCCESS) goto cleanup_shaders;

  uint32_t instance_version = 0u;

  result = vulkan_core11_select_instance_version (
    loader_version, required_api, &instance_version, stdout
  );

  if (result != VK_SUCCESS) goto cleanup_shaders;

  const VkApplicationInfo application =
  {
    .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
    .pApplicationName = "Vulkan 1.0 Reusable Synthetic Runtime",
    .applicationVersion = VK_MAKE_VERSION (1, 0, 0),
    .pEngineName = "None",
    .engineVersion = VK_MAKE_VERSION (1, 0, 0),
    .apiVersion = instance_version
  };
  const VkInstanceCreateInfo create_instance =
  {
    .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
    .pApplicationInfo = &application
  };
  VkInstance instance = VK_NULL_HANDLE;

  result = vkCreateInstance (
    &create_instance, NULL, &instance
  );

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr, "RUNTIME_INSTANCE_FAILED result=%d\n", result
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
      "RUNTIME_DEVICE_ENUMERATION_FAILED count=%" PRIu32
      " result=%d\n", device_count, result
    );

    goto cleanup_instance;
  }

  VkPhysicalDevice *physical = (VkPhysicalDevice *) calloc (
    device_count, sizeof (*physical)
  );

  if (physical == NULL) goto cleanup_instance;

  result = vkEnumeratePhysicalDevices (
    instance, &device_count, physical
  );

  if (result != VK_SUCCESS) goto cleanup_physical;

  const vulkan_auto_compute_requirements_t workload =
  {
    .local_size = { LOCAL_SIZE, 1u, 1u },
    .group_count = { ELEMENT_COUNT / LOCAL_SIZE, 1u, 1u },
    .storage_buffer_count = 4u,
    .largest_storage_buffer_bytes = OUTPUT_BYTES,
    .shared_memory_bytes = LOCAL_SIZE * sizeof (uint32_t)
  };

  for (uint32_t index = 0u; index < device_count; index++)
  {
    VkPhysicalDeviceProperties properties = { 0 };

    vkGetPhysicalDeviceProperties (
      physical[index], &properties
    );

    uint32_t effective = 0u;
    result = vulkan_core11_select_device_version (
      properties.apiVersion,
      instance_version, required_api, &effective, stdout
    );

    if (result != VK_SUCCESS)
    {
      fprintf (
        stdout,
        "RUNTIME_SKIP_DEVICE index=%" PRIu32
        " reason=api_version\n", index
      );

      continue;
    }

    vulkan_auto_device_t selected = { 0 };
    result = vulkan_auto_check_device (
      physical[index], index, &workload,
      &selected, stdout
    );

    if (result != VK_SUCCESS)
    {
      fprintf (
        stdout,
        "RUNTIME_SKIP_DEVICE index=%" PRIu32
        " reason=workload_limits_or_queue\n", index
      );

      continue;
    }

    fprintf (
      stdout,
      "RUNTIME_SELECTED_DEVICE index=%" PRIu32
      " effective_api=%" PRIu32 ".%" PRIu32
      " requested_api=%" PRIu32 ".%" PRIu32
      " workgroup=32 groups=64 storage_bindings=4 shared_bytes=128"
      " features=0 extensions=0\n",
      index,
      VK_API_VERSION_MAJOR (effective),
      VK_API_VERSION_MINOR (effective),
      VK_API_VERSION_MAJOR (required_api),
      VK_API_VERSION_MINOR (required_api)
    );

    status = run_device (
      &selected, shaders, submissions,
      force_noncoherent, fault_after
    );

    if (status == 0 || fault_after != 0u) break;

    fprintf (
      stdout,
      "RUNTIME_SKIP_DEVICE index=%" PRIu32
      " reason=resource_or_dispatch_failure\n", index
    );
  }

  if (status != 0)
  {
    fprintf (
      stderr, "RUNTIME_NO_COMPATIBLE_EXECUTION\n"
    );
  }

cleanup_physical:
  free (physical);

cleanup_instance:
  vkDestroyInstance (instance, NULL);

cleanup_shaders:
  for (uint32_t index = 0u;
       index < VULKAN_CORE10_RUNTIME_STAGES; index++)
  {
    free ((void *) shaders[index].words);
  }

  return status;
}
