/**
 * Implement Vulkan Compute capability checks and allocation policy from API.
 *
 * Every device-dependent value is obtained from Vulkan physical-device
 * properties or the memory requirements of actual storage buffers. The
 * module does not assign manufacturer profiles, choose undocumented queue
 * priorities or guess a shader's workgroup layout.
 */

#include "vulkan-auto-config.h"

#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/**
 * Return whether the requested local workgroup fits all compute limits.
 * Invalid zero-dimensional workgroups and overflowing products fail.
 */
static VkBool32 vulkan_auto_workgroup_fits (
  const VkPhysicalDeviceLimits *limits,
  const vulkan_auto_compute_requirements_t *requirements,
  FILE *diagnostics)
{
  uint64_t local_invocations = 1u;

  for (uint32_t dimension = 0u; dimension < 3u; dimension++)
  {
    const uint32_t local = requirements->local_size[dimension];
    const uint32_t groups = requirements->group_count[dimension];

    if (local == 0u || groups == 0u
     || local > limits->maxComputeWorkGroupSize[dimension]
     || groups > limits->maxComputeWorkGroupCount[dimension])
    {
      fprintf (
        diagnostics,
        "REJECT_COMPUTE dimension=%" PRIu32
        " local=%" PRIu32 " max_local=%" PRIu32
        " groups=%" PRIu32 " max_groups=%" PRIu32 "\n",
        dimension, local, limits->maxComputeWorkGroupSize[dimension],
        groups, limits->maxComputeWorkGroupCount[dimension]
      );

      return VK_FALSE;
    }

    if (local_invocations
        > limits->maxComputeWorkGroupInvocations / local)
    {
      fprintf (
        diagnostics,
        "REJECT_COMPUTE dimension=%" PRIu32
        " invocation_limit=%" PRIu32 "\n",
        dimension, limits->maxComputeWorkGroupInvocations
      );

      return VK_FALSE;
    }

    local_invocations *= local;
  }

  return VK_TRUE;
}

/**
 * Inspect one device's Vulkan core version, compute limits and queue families.
 * The first compute-capable queue family is selected without GPU-name rules.
 */
VkResult vulkan_auto_check_device (
  VkPhysicalDevice physical,
  uint32_t index,
  const vulkan_auto_compute_requirements_t *requirements,
  vulkan_auto_device_t *selected,
  FILE *diagnostics)
{
  if (physical == VK_NULL_HANDLE || requirements == NULL
   || selected == NULL || diagnostics == NULL)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  VkPhysicalDeviceProperties properties = { 0 };

  vkGetPhysicalDeviceProperties (physical, &properties);

  fprintf (
    diagnostics,
    "VULKAN_CANDIDATE index=%" PRIu32
    " name=%s vendor=0x%04" PRIx32
    " device=0x%04" PRIx32 " api=%" PRIu32 ".%" PRIu32 ".%" PRIu32
    " driver=0x%08" PRIx32 "\n",
    index, properties.deviceName, properties.vendorID, properties.deviceID,
    VK_VERSION_MAJOR (properties.apiVersion),
    VK_VERSION_MINOR (properties.apiVersion),
    VK_VERSION_PATCH (properties.apiVersion),
    properties.driverVersion
  );

  if (properties.apiVersion < VK_API_VERSION_1_0)
  {
    fprintf (
      diagnostics, "REJECT_VULKAN_API index=%" PRIu32
      " requires=1.0\n", index
    );

    return VK_ERROR_FEATURE_NOT_PRESENT;
  }

  const VkPhysicalDeviceLimits *limits = &properties.limits;

  if (vulkan_auto_workgroup_fits (
    limits, requirements, diagnostics
  ) == VK_FALSE)
  {
    return VK_ERROR_FEATURE_NOT_PRESENT;
  }

  if (requirements->storage_buffer_count == 0u
   || requirements->storage_buffer_count
      > limits->maxDescriptorSetStorageBuffers
   || requirements->storage_buffer_count
      > limits->maxPerStageDescriptorStorageBuffers
   || requirements->largest_storage_buffer_bytes == 0u
   || requirements->largest_storage_buffer_bytes
      > limits->maxStorageBufferRange
   || requirements->shared_memory_bytes > limits->maxComputeSharedMemorySize)
  {
    fprintf (
      diagnostics,
      "REJECT_STORAGE descriptors=%" PRIu32
      " max_descriptor_set=%" PRIu32
      " max_stage=%" PRIu32
      " requested_range=%" PRIu64
      " max_storage_range=%" PRIu32
      " requested_shared=%" PRIu32
      " max_shared=%" PRIu32 "\n",
      requirements->storage_buffer_count,
      limits->maxDescriptorSetStorageBuffers,
      limits->maxPerStageDescriptorStorageBuffers,
      (uint64_t) requirements->largest_storage_buffer_bytes,
      limits->maxStorageBufferRange,
      requirements->shared_memory_bytes,
      limits->maxComputeSharedMemorySize
    );

    return VK_ERROR_FEATURE_NOT_PRESENT;
  }

  uint32_t family_count = 0u;

  vkGetPhysicalDeviceQueueFamilyProperties (
    physical, &family_count, NULL
  );

  if (family_count == 0u)
  {
    fprintf (diagnostics, "REJECT_QUEUE no_queue_families\n");

    return VK_ERROR_FEATURE_NOT_PRESENT;
  }

  VkQueueFamilyProperties *families =
    (VkQueueFamilyProperties *) calloc (family_count, sizeof (*families));

  if (families == NULL) return VK_ERROR_OUT_OF_HOST_MEMORY;

  vkGetPhysicalDeviceQueueFamilyProperties (
    physical, &family_count, families
  );

  uint32_t family = UINT32_MAX;

  for (uint32_t i = 0u; i < family_count; i++)
  {
    if (families[i].queueCount > 0u
     && (families[i].queueFlags & VK_QUEUE_COMPUTE_BIT) != 0u)
    {
      family = i;

      break;
    }
  }

  free (families);

  if (family == UINT32_MAX)
  {
    fprintf (diagnostics, "REJECT_QUEUE no_compute_queue\n");

    return VK_ERROR_FEATURE_NOT_PRESENT;
  }

  memset (selected, 0, sizeof (*selected));

  selected->physical = physical;
  selected->index = index;
  selected->compute_queue_family = family;
  selected->properties = properties;

  vkGetPhysicalDeviceMemoryProperties (
    physical, &selected->memory_properties
  );

  fprintf (
    diagnostics,
    "ACCEPT_COMPUTE index=%" PRIu32 " queue_family=%" PRIu32
    " local_max=%" PRIu32
    " invocations_max=%" PRIu32
    " groups_max=%" PRIu32
    " shared_max=%" PRIu32
    " storage_range_max=%" PRIu32
    " descriptor_max=%" PRIu32
    " non_coherent_atom=%" PRIu64 "\n",
    index, family,
    limits->maxComputeWorkGroupSize[0],
    limits->maxComputeWorkGroupInvocations,
    limits->maxComputeWorkGroupCount[0],
    limits->maxComputeSharedMemorySize,
    limits->maxStorageBufferRange,
    limits->maxDescriptorSetStorageBuffers,
    (uint64_t) limits->nonCoherentAtomSize
  );

  return VK_SUCCESS;
}

