/**
 * Exercise test-controlled VkResult values at real Vulkan API call sites.
 *
 * The dedicated Android executable links vkQueueSubmit/vkWaitForFences
 * through ld.lld --wrap. Before arming any fault it performs genuine
 * Vulkan 1.0 public-integer GPU copies. At an explicit subsequent
 * call, the linker-bound function returns a test-generated negative
 * VkResult instead of entering the real driver. The existing transfer
 * method propagates that return value to a fail-closed policy.
 *
 * The same logical device is never used for additional GPU work after
 * a test terminal result; an independently created new VkDevice then
 * verifies a fresh real GPU copy. No physical GPU loss is induced.
 * This is separate from HASHCAT's production backend and algorithms.
 */

#include "vulkan-core10-api-fault-driver.h"
#include "vulkan-core10-device-catalog.h"
#include "vulkan-core10-result-gate.h"
#include "vulkan-core10-transfer-session.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/**
 * Verify one real Vulkan queue copy and all host buffer output words.
 *
 * Before the copy, explicitly consult the active submission policy.
 * The copy goes through the real Vulkan loader when no test is armed.
 * Each VkResult is observed at the exact TRANSFER operation site, and
 * successful fences are verified with actual vkGetFenceStatus.
 * Out-of-range host elements must retain their original sentinel.
 */
static VkResult api_fault_verified_copy (
  vulkan_core10_transfer_session_t *session,
  vulkan_core10_result_gate_t *gate,
  uint32_t trial,
  uint32_t round,
  uint32_t words,
  const char *phase)
{
  uint32_t input[1024] = { 0 };
  uint32_t output[1024] = { 0 };

  for (uint32_t i = 0u; i < 1024u; i++)
  {
    input[i] =
      UINT32_C (0x24680000)
      ^ (i * UINT32_C (0x00010103))
      ^ ((trial + 1u) * UINT32_C (0x00110011))
      ^ ((round + 1u) * UINT32_C (0x000f000f));

    output[i] = UINT32_C (0xdeadbeef);
  }

  if (vulkan_core10_result_gate_before_submit (
    gate
  ) != VK_SUCCESS)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  const VkDeviceSize bytes = (VkDeviceSize) words * 4u;

  const VkResult result = vulkan_core10_transfer_session_copy (
    session, input, output, bytes
  );

  const VkResult classified = vulkan_core10_result_gate_observe (
    gate, VULKAN_CORE10_RESULT_SITE_TRANSFER, result
  );

  if (result != VK_SUCCESS || classified != VK_SUCCESS)
  {
    fprintf (
      stderr,
      "API_FAULT_GOOD_COPY_FAILED trial=%" PRIu32
      " phase=%s round=%" PRIu32
      " driver=%d classified=%d\n",
      trial, phase, round, result, classified
    );

    return result != VK_SUCCESS ? result : classified;
  }

  const VkResult fence_status =
    vkGetFenceStatus (session->device, session->fence);

  if (vulkan_core10_result_gate_observe (
    gate, VULKAN_CORE10_RESULT_SITE_FENCE_STATUS, fence_status
  ) != VK_SUCCESS)
  {
    fprintf (
      stderr,
      "API_FAULT_GOOD_FENCE_FAILED trial=%" PRIu32
      " phase=%s round=%" PRIu32
      " fence_result=%d\n",
      trial, phase, round, fence_status
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
        "API_FAULT_GOOD_COMPARE_FAILED trial=%" PRIu32
        " phase=%s round=%" PRIu32
        " index=%" PRIu32 "\n",
        trial, phase, round, i
      );

      return VK_ERROR_INITIALIZATION_FAILED;
    }
  }

  fprintf (
    stdout,
    "API_FAULT_GOOD_COPY trial=%" PRIu32
    " phase=%s round=%" PRIu32
    " words=%" PRIu32
    " bytes=%" PRIu64
    " fence=0 valid=1 shaders=0\n",
    trial, phase, round, words, (uint64_t) bytes
  );

  return VK_SUCCESS;
}

