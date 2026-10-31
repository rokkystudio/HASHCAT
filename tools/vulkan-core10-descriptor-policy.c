/**
 * Validate multiple Vulkan 1.0 sets using exact storage/uniform limits.
 *
 * This policy is independent of the GPU driver and Vulkan object creation.
 * No descriptor binding or range is inferred, substituted or resized.
 */

#include "vulkan-core10-descriptor-policy.h"

#include <inttypes.h>
#include <stdint.h>

/**
 * Check all declared set indices, buffer ranges and per-stage device limits.
 */
VkResult vulkan_core10_descriptor_validate (
  const VkPhysicalDeviceLimits *limits,
  const vulkan_core10_descriptor_buffer_t *buffers,
  uint32_t buffer_count,
  const vulkan_core10_descriptor_binding_t *bindings,
  uint32_t binding_count,
  uint32_t set_count,
  vulkan_core10_descriptor_counts_t *counts,
  FILE *diagnostics)
{
  if (limits == NULL || buffers == NULL || bindings == NULL
   || counts == NULL || diagnostics == NULL || buffer_count == 0u
   || binding_count == 0u || set_count == 0u)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  if (set_count > limits->maxBoundDescriptorSets)
  {
    fprintf (
      diagnostics,
      "DESCRIPTOR_REJECT_SETS required=%" PRIu32
      " max_bound=%" PRIu32 "\n",
      set_count, limits->maxBoundDescriptorSets
    );

    return VK_ERROR_FEATURE_NOT_PRESENT;
  }

  if (binding_count > limits->maxPerStageResources)
  {
    fprintf (
      diagnostics,
      "DESCRIPTOR_REJECT_STAGE_RESOURCES required=%" PRIu32
      " max=%" PRIu32 "\n",
      binding_count, limits->maxPerStageResources
    );

    return VK_ERROR_FEATURE_NOT_PRESENT;
  }

  vulkan_core10_descriptor_counts_t proposed = { 0 };

  for (uint32_t index = 0u; index < binding_count; index++)
  {
    const vulkan_core10_descriptor_binding_t *item = &bindings[index];

    if (item->set >= set_count || item->buffer_index >= buffer_count
     || item->range == 0u
     || item->offset > buffers[item->buffer_index].size
     || item->range > buffers[item->buffer_index].size - item->offset)
    {
      fprintf (
        diagnostics,
        "DESCRIPTOR_REJECT_BINDING index=%" PRIu32
        " set=%" PRIu32 " binding=%" PRIu32
        " buffer=%" PRIu32 " offset=%" PRIu64
        " range=%" PRIu64 "\n",
        index, item->set, item->binding, item->buffer_index,
        (uint64_t) item->offset, (uint64_t) item->range
      );

      return VK_ERROR_INITIALIZATION_FAILED;
    }

    for (uint32_t before = 0u; before < index; before++)
    {
      if (bindings[before].set == item->set
       && bindings[before].binding == item->binding)
      {
        fprintf (
          diagnostics,
          "DESCRIPTOR_REJECT_DUPLICATE set=%" PRIu32
          " binding=%" PRIu32 "\n",
          item->set, item->binding
        );

        return VK_ERROR_INITIALIZATION_FAILED;
      }
    }

    VkBufferUsageFlags required = 0u;
    VkDeviceSize maximum_range = 0u;
    VkDeviceSize alignment = 0u;

    if (item->type == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
    {
      proposed.storage++;
      required = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
      maximum_range = limits->maxStorageBufferRange;
      alignment = limits->minStorageBufferOffsetAlignment;
    }
    else if (item->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
    {
      proposed.uniform++;
      required = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
      maximum_range = limits->maxUniformBufferRange;
      alignment = limits->minUniformBufferOffsetAlignment;
    }
    else
    {
      fprintf (
        diagnostics,
        "DESCRIPTOR_REJECT_TYPE index=%" PRIu32
        " type=%d\n", index, item->type
      );

      return VK_ERROR_FEATURE_NOT_PRESENT;
    }

    if ((buffers[item->buffer_index].usage & required) == 0u
     || alignment == 0u || item->offset % alignment != 0u
     || item->range > maximum_range)
    {
      fprintf (
        diagnostics,
        "DESCRIPTOR_REJECT_RANGE index=%" PRIu32
        " set=%" PRIu32 " binding=%" PRIu32
        " type=%d offset=%" PRIu64
        " range=%" PRIu64 " alignment=%" PRIu64
        " max=%" PRIu64 "\n",
        index, item->set, item->binding, item->type,
        (uint64_t) item->offset, (uint64_t) item->range,
        (uint64_t) alignment, (uint64_t) maximum_range
      );

      return VK_ERROR_FEATURE_NOT_PRESENT;
    }
  }

  for (uint32_t set = 0u; set < set_count; set++)
  {
    uint32_t count = 0u;

    for (uint32_t index = 0u; index < binding_count; index++)
    {
      if (bindings[index].set == set) count++;
    }

    if (count == 0u)
    {
      fprintf (
        diagnostics,
        "DESCRIPTOR_REJECT_EMPTY_SET set=%" PRIu32 "\n", set
      );

      return VK_ERROR_INITIALIZATION_FAILED;
    }
  }

  if (proposed.storage > limits->maxDescriptorSetStorageBuffers
   || proposed.storage > limits->maxPerStageDescriptorStorageBuffers
   || proposed.uniform > limits->maxDescriptorSetUniformBuffers
   || proposed.uniform > limits->maxPerStageDescriptorUniformBuffers)
  {
    fprintf (
      diagnostics,
      "DESCRIPTOR_REJECT_COUNTS storage=%" PRIu32
      " uniform=%" PRIu32
      " storage_set_max=%" PRIu32
      " storage_stage_max=%" PRIu32
      " uniform_set_max=%" PRIu32
      " uniform_stage_max=%" PRIu32 "\n",
      proposed.storage, proposed.uniform,
      limits->maxDescriptorSetStorageBuffers,
      limits->maxPerStageDescriptorStorageBuffers,
      limits->maxDescriptorSetUniformBuffers,
      limits->maxPerStageDescriptorUniformBuffers
    );

    return VK_ERROR_FEATURE_NOT_PRESENT;
  }

  *counts = proposed;

  fprintf (
    diagnostics,
    "DESCRIPTOR_ACCEPT sets=%" PRIu32
    " bindings=%" PRIu32 " storage=%" PRIu32
    " uniform=%" PRIu32 "\n",
    set_count, binding_count, proposed.storage, proposed.uniform
  );

  return VK_SUCCESS;
}
