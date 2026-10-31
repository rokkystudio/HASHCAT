/**
 * Execute an explicit one-, two- or three-stage Vulkan 1.0 Compute graph.
 *
 * The workload uses sparse STORAGE_BUFFER bindings in one or two descriptor
 * sets and a CPU-updated std140 UNIFORM_BUFFER shared by all Compute stages.
 * GPU staging transfers, per-stage readbacks, and fence-separated replay
 * are independently verified against public scalar integer calculations.
 *
 * No cryptographic algorithm, password data or hashcat kernel is used.
 */

#include "vulkan-auto-config.h"
#include "vulkan-core10-multiset.h"
#include "vulkan-core10-multiset-graph.h"
#include "vulkan-core11-policy.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MULTIGRAPH_MAX_STAGES 3u
#define MULTIGRAPH_MAX_BUFFERS 9u
#define MULTIGRAPH_WORDS 2048u
#define MULTIGRAPH_LOCAL_SIZE 32u
#define MULTIGRAPH_BYTES (MULTIGRAPH_WORDS * sizeof (uint32_t))
#define MULTIGRAPH_UNIFORM_BYTES (4u * sizeof (uint32_t))

/**
 * Parse one complete unsigned decimal integer with checked overflow.
 */
static VkBool32 decimal_value (
  const char *input,
  uint32_t *value)
{
  if (input == NULL || value == NULL || input[0] == '\0')
  {
    return VK_FALSE;
  }

  uint32_t result = 0u;

  for (const char *digit = input; *digit != '\0'; digit++)
  {
    if (*digit < '0' || *digit > '9') return VK_FALSE;

    const uint32_t decimal = (uint32_t) (*digit - '0');

    if (result > (UINT32_MAX - decimal) / 10u)
    {
      return VK_FALSE;
    }

    result = result * 10u + decimal;
  }

  *value = result;

  return VK_TRUE;
}

/**
 * Load an exact SPIR-V 1.0 shader into caller-owned aligned uint32 words.
 *
 * Rejects incomplete, oversized or incorrectly versioned modules before
 * any Vulkan instance or device is created.
 */
