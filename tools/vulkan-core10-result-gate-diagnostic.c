/**
 * Verify fail-closed Vulkan 1.0 submission decisions on real Android GPUs.
 *
 * Real VkDevice sessions first report genuine VK_NOT_READY, VK_TIMEOUT
 * and VK_SUCCESS results from Vulkan fence/transfer operations and
 * complete two public integer copies. The application then feeds one
 * explicitly labeled synthetic VkResult into a separate host-only
 * policy to verify that the same logical device is never submitted to
 * again after a terminal classification. The GPU driver is not asked
 * to fail, lose a device, execute a shader or process HASHCAT data.
 */

#include "vulkan-core10-device-catalog.h"
#include "vulkan-core10-result-gate.h"
#include "vulkan-core10-transfer-session.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/**
 * Provide the exact test-only VkResult for a declared terminal mode.
 *
 * DEVICE_LOST and DEVICE_MEMORY are synthetic negative VkResults.
 * UNEXPECTED_POSITIVE supplies VK_INCOMPLETE at a transfer site,
 * which the policy classifies as VK_ERROR_INITIALIZATION_FAILED.
 * None of these test values are claimed to originate from the GPU.
 */
static VkResult result_gate_test_error (
  const char *mode)
{
  if (strcmp (mode, "device-lost") == 0)
  {
    return VK_ERROR_DEVICE_LOST;
  }

  if (strcmp (mode, "device-memory") == 0)
  {
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  }

  return VK_INCOMPLETE;
}

/**
 * Verify two real GPU transfers before one host-only terminal transition.
 *
 * Every copy is authorized by a distinct gate permission and followed
 * by recording the actual successful Vulkan transfer and fence status.
 * The caller's output array is checked at all 1024 indices, including
 * sentinel preservation outside the submitted region.
 */
static VkResult result_gate_copy_verified (
  vulkan_core10_transfer_session_t *session,
  vulkan_core10_result_gate_t *gate,
  uint32_t trial,
  uint32_t round,
  uint32_t words)
{
  uint32_t input[1024] = { 0 };
  uint32_t output[1024] = { 0 };

  for (uint32_t i = 0u; i < 1024u; i++)
  {
    input[i] =
      UINT32_C (0x13570000)
      ^ (i * UINT32_C (0x00010203))
      ^ ((trial + 1u) * UINT32_C (0x00110011))
      ^ ((round + 1u) * UINT32_C (0x000f000f));

    output[i] = UINT32_C (0xdeadbeef);
  }

  const VkResult permission =
    vulkan_core10_result_gate_before_submit (gate);

  if (permission != VK_SUCCESS)
  {
    fprintf (
      stderr,
      "RESULT_GATE_UNEXPECTED_PERMISSION trial=%" PRIu32
      " round=%" PRIu32 " result=%d\n",
      trial, round, permission
    );

    return permission;
  }

  const VkDeviceSize bytes = (VkDeviceSize) words * 4u;

  const VkResult copied = vulkan_core10_transfer_session_copy (
    session, input, output, bytes
  );

  const VkResult classified = vulkan_core10_result_gate_observe (
    gate, VULKAN_CORE10_RESULT_SITE_TRANSFER, copied
  );

  if (copied != VK_SUCCESS || classified != VK_SUCCESS)
  {
    fprintf (
      stderr,
      "RESULT_GATE_DRIVER_COPY_FAILED trial=%" PRIu32
      " round=%" PRIu32 " driver=%d classified=%d\n",
      trial, round, copied, classified
    );

    return copied != VK_SUCCESS ? copied : classified;
  }

  const VkResult status =
    vkGetFenceStatus (session->device, session->fence);

  if (vulkan_core10_result_gate_observe (
    gate, VULKAN_CORE10_RESULT_SITE_FENCE_STATUS, status
  ) != VK_SUCCESS)
  {
    fprintf (
      stderr,
      "RESULT_GATE_FENCE_COMPLETION_FAILED trial=%" PRIu32
      " round=%" PRIu32 " driver=%d\n",
      trial, round, status
    );

    return VK_ERROR_INITIALIZATION_FAILED;
  }

  for (uint32_t i = 0u; i < 1024u; i++)
  {
    const uint32_t expected =
      i < words ? input[i] : UINT32_C (0xdeadbeef);

    if (output[i] != expected)
    {
      fprintf (
        stderr,
        "RESULT_GATE_INTEGER_MISMATCH trial=%" PRIu32
        " round=%" PRIu32 " index=%" PRIu32 "\n",
        trial, round, i
      );

      return VK_ERROR_INITIALIZATION_FAILED;
    }
  }

  fprintf (
    stdout,
    "RESULT_GATE_GPU_VERIFIED trial=%" PRIu32
    " round=%" PRIu32
    " words=%" PRIu32
    " bytes=%" PRIu64
    " fence=0 match=1 shaders=0\n",
    trial, round, words, (uint64_t) bytes
  );

  return VK_SUCCESS;
}

