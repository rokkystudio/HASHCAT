/**
 * Exercise the host-only Vulkan Compute program graph validator.
 *
 * Uses the exact production source/include backend_vulkan_program
 * implementation with the two embedded public SPIR-V 1.0 modules.
 * All device limits are explicit local test data. No Vulkan loader,
 * VkInstance, GPU command or HASHCAT candidate processing is used.
 */

#include "backend_vulkan_program.h"
#include "backend_vulkan_public_spirv.h"
#include "backend_vulkan_public_stage2_spirv.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/**
 * Return a complete explicit Vulkan Compute-limit set for validation.
 *
 * Values are deliberately sufficient for the two public 32-lane,
 * two-workgroup shaders and a 256-byte storage range. They are test
 * declarations, not properties inferred from the executing device.
 */
static VkPhysicalDeviceLimits program_test_limits (void)
{
  VkPhysicalDeviceLimits limits = { 0 };

  limits.maxStorageBufferRange = 65536u;
  limits.maxPerStageDescriptorStorageBuffers = 8u;
  limits.maxComputeWorkGroupInvocations = 1024u;

  for (uint32_t axis = 0u; axis < 3u; axis++)
  {
    limits.maxComputeWorkGroupCount[axis] = 65535u;
    limits.maxComputeWorkGroupSize[axis] = 1024u;
  }

  return limits;
}

/**
 * Build the exact two-stage public program declaration used by tests.
 *
 * Stage zero maps buffer 0 to 1 and stage one maps 1 to 2. Both use
 * the original immutable embedded SPIR-V 1.0 modules, local size
 * 32x1x1 and two workgroups in X. The returned struct references
 * caller-owned stage storage that remains valid for the test scope.
 */
static backend_vulkan_program_t program_test_valid (
  backend_vulkan_program_stage_t stages[2])
{
  stages[0] = (backend_vulkan_program_stage_t)
  {
    .name = "public_u32_transform",
    .spirv = backend_vulkan_public_spirv,
    .spirv_words = sizeof (backend_vulkan_public_spirv) / sizeof (uint32_t),
    .source_buffer_index = 0u,
    .destination_buffer_index = 1u,
    .local_size = { 32u, 1u, 1u },
    .group_count = { 2u, 1u, 1u }
  };

  stages[1] = (backend_vulkan_program_stage_t)
  {
    .name = "public_u32_rotate",
    .spirv = backend_vulkan_public_stage2_spirv,
    .spirv_words =
      sizeof (backend_vulkan_public_stage2_spirv) / sizeof (uint32_t),
    .source_buffer_index = 1u,
    .destination_buffer_index = 2u,
    .local_size = { 32u, 1u, 1u },
    .group_count = { 2u, 1u, 1u }
  };

  return (backend_vulkan_program_t)
  {
    .stages = stages,
    .stage_count = 2u,
    .buffer_count = 3u,
    .buffer_bytes = 256u
  };
}

/**
 * Require one validator call to produce the exact expected VkResult.
 *
 * Emits a stable pass marker containing the case name and result.
 * Any unexpected acceptance or error returns nonzero to main.
 */
static int program_expect (
  const char *name,
  const backend_vulkan_program_t *program,
  const VkPhysicalDeviceLimits *limits,
  VkResult expected)
{
  const VkResult actual = backend_vulkan_program_validate (
    program, limits, stdout
  );

  if (actual != expected)
  {
    fprintf (
      stderr,
      "BACKEND_VULKAN_PROGRAM_CASE_FAILED"
      " case=%s actual=%d expected=%d\n",
      name, actual, expected
    );

    return 1;
  }

  fprintf (
    stdout,
    "BACKEND_VULKAN_PROGRAM_CASE_PASS"
    " case=%s result=%d\n",
    name, actual
  );

  return 0;
}

/**
 * Validate the accepted graph and exact malformed/unsupported cases.
 *
 * The method copies declarations before each mutation so every case
 * starts from the same valid baseline. SPIR-V header corruption uses
 * local word arrays and never changes generated production headers.
 */