static uint32_t *load_spirv (
  const char *path,
  size_t *bytes)
{
  FILE *file = fopen (path, "rb");

  if (file == NULL)
  {
    perror ("MULTIGRAPH_SHADER_OPEN_FAILED");

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
    fprintf (
      stderr, "MULTIGRAPH_REJECT_SHADER_BYTES length=%ld\n", length
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

  const size_t received = fread (words, 1u, (size_t) length, file);

  fclose (file);

  if (received != (size_t) length
   || words[0] != UINT32_C (0x07230203)
   || words[1] != UINT32_C (0x00010000))
  {
    fprintf (stderr, "MULTIGRAPH_REJECT_SHADER_VERSION\n");

    free (words);

    return NULL;
  }

  *bytes = (size_t) length;

  return words;
}

/**
 * Generate a public deterministic input using 32-bit wrapping arithmetic.
 */
static uint32_t public_word (
  uint32_t index,
  uint32_t submission)
{
  const uint32_t value =
    index * UINT32_C (0x9e3779b9)
    ^ (index << 15u) ^ (index >> 3u)
    ^ UINT32_C (0xa5a5a5a5);

  return value ^ (submission * UINT32_C (0x9e3779b9));
}

/**
 * Compute four changing std140 uniform words for a GPU submission.
 *
 * The shift is always within 3..15 for four sequential submissions.
 */
static void public_parameters (
  uint32_t submission,
  uint32_t values[4])
{
  values[0] = UINT32_C (1664525) + submission * UINT32_C (101);
  values[1] = UINT32_C (1013904223) + submission * UINT32_C (97);
  values[2] = 3u + 4u * submission;
  values[3] =
    UINT32_C (0xa5a5a5a5)
    ^ (submission * UINT32_C (0x9e3779b9));
}

/**
 * Compute one public integer across the specified dependent GPU stages.
 *
 * Each stage consumes the previous stage's exact 32-bit result. No
 * floating-point arithmetic, GPU feature or driver behavior is inferred.
 */
static uint32_t expected_word (
  uint32_t index,
  uint32_t submission,
  uint32_t final_stage)
{
  uint32_t parameters[4] = { 0 };

  public_parameters (submission, parameters);

  uint32_t value = public_word (index, submission);

  for (uint32_t stage = 0u; stage <= final_stage; stage++)
  {
    const uint32_t mixed =
      value * (parameters[0] + stage * 17u)
      + parameters[1] + stage * UINT32_C (12345);
    const uint32_t rotated =
      (mixed << parameters[2])
      | (mixed >> (32u - parameters[2]));

    value = (mixed ^ rotated)
      + (index ^ parameters[3])
      + stage * UINT32_C (0x9e3779b9);
  }

  return value;
}

/**
 * Hash one public little-endian 32-bit word into an FNV-1a64 digest.
 */
static uint64_t hash_word (
  uint64_t digest,
  uint32_t value)
{
  for (uint32_t byte = 0u; byte < 4u; byte++)
  {
    digest ^= (value >> (byte * 8u)) & UINT32_C (0xff);
    digest *= UINT64_C (1099511628211);
  }

  return digest;
}

/**
 * Verify every word of a completed stage readback against a scalar CPU.
 *
 * Reports the exact failed index or one reproducible digest for the full
 * 2,048-word stage output after each fence-separated GPU submission.
 */
static int verify_stage (
  const uint32_t *output,
  uint32_t set_count,
  uint32_t stage_count,
  uint32_t submission,
  uint32_t stage)
{
  uint64_t digest = UINT64_C (14695981039346656037);

  for (uint32_t index = 0u; index < MULTIGRAPH_WORDS; index++)
  {
    const uint32_t expected = expected_word (
      index, submission, stage
    );

    if (output[index] != expected)
    {
      fprintf (
        stderr,
        "MULTIGRAPH_MISMATCH sets=%" PRIu32
        " stages=%" PRIu32 " submission=%" PRIu32
        " stage=%" PRIu32 " index=%" PRIu32
        " expected=%08" PRIx32 " actual=%08" PRIx32 "\n",
        set_count, stage_count, submission, stage,
        index, expected, output[index]
      );

      return 1;
    }

    digest = hash_word (digest, output[index]);
  }

  fprintf (
    stdout,
    "MULTIGRAPH_PASS sets=%" PRIu32
    " stages=%" PRIu32 " submission=%" PRIu32
    " stage=%" PRIu32 " words=%u fnv=%016" PRIx64 "\n",
    set_count, stage_count, submission, stage,
    MULTIGRAPH_WORDS, digest
  );

  return 0;
}

/**
 * Create and execute one explicit graph using current device memory types.
 *
 * Every GPU stage owns a distinct DEVICE_LOCAL-preferred storage buffer
 * with dedicated per-stage readback. All CPU-visible transfers and uniform
 * storage use coherent or Vulkan-flushed noncoherent HOST_VISIBLE memory.
 *
 * The graph is recorded once and submitted four times after updating both
 * input data and uniform parameters; all stage outputs are CPU-verified.
 */
static int execute_device (
  const vulkan_auto_device_t *selected,
  const vulkan_core10_multiset_shader_spec_t *shaders,
  uint32_t stage_count,
  uint32_t set_count,
  uint32_t submissions,
  VkBool32 require_noncoherent,
  uint32_t fail_after)
{
  const VkMemoryPropertyFlags forbidden =
    VK_MEMORY_PROPERTY_PROTECTED_BIT
    | VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD;
  const VkMemoryPropertyFlags host_forbidden =
    forbidden | (require_noncoherent == VK_TRUE
      ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : 0u);
  const uint32_t upload_index = stage_count + 1u;
  const uint32_t uniform_index = 2u * stage_count + 2u;
  const uint32_t buffer_count = uniform_index + 1u;
  const uint32_t binding_count = stage_count + 2u;

  vulkan_core10_multiset_buffer_spec_t buffers[MULTIGRAPH_MAX_BUFFERS] =
    { 0 };
  vulkan_core10_descriptor_binding_t bindings[MULTIGRAPH_MAX_STAGES + 2u] =
    { 0 };
  vulkan_core10_multiset_graph_stage_t stages[MULTIGRAPH_MAX_STAGES] =
    { 0 };
  uint32_t readbacks[MULTIGRAPH_MAX_STAGES] = { 0 };
  const char *labels[MULTIGRAPH_MAX_STAGES + 1u] =
  {
    "gpu-input", "gpu-stage-a", "gpu-stage-b", "gpu-stage-c"
  };

  for (uint32_t index = 0u; index <= stage_count; index++)
  {
    buffers[index] = (vulkan_core10_multiset_buffer_spec_t)
    {
      .label = labels[index],
      .size = MULTIGRAPH_BYTES,
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
        | (index == 0u
          ? VK_BUFFER_USAGE_TRANSFER_DST_BIT
          : VK_BUFFER_USAGE_TRANSFER_SRC_BIT),
      .role = VULKAN_HEAP_BUFFER_GPU_STORAGE,
      .forbidden_flags = forbidden,
      .map = VK_FALSE
    };

    bindings[index] = (vulkan_core10_descriptor_binding_t)
    {
      .set = index == 0u ? 0u : set_count - 1u,
      .binding = 2u + 2u * index,
      .buffer_index = index,
      .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .offset = 0u,
      .range = MULTIGRAPH_BYTES
    };
  }

  buffers[upload_index] = (vulkan_core10_multiset_buffer_spec_t)
  {
    .label = "staging-upload",
    .size = MULTIGRAPH_BYTES,
    .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    .role = VULKAN_HEAP_BUFFER_HOST_TRANSFER,
    .forbidden_flags = host_forbidden,
    .map = VK_TRUE
  };

  for (uint32_t stage = 0u; stage < stage_count; stage++)
  {
    const uint32_t readback = upload_index + 1u + stage;

    buffers[readback] = (vulkan_core10_multiset_buffer_spec_t)
    {
      .label = "staging-stage-output",
      .size = MULTIGRAPH_BYTES,
      .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .role = VULKAN_HEAP_BUFFER_HOST_TRANSFER,
      .forbidden_flags = host_forbidden,
      .map = VK_TRUE
    };

    stages[stage] = (vulkan_core10_multiset_graph_stage_t)
    {
      .pipeline_index = stage,
      .source_buffer_index = stage,
      .destination_buffer_index = stage + 1u,
      .groups = { MULTIGRAPH_WORDS / MULTIGRAPH_LOCAL_SIZE, 1u, 1u }
    };

    readbacks[stage] = readback;
  }

  buffers[uniform_index] = (vulkan_core10_multiset_buffer_spec_t)
  {
    .label = "uniform-parameters",
    .size = MULTIGRAPH_UNIFORM_BYTES,
    .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
    .role = VULKAN_HEAP_BUFFER_HOST_TRANSFER,
    .forbidden_flags = host_forbidden,
    .map = VK_TRUE
  };

  bindings[stage_count + 1u] = (vulkan_core10_descriptor_binding_t)
  {
    .set = set_count - 1u,
    .binding = 10u,
    .buffer_index = uniform_index,
    .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
    .offset = 0u,
    .range = MULTIGRAPH_UNIFORM_BYTES
  };

  const vulkan_core10_multiset_graph_spec_t graph =
  {
    .stages = stages,
    .readback_buffer_indices = readbacks,
    .stage_count = stage_count,
    .upload_buffer_index = upload_index,
    .uniform_buffer_index = uniform_index,
    .transfer_bytes = MULTIGRAPH_BYTES
  };

  fprintf (
    stdout,
    "MULTIGRAPH_REQUEST sets=%" PRIu32
    " stages=%" PRIu32 " buffers=%" PRIu32
    " bindings=%" PRIu32 " mapped=%" PRIu32
    " noncoherent=%u\n",
    set_count, stage_count, buffer_count,
    binding_count, stage_count + 2u, require_noncoherent
  );

  vulkan_core10_multiset_t runtime = { 0 };

  VkResult result = vulkan_core10_multiset_create (
    &runtime, selected, buffers, buffer_count,
    bindings, binding_count, set_count,
    shaders, stage_count, fail_after, stdout
  );

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr,
      "MULTIGRAPH_CREATE_FAILED result=%d fail_after=%" PRIu32 "\n",
      result, fail_after
    );

    return fail_after == 0u ? 1 : 42;
  }

  int status = 1;

  result = vulkan_core10_multiset_graph_record (
    &runtime, buffers, buffer_count, &graph, stdout
  );

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr, "MULTIGRAPH_RECORD_FAILED result=%d\n", result
    );

    goto cleanup;
  }

  uint32_t *upload = (uint32_t *) runtime.buffers[upload_index].mapped;
  uint32_t *uniforms = (uint32_t *) runtime.buffers[uniform_index].mapped;

  for (uint32_t submission = 0u; submission < submissions; submission++)
  {
    for (uint32_t index = 0u; index < MULTIGRAPH_WORDS; index++)
    {
      upload[index] = public_word (index, submission);
    }

    public_parameters (submission, uniforms);

    result = vulkan_core10_multiset_sync (
      &runtime, upload_index, VK_FALSE
    );

    if (result != VK_SUCCESS)
    {
      fprintf (
        stderr, "MULTIGRAPH_UPLOAD_SYNC_FAILED result=%d\n", result
      );

      goto cleanup;
    }

    result = vulkan_core10_multiset_sync (
      &runtime, uniform_index, VK_FALSE
    );

    if (result != VK_SUCCESS)
    {
      fprintf (
        stderr, "MULTIGRAPH_UNIFORM_SYNC_FAILED result=%d\n", result
      );

      goto cleanup;
    }

    result = vulkan_core10_multiset_submit_and_wait (&runtime);

    if (result != VK_SUCCESS)
    {
      fprintf (
        stderr, "MULTIGRAPH_SUBMIT_FAILED result=%d\n", result
      );

      goto cleanup;
    }

    for (uint32_t stage = 0u; stage < stage_count; stage++)
    {
      result = vulkan_core10_multiset_sync (
        &runtime, readbacks[stage], VK_TRUE
      );

      if (result != VK_SUCCESS)
      {
        fprintf (
          stderr,
          "MULTIGRAPH_READBACK_SYNC_FAILED stage=%" PRIu32
          " result=%d\n", stage, result
        );

        goto cleanup;
      }

      if (verify_stage (
        (const uint32_t *) runtime.buffers[readbacks[stage]].mapped,
        set_count, stage_count, submission, stage
      ) != 0)
      {
        goto cleanup;
      }
    }
  }

  status = 0;