/**
 * Open one real device, intercept a genuine Vulkan call and reopen safely.
 *
 * The first logical device completes two public buffer copies and
 * observes real VK_NOT_READY/VK_TIMEOUT. One explicitly armed
 * linker-intercepted Vulkan call then returns a negative test value
 * directly to the real transfer_session_copy implementation.
 *
 * The policy records the returned error, rejects three new copying
 * attempts and refuses a later success. The linker interposer also
 * blocks two direct zero-command vkQueueSubmit requests and one
 * zero-timeout vkWaitForFences request before reaching the driver.
 * The API interception is disarmed before destroying the old device;
 * a second independent VkDevice opens under the still-live VkInstance.
 *
 * One new device performs a genuine buffer transfer to demonstrate
 * its own working queue. No real device-loss recovery is claimed.
 */
static int api_fault_trial (
  uint32_t trial,
  uint32_t minimum_api,
  vulkan_core10_transfer_memory_mode_t memory_mode,
  vulkan_core10_api_fault_site_t site,
  VkResult terminal_result)
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
    &catalog, minimum_api,
    "public Vulkan linked API error test", stdout
  );

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr,
      "API_FAULT_CATALOG_FAILED trial=%" PRIu32
      " result=%d\n",
      trial, result
    );

    return 1;
  }

  int outcome = 1;
  VkBool32 armed = VK_FALSE;
  vulkan_auto_device_t selected = { 0 };
  uint32_t effective_api = 0u;

  result = vulkan_core10_device_catalog_select (
    &catalog, 0u, &workload, &selected, &effective_api
  );

  if (result != VK_SUCCESS || effective_api != minimum_api)
  {
    fprintf (
      stderr,
      "API_FAULT_SELECT_FAILED trial=%" PRIu32
      " result=%d\n",
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
      "API_FAULT_SESSION_FAILED trial=%" PRIu32
      " result=%d\n",
      trial, result
    );

    goto cleanup;
  }

  if (vulkan_core10_result_gate_open (
    &gate, stdout
  ) != VK_SUCCESS)
  {
    fprintf (stderr, "API_FAULT_POLICY_OPEN_FAILED\n");

    goto cleanup;
  }

  const VkResult not_ready =
    vkGetFenceStatus (session.device, session.fence);

  fprintf (
    stdout,
    "API_FAULT_REAL_DRIVER trial=%" PRIu32
    " site=fence_status observed=%d\n",
    trial, not_ready
  );

  if (not_ready != VK_NOT_READY
   || vulkan_core10_result_gate_observe (
     &gate, VULKAN_CORE10_RESULT_SITE_FENCE_STATUS, not_ready
   ) != VK_NOT_READY)
  {
    goto cleanup;
  }

  const VkResult timed_out = vkWaitForFences (
    session.device, 1u, &session.fence, VK_TRUE, 0u
  );

  fprintf (
    stdout,
    "API_FAULT_REAL_DRIVER trial=%" PRIu32
    " site=fence_wait observed=%d\n",
    trial, timed_out
  );

  if (timed_out != VK_TIMEOUT
   || vulkan_core10_result_gate_observe (
     &gate, VULKAN_CORE10_RESULT_SITE_FENCE_WAIT, timed_out
   ) != VK_TIMEOUT)
  {
    goto cleanup;
  }

  if (api_fault_verified_copy (
    &session, &gate, trial, 0u, 7u, "before"
  ) != VK_SUCCESS
   || api_fault_verified_copy (
     &session, &gate, trial, 1u, 1024u, "before"
   ) != VK_SUCCESS)
  {
    goto cleanup;
  }

  if (vulkan_core10_api_fault_arm (
    session.queue, session.device, site, terminal_result, stdout
  ) != VK_SUCCESS)
  {
    fprintf (stderr, "API_FAULT_ARM_FAILED\n");

    goto cleanup;
  }

  armed = VK_TRUE;

  uint32_t input[1024] = { 0 };
  uint32_t output[1024] = { 0 };

  for (uint32_t i = 0u; i < 1024u; i++)
  {
    input[i] = UINT32_C (0x44444444) ^ i;
    output[i] = UINT32_C (0xdeadbeef);
  }

  if (vulkan_core10_result_gate_before_submit (
    &gate
  ) != VK_SUCCESS)
  {
    goto cleanup;
  }

  const VkResult boundary_result =
    vulkan_core10_transfer_session_copy (
      &session, input, output, 256u
    );

  const VkResult classified = vulkan_core10_result_gate_observe (
    &gate, VULKAN_CORE10_RESULT_SITE_TRANSFER, boundary_result
  );

  if (boundary_result != terminal_result
   || classified != terminal_result
   || gate.first_error != terminal_result
   || gate.state != VULKAN_CORE10_RESULT_GATE_FAILED
   || gate.allowed != 3u
   || gate.accepted_statuses != 2u
   || gate.successful_results != 4u
   || session.completed_copies != 2u
   || session.copied_bytes != 4124u)
  {
    fprintf (
      stderr,
      "API_FAULT_TRANSFER_CLASSIFICATION_FAILED trial=%" PRIu32
      " observed=%d classified=%d expected=%d\n",
      trial, boundary_result, classified, terminal_result
    );

    goto cleanup;
  }

  for (uint32_t i = 0u; i < 1024u; i++)
  {
    if (output[i] != UINT32_C (0xdeadbeef))
    {
      fprintf (
        stderr,
        "API_FAULT_OUTPUT_MUTATED trial=%" PRIu32
        " index=%" PRIu32 "\n",
        trial, i
      );

      goto cleanup;
    }
  }

  fprintf (
    stdout,
    "API_FAULT_RETURNED trial=%" PRIu32
    " result=%d origin=test_linker_wrap"
    " real_driver_loss=0 copies=2 output_unchanged=1024\n",
    trial, boundary_result
  );

  for (uint32_t attempt = 1u; attempt <= 3u; attempt++)
  {
    if (vulkan_core10_result_gate_before_submit (
      &gate
    ) != terminal_result)
    {
      fprintf (
        stderr,
        "API_FAULT_HOST_GATE_FAILED trial=%" PRIu32
        " attempt=%" PRIu32 "\n",
        trial, attempt
      );

      goto cleanup;
    }

    fprintf (
      stdout,
      "API_FAULT_HOST_DENIED trial=%" PRIu32
      " attempt=%" PRIu32
      " error=%d copies=2 output_unchanged=1024\n",
      trial, attempt, terminal_result
    );
  }

  for (uint32_t attempt = 1u; attempt <= 2u; attempt++)
  {
    const VkResult denied = vkQueueSubmit (
      session.queue, 0u, NULL, VK_NULL_HANDLE
    );

    if (denied != terminal_result)
    {
      fprintf (
        stderr,
        "API_FAULT_BOUNDARY_RESUBMIT_FAILED trial=%" PRIu32
        " attempt=%" PRIu32 " result=%d\n",
        trial, attempt, denied
      );

      goto cleanup;
    }

    fprintf (
      stdout,
      "API_FAULT_BOUNDARY_DENIED trial=%" PRIu32
      " site=queue_submit attempt=%" PRIu32
      " result=%d real_driver_called=0\n",
      trial, attempt, denied
    );
  }

  const VkResult wait_denied = vkWaitForFences (
    session.device, 1u, &session.fence, VK_TRUE, 0u
  );

  if (wait_denied != terminal_result)
  {
    fprintf (
      stderr,
      "API_FAULT_BOUNDARY_WAIT_FAILED trial=%" PRIu32
      " result=%d\n",
      trial, wait_denied
    );

    goto cleanup;
  }

  fprintf (
    stdout,
    "API_FAULT_BOUNDARY_DENIED trial=%" PRIu32
    " site=fence_wait attempt=1"
    " result=%d real_driver_called=0\n",
    trial, wait_denied
  );

  vulkan_core10_api_fault_counts_t counts = { 0 };

  if (vulkan_core10_api_fault_snapshot (&counts) != VK_SUCCESS
   || counts.submit_attempts != 3u
   || counts.real_submissions !=
      (site == VULKAN_CORE10_API_FAULT_WAIT ? 1u : 0u)
   || counts.wait_attempts !=
      (site == VULKAN_CORE10_API_FAULT_WAIT ? 2u : 1u)
   || counts.real_waits != 0u
   || counts.injections != 1u
   || counts.blocked != 3u
   || counts.first_error != terminal_result
   || gate.blocked != 3u
   || vulkan_core10_result_gate_observe (
      &gate, VULKAN_CORE10_RESULT_SITE_TRANSFER, VK_SUCCESS
     ) != terminal_result)
  {
    fprintf (
      stderr,
      "API_FAULT_BOUNDARY_COUNTS_FAILED trial=%" PRIu32 "\n",
      trial
    );

    goto cleanup;
  }

  fprintf (
    stdout,
    "API_FAULT_COUNTS trial=%" PRIu32
    " submit_attempts=%" PRIu32
    " real_submissions=%" PRIu32
    " wait_attempts=%" PRIu32
    " real_waits=%" PRIu32
    " injected=%" PRIu32
    " blocked=%" PRIu32
    " host_blocked=%" PRIu32
    " sticky=1\n",
    trial,
    counts.submit_attempts, counts.real_submissions,
    counts.wait_attempts, counts.real_waits,
    counts.injections, counts.blocked,
    gate.blocked
  );

  if (vulkan_core10_api_fault_disarm () != VK_SUCCESS)
  {
    goto cleanup;
  }

  armed = VK_FALSE;

  result = vulkan_core10_transfer_session_close (&session);

  if (result != VK_SUCCESS || session.device != VK_NULL_HANDLE)
  {
    fprintf (
      stderr,
      "API_FAULT_OLD_DEVICE_CLOSE_FAILED trial=%" PRIu32
      " result=%d\n",
      trial, result
    );

    goto cleanup;
  }

  if (vulkan_core10_result_gate_close (&gate) != VK_SUCCESS)
  {
    goto cleanup;
  }

  fprintf (
    stdout,
    "API_FAULT_OLD_DEVICE_CLOSED trial=%" PRIu32
    " instance=1 owner=0 sticky_error=%d\n",
    trial, terminal_result
  );

  vulkan_core10_result_gate_t fresh_gate = { 0 };

  result = vulkan_core10_transfer_session_open (
    &session, &selected, 4096u, memory_mode, 0u, stdout
  );

  if (result != VK_SUCCESS
   || session.opened != VK_TRUE
   || vulkan_core10_result_gate_open (
     &fresh_gate, stdout
   ) != VK_SUCCESS)
  {
    fprintf (
      stderr,
      "API_FAULT_NEW_DEVICE_OPEN_FAILED trial=%" PRIu32
      " result=%d\n",
      trial, result
    );

    goto cleanup;
  }

  if (api_fault_verified_copy (
    &session, &fresh_gate, trial, 0u, 64u, "new"
  ) != VK_SUCCESS
   || session.completed_copies != 1u
   || session.copied_bytes != 256u)
  {
    fprintf (
      stderr, "API_FAULT_NEW_DEVICE_COPY_FAILED trial=%" PRIu32 "\n",
      trial
    );

    vulkan_core10_result_gate_close (&fresh_gate);

    goto cleanup;
  }

  fprintf (
    stdout,
    "API_FAULT_NEW_DEVICE_PASS trial=%" PRIu32
    " fresh_device=1 copies=1 words=64"
    " bytes=256 instance=1 valid=1\n",
    trial
  );

  result = vulkan_core10_transfer_session_close (&session);

  if (result != VK_SUCCESS || session.device != VK_NULL_HANDLE
   || vulkan_core10_result_gate_close (
      &fresh_gate
   ) != VK_SUCCESS)
  {
    fprintf (
      stderr, "API_FAULT_NEW_DEVICE_CLOSE_FAILED trial=%" PRIu32 "\n",
      trial
    );

    goto cleanup;
  }

  fprintf (
    stdout,
    "API_FAULT_NEW_DEVICE_CLOSED trial=%" PRIu32
    " instance=1 owner=0\n",
    trial
  );

  outcome = 0;

