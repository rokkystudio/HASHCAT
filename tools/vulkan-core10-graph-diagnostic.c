/**
 * Validate variable Vulkan 1.0 Compute graphs with public integer shaders.
 *
 * A one-stage graph requires four buffers, two storage descriptors and one
 * Compute pipeline. A two-stage graph requires five buffers, three storage
 * descriptors and two pipelines. A three-stage graph requires six buffers,
 * four descriptors and three pipelines. Every graph has two CPU staging
 * buffers and the same reusable command buffer synchronization contract.
 *
 * The application independently calculates every output word; no hashcat
 * module, credential or cryptographic GPU operation is involved.
 */

#include "vulkan-auto-config.h"
#include "vulkan-core10-graph.h"
#include "vulkan-core11-policy.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GRAPH_ELEMENTS 2048u
#define GRAPH_LOCAL_SIZE 32u
#define GRAPH_INPUT_BYTES (GRAPH_ELEMENTS * sizeof (uint32_t))
#define GRAPH_FINAL_BYTES (GRAPH_INPUT_BYTES * 4u)
#define GRAPH_MAX_STAGES 3u
#define GRAPH_MAX_BUFFERS (GRAPH_MAX_STAGES + 3u)

/**
 * Read a complete SPIR-V 1.0 shader from one explicitly supplied path.
 */
static uint32_t *read_spirv (
  const char *path,
  size_t *bytes)
{
  FILE *file = fopen (path, "rb");

  if (file == NULL)
  {
    perror ("GRAPH_SHADER_OPEN_FAILED");

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
      stderr, "GRAPH_REJECT_SHADER_SIZE bytes=%ld\n", length
    );

    fclose (file);

    return NULL;
  }

  uint32_t *words = (uint32_t *) malloc ((size_t) length);

  if (words == NULL)
  {
    fclose (file);

    return NULL;
  }

  const size_t read_size = fread (words, 1u, (size_t) length, file);

  fclose (file);

  if (read_size != (size_t) length
   || words[0] != UINT32_C (0x07230203)
   || words[1] != UINT32_C (0x00010000))
  {
    fprintf (
      stderr, "GRAPH_REJECT_SHADER_VERSION required=1.0\n"
    );

    free (words);

    return NULL;
  }

  *bytes = (size_t) length;

  return words;
}

/**
 * Parse an unsigned decimal 32-bit command-line number without fallback.
 */
static VkBool32 decimal_arg (
  const char *text,
  uint32_t *number)
{
  if (text == NULL || number == NULL || *text == '\0')
  {
    return VK_FALSE;
  }

  uint32_t value = 0u;

  for (const char *digit = text; *digit != '\0'; digit++)
  {
    if (*digit < '0' || *digit > '9') return VK_FALSE;

    const uint32_t next = (uint32_t) (*digit - '0');

    if (value > (UINT32_MAX - next) / 10u)
    {
      return VK_FALSE;
    }

    value = value * 10u + next;
  }

  *number = value;

  return VK_TRUE;
}

/**
 * Generate one deterministic public 32-bit input generation.
 */