int main (void)
{
  backend_vulkan_program_stage_t stages[2] = { 0 };
  backend_vulkan_program_t program = program_test_valid (stages);
  VkPhysicalDeviceLimits limits = program_test_limits ();

  if (program_expect (
    "two_stage_valid", &program, &limits, VK_SUCCESS
  ) != 0) return 1;

  backend_vulkan_program_stage_t one_stage[2] = { 0 };
  backend_vulkan_program_t single = program_test_valid (one_stage);

  single.stage_count = 1u;
  single.buffer_count = 2u;

  if (program_expect (
    "one_stage_valid", &single, &limits, VK_SUCCESS
  ) != 0) return 1;

  if (program_expect (
    "null_program", NULL, &limits, VK_ERROR_INITIALIZATION_FAILED
  ) != 0) return 1;

  if (program_expect (
    "null_limits", &program, NULL, VK_ERROR_INITIALIZATION_FAILED
  ) != 0) return 1;

  backend_vulkan_program_stage_t bad_stages[2] = { 0 };
  backend_vulkan_program_t bad = program_test_valid (bad_stages);

  bad_stages[1].source_buffer_index = 0u;

  if (program_expect (
    "broken_dependency", &bad, &limits,
    VK_ERROR_INITIALIZATION_FAILED
  ) != 0) return 1;

  bad = program_test_valid (bad_stages);
  bad_stages[1].destination_buffer_index = 1u;

  if (program_expect (
    "source_destination_alias", &bad, &limits,
    VK_ERROR_INITIALIZATION_FAILED
  ) != 0) return 1;

  backend_vulkan_program_stage_t repeated_stages[3] = { 0 };

  repeated_stages[0] = stages[0];
  repeated_stages[1] = stages[1];
  repeated_stages[2] = stages[1];
  repeated_stages[2].name = "public_u32_repeat_destination";
  repeated_stages[2].source_buffer_index = 2u;
  repeated_stages[2].destination_buffer_index = 1u;

  const backend_vulkan_program_t repeated =
  {
    .stages = repeated_stages,
    .stage_count = 3u,
    .buffer_count = 3u,
    .buffer_bytes = 256u
  };

  if (program_expect (
    "reused_destination", &repeated, &limits,
    VK_ERROR_INITIALIZATION_FAILED
  ) != 0) return 1;

  bad = program_test_valid (bad_stages);
  bad_stages[1].group_count[0] = 0u;

  if (program_expect (
    "zero_group", &bad, &limits,
    VK_ERROR_INITIALIZATION_FAILED
  ) != 0) return 1;

  bad = program_test_valid (bad_stages);
  bad_stages[0].local_size[0] = 0u;

  if (program_expect (
    "zero_local", &bad, &limits,
    VK_ERROR_INITIALIZATION_FAILED
  ) != 0) return 1;

  bad = program_test_valid (bad_stages);
  bad_stages[0].group_count[0] = limits.maxComputeWorkGroupCount[0] + 1u;

  if (program_expect (
    "group_limit", &bad, &limits,
    VK_ERROR_FEATURE_NOT_PRESENT
  ) != 0) return 1;

  bad = program_test_valid (bad_stages);
  bad_stages[0].local_size[0] = 33u;
  limits.maxComputeWorkGroupInvocations = 32u;

  if (program_expect (
    "invocation_limit", &bad, &limits,
    VK_ERROR_FEATURE_NOT_PRESENT
  ) != 0) return 1;

  limits = program_test_limits ();
  bad = program_test_valid (bad_stages);
  bad.buffer_bytes = (VkDeviceSize) limits.maxStorageBufferRange + 1u;

  if (program_expect (
    "storage_range", &bad, &limits,
    VK_ERROR_FEATURE_NOT_PRESENT
  ) != 0) return 1;

  bad = program_test_valid (bad_stages);
  limits.maxPerStageDescriptorStorageBuffers = 1u;

  if (program_expect (
    "descriptor_limit", &bad, &limits,
    VK_ERROR_FEATURE_NOT_PRESENT
  ) != 0) return 1;

  limits = program_test_limits ();
  bad = program_test_valid (bad_stages);

  uint32_t bad_magic[
    sizeof (backend_vulkan_public_spirv) / sizeof (uint32_t)
  ];

  memcpy (
    bad_magic, backend_vulkan_public_spirv,
    sizeof (backend_vulkan_public_spirv)
  );

  bad_magic[0] = 0u;
  bad_stages[0].spirv = bad_magic;

  if (program_expect (
    "spirv_magic", &bad, &limits,
    VK_ERROR_INITIALIZATION_FAILED
  ) != 0) return 1;

  bad = program_test_valid (bad_stages);

  uint32_t bad_version[
    sizeof (backend_vulkan_public_stage2_spirv) / sizeof (uint32_t)
  ];

  memcpy (
    bad_version, backend_vulkan_public_stage2_spirv,
    sizeof (backend_vulkan_public_stage2_spirv)
  );

  bad_version[1] = UINT32_C (0x00010100);
  bad_stages[1].spirv = bad_version;

  if (program_expect (
    "spirv_version", &bad, &limits,
    VK_ERROR_INITIALIZATION_FAILED
  ) != 0) return 1;

  bad = program_test_valid (bad_stages);
  bad_stages[0].name = "";

  if (program_expect (
    "empty_name", &bad, &limits,
    VK_ERROR_INITIALIZATION_FAILED
  ) != 0) return 1;

  fprintf (
    stdout,
    "BACKEND_VULKAN_PROGRAM_DIAGNOSTIC_PASS"
    " accepted=2 structural_rejects=10 limit_rejects=4"
    " driver_calls=0 gpu_dispatches=0"
    " password_inputs=0 scheduled=0\n"
  );

  return 0;
}
