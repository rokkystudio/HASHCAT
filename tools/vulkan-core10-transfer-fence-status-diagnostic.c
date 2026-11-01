/**
 * Verify real Vulkan 1.0 fence status transitions on Android GPUs.
 *
 * A standalone public-integer buffer session queries a genuinely
 * unsignaled fence through vkGetFenceStatus and vkWaitForFences and
 * requires VK_NOT_READY and VK_TIMEOUT from the Vulkan implementation.
 * It then records and completes actual vkCmdCopyBuffer operations,
 * verifies the signaled fence status and all copied 32-bit integers,
 * resets the fence and repeats on the same VkDevice and Compute queue.
 *
 * This diagnostic never induces device loss, dispatches a shader,
 * processes password candidates or enters the HASHCAT scheduler.
 */

#include "vulkan-core10-device-catalog.h"
#include "vulkan-core10-transfer-session.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/**
 * Verify that an unsignaled Vulkan fence reports genuine pending status.
 *
 * Uses Vulkan core 1.0 vkGetFenceStatus, zero-timeout vkWaitForFences
 * and (on the first check) one 1 ms bounded wait without a queue
 * submission. The exact expected results are VK_NOT_READY and
 * VK_TIMEOUT. All unexpected results, including VK_ERROR_DEVICE_LOST,
 * are reported as failures; no automatic retry or device reconstruction
 * occurs inside this method.
 */
static VkResult fence_verify_unsignaled (
  vulkan_core10_transfer_session_t *session,
  uint32_t trial,
  uint32_t step)
{
  const VkResult status =
    vkGetFenceStatus (session->device, session->fence);

  const VkResult wait_zero = vkWaitForFences (
    session->device, 1u, &session->fence, VK_TRUE, 0u
  );

  fprintf (
    stdout,
    "FENCE_UNSIGNALED trial=%" PRIu32
    " step=%" PRIu32
    " status=%d wait_zero=%d\n",
    trial, step, status, wait_zero
  );

  if (status != VK_NOT_READY || wait_zero != VK_TIMEOUT)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  if (step == 0u)
  {
    const uint64_t timeout_ns = UINT64_C (1000000);

    const VkResult timed_wait = vkWaitForFences (
      session->device, 1u, &session->fence,
      VK_TRUE, timeout_ns
    );

    fprintf (
      stdout,
      "FENCE_TIMED_WAIT trial=%" PRIu32
      " timeout_ns=%" PRIu64
      " result=%d\n",
      trial, timeout_ns, timed_wait
    );

    if (timed_wait != VK_TIMEOUT)
    {
      return VK_ERROR_INITIALIZATION_FAILED;
    }
  }

  return VK_SUCCESS;
}

/**
 * Verify one signaled fence after a completed public buffer copy.
 *
 * vkGetFenceStatus and a zero-timeout wait must both report VK_SUCCESS.
 * The fence remains signaled until the caller explicitly resets it.
 * Any driver-returned device loss or other unexpected status ends
 * the trial without submitting further work.
 */
static VkResult fence_verify_signaled (
  vulkan_core10_transfer_session_t *session,
  uint32_t trial,
  uint32_t round)
{
  const VkResult status =
    vkGetFenceStatus (session->device, session->fence);

  const VkResult wait_zero = vkWaitForFences (
    session->device, 1u, &session->fence, VK_TRUE, 0u
  );

  fprintf (
    stdout,
    "FENCE_SIGNALED trial=%" PRIu32
    " round=%" PRIu32
    " status=%d wait_zero=%d\n",
    trial, round, status, wait_zero
  );

  if (status != VK_SUCCESS || wait_zero != VK_SUCCESS)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  return VK_SUCCESS;
}

/**
 * Perform one real Vulkan fence polling and buffer-transfer lifecycle.
 *
 * Opens a Vulkan physical-device catalogue and exactly one logical
 * device with no optional features or extensions. Queries real fence
 * status before any work, performs five fence-completed GPU copies
 * of public integer arrays, compares all bytes in and outside each
 * copied range and explicitly resets/polls the fence again.
 *
 * Two independently opened trials are performed by main. On any
 * unexpected Vulkan result the trial stops and releases VkDevice
 * before the caller-owned VkInstance; VK_ERROR_DEVICE_LOST is not
 * treated as a recoverable timeout.
 */
