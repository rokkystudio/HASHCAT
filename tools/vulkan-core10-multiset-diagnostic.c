/**
 * Execute public integer Vulkan 1.0 Compute with one/two descriptor sets.
 *
 * The workload uses exactly two STORAGE_BUFFER descriptors and one
 * UNIFORM_BUFFER descriptor, with explicit sparse binding numbers.
 * The uniform parameters are updated for every sequential GPU submission.
 * Real GPU results are compared to an independent scalar CPU implementation.
 *
 * This is noncryptographic Compute infrastructure, unrelated to hashcat.
 */

#include "vulkan-auto-config.h"
#include "vulkan-core10-multiset.h"
#include "vulkan-core11-policy.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MULTI_WORDS 2048u
#define MULTI_LOCAL_SIZE 32u
#define MULTI_BUFFER_BYTES (MULTI_WORDS * sizeof (uint32_t))
#define MULTI_UNIFORM_BYTES (4u * sizeof (uint32_t))
#define MULTI_BUFFER_COUNT 5u
#define MULTI_BINDING_COUNT 3u

/**
 * Read a caller-selected public SPIR-V 1.0 shader module from disk.
 *
 * Requires complete 32-bit words and an exact SPIR-V 1.0 header. The
 * caller owns the allocated shader memory and frees it after execution.
 */
static uint32_t *load_spirv (
  const char *path,
  size_t *bytes)
{
  FILE *file = fopen (path, "rb");

  if (file == NULL)
  {
    perror ("MULTI_SHADER_OPEN_FAILED");

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
      stderr, "MULTI_REJECT_SPIRV_BYTES length=%ld\n", length
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
    fprintf (
      stderr, "MULTI_REJECT_SPIRV_VERSION required=1.0\n"
    );

    free (words);

    return NULL;
  }

  *bytes = (size_t) length;

  return words;
}

/**
 * Parse one required unsigned decimal integer without truncation or fallback.
 */
static VkBool32 decimal_value (
  const char *input,
  uint32_t *result)
{
  if (input == NULL || result == NULL || *input == '\0')
  {
    return VK_FALSE;
  }

  uint32_t value = 0u;

  for (const char *digit = input; *digit != '\0'; digit++)
  {
    if (*digit < '0' || *digit > '9') return VK_FALSE;

    const uint32_t next = (uint32_t) (*digit - '0');

    if (value > (UINT32_MAX - next) / 10u)
    {
      return VK_FALSE;
    }

    value = value * 10u + next;
  }

  *result = value;

  return VK_TRUE;
}

/**
 * Generate one public 32-bit input word for one GPU submission.
 */
static uint32_t public_word (
  uint32_t index,
  uint32_t submission)
{
  const uint32_t value =
    index * UINT32_C (0x9e3779b9)
    ^ (index << 15u)
    ^ (index >> 3u)
    ^ UINT32_C (0xa5a5a5a5);

  return value ^ (submission * UINT32_C (0x9e3779b9));
}

/**
 * Define four exact uniform words, including a nonzero shift of 3..15.
 *
 * The parameters fit one std140 uvec4 and use 32-bit wrapping arithmetic.
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
 * Calculate the exact integer value produced by either descriptor layout.
 */
static uint32_t expected_word (
  uint32_t index,
  uint32_t submission)
{
  uint32_t factors[4] = { 0 };

  public_parameters (submission, factors);

  const uint32_t input = public_word (index, submission);
  const uint32_t mixed =
    input * factors[0] + factors[1];
  const uint32_t rotated =
    (mixed << factors[2]) | (mixed >> (32u - factors[2]));

  return (mixed ^ rotated) + (index ^ factors[3]);
}

/**
 * Hash one little-endian public integer with FNV-1a64.
 */
static uint64_t hash_word (
  uint64_t hash,
  uint32_t value)
{
  for (uint32_t byte = 0u; byte < 4u; byte++)
  {
    hash ^= (value >> (byte * 8u)) & UINT32_C (0xff);
    hash *= UINT64_C (1099511628211);
  }

  return hash;
}

/**
 * Compare every completed GPU word and output its independently known digest.
 *
 * This checks that uniform parameters update across fence-separated replays,
 * including the second descriptor set in a two-set layout.
 */