cleanup:
  if (armed == VK_TRUE)
  {
    vulkan_core10_api_fault_disarm ();
  }

  result = vulkan_core10_transfer_session_close (&session);

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr,
      "API_FAULT_FINAL_DEVICE_CLOSE_FAILED trial=%" PRIu32
      " result=%d\n",
      trial, result
    );

    outcome = 1;
  }

  if (gate.state == VULKAN_CORE10_RESULT_GATE_ACTIVE
   || gate.state == VULKAN_CORE10_RESULT_GATE_FAILED)
  {
    if (vulkan_core10_result_gate_close (
      &gate
    ) != VK_SUCCESS)
    {
      outcome = 1;
    }
  }

  result = vulkan_core10_device_catalog_close (&catalog);

  if (result != VK_SUCCESS || catalog.opened != VK_FALSE)
  {
    fprintf (
      stderr,
      "API_FAULT_INSTANCE_CLOSE_FAILED trial=%" PRIu32
      " result=%d\n",
      trial, result
    );

    outcome = 1;
  }

  if (outcome == 0)
  {
    fprintf (
      stdout,
      "API_FAULT_TRIAL_PASS trial=%" PRIu32
      " injected=1 actual_driver_loss=0"
      " completed_gpu_copies=3 verified_words=1095"
      " verified_bytes=4380 blocked_host=3"
      " blocked_driver_calls=3 devices=2 instance=1"
      " shaders=0\n",
      trial
    );
  }

  return outcome;
}