/**
 * Run one real Vulkan queue lifecycle followed by test-only fault policy.
 *
 * An actual VkInstance and VkDevice supply the unsignaled fence statuses
 * and two successful public integer copies. The policy then receives
 * one explicitly labeled synthetic terminal error, refuses three
 * later submissions, preserves the output sentinel and keeps its
 * original negative error even when a later success is observed.
 *
 * The real GPU device is still healthy; closing the gate is performed
 * after closing the owned logical-device session and before the
 * parent VkInstance is destroyed.
 */
static int result_gate_trial (
  uint32_t trial,
  uint32_t minimum_api,
  vulkan_core10_transfer_memory_mode_t memory_mode,
  const char *terminal_mode)
{
  const vulkan_auto_compute_requirements_t workload =
  {
    .local_size = { 1u, 1u, 1u },
    .group_count = { 1u, 1u, 1u },
    .storage_buffer_count = 2u,
    .largest_storage_buffer_bytes = 4096u,
    .shared_memory_bytes = 0u
  };

  vulkan_core10_device_catalog_t catalog = { 0 };
  vulkan_core10_transfer_session_t session = { 0 };
  vulkan_core10_result_gate_t gate = { 0 };

  VkResult result = vulkan_core10_device_catalog_open (
    &catalog, minimum_api, "public Vulkan fail-closed policy", stdout
  );

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr,
      "RESULT_GATE_CATALOG_OPEN_FAILED trial=%" PRIu32
      " driver=%d\n",
      trial, result
    );

    return 1;
  }

  int outcome = 1;
  vulkan_auto_device_t selected = { 0 };
  uint32_t effective_api = 0u;

  result = vulkan_core10_device_catalog_select (
    &catalog, 0u, &workload, &selected, &effective_api
  );

  if (result != VK_SUCCESS || effective_api != minimum_api)
  {
    fprintf (
      stderr,
      "RESULT_GATE_DEVICE_SELECT_FAILED trial=%" PRIu32
      " driver=%d\n",
      trial, result
    );

    goto cleanup;
  }

  result = vulkan_core10_transfer_session_open (
    &session, &selected, 4096u, memory_mode, 0u, stdout
  );

  if (result != VK_SUCCESS || session.opened != VK_TRUE)
  {
    fprintf (
      stderr,
      "RESULT_GATE_SESSION_OPEN_FAILED trial=%" PRIu32
      " driver=%d\n",
      trial, result
    );

    goto cleanup;
  }

  result = vulkan_core10_result_gate_open (&gate, stdout);

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr,
      "RESULT_GATE_POLICY_OPEN_FAILED trial=%" PRIu32 "\n",
      trial
    );

    goto cleanup;
  }

  const VkResult real_status =
    vkGetFenceStatus (session.device, session.fence);

  fprintf (
    stdout,
    "RESULT_GATE_REAL_DRIVER trial=%" PRIu32
    " site=fence_status observed=%d\n",
    trial, real_status
  );

  if (real_status != VK_NOT_READY
   || vulkan_core10_result_gate_observe (
     &gate, VULKAN_CORE10_RESULT_SITE_FENCE_STATUS, real_status
   ) != VK_NOT_READY)
  {
    fprintf (stderr, "RESULT_GATE_INITIAL_STATUS_FAILED\n");

    goto cleanup;
  }

  const VkResult real_timeout = vkWaitForFences (
    session.device, 1u, &session.fence, VK_TRUE, 0u
  );

  fprintf (
    stdout,
    "RESULT_GATE_REAL_DRIVER trial=%" PRIu32
    " site=fence_wait observed=%d\n",
    trial, real_timeout
  );

  if (real_timeout != VK_TIMEOUT
   || vulkan_core10_result_gate_observe (
     &gate, VULKAN_CORE10_RESULT_SITE_FENCE_WAIT, real_timeout
   ) != VK_TIMEOUT)
  {
    fprintf (stderr, "RESULT_GATE_INITIAL_WAIT_FAILED\n");

    goto cleanup;
  }

  if (result_gate_copy_verified (
    &session, &gate, trial, 0u, 7u
  ) != VK_SUCCESS
   || result_gate_copy_verified (
    &session, &gate, trial, 1u, 1024u
   ) != VK_SUCCESS)
  {
    goto cleanup;
  }

  const VkResult artificial =
    result_gate_test_error (terminal_mode);

  const VkResult expected =
    artificial == VK_INCOMPLETE
      ? VK_ERROR_INITIALIZATION_FAILED
      : artificial;

  fprintf (
    stdout,
    "RESULT_GATE_TEST_INJECT trial=%" PRIu32
    " mode=%s origin=application_only"
    " observed=%d driver_error=0\n",
    trial, terminal_mode, artificial
  );

  result = vulkan_core10_result_gate_observe (
    &gate, VULKAN_CORE10_RESULT_SITE_TRANSFER, artificial
  );

  if (result != expected
   || gate.state != VULKAN_CORE10_RESULT_GATE_FAILED
   || gate.first_error != expected
   || gate.allowed != 2u
   || gate.accepted_statuses != 2u
   || gate.successful_results != 4u
   || session.completed_copies != 2u
   || session.copied_bytes != 4124u)
  {
    fprintf (
      stderr,
      "RESULT_GATE_TERMINAL_STATE_FAILED trial=%" PRIu32
      " result=%d expected=%d\n",
      trial, result, expected
    );

    goto cleanup;
  }

  uint32_t refused_output[1024] = { 0 };

  for (uint32_t i = 0u; i < 1024u; i++)
  {
    refused_output[i] = UINT32_C (0xdeadbeef);
  }

  for (uint32_t attempt = 0u; attempt < 3u; attempt++)
  {
    const VkResult denied =
      vulkan_core10_result_gate_before_submit (&gate);

    if (denied != expected)
    {
      fprintf (
        stderr,
        "RESULT_GATE_REFUSAL_FAILED trial=%" PRIu32
        " attempt=%" PRIu32 " result=%d\n",
        trial, attempt, denied
      );

      goto cleanup;
    }

    for (uint32_t i = 0u; i < 1024u; i++)
    {
      if (refused_output[i] != UINT32_C (0xdeadbeef))
      {
        fprintf (
          stderr,
          "RESULT_GATE_REFUSAL_OUTPUT_MUTATED trial=%" PRIu32
          " index=%" PRIu32 "\n",
          trial, i
        );

        goto cleanup;
      }
    }

    if (session.completed_copies != 2u
     || session.copied_bytes != 4124u)
    {
      fprintf (stderr, "RESULT_GATE_UNEXPECTED_RESUBMISSION\n");

      goto cleanup;
    }

    fprintf (
      stdout,
      "RESULT_GATE_REFUSED trial=%" PRIu32
      " attempt=%" PRIu32
      " error=%d copies=2 bytes=4124"
      " output_unchanged=1024\n",
      trial, attempt + 1u, denied
    );
  }

  if (vulkan_core10_result_gate_observe (
    &gate, VULKAN_CORE10_RESULT_SITE_TRANSFER, VK_SUCCESS
  ) != expected
   || gate.state != VULKAN_CORE10_RESULT_GATE_FAILED
   || gate.first_error != expected
   || gate.blocked != 3u)
  {
    fprintf (stderr, "RESULT_GATE_TERMINAL_STICKINESS_FAILED\n");

    goto cleanup;
  }

  fprintf (
    stdout,
    "RESULT_GATE_STICKY trial=%" PRIu32
    " first_error=%d later_success_ignored=1"
    " blocked=3\n",
    trial, expected
  );

  outcome = 0;