static int verify_output (
  const uint32_t *output,
  uint32_t set_count,
  uint32_t submission)
{
  uint64_t digest = UINT64_C (14695981039346656037);

  for (uint32_t index = 0u; index < MULTI_WORDS; index++)
  {
    const uint32_t expected = expected_word (index, submission);

    if (output[index] != expected)
    {
      fprintf (
        stderr,
        "MULTI_OUTPUT_MISMATCH sets=%" PRIu32
        " submission=%" PRIu32 " index=%" PRIu32
        " expected=%08" PRIx32 " actual=%08" PRIx32 "\n",
        set_count, submission, index, expected, output[index]
      );

      return 1;
    }

    digest = hash_word (digest, output[index]);
  }

  fprintf (
    stdout,
    "MULTI_PASS sets=%" PRIu32
    " submission=%" PRIu32
    " verified_words=%u output=%016" PRIx64 "\n",
    set_count, submission, MULTI_WORDS, digest
  );

  return 0;
}

/**
 * Record host uploads, a multi-set Compute dispatch and host readback.
 *
 * Explicit HOST→TRANSFER and HOST→COMPUTE barriers make mapped uniform
 * writes visible to the shader, while a COMPUTE→TRANSFER and TRANSFER→HOST
 * chain makes the downloaded result available after the GPU fence.
 */
static VkResult record_commands (
  vulkan_core10_multiset_t *runtime)
{
  VkResult result = vulkan_core10_multiset_begin (runtime);

  if (result != VK_SUCCESS) return result;

  const VkMemoryBarrier host_barrier =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_HOST_WRITE_BIT,
    .dstAccessMask =
      VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT
  };

  vkCmdPipelineBarrier (
    runtime->command,
    VK_PIPELINE_STAGE_HOST_BIT,
    VK_PIPELINE_STAGE_TRANSFER_BIT
      | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
    0u, 1u, &host_barrier, 0u, NULL, 0u, NULL
  );

  const VkBufferCopy upload =
  {
    .srcOffset = 0u,
    .dstOffset = 0u,
    .size = MULTI_BUFFER_BYTES
  };

  vkCmdCopyBuffer (
    runtime->command,
    runtime->buffers[2].buffer,
    runtime->buffers[0].buffer,
    1u, &upload
  );

  const VkMemoryBarrier transfer_to_compute =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT
  };

  vkCmdPipelineBarrier (
    runtime->command,
    VK_PIPELINE_STAGE_TRANSFER_BIT,
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
    0u, 1u, &transfer_to_compute, 0u, NULL, 0u, NULL
  );

  vkCmdBindDescriptorSets (
    runtime->command,
    VK_PIPELINE_BIND_POINT_COMPUTE,
    runtime->pipeline_layout,
    0u, runtime->set_count, runtime->sets,
    0u, NULL
  );

  vkCmdBindPipeline (
    runtime->command,
    VK_PIPELINE_BIND_POINT_COMPUTE,
    runtime->pipelines[0]
  );

  vkCmdDispatch (
    runtime->command, MULTI_WORDS / MULTI_LOCAL_SIZE, 1u, 1u
  );

  const VkMemoryBarrier compute_to_transfer =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT
  };

  vkCmdPipelineBarrier (
    runtime->command,
    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
    VK_PIPELINE_STAGE_TRANSFER_BIT,
    0u, 1u, &compute_to_transfer, 0u, NULL, 0u, NULL
  );

  const VkBufferCopy download =
  {
    .srcOffset = 0u,
    .dstOffset = 0u,
    .size = MULTI_BUFFER_BYTES
  };

  vkCmdCopyBuffer (
    runtime->command,
    runtime->buffers[1].buffer,
    runtime->buffers[3].buffer,
    1u, &download
  );

  const VkMemoryBarrier transfer_to_host =
  {
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_HOST_READ_BIT
  };

  vkCmdPipelineBarrier (
    runtime->command,
    VK_PIPELINE_STAGE_TRANSFER_BIT,
    VK_PIPELINE_STAGE_HOST_BIT,
    0u, 1u, &transfer_to_host, 0u, NULL, 0u, NULL
  );

  return vulkan_core10_multiset_end (runtime);
}

