/**
 * Prove real Vulkan 1.0 buffer recovery after each acquired-resource fault.
 *
 * Each trial owns one physical-device catalogue and attempts an explicit
 * controlled acquisition failure. The same VkInstance and selected
 * VkPhysicalDevice then support two new fully owned logical-device
 * sessions, each recording genuine VkCmdCopyBuffer commands on a
 * Compute-capable GPU queue. A fence and mapped-memory synchronization
 * complete before every public integer comparison. No shader or hashcat
 * password-processing path is executed.
 */

#include "vulkan-core10-device-catalog.h"
#include "vulkan-core10-transfer-session.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/**
 * Compare one completed public GPU transfer with its exact input.
 *
 * Rewrites every input word using a deterministic noncryptographic
 * integer pattern, fills the full caller-owned output with a sentinel,
 * executes one real Vulkan copy, and compares both copied words and
 * untouched host-array elements outside the requested transfer range.
 */
static VkResult recovery_copy_verified (
  vulkan_core10_transfer_session_t *session,
  uint32_t trial,
  uint32_t phase,
  uint32_t round,
  uint32_t words)
{
  uint32_t input[1024] = { 0 };
  uint32_t output[1024] = { 0 };

  for (uint32_t index = 0u; index < 1024u; index++)
  {
    input[index] =
      UINT32_C (0x12340000)
      ^ (index * UINT32_C (0x00010101))
      ^ (trial * UINT32_C (0x00110011))
      ^ (phase * UINT32_C (0x01000100))
      ^ ((round + 1u) * UINT32_C (0x00000f0f));

    output[index] = UINT32_C (0xdeadbeef);
  }

  const VkDeviceSize bytes = (VkDeviceSize) words * 4u;

  VkResult result = vulkan_core10_transfer_session_copy (
    session, input, output, bytes
  );

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr,
      "RECOVERY_COPY_FAILED trial=%" PRIu32
      " phase=%" PRIu32
      " round=%" PRIu32
      " result=%d\n",
      trial, phase, round, result
    );

    return result;
  }

  for (uint32_t index = 0u; index < 1024u; index++)
  {
    const uint32_t expected =
      index < words ? input[index] : UINT32_C (0xdeadbeef);

    if (output[index] != expected)
    {
      fprintf (
        stderr,
        "RECOVERY_COMPARE_FAILED trial=%" PRIu32
        " phase=%" PRIu32
        " round=%" PRIu32
        " index=%" PRIu32
        " observed=0x%08" PRIx32
        " expected=0x%08" PRIx32 "\n",
        trial, phase, round, index, output[index], expected
      );

      return VK_ERROR_INITIALIZATION_FAILED;
    }
  }

  fprintf (
    stdout,
    "RECOVERY_VERIFIED trial=%" PRIu32
    " phase=%" PRIu32
    " round=%" PRIu32
    " words=%" PRIu32
    " bytes=%" PRIu64
    " valid=1 shaders=0\n",
    trial, phase, round, words, (uint64_t) bytes
  );

  return VK_SUCCESS;
}

/**
 * Recover two genuine Vulkan sessions from one failed acquisition.
 *
 * Uses precisely one VkInstance and one selected physical device per
 * trial. The first attempted session fails after an explicitly selected
 * real Vulkan handle acquisition and leaves its owner empty. The next
 * logical-device session verifies five fence-completed public transfers;
 * a second independent VkDevice within the same parent instance
 * verifies a full-buffer copy after the first recovery is closed.
 *
 * Returns failure on any unexpected Vulkan result, wrong byte,
 * resource-owner leak or incomplete VkDevice/VkInstance teardown.
 */
