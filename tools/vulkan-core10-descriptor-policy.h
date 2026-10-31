#ifndef VULKAN_CORE10_DESCRIPTOR_POLICY_H
#define VULKAN_CORE10_DESCRIPTOR_POLICY_H

/**
 * Validate explicit Vulkan 1.0 multi-set storage/uniform buffer descriptors.
 *
 * The policy uses the currently selected physical device's Vulkan limits.
 * It neither depends on a GPU vendor nor infers bindings from SPIR-V.
 * Unsupported descriptor types and invalid ranges return exact errors.
 */

#include <vulkan/vulkan.h>

#include <stdint.h>
#include <stdio.h>

/**
 * Describe one known Vulkan buffer's logical byte size and usage.
 */
typedef struct vulkan_core10_descriptor_buffer
{
  VkDeviceSize size;
  VkBufferUsageFlags usage;
} vulkan_core10_descriptor_buffer_t;

/**
 * Describe one exact VkDescriptorBufferInfo in a numbered descriptor set.
 *
 * The Vulkan shader interface supplies set and binding numbers. Type is
 * exactly STORAGE_BUFFER or UNIFORM_BUFFER in this runtime. Offsets and
 * ranges are caller-supplied, bounded by the selected physical limits.
 */
typedef struct vulkan_core10_descriptor_binding
{
  uint32_t set;
  uint32_t binding;
  uint32_t buffer_index;
  VkDescriptorType type;
  VkDeviceSize offset;
  VkDeviceSize range;
} vulkan_core10_descriptor_binding_t;

/**
 * Count the two supported descriptor types for one Vulkan pipeline layout.
 */
typedef struct vulkan_core10_descriptor_counts
{
  uint32_t storage;
  uint32_t uniform;
} vulkan_core10_descriptor_counts_t;

/**
 * Validate one complete explicitly defined Vulkan 1.0 descriptor interface.
 *
 * Requires 1..maxBoundDescriptorSets sets and nonempty buffer/binding lists.
 * Each set has at least one declared binding. Detects duplicate (set,binding),
 * inaccessible buffer indices, wrong VkBufferUsageFlags, invalid offsets,
 * invalid sizes and unsupported VkDescriptorType.
 *
 * Checks maxDescriptorSetStorageBuffers/maxDescriptorSetUniformBuffers,
 * maxPerStageDescriptorStorageBuffers/maxPerStageDescriptorUniformBuffers,
 * maxPerStageResources, minStorageBufferOffsetAlignment and
 * minUniformBufferOffsetAlignment, and both maximum descriptor ranges.
 * The per-stage limits conservatively apply to the entire declared layout;
 * the application must separately validate actual shader workgroup limits.
 *
 * Returns VK_ERROR_INITIALIZATION_FAILED for malformed input or duplicate
 * bindings, VK_ERROR_FEATURE_NOT_PRESENT for unsupported descriptor types,
 * capabilities or ranges, and VK_SUCCESS for an exact supported contract.
 * A successful result writes counts; failures leave counts unchanged.
 */
VkResult vulkan_core10_descriptor_validate (
  const VkPhysicalDeviceLimits *limits,
  const vulkan_core10_descriptor_buffer_t *buffers,
  uint32_t buffer_count,
  const vulkan_core10_descriptor_binding_t *bindings,
  uint32_t binding_count,
  uint32_t set_count,
  vulkan_core10_descriptor_counts_t *counts,
  FILE *diagnostics);

#endif