static int fence_status_trial (
  uint32_t trial,
  uint32_t minimum_api,
  vulkan_core10_transfer_memory_mode_t memory_mode)
{
  const vulkan_auto_compute_requirements_t workload =
  {
    .local_size = { 1u, 1u, 1u },
    .group_count = { 1u, 1u, 1u },
    .storage_buffer_count = 2u,
    .largest_storage_buffer_bytes = 4096u,
    .shared_memory_bytes = 0u
  };

  const uint32_t lengths[5] = { 1u, 7u, 64u, 257u, 1024u };

  vulkan_core10_device_catalog_t catalog = { 0 };
  vulkan_core10_transfer_session_t session = { 0 };

  VkResult result = vulkan_core10_device_catalog_open (
    &catalog, minimum_api, "public Vulkan fence status", stdout
  );

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr,
      "FENCE_CATALOG_OPEN_FAILED trial=%" PRIu32
      " result=%d\n",
      trial, result
    );

    return 1;
  }

  int exit_status = 1;

  vulkan_auto_device_t selected = { 0 };
  uint32_t effective_api = 0u;

  result = vulkan_core10_device_catalog_select (
    &catalog, 0u, &workload, &selected, &effective_api
  );

  if (result != VK_SUCCESS || effective_api != minimum_api)
  {
    fprintf (
      stderr,
      "FENCE_DEVICE_SELECT_FAILED trial=%" PRIu32
      " result=%d effective_api=%" PRIu32 "\n",
      trial, result, effective_api
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
      "FENCE_SESSION_OPEN_FAILED trial=%" PRIu32
      " result=%d\n",
      trial, result
    );

    goto cleanup;
  }

  fprintf (
    stdout,
    "FENCE_SESSION_OPEN trial=%" PRIu32
    " mode=%" PRIu32
    " owner=1 copies=0\n",
    trial, (uint32_t) memory_mode
  );

  uint32_t input[1024] = { 0 };
  uint32_t output[1024] = { 0 };

  for (uint32_t index = 0u; index < 1024u; index++)
  {
    output[index] = UINT32_C (0xdeadbeef);
  }

  result = fence_verify_unsignaled (&session, trial, 0u);

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr,
      "FENCE_INITIAL_STATUS_FAILED trial=%" PRIu32
      " result=%d\n",
      trial, result
    );

    goto cleanup;
  }

  for (uint32_t index = 0u; index < 1024u; index++)
  {
    if (output[index] != UINT32_C (0xdeadbeef))
    {
      fprintf (
        stderr,
        "FENCE_PENDING_OUTPUT_MUTATED trial=%" PRIu32
        " index=%" PRIu32 "\n",
        trial, index
      );

      goto cleanup;
    }
  }

  fprintf (
    stdout,
    "FENCE_PENDING_NO_COPY trial=%" PRIu32
    " output_unchanged=1024 copies=0\n",
    trial
  );

  uint32_t verified_words = 0u;

  for (uint32_t round = 0u; round < 5u; round++)
  {
    const uint32_t words = lengths[round];
    const VkDeviceSize bytes = (VkDeviceSize) words * 4u;

    for (uint32_t index = 0u; index < 1024u; index++)
    {
      input[index] =
        UINT32_C (0xabc00000)
        ^ (index * UINT32_C (0x00010101))
        ^ (trial * UINT32_C (0x00100010))
        ^ ((round + 1u) * UINT32_C (0x00000f0f));

      output[index] = UINT32_C (0xdeadbeef);
    }

    result = vulkan_core10_transfer_session_copy (
      &session, input, output, bytes
    );

    if (result != VK_SUCCESS)
    {
      fprintf (
        stderr,
        "FENCE_GPU_COPY_FAILED trial=%" PRIu32
        " round=%" PRIu32 " result=%d\n",
        trial, round, result
      );

      goto cleanup;
    }

    for (uint32_t index = 0u; index < 1024u; index++)
    {
      const uint32_t expected =
        index < words ? input[index] : UINT32_C (0xdeadbeef);

      if (output[index] != expected)
      {
        fprintf (
          stderr,
          "FENCE_INTEGER_MISMATCH trial=%" PRIu32
          " round=%" PRIu32
          " index=%" PRIu32
          " observed=0x%08" PRIx32
          " expected=0x%08" PRIx32 "\n",
          trial, round, index, output[index], expected
        );

        goto cleanup;
      }
    }

    result = fence_verify_signaled (&session, trial, round);

    if (result != VK_SUCCESS)
    {
      fprintf (
        stderr,
        "FENCE_COMPLETED_STATUS_FAILED trial=%" PRIu32
        " round=%" PRIu32 " result=%d\n",
        trial, round, result
      );

      goto cleanup;
    }

    verified_words += words;

    fprintf (
      stdout,
      "FENCE_COPY_VERIFIED trial=%" PRIu32
      " round=%" PRIu32
      " words=%" PRIu32
      " bytes=%" PRIu64
      " valid=1\n",
      trial, round, words, (uint64_t) bytes
    );

    result = vkResetFences (
      session.device, 1u, &session.fence
    );

    if (result != VK_SUCCESS)
    {
      fprintf (
        stderr,
        "FENCE_RESET_FAILED trial=%" PRIu32
        " round=%" PRIu32 " result=%d\n",
        trial, round, result
      );

      goto cleanup;
    }

    fprintf (
      stdout,
      "FENCE_RESET trial=%" PRIu32
      " round=%" PRIu32
      " result=%d\n",
      trial, round, result
    );

    result = fence_verify_unsignaled (
      &session, trial, round + 1u
    );

    if (result != VK_SUCCESS)
    {
      fprintf (
        stderr,
        "FENCE_RESET_STATUS_FAILED trial=%" PRIu32
        " round=%" PRIu32 " result=%d\n",
        trial, round, result
      );

      goto cleanup;
    }
  }

  if (session.completed_copies != 5u
   || session.copied_bytes != 5412u
   || verified_words != 1353u
   || session.flushes !=
      (session.buffers[0].plan.coherent == VK_FALSE ? 5u : 0u)
   || session.invalidates !=
      (session.buffers[1].plan.coherent == VK_FALSE ? 5u : 0u))
  {
    fprintf (
      stderr,
      "FENCE_COPY_COUNT_FAILED trial=%" PRIu32 "\n",
      trial
    );

    goto cleanup;
  }

  fprintf (
    stdout,
    "FENCE_TRIAL_PASS trial=%" PRIu32
    " copies=5 verified_words=1353 copied_bytes=5412"
    " not_ready=6 zero_timeouts=6 bounded_timeouts=1"
    " signaled=5 zero_success=5 resets=5 shaders=0\n",
    trial
  );

  exit_status = 0;