static int recovery_trial (
  uint32_t trial,
  uint32_t minimum_api,
  uint32_t fault_point,
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

  const uint32_t word_counts[5] =
  {
    1u, 7u, 64u, 257u, 1024u
  };

  vulkan_core10_device_catalog_t catalog = { 0 };
  vulkan_core10_transfer_session_t session = { 0 };

  VkResult result = vulkan_core10_device_catalog_open (
    &catalog, minimum_api, "public Vulkan transfer recovery", stdout
  );

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr, "RECOVERY_CATALOG_OPEN_FAILED trial=%" PRIu32
      " result=%d\n",
      trial, result
    );

    return 1;
  }

  int status = 1;
  vulkan_auto_device_t selected = { 0 };
  uint32_t effective_api = 0u;

  result = vulkan_core10_device_catalog_select (
    &catalog, 0u, &workload, &selected, &effective_api
  );

  if (result != VK_SUCCESS || effective_api != minimum_api)
  {
    fprintf (
      stderr, "RECOVERY_DEVICE_SELECT_FAILED trial=%" PRIu32
      " result=%d\n",
      trial, result
    );

    goto cleanup;
  }

  result = vulkan_core10_transfer_session_open (
    &session, &selected, 4096u, memory_mode, fault_point, stdout
  );

  const vulkan_core10_transfer_session_t cleared = { 0 };

  if (result != VK_ERROR_INITIALIZATION_FAILED
   || memcmp (&session, &cleared, sizeof (session)) != 0
   || catalog.opened != VK_TRUE)
  {
    fprintf (
      stderr, "RECOVERY_FAULT_OWNER_FAILED trial=%" PRIu32
      " point=%" PRIu32 " result=%d\n",
      trial, fault_point, result
    );

    goto cleanup;
  }

  fprintf (
    stdout,
    "RECOVERY_FAULT_CONFIRMED trial=%" PRIu32
    " point=%" PRIu32
    " result=-3 owner=0 instance=1 copies=0\n",
    trial, fault_point
  );

  for (uint32_t phase = 0u; phase < 2u; phase++)
  {
    result = vulkan_core10_transfer_session_open (
      &session, &selected, 4096u, memory_mode, 0u, stdout
    );

    if (result != VK_SUCCESS || session.opened != VK_TRUE
     || session.completed_copies != 0u
     || session.acquisitions != 10u)
    {
      fprintf (
        stderr,
        "RECOVERY_REOPEN_FAILED trial=%" PRIu32
        " phase=%" PRIu32 " result=%d\n",
        trial, phase, result
      );

      goto cleanup;
    }

    fprintf (
      stdout,
      "RECOVERY_REOPEN trial=%" PRIu32
      " phase=%" PRIu32
      " point=%" PRIu32
      " owner=1 acquisitions=10 copied=0\n",
      trial, phase, fault_point
    );

    const uint32_t rounds = phase == 0u ? 5u : 1u;
    const uint32_t expected_words = phase == 0u ? 1353u : 1024u;
    const uint64_t expected_bytes = (uint64_t) expected_words * 4u;

    uint32_t verified_words = 0u;

    for (uint32_t round = 0u; round < rounds; round++)
    {
      const uint32_t words =
        phase == 0u ? word_counts[round] : 1024u;

      result = recovery_copy_verified (
        &session, trial, phase, round, words
      );

      if (result != VK_SUCCESS) goto cleanup;

      verified_words += words;
    }

    const uint32_t expected_flushes =
      session.buffers[0].plan.coherent == VK_FALSE ? rounds : 0u;

    const uint32_t expected_invalidates =
      session.buffers[1].plan.coherent == VK_FALSE ? rounds : 0u;

    if (session.completed_copies != rounds
     || session.copied_bytes != expected_bytes
     || verified_words != expected_words
     || session.flushes != expected_flushes
     || session.invalidates != expected_invalidates)
    {
      fprintf (
        stderr,
        "RECOVERY_SUBMISSION_COUNTS_FAILED trial=%" PRIu32
        " phase=%" PRIu32 "\n",
        trial, phase
      );

      goto cleanup;
    }

    fprintf (
      stdout,
      "RECOVERY_PHASE_PASS trial=%" PRIu32
      " phase=%" PRIu32
      " copies=%" PRIu32
      " verified_words=%" PRIu32
      " copied_bytes=%" PRIu64
      " flushes=%" PRIu32
      " invalidates=%" PRIu32
      " shaders=0\n",
      trial, phase, rounds, verified_words,
      expected_bytes, session.flushes, session.invalidates
    );

    result = vulkan_core10_transfer_session_close (&session);

    if (result != VK_SUCCESS
     || memcmp (&session, &cleared, sizeof (session)) != 0
     || vulkan_core10_transfer_session_close (&session) != VK_SUCCESS)
    {
      fprintf (
        stderr,
        "RECOVERY_REOPEN_CLOSE_FAILED trial=%" PRIu32
        " phase=%" PRIu32 "\n",
        trial, phase
      );

      goto cleanup;
    }

    fprintf (
      stdout,
      "RECOVERY_PHASE_CLOSED trial=%" PRIu32
      " phase=%" PRIu32
      " owner=0 instance=1\n",
      trial, phase
    );
  }

  status = 0;