cleanup:
  result = vulkan_core10_multiset_destroy (&runtime);

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr, "MULTIGRAPH_DESTROY_FAILED result=%d\n", result
    );

    status = 1;
  }

  return status;
}

/**
 * Select a Vulkan physical device for an exact multi-stage descriptor graph.
 *
 * The first three paths provide SPIR-V 1.0 stages zero, one and two.
 * --stages and --sets select the exact shader interface and GPU graph.
 * The requested Vulkan 1.0 baseline never uses Vulkan 1.1+ entry points.
 * Error exit 2 indicates malformed input, 42 an injected acquisition fault,
 * and 1 unsupported capabilities or an execution failure.
 */
int main (int argc, char **argv)
{
  if (argc < 7)
  {
    fprintf (
      stderr,
      "Usage: %s <a.spv> <b.spv> <c.spv> --stages <1..3>"
      " --sets <1|2> [--minimum <1.0|1.1>]"
      " [--submissions <2..4>] [--require-noncoherent-staging]"
      " [--fail-after <N>]\n",
      argv[0]
    );

    return 2;
  }

  uint32_t stage_count = 0u;
  uint32_t set_count = 0u;
  uint32_t submissions = 4u;
  uint32_t fail_after = 0u;
  uint32_t required_api = VK_API_VERSION_1_0;
  VkBool32 stages_explicit = VK_FALSE;
  VkBool32 sets_explicit = VK_FALSE;
  VkBool32 submissions_explicit = VK_FALSE;
  VkBool32 minimum_explicit = VK_FALSE;
  VkBool32 fault_explicit = VK_FALSE;
  VkBool32 noncoherent = VK_FALSE;

  for (int argument = 4; argument < argc; argument++)
  {
    if (strcmp (argv[argument], "--stages") == 0
     && stages_explicit == VK_FALSE && argument + 1 < argc)
    {
      stages_explicit = VK_TRUE;

      if (decimal_value (
        argv[++argument], &stage_count
      ) != VK_TRUE || stage_count < 1u
       || stage_count > MULTIGRAPH_MAX_STAGES)
      {
        fprintf (stderr, "MULTIGRAPH_REJECT_STAGES\n");

        return 2;
      }
    }
    else if (strcmp (argv[argument], "--sets") == 0
      && sets_explicit == VK_FALSE && argument + 1 < argc)
    {
      sets_explicit = VK_TRUE;

      if (decimal_value (
        argv[++argument], &set_count
      ) != VK_TRUE || set_count < 1u || set_count > 2u)
      {
        fprintf (stderr, "MULTIGRAPH_REJECT_SETS\n");

        return 2;
      }
    }
    else if (strcmp (argv[argument], "--minimum") == 0
      && minimum_explicit == VK_FALSE && argument + 1 < argc)
    {
      minimum_explicit = VK_TRUE;
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
        fprintf (stderr, "MULTIGRAPH_REJECT_API\n");

        return 2;
      }
    }
    else if (strcmp (argv[argument], "--submissions") == 0
      && submissions_explicit == VK_FALSE && argument + 1 < argc)
    {
      submissions_explicit = VK_TRUE;

      if (decimal_value (
        argv[++argument], &submissions
      ) != VK_TRUE || submissions < 2u || submissions > 4u)
      {
        fprintf (stderr, "MULTIGRAPH_REJECT_SUBMISSIONS\n");

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
      && fault_explicit == VK_FALSE && argument + 1 < argc)
    {
      fault_explicit = VK_TRUE;

      if (decimal_value (
        argv[++argument], &fail_after
      ) != VK_TRUE || fail_after == 0u)
      {
        fprintf (stderr, "MULTIGRAPH_REJECT_FAULT\n");

        return 2;
      }
    }
    else
    {
      fprintf (
        stderr, "MULTIGRAPH_REJECT_ARGUMENT value=%s\n",
        argv[argument]
      );

      return 2;
    }
  }

  if (stages_explicit == VK_FALSE || sets_explicit == VK_FALSE)
  {
    fprintf (stderr, "MULTIGRAPH_REJECT_MISSING_CONFIGURATION\n");

    return 2;
  }

  const uint32_t maximum_fault =
    14u + 7u * stage_count + 2u * set_count;

  if (fail_after > maximum_fault)
  {
    fprintf (
      stderr,
      "MULTIGRAPH_REJECT_FAULT_RANGE requested=%" PRIu32
      " maximum=%" PRIu32 "\n",
      fail_after, maximum_fault
    );

    return 2;
  }

  vulkan_core10_multiset_shader_spec_t shaders[MULTIGRAPH_MAX_STAGES] =
    { 0 };

  for (uint32_t stage = 0u; stage < stage_count; stage++)
  {
    uint32_t *words = load_spirv (
      argv[1 + (int) stage], &shaders[stage].bytes
    );

    if (words == NULL)
    {
      for (uint32_t before = 0u; before < stage; before++)
      {
        free ((void *) shaders[before].words);
      }

      return 2;
    }

    shaders[stage].words = words;
  }

  int status = 1;
  uint32_t loader = 0u;
  VkResult result = vulkan_core11_query_loader_version (
    vkGetInstanceProcAddr, &loader, stdout
  );

  if (result != VK_SUCCESS) goto cleanup_shader;

  uint32_t instance_api = 0u;
  result = vulkan_core11_select_instance_version (
    loader, required_api, &instance_api, stdout
  );

  if (result != VK_SUCCESS) goto cleanup_shader;

  const VkApplicationInfo application =
  {
    .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
    .pApplicationName = "Vulkan 1.0 Explicit Multi-stage Multi-set Compute",
    .applicationVersion = VK_MAKE_VERSION (1, 0, 0),
    .pEngineName = "None",
    .engineVersion = VK_MAKE_VERSION (1, 0, 0),
    .apiVersion = instance_api
  };
  const VkInstanceCreateInfo instance_info =
  {
    .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
    .pApplicationInfo = &application
  };

  VkInstance instance = VK_NULL_HANDLE;

  result = vkCreateInstance (
    &instance_info, NULL, &instance
  );

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr, "MULTIGRAPH_INSTANCE_FAILED result=%d\n", result
    );

    goto cleanup_shader;
  }

  uint32_t device_count = 0u;

  result = vkEnumeratePhysicalDevices (
    instance, &device_count, NULL
  );

  if (result != VK_SUCCESS || device_count == 0u)
  {
    fprintf (
      stderr,
      "MULTIGRAPH_ENUM_DEVICES_FAILED result=%d devices=%" PRIu32 "\n",
      result, device_count
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
    .local_size = { MULTIGRAPH_LOCAL_SIZE, 1u, 1u },
    .group_count =
    {
      MULTIGRAPH_WORDS / MULTIGRAPH_LOCAL_SIZE, 1u, 1u
    },
    .storage_buffer_count = stage_count + 1u,
    .largest_storage_buffer_bytes = MULTIGRAPH_BYTES,
    .shared_memory_bytes = 0u
  };

  for (uint32_t index = 0u; index < device_count; index++)
  {
    VkPhysicalDeviceProperties properties = { 0 };

    vkGetPhysicalDeviceProperties (
      physical[index], &properties
    );

    uint32_t effective = 0u;
    result = vulkan_core11_select_device_version (
      properties.apiVersion, instance_api, required_api,
      &effective, stdout
    );

    if (result != VK_SUCCESS)
    {
      fprintf (
        stdout,
        "MULTIGRAPH_SKIP_DEVICE index=%" PRIu32
        " reason=api\n", index
      );

      continue;
    }

    vulkan_auto_device_t selected = { 0 };

    result = vulkan_auto_check_device (
      physical[index], index, &workload, &selected, stdout
    );

    if (result != VK_SUCCESS)
    {
      fprintf (
        stdout,
        "MULTIGRAPH_SKIP_DEVICE index=%" PRIu32
        " reason=workload_limits\n", index
      );

      continue;
    }

    fprintf (
      stdout,
      "MULTIGRAPH_SELECTED_DEVICE index=%" PRIu32
      " requested_api=%" PRIu32 ".%" PRIu32
      " effective_api=%" PRIu32 ".%" PRIu32
      " sets=%" PRIu32 " stages=%" PRIu32
      " storage=%" PRIu32 " uniform=1"
      " workgroup=32 groups=64 features=0 extensions=0\n",
      index,
      VK_API_VERSION_MAJOR (required_api),
      VK_API_VERSION_MINOR (required_api),
      VK_API_VERSION_MAJOR (effective),
      VK_API_VERSION_MINOR (effective),
      set_count, stage_count, stage_count + 1u
    );

    status = execute_device (
      &selected, shaders, stage_count, set_count,
      submissions, noncoherent, fail_after
    );

    if (status == 0 || fail_after != 0u) break;

    fprintf (
      stdout,
      "MULTIGRAPH_SKIP_DEVICE index=%" PRIu32
      " reason=memory_or_descriptor_or_dispatch\n", index
    );
  }

  if (status != 0)
  {
    fprintf (
      stderr, "MULTIGRAPH_NO_COMPATIBLE_CONFIGURATION\n"
    );
  }

cleanup_physical:
  free (physical);

cleanup_instance:
  vkDestroyInstance (instance, NULL);

cleanup_shader:
  for (uint32_t stage = 0u; stage < stage_count; stage++)
  {
    free ((void *) shaders[stage].words);
  }

  return status;
}
