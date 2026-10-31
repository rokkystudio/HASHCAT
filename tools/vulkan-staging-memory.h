#ifndef VULKAN_STAGING_MEMORY_H
#define VULKAN_STAGING_MEMORY_H

/**
 * Choose explicit GPU storage and CPU staging memory for portable Vulkan.
 *
 * The selection uses the existing Vulkan 1.0 capability API and actual memory
 * type bitmasks, without looking at GPU vendor, driver version or phone model.
 */

#include "vulkan-auto-config.h"

/**
 * Define whether compute buffers can be accessed directly or require copies.
 *
 * A direct path maps compatible storage-buffer memory. A staging path
 * allocates separate GPU compute and CPU-visible transfer buffers.
 */
typedef enum vulkan_auto_transfer_mode
{
  VULKAN_AUTO_TRANSFER_DIRECT = 0,
  VULKAN_AUTO_TRANSFER_STAGING = 1
} vulkan_auto_transfer_mode_t;

/**
 * Select a compatible memory type for GPU storage or transfer usage.
 *
 * A memory type with DEVICE_LOCAL is preferred. When no compatible
 * DEVICE_LOCAL type exists, the first compatible type is selected. An empty
 * intersection is an explicit VK_ERROR_FEATURE_NOT_PRESENT error.
 */
VkResult vulkan_auto_select_gpu_memory_type (
  const VkPhysicalDeviceMemoryProperties *properties,
  uint32_t compatible_bits,
  uint32_t *index);

/**
 * Decide the transfer path from two real storage-buffer requirements.
 *
 * Without force_staging, direct mapping is selected when both compute
 * buffers support host-visible memory. Otherwise, separate staging buffers
 * are required. With force_staging, the caller exercises the copy path even
 * if the storage buffers can be mapped directly.
 *
 * The caller must separately create and verify the actual transfer buffers
 * before allocating their host-visible memory. This function does not
 * create Vulkan objects or choose GPU behavior from the model name.
 */
VkResult vulkan_auto_choose_transfer_mode (
  const VkPhysicalDeviceMemoryProperties *properties,
  uint32_t input_storage_bits,
  uint32_t output_storage_bits,
  VkBool32 force_staging,
  vulkan_auto_transfer_mode_t *mode,
  FILE *diagnostics);

#endif