static uint32_t public_input (
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
 * Evaluate stage A with unsigned 32-bit arithmetic.
 */
static uint32_t reference_a (
  uint32_t index,
  uint32_t submission)
{
  const uint32_t input = public_input (index, submission);
  const uint32_t affine =
    input * UINT32_C (1664525) + UINT32_C (1013904223);
  const uint32_t rotated = (input << 7u) | (input >> 25u);

  return (affine ^ rotated) + index * UINT32_C (2654435761);
}

/**
 * Evaluate shared-memory stage B with an exact wrap within 32 GPU lanes.
 */
static uint32_t reference_b (
  uint32_t index,
  uint32_t submission)
{
  const uint32_t neighbor =
    (index & ~(GRAPH_LOCAL_SIZE - 1u))
    + ((index + 7u) & (GRAPH_LOCAL_SIZE - 1u));
  const uint32_t a = reference_a (index, submission);
  const uint32_t n = reference_a (neighbor, submission);
  const uint32_t rotated = (a << 11u) | (a >> 21u);

  return (a + n) ^ rotated ^ UINT32_C (0x6c8e9cf5);
}

/**
 * Append one little-endian 32-bit integer to FNV-1a64.
 */
static uint64_t hash_word (
  uint64_t hash,
  uint32_t word)
{
  for (uint32_t index = 0u; index < 4u; index++)
  {
    hash ^= (word >> (index * 8u)) & UINT32_C (0xff);
    hash *= UINT64_C (1099511628211);
  }

  return hash;
}

/**
 * Validate all GPU-written words of an explicitly selected stage graph.
 *
 * The one-stage output contains stage A, the two-stage output contains
 * stage B, and the three-stage output contains four words from stage C
 * including the actual A/B values. Intermediate GPU buffers are not
 * directly downloaded in the single- or two-stage layouts.
 */
static int verify_graph (
  const uint32_t *output,
  uint32_t stage_count,
  uint32_t submission)
{
  uint64_t digest_a = UINT64_C (14695981039346656037);
  uint64_t digest_b = UINT64_C (14695981039346656037);
  uint64_t digest_out = UINT64_C (14695981039346656037);
  const uint32_t words_per_element = stage_count == 3u ? 4u : 1u;

  for (uint32_t index = 0u; index < GRAPH_ELEMENTS; index++)
  {
    const uint32_t a = reference_a (index, submission);
    const uint32_t b =
      stage_count >= 2u ? reference_b (index, submission) : 0u;
    const uint32_t rotated = (b << 3u) | (b >> 29u);
    const uint32_t expected[4] =
    {
      stage_count == 1u ? a : (stage_count == 2u ? b : a),
      stage_count == 3u ? b : 0u,
      stage_count == 3u ? (a ^ b) : 0u,
      stage_count == 3u
        ? (a * UINT32_C (2246822519)
         + b * UINT32_C (3266489917)) ^ rotated : 0u
    };

    for (uint32_t word = 0u; word < words_per_element; word++)
    {
      const uint32_t actual =
        output[index * words_per_element + word];

      if (actual != expected[word])
      {
        fprintf (
          stderr,
          "GRAPH_OUTPUT_MISMATCH stages=%" PRIu32
          " submission=%" PRIu32 " index=%" PRIu32
          " word=%" PRIu32 " expected=%08" PRIx32
          " actual=%08" PRIx32 "\n",
          stage_count, submission, index, word,
          expected[word], actual
        );

        return 1;
      }

      digest_out = hash_word (digest_out, actual);
    }

    if (stage_count == 1u)
    {
      digest_a = hash_word (digest_a, output[index]);
    }
    else if (stage_count == 2u)
    {
      digest_b = hash_word (digest_b, output[index]);
    }
    else
    {
      digest_a = hash_word (digest_a, output[index * 4u]);
      digest_b = hash_word (digest_b, output[index * 4u + 1u]);
    }
  }

  const uint32_t checked_words =
    GRAPH_ELEMENTS * (stage_count == 3u ? 6u : 1u);

  fprintf (
    stdout,
    "GRAPH_PASS stages=%" PRIu32
    " submission=%" PRIu32 " verified_words=%" PRIu32
    " stageA=%016" PRIx64 " stageB=%016" PRIx64
    " output=%016" PRIx64 "\n",
    stage_count, submission, checked_words,
    digest_a, digest_b, digest_out
  );

  return 0;
}

/**
 * Record transfer/Compute/transfer barriers for any of three known shaders.
 *
 * This code only contains the synthetic workload's command structure.
 * All allocated buffer, descriptor, pipeline and fence handles belong to
 * the independently reusable graph runtime.
 */
static VkResult record_commands (
  vulkan_core10_graph_t *graph)
{
  VkResult result = vulkan_core10_graph_begin (graph);

  if (result != VK_SUCCESS) return result;

  const uint32_t output = graph->descriptor_count - 1u;
  const uint32_t upload = graph->buffer_count - 2u;
  const uint32_t download = graph->buffer_count - 1u;
  const VkDeviceSize output_bytes = graph->buffers[output].logical_size;

  const VkMemoryBarrier host_to_transfer =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT
  };

  vkCmdPipelineBarrier (
    graph->command,
    VK_PIPELINE_STAGE_HOST_BIT,
    VK_PIPELINE_STAGE_TRANSFER_BIT,
    0u, 1u, &host_to_transfer, 0u, NULL, 0u, NULL
  );

  const VkBufferCopy upload_copy =
  {
    .srcOffset = 0u,
    .dstOffset = 0u,
    .size = GRAPH_INPUT_BYTES
  };

  vkCmdCopyBuffer (
    graph->command,
    graph->buffers[upload].buffer,
    graph->buffers[0].buffer,
    1u, &upload_copy
  );

  const VkMemoryBarrier transfer_to_compute =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT
  };

  vkCmdPipelineBarrier (
    graph->command,
    VK_PIPELINE_STAGE_TRANSFER_BIT,
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
    0u, 1u, &transfer_to_compute, 0u, NULL, 0u, NULL
  );

  vkCmdBindDescriptorSets (
    graph->command,
    VK_PIPELINE_BIND_POINT_COMPUTE,
    graph->pipeline_layout,
    0u, 1u, &graph->descriptor_set, 0u, NULL
  );

  const VkMemoryBarrier stage_barrier =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT
  };

  for (uint32_t stage = 0u; stage < graph->stage_count; stage++)
  {
    vkCmdBindPipeline (
      graph->command, VK_PIPELINE_BIND_POINT_COMPUTE,
      graph->pipelines[stage]
    );

    vkCmdDispatch (
      graph->command,
      GRAPH_ELEMENTS / GRAPH_LOCAL_SIZE, 1u, 1u
    );

    if (stage + 1u < graph->stage_count)
    {
      vkCmdPipelineBarrier (
        graph->command,
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
    graph->command,
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
    VK_PIPELINE_STAGE_TRANSFER_BIT,
    0u, 1u, &compute_to_transfer, 0u, NULL, 0u, NULL
  );

  const VkBufferCopy download_copy =
  {
    .srcOffset = 0u,
    .dstOffset = 0u,
    .size = output_bytes
  };

  vkCmdCopyBuffer (
    graph->command,
    graph->buffers[output].buffer,
    graph->buffers[download].buffer,
    1u, &download_copy
  );

  const VkMemoryBarrier transfer_to_host =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_HOST_READ_BIT
  };

  vkCmdPipelineBarrier (
    graph->command,
    VK_PIPELINE_STAGE_TRANSFER_BIT,
    VK_PIPELINE_STAGE_HOST_BIT,
    0u, 1u, &transfer_to_host, 0u, NULL, 0u, NULL
  );

  return vulkan_core10_graph_end (graph);
}