cleanup:
  result = vulkan_core10_transfer_session_close (&session);

  if (result != VK_SUCCESS || session.device != VK_NULL_HANDLE)
  {
    fprintf (
      stderr,
      "RESULT_GATE_DEVICE_CLOSE_FAILED trial=%" PRIu32
      " result=%d\n",
      trial, result
    );

    outcome = 1;
  }

  if (gate.state == VULKAN_CORE10_RESULT_GATE_ACTIVE
   || gate.state == VULKAN_CORE10_RESULT_GATE_FAILED)
  {
    result = vulkan_core10_result_gate_close (&gate);

    if (result != VK_SUCCESS
     || gate.state != VULKAN_CORE10_RESULT_GATE_CLOSED
     || vulkan_core10_result_gate_before_submit (&gate)
       != VK_ERROR_INITIALIZATION_FAILED)
    {
      fprintf (
        stderr,
        "RESULT_GATE_CLOSE_FAILED trial=%" PRIu32 "\n",
        trial
      );

      outcome = 1;
    }
  }

  result = vulkan_core10_device_catalog_close (&catalog);

  if (result != VK_SUCCESS || catalog.opened != VK_FALSE)
  {
    fprintf (
      stderr,
      "RESULT_GATE_INSTANCE_CLOSE_FAILED trial=%" PRIu32
      " result=%d\n",
      trial, result
    );

    outcome = 1;
  }

  if (outcome == 0)
  {
    fprintf (
      stdout,
      "RESULT_GATE_TRIAL_PASS trial=%" PRIu32
      " mode=%s injected=1 real_driver_loss=0"
      " completed_copies=2 verified_words=1031"
      " verified_bytes=4124 allowed=2 blocked=3"
      " nonfatal=2 successes=4 shaders=0\n",
      trial, terminal_mode
    );
  }

  return outcome;
}