/**
 * Select a compatible host-visible type, preferring coherent access.
 * This is a fixed functional priority, never a GPU vendor heuristic.
 */
VkResult vulkan_auto_select_memory_type (
  const VkPhysicalDeviceMemoryProperties *properties,
  uint32_t compatible_bits,
  uint32_t *index,
  VkBool32 *coherent)
{
  if (properties == NULL || index == NULL || coherent == NULL
   || properties->memoryTypeCount > 32u)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  const VkMemoryPropertyFlags visible =
    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
  const VkMemoryPropertyFlags coherent_flags =
    visible | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

  for (uint32_t pass = 0u; pass < 2u; pass++)
  {
    const VkMemoryPropertyFlags required =
      (pass == 0u) ? coherent_flags : visible;

    for (uint32_t candidate = 0u;
         candidate < properties->memoryTypeCount; candidate++)
    {
      const uint32_t bit = UINT32_C (1) << candidate;
      const VkMemoryPropertyFlags actual =
        properties->memoryTypes[candidate].propertyFlags;

      if ((compatible_bits & bit) != 0u
       && (actual & required) == required)
      {
        *index = candidate;
        *coherent = (actual & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
          != 0u ? VK_TRUE : VK_FALSE;

        return VK_SUCCESS;
      }
    }
  }

  return VK_ERROR_FEATURE_NOT_PRESENT;
}

/**
 * Round up a VkDeviceSize to an API-provided power-of-two alignment.
 * Return an error before overflowing a 64-bit allocation offset.
 */
static VkResult vulkan_auto_align_up (
  VkDeviceSize offset,
  VkDeviceSize alignment,
  VkDeviceSize *result)
{
  if (result == NULL || alignment == 0u
   || (alignment & (alignment - 1u)) != 0u)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  const VkDeviceSize adjustment = alignment - 1u;

  if (offset > UINT64_MAX - adjustment)
  {
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  }

  *result = (offset + adjustment) & ~adjustment;

  return VK_SUCCESS;
}

/**
 * Calculate a single allocation with four-or-more exact Vulkan buffer ranges.
 * The noncoherent path also keeps subranges on different cache atoms.
 */
VkResult vulkan_auto_plan_shared_memory (
  const VkPhysicalDeviceMemoryProperties *properties,
  VkDeviceSize non_coherent_atom_size,
  const VkMemoryRequirements *requirements,
  const VkDeviceSize *requested_sizes,
  uint32_t buffer_count,
  VkDeviceSize *offsets,
  vulkan_auto_memory_plan_t *plan,
  FILE *diagnostics)
{
  if (properties == NULL || requirements == NULL
   || requested_sizes == NULL || offsets == NULL || plan == NULL
   || diagnostics == NULL || buffer_count == 0u
   || non_coherent_atom_size == 0u
   || (non_coherent_atom_size & (non_coherent_atom_size - 1u)) != 0u)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  uint32_t compatible_bits = UINT32_MAX;

  for (uint32_t i = 0u; i < buffer_count; i++)
  {
    if (requested_sizes[i] == 0u
     || requirements[i].size < requested_sizes[i]
     || requirements[i].alignment == 0u
     || (requirements[i].alignment
       & (requirements[i].alignment - 1u)) != 0u)
    {
      fprintf (diagnostics, "REJECT_MEMORY_REQUIREMENTS index=%" PRIu32 "\n", i);

      return VK_ERROR_INITIALIZATION_FAILED;
    }

    compatible_bits &= requirements[i].memoryTypeBits;
  }

  uint32_t memory_type = UINT32_MAX;
  VkBool32 coherent = VK_FALSE;

  const VkResult selection = vulkan_auto_select_memory_type (
    properties, compatible_bits, &memory_type, &coherent
  );

  if (selection != VK_SUCCESS)
  {
    fprintf (
      diagnostics, "REJECT_MEMORY_TYPE compatible_bits=0x%08" PRIx32
      " need=HOST_VISIBLE fallback=staging-required\n",
      compatible_bits
    );

    return selection;
  }

  VkDeviceSize next = 0u;

  for (uint32_t i = 0u; i < buffer_count; i++)
  {
    VkDeviceSize alignment = requirements[i].alignment;

    if (coherent == VK_FALSE && alignment < non_coherent_atom_size)
    {
      alignment = non_coherent_atom_size;
    }

    VkDeviceSize start = 0u;
    const VkResult aligned = vulkan_auto_align_up (
      next, alignment, &start
    );

    if (aligned != VK_SUCCESS) return aligned;

    if (start > UINT64_MAX - requirements[i].size)
    {
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    offsets[i] = start;
    next = start + requirements[i].size;

    fprintf (
      diagnostics,
      "AUTO_BUFFER index=%" PRIu32
      " offset=%" PRIu64 " required=%" PRIu64
      " driver_alignment=%" PRIu64
      " effective_alignment=%" PRIu64
      " compatible_bits=0x%08" PRIx32 "\n",
      i, (uint64_t) start,
      (uint64_t) requirements[i].size,
      (uint64_t) requirements[i].alignment,
      (uint64_t) alignment,
      requirements[i].memoryTypeBits
    );
  }

  if (next == 0u) return VK_ERROR_INITIALIZATION_FAILED;

  *plan = (vulkan_auto_memory_plan_t)
  {
    .allocation_bytes = next,
    .memory_type_index = memory_type,
    .compatible_memory_type_bits = compatible_bits,
    .coherent = coherent,
    .non_coherent_atom_size = non_coherent_atom_size
  };

  fprintf (
    diagnostics,
    "AUTO_MEMORY type=%" PRIu32
    " coherent=%u bytes=%" PRIu64
    " compatible_bits=0x%08" PRIx32
    " atom=%" PRIu64 "\n",
    memory_type, coherent,
    (uint64_t) next, compatible_bits,
    (uint64_t) non_coherent_atom_size
  );

  return VK_SUCCESS;
}

/**
 * Apply the required whole-allocation flush or invalidate to mapped memory.
 * Host-coherent memory needs no explicit mapped-range synchronization.
 */
VkResult vulkan_auto_sync_mapped_memory (
  VkDevice device,
  VkDeviceMemory memory,
  const vulkan_auto_memory_plan_t *plan,
  VkBool32 gpu_to_host)
{
  if (device == VK_NULL_HANDLE || memory == VK_NULL_HANDLE
   || plan == NULL || plan->allocation_bytes == 0u)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  if (plan->coherent == VK_TRUE) return VK_SUCCESS;

  const VkMappedMemoryRange range =
  {
    .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
    .memory = memory,
    .offset = 0u,
    .size = VK_WHOLE_SIZE
  };

  if (gpu_to_host == VK_TRUE)
  {
    return vkInvalidateMappedMemoryRanges (device, 1u, &range);
  }

  return vkFlushMappedMemoryRanges (device, 1u, &range);
}