/**
 * Run two real Android Vulkan driver-entry fault tests per selected site.
 *
 * The CLI explicitly selects requested core API, host-visible memory
 * policy, intercepted SUBMIT/WAIT operation and negative test result.
 * Native success requires two test-controlled Vulkan API returns,
 * four independently closed logical devices, six real public GPU
 * buffer copies and zero post-failure calls entering the driver.
 * Malformed arguments exit 2; unexpected driver results exit 1.
 */
int main (int argc, char **argv)
{
  if (argc != 9
   || strcmp (argv[1], "--minimum") != 0
   || strcmp (argv[3], "--memory") != 0
   || strcmp (argv[5], "--fault-site") != 0
   || strcmp (argv[7], "--result") != 0
   || (strcmp (argv[2], "1.0") != 0
    && strcmp (argv[2], "1.1") != 0)
   || (strcmp (argv[4], "prefer") != 0
    && strcmp (argv[4], "noncoherent") != 0)
   || (strcmp (argv[6], "submit") != 0
    && strcmp (argv[6], "wait") != 0)
   || (strcmp (argv[8], "device-lost") != 0
    && strcmp (argv[8], "device-memory") != 0))
  {
    fprintf (
      stderr,
      "Usage: %s --minimum 1.0|1.1 --memory prefer|noncoherent"
      " --fault-site submit|wait"
      " --result device-lost|device-memory\n",
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

  const vulkan_core10_api_fault_site_t site =
    strcmp (argv[6], "submit") == 0
      ? VULKAN_CORE10_API_FAULT_SUBMIT
      : VULKAN_CORE10_API_FAULT_WAIT;

  const VkResult fault_result =
    strcmp (argv[8], "device-lost") == 0
      ? VK_ERROR_DEVICE_LOST
      : VK_ERROR_OUT_OF_DEVICE_MEMORY;

  for (uint32_t trial = 0u; trial < 2u; trial++)
  {
    if (api_fault_trial (
      trial, api, memory_mode, site, fault_result
    ) != 0)
    {
      return 1;
    }
  }

  fprintf (
    stdout,
    "API_FAULT_DIAGNOSTIC_PASS api=%s"
    " mode=%" PRIu32
    " fault_site=%s result=%d"
    " trials=2 injected=2 actual_driver_loss=0"
    " live_devices=4 completed_gpu_copies=6"
    " verified_words=2190 verified_bytes=8760"
    " blocked_host=6 blocked_driver_calls=6"
    " shaders=0\n",
    argv[2], (uint32_t) memory_mode,
    argv[6], fault_result
  );

  return 0;
}