/**
 * Validate invalid Vulkan sites and nonfatal statuses without a driver.
 *
 * Creates fresh host-only policy objects for four invalid site/result
 * combinations and a synthetic negative host-memory error. Each case
 * must become terminal, reject further submissions and close without
 * creating or touching any Vulkan object. These are deliberately
 * labeled policy-only tests, not genuine driver observations.
 */
static int result_gate_policy_negative_controls (void)
{
  const vulkan_core10_result_site_t sites[5] =
  {
    VULKAN_CORE10_RESULT_SITE_FENCE_STATUS,
    VULKAN_CORE10_RESULT_SITE_FENCE_WAIT,
    VULKAN_CORE10_RESULT_SITE_TRANSFER,
    VULKAN_CORE10_RESULT_SITE_TRANSFER,
    (vulkan_core10_result_site_t) 99u
  };

  const VkResult values[5] =
  {
    VK_TIMEOUT,
    VK_NOT_READY,
    VK_INCOMPLETE,
    VK_ERROR_OUT_OF_HOST_MEMORY,
    VK_SUCCESS
  };

  const VkResult expected[5] =
  {
    VK_ERROR_INITIALIZATION_FAILED,
    VK_ERROR_INITIALIZATION_FAILED,
    VK_ERROR_INITIALIZATION_FAILED,
    VK_ERROR_OUT_OF_HOST_MEMORY,
    VK_ERROR_INITIALIZATION_FAILED
  };

  const char *names[5] =
  {
    "timeout_from_status",
    "not_ready_from_wait",
    "incomplete_transfer",
    "host_memory_error",
    "invalid_site"
  };

  for (uint32_t i = 0u; i < 5u; i++)
  {
    vulkan_core10_result_gate_t policy = { 0 };

    if (vulkan_core10_result_gate_open (
      &policy, stdout
    ) != VK_SUCCESS
     || vulkan_core10_result_gate_observe (
       &policy, sites[i], values[i]
     ) != expected[i]
     || vulkan_core10_result_gate_before_submit (
       &policy
     ) != expected[i]
     || policy.state != VULKAN_CORE10_RESULT_GATE_FAILED
     || policy.blocked != 1u
     || vulkan_core10_result_gate_close (
       &policy
     ) != VK_SUCCESS
     || vulkan_core10_result_gate_before_submit (
       &policy
     ) != VK_ERROR_INITIALIZATION_FAILED)
    {
      fprintf (
        stderr,
        "RESULT_GATE_POLICY_ONLY_FAILED case=%s\n",
        names[i]
      );

      return 1;
    }

    fprintf (
      stdout,
      "RESULT_GATE_POLICY_ONLY_PASS case=%s"
      " origin=synthetic observed=%d"
      " first_error=%d blocked=1 closed=1\n",
      names[i], values[i], expected[i]
    );
  }

  return 0;
}

