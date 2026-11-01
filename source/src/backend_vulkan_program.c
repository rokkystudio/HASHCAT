/**
 * Validate explicit Vulkan 1.0 Compute-stage graphs before GPU execution.
 *
 * This source contains only host-side declaration checks. It does not
 * create Vulkan objects, inspect device names, infer shader interfaces
 * or submit work. Every stage topology and storage dependency comes
 * from the caller's immutable program declaration.
 */

#include "backend_vulkan_program.h"

#include <inttypes.h>
#include <string.h>

#define BACKEND_VULKAN_SPIRV_MAGIC UINT32_C (0x07230203)
#define BACKEND_VULKAN_SPIRV_1_0 UINT32_C (0x00010000)

/**
 * Validate the five-word canonical SPIR-V module header.
 *
 * Requires a complete header, canonical little-endian magic, version
 * exactly 1.0 and reserved schema zero. Bound and generator fields are
 * intentionally not interpreted because they do not describe execution
 * topology or storage dependencies needed by this host declaration.
 */
static VkBool32 backend_vulkan_program_spirv_valid (
  const backend_vulkan_program_stage_t *stage)
{
  if (stage->spirv == NULL || stage->spirv_words < 5u)
  {
    return VK_FALSE;
  }

  return stage->spirv[0] == BACKEND_VULKAN_SPIRV_MAGIC
      && stage->spirv[1] == BACKEND_VULKAN_SPIRV_1_0
      && stage->spirv[4] == 0u
    ? VK_TRUE : VK_FALSE;
}

/**
 * Validate exact local and group topology against physical-device limits.
 *
 * Multiplies the three local dimensions in uint64 arithmetic to avoid
 * host overflow while checking maxComputeWorkGroupInvocations. A zero
 * local or group dimension is a malformed declaration. Exceeding any
 * physical limit is reported as unsupported rather than rewritten.
 */
static VkResult backend_vulkan_program_validate_topology (
  const backend_vulkan_program_stage_t *stage,
  const VkPhysicalDeviceLimits *limits,
  uint32_t stage_index,
  FILE *diagnostics)
{
  uint64_t invocations = 1u;

  for (uint32_t axis = 0u; axis < 3u; axis++)
  {
    if (stage->local_size[axis] == 0u
     || stage->group_count[axis] == 0u)
    {
      fprintf (
        diagnostics,
        "BACKEND_VULKAN_PROGRAM_REJECT_TOPOLOGY"
        " stage=%" PRIu32 " axis=%" PRIu32 " zero=1\n",
        stage_index, axis
      );

      return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (stage->local_size[axis]
        > limits->maxComputeWorkGroupSize[axis]
     || stage->group_count[axis]
        > limits->maxComputeWorkGroupCount[axis])
    {
      fprintf (
        diagnostics,
        "BACKEND_VULKAN_PROGRAM_REJECT_LIMIT"
        " stage=%" PRIu32 " axis=%" PRIu32
        " local=%" PRIu32 " local_limit=%" PRIu32
        " groups=%" PRIu32 " group_limit=%" PRIu32 "\n",
        stage_index, axis,
        stage->local_size[axis],
        limits->maxComputeWorkGroupSize[axis],
        stage->group_count[axis],
        limits->maxComputeWorkGroupCount[axis]
      );

      return VK_ERROR_FEATURE_NOT_PRESENT;
    }

    invocations *= stage->local_size[axis];
  }

  if (invocations > limits->maxComputeWorkGroupInvocations)
  {
    fprintf (
      diagnostics,
      "BACKEND_VULKAN_PROGRAM_REJECT_INVOCATIONS"
      " stage=%" PRIu32 " requested=%" PRIu64
      " limit=%" PRIu32 "\n",
      stage_index, invocations,
      limits->maxComputeWorkGroupInvocations
    );

    return VK_ERROR_FEATURE_NOT_PRESENT;
  }

  return VK_SUCCESS;
}

/**
 * Validate one exact sequential program without modifying caller state.
 *
 * Every destination buffer is unique across the graph, and stage N must
 * consume stage N-1's destination. This yields an explicit linear data
 * dependency suitable for later shader-write-to-shader-read barriers.
 * The validator accepts no aliasing shortcut or inferred dependency.
 */
VkResult backend_vulkan_program_validate (
  const backend_vulkan_program_t *program,
  const VkPhysicalDeviceLimits *limits,
  FILE *diagnostics)
{
  if (program == NULL || limits == NULL || diagnostics == NULL
   || program->stages == NULL || program->stage_count == 0u
   || program->buffer_count < 2u || program->buffer_bytes == 0u)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  if (program->buffer_bytes > limits->maxStorageBufferRange
   || limits->maxPerStageDescriptorStorageBuffers < 2u)
  {
    fprintf (
      diagnostics,
      "BACKEND_VULKAN_PROGRAM_REJECT_STORAGE"
      " bytes=%" PRIu64 " range_limit=%" PRIu32
      " descriptors_required=2 descriptor_limit=%" PRIu32 "\n",
      (uint64_t) program->buffer_bytes,
      limits->maxStorageBufferRange,
      limits->maxPerStageDescriptorStorageBuffers
    );

    return VK_ERROR_FEATURE_NOT_PRESENT;
  }

  for (uint32_t stage_index = 0u;
       stage_index < program->stage_count;
       stage_index++)
  {
    const backend_vulkan_program_stage_t *stage =
      &program->stages[stage_index];

    if (stage->name == NULL || stage->name[0] == 0
     || backend_vulkan_program_spirv_valid (stage) != VK_TRUE
     || stage->source_buffer_index >= program->buffer_count
     || stage->destination_buffer_index >= program->buffer_count
     || stage->source_buffer_index == stage->destination_buffer_index)
    {
      fprintf (
        diagnostics,
        "BACKEND_VULKAN_PROGRAM_REJECT_STAGE"
        " stage=%" PRIu32 "\n",
        stage_index
      );

      return VK_ERROR_INITIALIZATION_FAILED;
    }

    if (stage_index != 0u
     && stage->source_buffer_index
        != program->stages[stage_index - 1u].destination_buffer_index)
    {
      fprintf (
        diagnostics,
        "BACKEND_VULKAN_PROGRAM_REJECT_DEPENDENCY"
        " stage=%" PRIu32
        " source=%" PRIu32
        " expected=%" PRIu32 "\n",
        stage_index,
        stage->source_buffer_index,
        program->stages[stage_index - 1u].destination_buffer_index
      );

      return VK_ERROR_INITIALIZATION_FAILED;
    }

    for (uint32_t prior = 0u; prior < stage_index; prior++)
    {
      if (stage->destination_buffer_index
          == program->stages[prior].destination_buffer_index)
      {
        fprintf (
          diagnostics,
          "BACKEND_VULKAN_PROGRAM_REJECT_ALIAS"
          " stage=%" PRIu32 " prior=%" PRIu32
          " destination=%" PRIu32 "\n",
          stage_index, prior,
          stage->destination_buffer_index
        );

        return VK_ERROR_INITIALIZATION_FAILED;
      }
    }

    const VkResult topology =
      backend_vulkan_program_validate_topology (
        stage, limits, stage_index, diagnostics
      );

    if (topology != VK_SUCCESS) return topology;
  }

  fprintf (
    diagnostics,
    "BACKEND_VULKAN_PROGRAM_ACCEPT"
    " stages=%" PRIu32
    " buffers=%" PRIu32
    " bytes=%" PRIu64
    " descriptors_per_stage=2\n",
    program->stage_count,
    program->buffer_count,
    (uint64_t) program->buffer_bytes
  );

  return VK_SUCCESS;
}
