/**
 * Test exact Vulkan 1.0 multi-set descriptor validation without a GPU driver.
 *
 * Exercises one/two-set storage and std140 uniform contracts with sparse
 * bindings, and explicitly invalid descriptor types, limits, alignment,
 * duplicate slots and buffer ranges using synthetic device capabilities.
 */

#include "vulkan-core10-descriptor-policy.h"

#include <stdio.h>
#include <string.h>

#define ASSERT(condition) \
  do \
  { \
    if (!(condition)) \
    { \
      fprintf (stderr, \
        "DESCRIPTOR_HOST_FAILED line=%d requirement=%s\n", \
        __LINE__, #condition); \
      return 1; \
    } \
  } while (0)

/**
 * Supply conservative Vulkan 1.0 limits for a public three-binding shader.
 */
static VkPhysicalDeviceLimits device_limits (void)
{
  VkPhysicalDeviceLimits limits = { 0 };

  limits.maxBoundDescriptorSets = 4u;
  limits.maxPerStageResources = 8u;
  limits.maxDescriptorSetStorageBuffers = 4u;
  limits.maxPerStageDescriptorStorageBuffers = 4u;
  limits.maxDescriptorSetUniformBuffers = 2u;
  limits.maxPerStageDescriptorUniformBuffers = 2u;
  limits.maxStorageBufferRange = 16384u;
  limits.maxUniformBufferRange = 256u;
  limits.minStorageBufferOffsetAlignment = 16u;
  limits.minUniformBufferOffsetAlignment = 64u;

  return limits;
}

/**
 * Test exact single-set and two-set descriptors with sparse binding numbers.
 */
static int test_valid (void)
{
  VkPhysicalDeviceLimits limits = device_limits ();
  const vulkan_core10_descriptor_buffer_t buffers[3] =
  {
    { 8192u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT },
    { 8192u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT },
    { 256u, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT }
  };
  vulkan_core10_descriptor_binding_t bindings[3] =
  {
    { 0u, 2u, 0u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0u, 8192u },
    { 0u, 7u, 1u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0u, 8192u },
    { 0u, 4u, 2u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0u, 16u }
  };
  vulkan_core10_descriptor_counts_t counts = { 0 };

  ASSERT (vulkan_core10_descriptor_validate (
    &limits, buffers, 3u, bindings, 3u, 1u,
    &counts, stdout
  ) == VK_SUCCESS);

  ASSERT (counts.storage == 2u && counts.uniform == 1u);
  puts ("PASS_DESCRIPTOR_ONE_SET sparse_storage_and_uniform");

  bindings[1].set = 1u;
  bindings[1].binding = 1u;
  bindings[2].set = 1u;
  bindings[2].binding = 5u;

  ASSERT (vulkan_core10_descriptor_validate (
    &limits, buffers, 3u, bindings, 3u, 2u,
    &counts, stdout
  ) == VK_SUCCESS);

  ASSERT (counts.storage == 2u && counts.uniform == 1u);
  puts ("PASS_DESCRIPTOR_TWO_SETS independent_layouts_and_types");

  return 0;
}

/**
 * Assert unsupported device limits and missing binding capabilities reject.
 */
static int test_limits (void)
{
  VkPhysicalDeviceLimits limits = device_limits ();
  const vulkan_core10_descriptor_buffer_t buffers[3] =
  {
    { 8192u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT },
    { 8192u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT },
    { 256u, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT }
  };
  const vulkan_core10_descriptor_binding_t bindings[3] =
  {
    { 0u, 2u, 0u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0u, 8192u },
    { 1u, 1u, 1u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0u, 8192u },
    { 1u, 5u, 2u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0u, 16u }
  };
  vulkan_core10_descriptor_counts_t counts = { 99u, 100u };

  limits.maxBoundDescriptorSets = 1u;

  ASSERT (vulkan_core10_descriptor_validate (
    &limits, buffers, 3u, bindings, 3u, 2u,
    &counts, stdout
  ) == VK_ERROR_FEATURE_NOT_PRESENT);

  limits = device_limits ();
  limits.maxPerStageResources = 2u;

  ASSERT (vulkan_core10_descriptor_validate (
    &limits, buffers, 3u, bindings, 3u, 2u,
    &counts, stdout
  ) == VK_ERROR_FEATURE_NOT_PRESENT);

  limits = device_limits ();
  limits.maxDescriptorSetUniformBuffers = 0u;

  ASSERT (vulkan_core10_descriptor_validate (
    &limits, buffers, 3u, bindings, 3u, 2u,
    &counts, stdout
  ) == VK_ERROR_FEATURE_NOT_PRESENT);

  limits = device_limits ();
  limits.maxPerStageDescriptorStorageBuffers = 1u;

  ASSERT (vulkan_core10_descriptor_validate (
    &limits, buffers, 3u, bindings, 3u, 2u,
    &counts, stdout
  ) == VK_ERROR_FEATURE_NOT_PRESENT);

  ASSERT (counts.storage == 99u && counts.uniform == 100u);
  puts ("PASS_DESCRIPTOR_LIMITS bound_sets_stage_resources_storage_uniform");

  return 0;
}

/**
 * Detect unsupported descriptor types, missing buffer usage and byte limits.
 */
static int test_ranges (void)
{
  VkPhysicalDeviceLimits limits = device_limits ();
  vulkan_core10_descriptor_buffer_t buffers[3] =
  {
    { 8192u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT },
    { 8192u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT },
    { 256u, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT }
  };
  vulkan_core10_descriptor_binding_t bindings[3] =
  {
    { 0u, 2u, 0u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0u, 8192u },
    { 1u, 1u, 1u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0u, 8192u },
    { 1u, 5u, 2u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0u, 16u }
  };
  vulkan_core10_descriptor_counts_t counts = { 0 };

  bindings[2].type = VK_DESCRIPTOR_TYPE_SAMPLER;

  ASSERT (vulkan_core10_descriptor_validate (
    &limits, buffers, 3u, bindings, 3u, 2u,
    &counts, stdout
  ) == VK_ERROR_FEATURE_NOT_PRESENT);
  bindings[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;

  buffers[2].usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

  ASSERT (vulkan_core10_descriptor_validate (
    &limits, buffers, 3u, bindings, 3u, 2u,
    &counts, stdout
  ) == VK_ERROR_FEATURE_NOT_PRESENT);
  buffers[2].usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

  bindings[2].offset = 16u;

  ASSERT (vulkan_core10_descriptor_validate (
    &limits, buffers, 3u, bindings, 3u, 2u,
    &counts, stdout
  ) == VK_ERROR_FEATURE_NOT_PRESENT);
  bindings[2].offset = 0u;

  limits.maxUniformBufferRange = 8u;

  ASSERT (vulkan_core10_descriptor_validate (
    &limits, buffers, 3u, bindings, 3u, 2u,
    &counts, stdout
  ) == VK_ERROR_FEATURE_NOT_PRESENT);
  limits = device_limits ();

  bindings[2].range = 300u;

  ASSERT (vulkan_core10_descriptor_validate (
    &limits, buffers, 3u, bindings, 3u, 2u,
    &counts, stdout
  ) == VK_ERROR_INITIALIZATION_FAILED);
  bindings[2].range = 16u;

  bindings[2].buffer_index = 3u;

  ASSERT (vulkan_core10_descriptor_validate (
    &limits, buffers, 3u, bindings, 3u, 2u,
    &counts, stdout
  ) == VK_ERROR_INITIALIZATION_FAILED);
  bindings[2].buffer_index = 2u;

  puts ("PASS_DESCRIPTOR_RANGES unsupported_type_wrong_usage_alignment_length");

  return 0;
}

/**
 * Detect malformed layout interfaces before any Vulkan driver object exists.
 */
static int test_structure (void)
{
  VkPhysicalDeviceLimits limits = device_limits ();
  const vulkan_core10_descriptor_buffer_t buffers[3] =
  {
    { 8192u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT },
    { 8192u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT },
    { 256u, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT }
  };
  vulkan_core10_descriptor_binding_t bindings[3] =
  {
    { 0u, 2u, 0u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0u, 8192u },
    { 1u, 1u, 1u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0u, 8192u },
    { 1u, 5u, 2u, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0u, 16u }
  };
  vulkan_core10_descriptor_counts_t counts = { 0 };

  bindings[2].binding = 1u;

  ASSERT (vulkan_core10_descriptor_validate (
    &limits, buffers, 3u, bindings, 3u, 2u,
    &counts, stdout
  ) == VK_ERROR_INITIALIZATION_FAILED);
  bindings[2].binding = 5u;

  ASSERT (vulkan_core10_descriptor_validate (
    &limits, buffers, 3u, bindings, 3u, 3u,
    &counts, stdout
  ) == VK_ERROR_INITIALIZATION_FAILED);

  bindings[2].set = 2u;

  ASSERT (vulkan_core10_descriptor_validate (
    &limits, buffers, 3u, bindings, 3u, 2u,
    &counts, stdout
  ) == VK_ERROR_INITIALIZATION_FAILED);
  bindings[2].set = 1u;

  ASSERT (vulkan_core10_descriptor_validate (
    &limits, buffers, 3u, bindings, 0u, 2u,
    &counts, stdout
  ) == VK_ERROR_INITIALIZATION_FAILED);

  puts ("PASS_DESCRIPTOR_STRUCTURE duplicate_empty_set_invalid_set_empty_input");

  return 0;
}

/**
 * Run all CPU-only Vulkan descriptor layout and range compatibility tests.
 */
int main (void)
{
  if (test_valid () != 0) return 1;
  if (test_limits () != 0) return 1;
  if (test_ranges () != 0) return 1;
  if (test_structure () != 0) return 1;

  puts ("PASS_DESCRIPTOR_POLICY no_vulkan_driver no_model_profiles");

  return 0;
}
