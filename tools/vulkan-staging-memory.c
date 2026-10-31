/**
 * Implement vendor-independent Vulkan 1.0 GPU memory and transfer selection.
 *
 * Storage types are chosen from actual VkMemoryRequirements.memoryTypeBits
 * and physical-device memory properties. The transfer mode is determined by
 * host visibility, with an optional explicit staging test override.
 */

#include "vulkan-staging-memory.h"

#include <inttypes.h>
#include <stdint.h>

/**
 * Prefer DEVICE_LOCAL for compute storage while respecting exact type bits.
 * A compatible nonlocal type is accepted when no local type is available.
 */
VkResult vulkan_auto_select_gpu_memory_type (
  const VkPhysicalDeviceMemoryProperties *properties,
  uint32_t compatible_bits,
  uint32_t *index)
{
  if (properties == NULL || index == NULL
   || properties->memoryTypeCount > 32u)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  for (uint32_t pass = 0u; pass < 2u; pass++)
  {
    for (uint32_t candidate = 0u;
         candidate < properties->memoryTypeCount; candidate++)
    {
      const uint32_t bit = UINT32_C (1) << candidate;

      if ((compatible_bits & bit) == 0u) continue;

      const VkMemoryPropertyFlags flags =
        properties->memoryTypes[candidate].propertyFlags;

      if (pass == 0u && (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0u)
      {
        continue;
      }

      *index = candidate;

      return VK_SUCCESS;
    }
  }

  return VK_ERROR_FEATURE_NOT_PRESENT;
}

/**
 * Select direct mapping for two mappable compute buffers, or require staging.
 *
 * Both buffer bitmasks must first admit some compatible memory type. Direct
 * mode requires host-visible memory for each storage buffer. Staging mode
 * permits GPU-only storage memory, with the caller responsible for transfer
 * buffer creation and explicit Vulkan synchronization.
 */
VkResult vulkan_auto_choose_transfer_mode (
  const VkPhysicalDeviceMemoryProperties *properties,
  uint32_t input_storage_bits,
  uint32_t output_storage_bits,
  VkBool32 force_staging,
  vulkan_auto_transfer_mode_t *mode,
  FILE *diagnostics)
{
  if (properties == NULL || mode == NULL || diagnostics == NULL)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  uint32_t input_type = UINT32_MAX;
  uint32_t output_type = UINT32_MAX;

  const VkResult input_supported = vulkan_auto_select_gpu_memory_type (
    properties, input_storage_bits, &input_type
  );
  const VkResult output_supported = vulkan_auto_select_gpu_memory_type (
    properties, output_storage_bits, &output_type
  );

  if (input_supported != VK_SUCCESS || output_supported != VK_SUCCESS)
  {
    fprintf (
      diagnostics,
      "REJECT_STORAGE_MEMORY input_bits=0x%08" PRIx32
      " output_bits=0x%08" PRIx32
      " input_result=%d output_result=%d\n",
      input_storage_bits, output_storage_bits,
      input_supported, output_supported
    );

    return (input_supported != VK_SUCCESS)
      ? input_supported : output_supported;
  }

  uint32_t mapped_input = UINT32_MAX;
  uint32_t mapped_output = UINT32_MAX;
  VkBool32 input_coherent = VK_FALSE;
  VkBool32 output_coherent = VK_FALSE;

  const VkResult input_visible = vulkan_auto_select_memory_type (
    properties, input_storage_bits, &mapped_input, &input_coherent
  );
  const VkResult output_visible = vulkan_auto_select_memory_type (
    properties, output_storage_bits, &mapped_output, &output_coherent
  );

  if (force_staging == VK_FALSE
   && input_visible == VK_SUCCESS
   && output_visible == VK_SUCCESS)
  {
    *mode = VULKAN_AUTO_TRANSFER_DIRECT;

    fprintf (
      diagnostics,
      "AUTO_TRANSFER mode=direct input_memory_type=%" PRIu32
      " output_memory_type=%" PRIu32
      " input_coherent=%u output_coherent=%u\n",
      mapped_input, mapped_output, input_coherent, output_coherent
    );
  }
  else
  {
    *mode = VULKAN_AUTO_TRANSFER_STAGING;

    fprintf (
      diagnostics,
      "AUTO_TRANSFER mode=staging input_gpu_type=%" PRIu32
      " output_gpu_type=%" PRIu32
      " input_visible=%u output_visible=%u forced=%u\n",
      input_type, output_type,
      input_visible == VK_SUCCESS,
      output_visible == VK_SUCCESS,
      force_staging
    );
  }

  return VK_SUCCESS;
}