/**
 * Instantiate one exact one-set or two-set synthetic Vulkan 1.0 layout.
 *
 * GPU storage buffers are independently allocated, upload/readback/uniform
 * buffers require real HOST_VISIBLE memory and explicit flush/invalidate
 * when not HOST_COHERENT. Each completed GPU submission reads new public
 * input words and a new four-word std140 uniform parameter block.
 */
static int execute_device (
  const vulkan_auto_device_t *selected,
  const vulkan_core10_multiset_shader_spec_t *shader,
  uint32_t set_count,
  uint32_t submissions,
  VkBool32 require_noncoherent,
  uint32_t fail_after)
{
  const VkMemoryPropertyFlags forbidden =
    VK_MEMORY_PROPERTY_PROTECTED_BIT
    | VK_MEMORY_PROPERTY_DEVICE_COHERENT_BIT_AMD;
  const VkMemoryPropertyFlags staging_forbidden =
    forbidden | (require_noncoherent == VK_TRUE
      ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : 0u);

  const vulkan_core10_multiset_buffer_spec_t buffers[MULTI_BUFFER_COUNT] =
  {
    {
      "gpu-input", MULTI_BUFFER_BYTES,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
        | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VULKAN_HEAP_BUFFER_GPU_STORAGE, forbidden, VK_FALSE
    },
    {
      "gpu-output", MULTI_BUFFER_BYTES,
      VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
        | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      VULKAN_HEAP_BUFFER_GPU_STORAGE, forbidden, VK_FALSE
    },
    {
      "staging-upload", MULTI_BUFFER_BYTES,
      VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
      VULKAN_HEAP_BUFFER_HOST_TRANSFER, staging_forbidden, VK_TRUE
    },
    {
      "staging-download", MULTI_BUFFER_BYTES,
      VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      VULKAN_HEAP_BUFFER_HOST_TRANSFER, staging_forbidden, VK_TRUE
    },
    {
      "uniform-parameters", MULTI_UNIFORM_BYTES,
      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
      VULKAN_HEAP_BUFFER_HOST_TRANSFER, staging_forbidden, VK_TRUE
    }
  };

  const vulkan_core10_descriptor_binding_t bindings[MULTI_BINDING_COUNT] =
  {
    {
      0u, 2u, 0u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      0u, MULTI_BUFFER_BYTES
    },
    {
      set_count - 1u, set_count == 1u ? 7u : 1u,
      1u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      0u, MULTI_BUFFER_BYTES
    },
    {
      set_count - 1u, set_count == 1u ? 4u : 5u,
      4u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
      0u, MULTI_UNIFORM_BYTES
    }
  };

  fprintf (
    stdout,
    "MULTI_REQUEST sets=%" PRIu32
    " buffers=5 bindings=3 storage=2 uniform=1"
    " stages=1 mapped=3 noncoherent=%u\n",
    set_count, require_noncoherent
  );

  vulkan_core10_multiset_t runtime = { 0 };
  const VkResult create_result = vulkan_core10_multiset_create (
    &runtime, selected, buffers, MULTI_BUFFER_COUNT,
    bindings, MULTI_BINDING_COUNT,
    set_count, shader, 1u, fail_after, stdout
  );

  if (create_result != VK_SUCCESS)
  {
    fprintf (
      stderr,
      "MULTI_CREATE_FAILED result=%d fail_after=%" PRIu32 "\n",
      create_result, fail_after
    );

    return fail_after != 0u ? 42 : 1;
  }

  int status = 1;

  if (fail_after != 0u)
  {
    fprintf (
      stderr, "MULTI_UNREACHED_FAULT point=%" PRIu32 "\n",
      fail_after
    );

    goto cleanup;
  }

  VkResult result = record_commands (&runtime);

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr, "MULTI_RECORD_FAILED result=%d\n", result
    );

    goto cleanup;
  }

  uint32_t *upload = (uint32_t *) runtime.buffers[2].mapped;
  const uint32_t *download =
    (const uint32_t *) runtime.buffers[3].mapped;
  uint32_t *uniforms = (uint32_t *) runtime.buffers[4].mapped;

  for (uint32_t submission = 0u; submission < submissions; submission++)
  {
    for (uint32_t index = 0u; index < MULTI_WORDS; index++)
    {
      upload[index] = public_word (index, submission);
    }

    public_parameters (submission, uniforms);

    result = vulkan_core10_multiset_sync (
      &runtime, 2u, VK_FALSE
    );

    if (result != VK_SUCCESS)
    {
      fprintf (
        stderr, "MULTI_UPLOAD_FLUSH_FAILED result=%d\n", result
      );

      goto cleanup;
    }

    result = vulkan_core10_multiset_sync (
      &runtime, 4u, VK_FALSE
    );

    if (result != VK_SUCCESS)
    {
      fprintf (
        stderr, "MULTI_UNIFORM_FLUSH_FAILED result=%d\n", result
      );

      goto cleanup;
    }

    result = vulkan_core10_multiset_submit_and_wait (&runtime);

    if (result != VK_SUCCESS)
    {
      fprintf (
        stderr, "MULTI_SUBMIT_FAILED result=%d\n", result
      );

      goto cleanup;
    }

    result = vulkan_core10_multiset_sync (
      &runtime, 3u, VK_TRUE
    );

    if (result != VK_SUCCESS)
    {
      fprintf (
        stderr, "MULTI_DOWNLOAD_INVALIDATE_FAILED result=%d\n", result
      );

      goto cleanup;
    }

    if (verify_output (
      download, set_count, submission
    ) != 0)
    {
      goto cleanup;
    }
  }

  status = 0;