cleanup:
  result = vulkan_core10_transfer_session_close (&session);

  if (result != VK_SUCCESS || session.device != VK_NULL_HANDLE)
  {
    fprintf (
      stderr, "RECOVERY_CLEANUP_DEVICE_FAILED trial=%" PRIu32
      " result=%d\n",
      trial, result
    );

    status = 1;
  }

  result = vulkan_core10_device_catalog_close (&catalog);

  if (result != VK_SUCCESS || catalog.opened != VK_FALSE)
  {
    fprintf (
      stderr,
      "RECOVERY_CLEANUP_CATALOG_FAILED trial=%" PRIu32
      " result=%d\n",
      trial, result
    );

    status = 1;
  }

  if (status == 0)
  {
    fprintf (
      stdout,
      "RECOVERY_TRIAL_PASS trial=%" PRIu32
      " fault=%" PRIu32
      " failed=1 reopened=2 copies=6"
      " verified_words=2377 copied_bytes=9508"
      " shaders=0\n",
      trial, fault_point
    );
  }

  return status;
}

/**
 * Run two full real-driver fault/reopen/copy/close recovery trials.
 *
 * Requires exactly one Vulkan 1.0 or 1.1 API selection, one explicit
 * supported memory policy and one integer fault point from 1 through
 * 10. Exit zero means two intentional faults, four independently
 * created recovery VkDevices, twelve completed GPU copies and exact
 * output verification. Malformed arguments exit 2; all driver or
 * test failures exit 1 without inventing successful recovery.
 */
int main (int argc, char **argv)
{
  if (argc != 7
   || strcmp (argv[1], "--minimum") != 0
   || strcmp (argv[3], "--memory") != 0
   || strcmp (argv[5], "--fault-after") != 0
   || (strcmp (argv[2], "1.0") != 0
    && strcmp (argv[2], "1.1") != 0)
   || (strcmp (argv[4], "prefer") != 0
    && strcmp (argv[4], "noncoherent") != 0)
   || argv[6][0] == '\0')
  {
    fprintf (
      stderr,
      "Usage: %s --minimum 1.0|1.1 --memory prefer|noncoherent"
      " --fault-after 1..10\n",
      argv[0]
    );

    return 2;
  }

  uint32_t point = 0u;

  for (const char *digit = argv[6]; *digit != '\0'; digit++)
  {
    if (*digit < '0' || *digit > '9')
    {
      fprintf (stderr, "RECOVERY_INVALID_FAULT_FORMAT\n");

      return 2;
    }

    point = point * 10u + (uint32_t) (*digit - '0');

    if (point > 10u)
    {
      fprintf (stderr, "RECOVERY_INVALID_FAULT_RANGE\n");

      return 2;
    }
  }

  if (point == 0u)
  {
    fprintf (stderr, "RECOVERY_INVALID_FAULT_RANGE\n");

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
    if (recovery_trial (trial, api, point, memory_mode) != 0)
    {
      return 1;
    }
  }

  fprintf (
    stdout,
    "RECOVERY_DIAGNOSTIC_PASS api=%s"
    " mode=%" PRIu32
    " point=%" PRIu32
    " trials=2 faults=2 reopened=4 copies=12"
    " verified_words=4754 copied_bytes=19016"
    " shaders=0\n",
    argv[2], (uint32_t) memory_mode, point
  );

  return 0;
}