/**
 * Create and execute one explicitly selected synthetic Vulkan resource graph.
 *
 * Exactly stage_count+1 GPU storage buffers and two mapped CPU staging
 * buffers are declared. The generic runtime receives these counts and
 * explicit descriptor mappings instead of deriving them from shader names.
 *
 * Faults stop immediately after a genuine successful resource acquisition;
 * no GPU submission is made in fault-injection mode.
 */
static int execute_graph (
  const vulkan_auto_device_t *selected,
  const vulkan_core10_graph_shader_spec_t shaders[GRAPH_MAX_STAGES],
  uint32_t stage_count,
  uint32_t submissions,
  VkBool32 noncoherent,
  uint32_t fail_after)
{
  const uint32_t descriptor_count = stage_count + 1u;
  const uint32_t buffer_count = descriptor_count + 2u;
  const uint32_t upload_index = buffer_count - 2u;
  const uint32_t download_index = buffer_count - 1u;
  const VkDeviceSize output_bytes =
    stage_count == 3u ? GRAPH_FINAL_BYTES : GRAPH_INPUT_BYTES;

  static const char *gpu_labels[] =
  {
    "gpu-input", "stage-a", "stage-b", "stage-c"
  };

  vulkan_core10_graph_buffer_spec_t specs[GRAPH_MAX_BUFFERS] = { 0 };
  vulkan_core10_graph_descriptor_spec_t
    descriptors[GRAPH_MAX_STAGES + 1u] = { 0 };

  for (uint32_t index = 0u; index < descriptor_count; index++)
  {
    const VkDeviceSize bytes =
      index + 1u == descriptor_count
      ? output_bytes : GRAPH_INPUT_BYTES;
    VkBufferUsageFlags flags = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

    if (index == 0u) flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (index + 1u == descriptor_count)
    {
      flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    }

    specs[index] = (vulkan_core10_graph_buffer_spec_t)
    {
      .label = gpu_labels[index],
      .size = bytes,
      .usage = flags,
      .role = VULKAN_HEAP_BUFFER_GPU_STORAGE,
      .forbidden_flags =
        VK_MEMORY_PROPERTY_PROTECTED_BIT
        | VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD,
      .map = VK_FALSE
    };

    descriptors[index] = (vulkan_core10_graph_descriptor_spec_t)
    {
      .binding = index,
      .buffer_index = index,
      .offset = 0u,
      .range = bytes
    };
  }

  specs[upload_index] = (vulkan_core10_graph_buffer_spec_t)
  {
    .label = "staging-upload",
    .size = GRAPH_INPUT_BYTES,
    .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    .role = VULKAN_HEAP_BUFFER_HOST_TRANSFER,
    .forbidden_flags =
      VK_MEMORY_PROPERTY_PROTECTED_BIT
      | VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD
      | (noncoherent == VK_TRUE
        ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : 0u),
    .map = VK_TRUE
  };
  specs[download_index] = (vulkan_core10_graph_buffer_spec_t)
  {
    .label = "staging-download",
    .size = output_bytes,
    .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    .role = VULKAN_HEAP_BUFFER_HOST_TRANSFER,
    .forbidden_flags =
      VK_MEMORY_PROPERTY_PROTECTED_BIT
      | VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD
      | (noncoherent == VK_TRUE
        ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : 0u),
    .map = VK_TRUE
  };

  fprintf (
    stdout,
    "GRAPH_REQUEST stages=%" PRIu32
    " buffers=%" PRIu32 " descriptors=%" PRIu32
    " mapped=2 output_bytes=%" PRIu64
    " requested_noncoherent=%u\n",
    stage_count, buffer_count, descriptor_count,
    (uint64_t) output_bytes, noncoherent
  );

  vulkan_core10_graph_t graph = { 0 };
  VkResult result = vulkan_core10_graph_create (
    &graph, selected,
    specs, buffer_count,
    descriptors, descriptor_count,
    shaders, stage_count, fail_after, stdout
  );

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr,
      "GRAPH_CREATE_FAILED result=%d fail_after=%" PRIu32 "\n",
      result, fail_after
    );

    return fail_after != 0u ? 42 : 1;
  }

  int status = 1;

  if (fail_after != 0u)
  {
    fprintf (
      stderr, "GRAPH_REJECT_UNREACHED_FAULT point=%" PRIu32 "\n",
      fail_after
    );

    goto cleanup;
  }

  result = record_commands (&graph);

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr, "GRAPH_RECORD_FAILED result=%d\n", result
    );

    goto cleanup;
  }

  uint32_t *upload = (uint32_t *) graph.buffers[upload_index].mapped;
  const uint32_t *download =
    (const uint32_t *) graph.buffers[download_index].mapped;

  for (uint32_t submission = 0u; submission < submissions; submission++)
  {
    for (uint32_t index = 0u; index < GRAPH_ELEMENTS; index++)
    {
      upload[index] = public_input (index, submission);
    }

    result = vulkan_core10_graph_sync (
      &graph, upload_index, VK_FALSE
    );

    if (result != VK_SUCCESS)
    {
      fprintf (
        stderr, "GRAPH_UPLOAD_SYNC_FAILED result=%d\n", result
      );

      goto cleanup;
    }

    result = vulkan_core10_graph_submit_and_wait (&graph);

    if (result != VK_SUCCESS)
    {
      fprintf (
        stderr, "GRAPH_SUBMISSION_FAILED result=%d\n", result
      );

      goto cleanup;
    }

    result = vulkan_core10_graph_sync (
      &graph, download_index, VK_TRUE
    );

    if (result != VK_SUCCESS)
    {
      fprintf (
        stderr, "GRAPH_DOWNLOAD_SYNC_FAILED result=%d\n", result
      );

      goto cleanup;
    }

    if (verify_graph (download, stage_count, submission) != 0)
    {
      goto cleanup;
    }
  }

  status = 0;