cleanup:
  result = vulkan_core10_multiset_destroy (&runtime);

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr, "MULTI_DESTROY_FAILED result=%d\n", result
    );

    status = 1;
  }

  return status;
}

/**
 * Select a suitable Vulkan 1.0 physical device using exact Compute limits.
 *
 * The first two positional shader paths specify one-set and two-set SPIR-V
 * 1.0 variants. The --sets argument is mandatory, and the selected shader
 * has a matching explicit descriptor interface. Optional Vulkan 1.1,
 * noncoherent staging and resource-fault controls never silently downgrade
 * the requested hardware capabilities.
 */
int main (int argc, char **argv)
{
  if (argc < 5)
  {
    fprintf (
      stderr,
      "Usage: %s <one-set.spv> <two-set.spv> --sets <1|2>"
      " [--minimum <1.0|1.1>] [--submissions <2..4>]"
      " [--require-noncoherent-staging] [--fail-after <N>]\n",
      argv[0]
    );

    return 2;
  }

  uint32_t sets = 0u;
  uint32_t submissions = 4u;
  uint32_t fail_after = 0u;
  uint32_t required_api = VK_API_VERSION_1_0;
  VkBool32 sets_explicit = VK_FALSE;
  VkBool32 submissions_explicit = VK_FALSE;
  VkBool32 api_explicit = VK_FALSE;
  VkBool32 fault_explicit = VK_FALSE;
  VkBool32 noncoherent = VK_FALSE;

  for (int argument = 3; argument < argc; argument++)
  {
    if (strcmp (argv[argument], "--sets") == 0
     && sets_explicit == VK_FALSE && argument + 1 < argc)
    {
      sets_explicit = VK_TRUE;

      if (decimal_value (
        argv[++argument], &sets
      ) != VK_TRUE || sets < 1u || sets > 2u)
      {
        fprintf (
          stderr, "MULTI_REJECT_SETS supported_shader_layouts=1..2\n"
        );

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
        fprintf (
          stderr, "MULTI_REJECT_SUBMISSIONS allowed=2..4\n"
        );

        return 2;
      }
    }
    else if (strcmp (argv[argument], "--minimum") == 0
      && api_explicit == VK_FALSE && argument + 1 < argc)
    {
      api_explicit = VK_TRUE;
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
        fprintf (stderr, "MULTI_REJECT_API_VERSION\n");

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
        fprintf (stderr, "MULTI_REJECT_FAULT\n");

        return 2;
      }
    }
    else
    {
      fprintf (
        stderr, "MULTI_REJECT_ARGUMENT value=%s\n", argv[argument]
      );

      return 2;
    }
  }

  if (sets_explicit == VK_FALSE)
  {
    fprintf (stderr, "MULTI_REJECT_MISSING_SETS\n");

    return 2;
  }

  const uint32_t maximum_fault = 21u + 2u * sets;

  if (fail_after > maximum_fault)
  {
    fprintf (
      stderr,
      "MULTI_REJECT_FAULT_RANGE requested=%" PRIu32
      " maximum=%" PRIu32 "\n",
      fail_after, maximum_fault
    );

    return 2;
  }

  size_t shader_bytes = 0u;
  uint32_t *shader_code = load_spirv (
    argv[sets], &shader_bytes
  );

  if (shader_code == NULL) return 2;

  const vulkan_core10_multiset_shader_spec_t shader =
  {
    .words = shader_code,
    .bytes = shader_bytes
  };

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

  const VkApplicationInfo app_info =
  {
    .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
    .pApplicationName = "Vulkan 1.0 Multiple Descriptor Set Synthetic Compute",
    .applicationVersion = VK_MAKE_VERSION (1, 0, 0),
    .pEngineName = "None",
    .engineVersion = VK_MAKE_VERSION (1, 0, 0),
    .apiVersion = instance_api
  };
  const VkInstanceCreateInfo instance_info =
  {
    .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
    .pApplicationInfo = &app_info
  };
  VkInstance instance = VK_NULL_HANDLE;

  result = vkCreateInstance (
    &instance_info, NULL, &instance
  );

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr, "MULTI_INSTANCE_FAILED result=%d\n", result
    );

    goto cleanup_shader;
  }

  uint32_t devices = 0u;

  result = vkEnumeratePhysicalDevices (
    instance, &devices, NULL
  );

  if (result != VK_SUCCESS || devices == 0u)
  {
    fprintf (
      stderr,
      "MULTI_ENUM_DEVICES_FAILED result=%d devices=%" PRIu32 "\n",
      result, devices
    );

    goto cleanup_instance;
  }

  VkPhysicalDevice *physical = (VkPhysicalDevice *) calloc (
    devices, sizeof (*physical)
  );

  if (physical == NULL) goto cleanup_instance;

  result = vkEnumeratePhysicalDevices (
    instance, &devices, physical
  );

  if (result != VK_SUCCESS) goto cleanup_physical;

  const vulkan_auto_compute_requirements_t workload =
  {
    .local_size = { MULTI_LOCAL_SIZE, 1u, 1u },
    .group_count = { MULTI_WORDS / MULTI_LOCAL_SIZE, 1u, 1u },
    .storage_buffer_count = 2u,
    .largest_storage_buffer_bytes = MULTI_BUFFER_BYTES,
    .shared_memory_bytes = 0u
  };

  for (uint32_t index = 0u; index < devices; index++)
  {
    VkPhysicalDeviceProperties properties = { 0 };

    vkGetPhysicalDeviceProperties (
      physical[index], &properties
    );

    uint32_t effective = 0u;

    result = vulkan_core11_select_device_version (
      properties.apiVersion,
      instance_api, required_api, &effective, stdout
    );

    if (result != VK_SUCCESS)
    {
      fprintf (
        stdout,
        "MULTI_SKIP_DEVICE index=%" PRIu32 " reason=api\n", index
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
        "MULTI_SKIP_DEVICE index=%" PRIu32
        " reason=workload_limits\n", index
      );

      continue;
    }

    fprintf (
      stdout,
      "MULTI_SELECTED_DEVICE index=%" PRIu32
      " requested_api=%" PRIu32 ".%" PRIu32
      " effective_api=%" PRIu32 ".%" PRIu32
      " sets=%" PRIu32
      " storage=2 uniform=1 workgroup=32 groups=64"
      " features=0 extensions=0\n",
      index,
      VK_API_VERSION_MAJOR (required_api),
      VK_API_VERSION_MINOR (required_api),
      VK_API_VERSION_MAJOR (effective),
      VK_API_VERSION_MINOR (effective),
      sets
    );

    status = execute_device (
      &selected, &shader, sets,
      submissions, noncoherent, fail_after
    );

    if (status == 0 || fail_after != 0u) break;

    fprintf (
      stdout,
      "MULTI_SKIP_DEVICE index=%" PRIu32
      " reason=descriptor_or_memory_or_dispatch\n", index
    );
  }

  if (status != 0)
  {
    fprintf (
      stderr, "MULTI_NO_COMPATIBLE_CONFIGURATION\n"
    );
  }

cleanup_physical:
  free (physical);

cleanup_instance:
  vkDestroyInstance (instance, NULL);

cleanup_shader:
  free (shader_code);

  return status;
}
