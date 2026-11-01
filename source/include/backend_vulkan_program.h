#ifndef HC_BACKEND_VULKAN_PROGRAM_H
#define HC_BACKEND_VULKAN_PROGRAM_H

/**
 * Describe and validate an ordered Vulkan 1.0 Compute program.
 *
 * A program is an explicit sequence of immutable SPIR-V modules with
 * one source and one destination storage buffer per stage. Consecutive
 * stages must consume the previous stage's destination buffer. The
 * declaration contains exact local sizes and group counts, so no GPU
 * property or shader-interface behavior is inferred heuristically.
 *
 * This layer performs no Vulkan allocation, command recording or queue
 * submission. It validates declarations against VkPhysicalDeviceLimits
 * before a caller creates device resources.
 */

#include <stdint.h>
#include <stdio.h>
#include <vulkan/vulkan.h>

/**
 * Declare one immutable public Compute stage and its storage dependency.
 *
 * spirv points to the complete canonical little-endian SPIR-V module.
 * spirv_words is the exact uint32 word count. source_buffer_index and
 * destination_buffer_index are distinct indices owned by the caller.
 * local_size and group_count are exact VkCmdDispatch topology values.
 * name is a diagnostics-only stable identifier and must be non-empty.
 */
typedef struct backend_vulkan_program_stage
{
  const char *name;
  const uint32_t *spirv;
  uint32_t spirv_words;
  uint32_t source_buffer_index;
  uint32_t destination_buffer_index;
  uint32_t local_size[3];
  uint32_t group_count[3];
} backend_vulkan_program_stage_t;

/**
 * Declare a complete sequential storage-buffer Compute graph.
 *
 * stages contains stage_count explicit stage declarations. buffer_count
 * is the number of distinct caller-owned storage buffers. buffer_bytes
 * is the logical byte range exposed to every stage descriptor.
 *
 * The first stage may read any declared buffer. Every later stage must
 * read exactly the preceding destination. Every stage destination must
 * be unique, and no stage may write its own source buffer.
 */
typedef struct backend_vulkan_program
{
  const backend_vulkan_program_stage_t *stages;
  uint32_t stage_count;
  uint32_t buffer_count;
  VkDeviceSize buffer_bytes;
} backend_vulkan_program_t;

/**
 * Validate one Vulkan 1.0 Compute graph against exact physical limits.
 *
 * Checks null inputs, stage and buffer counts, canonical SPIR-V 1.0
 * headers, storage dependencies, destination uniqueness, descriptor
 * range, local dimensions, total local invocations and dispatch group
 * counts. Requires each stage to use exactly two storage descriptors.
 *
 * Returns VK_SUCCESS only when every declaration is internally valid
 * and within the supplied VkPhysicalDeviceLimits. Structural errors
 * return VK_ERROR_INITIALIZATION_FAILED; unsupported device limits
 * return VK_ERROR_FEATURE_NOT_PRESENT. No Vulkan driver function is
 * called and no command buffer is modified.
 */
VkResult backend_vulkan_program_validate (
  const backend_vulkan_program_t *program,
  const VkPhysicalDeviceLimits *limits,
  FILE *diagnostics);

#endif // HC_BACKEND_VULKAN_PROGRAM_H