/**
 * Run two actual Android Vulkan sessions per explicit host-fault mode.
 *
 * Requires the exact Vulkan API, memory mode and test-only error name.
 * The native exit is zero only when real Vulkan fence statuses, four
 * public GPU buffer copies, six post-error submission refusals, two
 * cleaned logical devices and five pure policy controls pass.
 *
 * The test-only negative VkResult never originates from the driver
 * and is not interpreted as evidence of physical GPU device loss.
 */
int main (int argc, char **argv)
{
  if (argc != 7
   || strcmp (argv[1], "--minimum") != 0
   || strcmp (argv[3], "--memory") != 0
   || strcmp (argv[5], "--terminal") != 0
   || (strcmp (argv[2], "1.0") != 0
    && strcmp (argv[2], "1.1") != 0)
   || (strcmp (argv[4], "prefer") != 0
    && strcmp (argv[4], "noncoherent") != 0)
   || (strcmp (argv[6], "device-lost") != 0
    && strcmp (argv[6], "device-memory") != 0
    && strcmp (argv[6], "unexpected-positive") != 0))
  {
    fprintf (
      stderr,
      "Usage: %s --minimum 1.0|1.1 --memory prefer|noncoherent"
      " --terminal device-lost|device-memory|unexpected-positive\n",
      argv[0]
    );

    return 2;
  }

  const uint32_t api =
    strcmp (argv[2], "1.0") == 0
      ? VK_API_VERSION_1_0 : VK_API_VERSION_1_1;

  const vulkan_core10_transfer_memory_mode_t memory_mode =
    strcmp (argv[4], "prefer") == 0
      ? VULKAN_CORE10_TRANSFER_PREFER_COHERENT
      : VULKAN_CORE10_TRANSFER_REQUIRE_NONCOHERENT;

  for (uint32_t trial = 0u; trial < 2u; trial++)
  {
    if (result_gate_trial (
      trial, api, memory_mode, argv[6]
    ) != 0)
    {
      return 1;
    }
  }

  if (result_gate_policy_negative_controls () != 0)
  {
    return 1;
  }

  fprintf (
    stdout,
    "RESULT_GATE_DIAGNOSTIC_PASS api=%s mode=%" PRIu32
    " terminal=%s trials=2 injected=2"
    " actual_driver_loss=0 real_copies=4"
    " verified_words=2062 verified_bytes=8248"
    " nonfatal=4 blocked=6 host_controls=5 shaders=0\n",
    argv[2], (uint32_t) memory_mode, argv[6]
  );

  return 0;
}