cleanup:
  result = vulkan_core10_graph_destroy (&graph);

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr, "GRAPH_DESTROY_FAILED result=%d\n", result
    );

    status = 1;
  }

  return status;
}

/**
 * Run the public GPU workload with explicit stage count and Vulkan API.
 *
 * The first three positional paths contain valid SPIR-V 1.0 stage A, B
 * and C modules; --stages controls how many of them execute. Every
 * resource/descriptor count follows this declared public shader ABI,
 * while driver compatibility and memory properties are queried at runtime.
 *
 * Requests with invalid stage/submission/fault limits return exit code 2.
 */
int main (int argc, char **argv)
{
  if (argc < 6)
  {
    fprintf (
      stderr,
      "Usage: %s <stage-a.spv> <stage-b.spv> <stage-c.spv>"
      " --stages <1|2|3> [--submissions <2..16>]"
      " [--minimum <1.0|1.1>] [--require-noncoherent-staging]"
      " [--fail-after <N>]\n",
      argv[0]
    );

    return 2;
  }

  uint32_t stage_count = 0u;
  uint32_t submissions = 4u;
  uint32_t fail_after = 0u;
  uint32_t api = VK_API_VERSION_1_0;
  VkBool32 selected_stages = VK_FALSE;
  VkBool32 selected_submissions = VK_FALSE;
  VkBool32 selected_api = VK_FALSE;
  VkBool32 selected_fault = VK_FALSE;
  VkBool32 noncoherent = VK_FALSE;

  for (int argument = 4; argument < argc; argument++)
  {
    if (strcmp (argv[argument], "--stages") == 0
     && selected_stages == VK_FALSE && argument + 1 < argc)
    {
      selected_stages = VK_TRUE;

      if (decimal_arg (
        argv[++argument], &stage_count
      ) == VK_FALSE
       || stage_count < 1u || stage_count > GRAPH_MAX_STAGES)
      {
        fprintf (stderr, "GRAPH_REJECT_STAGES permitted=1..3\n");

        return 2;
      }
    }
    else if (strcmp (argv[argument], "--submissions") == 0
      && selected_submissions == VK_FALSE && argument + 1 < argc)
    {
      selected_submissions = VK_TRUE;

      if (decimal_arg (
        argv[++argument], &submissions
      ) == VK_FALSE
       || submissions < 2u || submissions > 16u)
      {
        fprintf (
          stderr, "GRAPH_REJECT_SUBMISSIONS permitted=2..16\n"
        );

        return 2;
      }
    }
    else if (strcmp (argv[argument], "--minimum") == 0
      && selected_api == VK_FALSE && argument + 1 < argc)
    {
      selected_api = VK_TRUE;
      argument++;

      if (strcmp (argv[argument], "1.0") == 0)
      {
        api = VK_API_VERSION_1_0;
      }
      else if (strcmp (argv[argument], "1.1") == 0)
      {
        api = VK_API_VERSION_1_1;
      }
      else
      {
        fprintf (stderr, "GRAPH_REJECT_API_VERSION\n");

        return 2;
      }
    }
    else if (strcmp (
      argv[argument], "--require-noncoherent-staging"
    ) == 0 && noncoherent == VK_FALSE)
    {
      noncoherent = VK_TRUE;
    }
    else if (strcmp (argv[argument], "--fail-after") == 0
      && selected_fault == VK_FALSE && argument + 1 < argc)
    {
      selected_fault = VK_TRUE;

      if (decimal_arg (
        argv[++argument], &fail_after
      ) == VK_FALSE || fail_after == 0u)
      {
        fprintf (stderr, "GRAPH_REJECT_FAULT_NUMBER\n");

        return 2;
      }
    }
    else
    {
      fprintf (
        stderr, "GRAPH_REJECT_ARGUMENT value=%s\n", argv[argument]
      );

      return 2;
    }
  }

  if (selected_stages == VK_FALSE)
  {
    fprintf (stderr, "GRAPH_REJECT_MISSING_STAGE_COUNT\n");

    return 2;
  }

  const uint32_t max_faults = 16u + 4u * stage_count;

  if (fail_after > max_faults)
  {
    fprintf (
      stderr,
      "GRAPH_REJECT_FAULT_RANGE maximum=%" PRIu32 "\n",
      max_faults
    );

    return 2;
  }

  vulkan_core10_graph_shader_spec_t shaders[GRAPH_MAX_STAGES] = { 0 };
  int status = 1;

  for (uint32_t stage = 0u; stage < stage_count; stage++)
  {
    size_t size = 0u;
    uint32_t *words = read_spirv (argv[stage + 1u], &size);

    if (words == NULL)
    {
      status = 2;

      goto cleanup_shaders;
    }

    shaders[stage].words = words;
    shaders[stage].bytes = size;
  }

  uint32_t loader = 0u;
  VkResult result = vulkan_core11_query_loader_version (
    vkGetInstanceProcAddr, &loader, stdout
  );

  if (result != VK_SUCCESS) goto cleanup_shaders;

  uint32_t instance_api = 0u;

  result = vulkan_core11_select_instance_version (
    loader, api, &instance_api, stdout
  );

  if (result != VK_SUCCESS) goto cleanup_shaders;

  const VkApplicationInfo application =
  {
    .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
    .pApplicationName = "Vulkan 1.0 Dynamic Synthetic Graph",
    .applicationVersion = VK_MAKE_VERSION (1, 0, 0),
    .pEngineName = "None",
    .engineVersion = VK_MAKE_VERSION (1, 0, 0),
    .apiVersion = instance_api
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
      stderr, "GRAPH_INSTANCE_FAILED result=%d\n", result
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
      "GRAPH_DEVICE_ENUM_FAILED count=%" PRIu32
      " result=%d\n",
      device_count, result
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
    .local_size = { GRAPH_LOCAL_SIZE, 1u, 1u },
    .group_count = { GRAPH_ELEMENTS / GRAPH_LOCAL_SIZE, 1u, 1u },
    .storage_buffer_count = stage_count + 1u,
    .largest_storage_buffer_bytes =
      stage_count == 3u ? GRAPH_FINAL_BYTES : GRAPH_INPUT_BYTES,
    .shared_memory_bytes =
      stage_count >= 2u ? 32u * sizeof (uint32_t) : 0u
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
      instance_api, api, &effective, stdout
    );

    if (result != VK_SUCCESS)
    {
      fprintf (
        stdout,
        "GRAPH_SKIP_DEVICE index=%" PRIu32 " reason=api\n", index
      );

      continue;
    }

    vulkan_auto_device_t selected = { 0 };

    result = vulkan_auto_check_device (
      physical[index], index,
      &workload, &selected, stdout
    );

    if (result != VK_SUCCESS)
    {
      fprintf (
        stdout,
        "GRAPH_SKIP_DEVICE index=%" PRIu32
        " reason=compute_limits\n", index
      );

      continue;
    }

    fprintf (
      stdout,
      "GRAPH_SELECTED_DEVICE index=%" PRIu32
      " requested_api=%" PRIu32 ".%" PRIu32
      " effective_api=%" PRIu32 ".%" PRIu32
      " stages=%" PRIu32 " descriptors=%" PRIu32
      " workgroup=32 groups=64 features=0 extensions=0\n",
      index,
      VK_API_VERSION_MAJOR (api),
      VK_API_VERSION_MINOR (api),
      VK_API_VERSION_MAJOR (effective),
      VK_API_VERSION_MINOR (effective),
      stage_count, stage_count + 1u
    );

    status = execute_graph (
      &selected, shaders, stage_count,
      submissions, noncoherent, fail_after
    );

    if (status == 0 || fail_after != 0u) break;

    fprintf (
      stdout,
      "GRAPH_SKIP_DEVICE index=%" PRIu32
      " reason=memory_or_compute\n", index
    );
  }

  if (status != 0)
  {
    fprintf (
      stderr, "GRAPH_NO_COMPATIBLE_CONFIGURATION\n"
    );
  }

cleanup_physical:
  free (physical);

cleanup_instance:
  vkDestroyInstance (instance, NULL);

cleanup_shaders:
  for (uint32_t stage = 0u; stage < stage_count; stage++)
  {
    free ((void *) shaders[stage].words);
  }

  return status;
}