cleanup:
  result = vulkan_core10_transfer_session_close (&session);

  if (result != VK_SUCCESS || session.device != VK_NULL_HANDLE)
  {
    fprintf (
      stderr,
      "FENCE_DEVICE_CLOSE_FAILED trial=%" PRIu32
      " result=%d\n",
      trial, result
    );

    exit_status = 1;
  }

  result = vulkan_core10_device_catalog_close (&catalog);

  if (result != VK_SUCCESS || catalog.opened != VK_FALSE)
  {
    fprintf (
      stderr,
      "FENCE_INSTANCE_CLOSE_FAILED trial=%" PRIu32
      " result=%d\n",
      trial, result
    );

    exit_status = 1;
  }

  return exit_status;
}

/**
 * Run two independent real Vulkan fence-status and GPU transfer tests.
 *
 * Selects explicit core Vulkan 1.0 or 1.1 and a declared memory mode.
 * Returns zero only after two real device lifecycles, twelve genuine
 * VK_NOT_READY results, fourteen genuine VK_TIMEOUT results, ten
 * completed GPU transfers and exact public-data comparison. Invalid
 * arguments exit 2 and any unexpected driver status exits 1.
 */
int main (int argc, char **argv)
{
  if (argc != 5
   || strcmp (argv[1], "--minimum") != 0
   || strcmp (argv[3], "--memory") != 0
   || (strcmp (argv[2], "1.0") != 0
    && strcmp (argv[2], "1.1") != 0)
   || (strcmp (argv[4], "prefer") != 0
    && strcmp (argv[4], "noncoherent") != 0))
  {
    fprintf (
      stderr,
      "Usage: %s --minimum 1.0|1.1"
      " --memory prefer|noncoherent\n",
      argv[0]
    );

    return 2;
  }

  const uint32_t api =
    strcmp (argv[2], "1.0") == 0
      ? VK_API_VERSION_1_0 : VK_API_VERSION_1_1;

  const vulkan_core10_transfer_memory_mode_t mode =
    strcmp (argv[4], "prefer") == 0
      ? VULKAN_CORE10_TRANSFER_PREFER_COHERENT
      : VULKAN_CORE10_TRANSFER_REQUIRE_NONCOHERENT;

  for (uint32_t trial = 0u; trial < 2u; trial++)
  {
    if (fence_status_trial (trial, api, mode) != 0)
    {
      return 1;
    }
  }

  fprintf (
    stdout,
    "FENCE_DIAGNOSTIC_PASS api=%s"
    " mode=%" PRIu32
    " trials=2 copies=10 verified_words=2706"
    " copied_bytes=10824 not_ready=12"
    " zero_timeouts=12 bounded_timeouts=2"
    " signaled=10 zero_success=10 resets=10 shaders=0\n",
    argv[2], (uint32_t) mode
  );

  return 0;
}
