#ifndef VULKAN_AUTO_CONFIG_H
#define VULKAN_AUTO_CONFIG_H

/**
 * Select Vulkan Compute capabilities and storage memory from runtime API data.
 *
 * These declarations are independent of GPU manufacturers, device names
 * and Android versions. Workload requirements are supplied by the shader
 * and dispatch metadata, not estimated from a physical device's model.
 */

#include <vulkan/vulkan.h>

#include <stdint.h>
#include <stdio.h>

/**
 * Declare the exact requirements of one known Vulkan compute workload.
 *
 * local_size and group_count describe the shader's local invocation topology
 * and the number of groups in a dispatch. storage_buffer_count is the
 * descriptor requirement; largest_storage_buffer_bytes is the largest
 * descriptor-visible buffer range. shared_memory_bytes is the shader's
 * statically declared workgroup storage.
 */
typedef struct vulkan_auto_compute_requirements
{
  uint32_t local_size[3];
  uint32_t group_count[3];
  uint32_t storage_buffer_count;
  VkDeviceSize largest_storage_buffer_bytes;
  uint32_t shared_memory_bytes;
} vulkan_auto_compute_requirements_t;

/**
 * Represent an API-verified physical device and compute queue family.
 *
 * The Vulkan handles remain owned by the caller's instance and are not
 * destroyed by the capability checker.
 */
typedef struct vulkan_auto_device
{
  VkPhysicalDevice physical;
  uint32_t index;
  uint32_t compute_queue_family;
  VkPhysicalDeviceProperties properties;
  VkPhysicalDeviceMemoryProperties memory_properties;
} vulkan_auto_device_t;

/**
 * Describe the complete result of a compatible Vulkan memory layout.
 *
 * Buffer offsets are computed from the driver's own requirements, and the
 * selected memory type is host-visible. If coherent is VK_FALSE the caller
 * must synchronize mapped writes and reads using the Vulkan cache APIs.
 */
typedef struct vulkan_auto_memory_plan
{
  VkDeviceSize allocation_bytes;
  uint32_t memory_type_index;
  uint32_t compatible_memory_type_bits;
  VkBool32 coherent;
  VkDeviceSize non_coherent_atom_size;
} vulkan_auto_memory_plan_t;

/**
 * Check one physical device against exact compute and shader requirements.
 *
 * Returns VK_SUCCESS and writes a fully populated selected device on
 * success. Returns VK_ERROR_FEATURE_NOT_PRESENT when a required limit,
 * API version or compute queue is unavailable. Other failures are reported
 * explicitly. Diagnostics use a caller-supplied stream.
 */
VkResult vulkan_auto_check_device (
  VkPhysicalDevice physical,
  uint32_t index,
  const vulkan_auto_compute_requirements_t *requirements,
  vulkan_auto_device_t *selected,
  FILE *diagnostics);

/**
 * Select a compatible host-visible memory type by the driver's bitmask.
 *
 * First selects a compatible HOST_VISIBLE|HOST_COHERENT type if one exists;
 * otherwise selects a compatible HOST_VISIBLE type requiring explicit
 * flush/invalidate. The first matching Vulkan memory-type index is used
 * within each exact priority class. No vendor- or device-specific mapping
 * is involved. Returns VK_ERROR_FEATURE_NOT_PRESENT if neither exists.
 */
VkResult vulkan_auto_select_memory_type (
  const VkPhysicalDeviceMemoryProperties *properties,
  uint32_t compatible_bits,
  uint32_t *index,
  VkBool32 *coherent);

/**
 * Create an aligned, nonoverlapping plan for shared VkDeviceMemory storage.
 *
 * Each buffer's exact VkMemoryRequirements and requested byte count are
 * supplied by the caller. The planner intersects memoryTypeBits, verifies
 * alignment and integer ranges, and chooses a host-visible memory type.
 * Noncoherent ranges are additionally aligned to nonCoherentAtomSize to
 * prevent multiple buffers sharing a host-cache atom.
 *
 * Writes one offset per buffer and a complete allocation plan on success.
 * Does not allocate Vulkan device memory or silently use staging buffers.
 */
VkResult vulkan_auto_plan_shared_memory (
  const VkPhysicalDeviceMemoryProperties *properties,
  VkDeviceSize non_coherent_atom_size,
  const VkMemoryRequirements *requirements,
  const VkDeviceSize *requested_sizes,
  uint32_t buffer_count,
  VkDeviceSize *offsets,
  vulkan_auto_memory_plan_t *plan,
  FILE *diagnostics);

/**
 * Synchronize an entire mapped VkDeviceMemory allocation if noncoherent.
 *
 * Call with gpu_to_host=VK_FALSE after CPU writes and before GPU submission
 * to flush the allocation. Call with gpu_to_host=VK_TRUE after a successful
 * GPU fence wait and a shader-write-to-host-read dependency to invalidate
 * the allocation. Coherent memory returns VK_SUCCESS without cache calls.
 *
 * The memory must already be mapped with offset zero for the whole size.
 * Full-allocation ranges avoid partial nonCoherentAtomSize rounding errors.
 */
VkResult vulkan_auto_sync_mapped_memory (
  VkDevice device,
  VkDeviceMemory memory,
  const vulkan_auto_memory_plan_t *plan,
  VkBool32 gpu_to_host);

#endif
