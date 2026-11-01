/**
 * Verify real Vulkan 1.0 buffer transfers using public integer arrays.
 *
 * Creates its own VkInstance catalogue and one independent VkDevice
 * transfer session per trial. All GPU submissions use vkCmdCopyBuffer on
 * an actual Compute queue, followed by a fence and host-memory barrier.
 * No shader module, descriptor binding, password candidate or hashing
 * algorithm is involved.
 */

#include "vulkan-core10-device-catalog.h"
#include "vulkan-core10-transfer-session.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/**
 * Record one real GPU memory-transfer session and verify each copied word.
 *
 * Reuses the same source/destination VkBuffers and fence for five exact
 * transfer lengths. A noncoherent-only selection is allowed to return
 * VK_ERROR_FEATURE_NOT_PRESENT when the driver has no compatible memory
 * type. That refusal is reported distinctly from a successful transfer.
 */
static int verify_transfer_trial (
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

  const uint32_t words_each[5] = { 1u, 7u, 64u, 257u, 1024u };

  vulkan_core10_device_catalog_t catalog = { 0 };
  vulkan_core10_transfer_session_t session = { 0 };

  uint32_t source[1024] = { 0 };
  uint32_t output[1024] = { 0 };

  if (vulkan_core10_transfer_session_copy (
    &session, source, output, 4u
  ) != VK_ERROR_INITIALIZATION_FAILED
   || vulkan_core10_transfer_session_open (
    &session, NULL, 4096u, memory_mode, stdout
  ) != VK_ERROR_INITIALIZATION_FAILED
   || vulkan_core10_transfer_session_close (
    NULL
  ) != VK_ERROR_INITIALIZATION_FAILED
   || vulkan_core10_transfer_session_close (
    &session
  ) != VK_SUCCESS)
  {
    fprintf (stderr, "TRANSFER_UNOPENED_CONTROLS_FAILED\n");

    return 1;
  }

  VkResult result = vulkan_core10_device_catalog_open (
    &catalog, minimum_api, "public Vulkan buffer transfer", stdout
  );

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr,
      "TRANSFER_CATALOG_OPEN_FAILED trial=%" PRIu32 " result=%d\n",
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

  if (result != VK_SUCCESS)
  {
    fprintf (
      stderr,
      "TRANSFER_DEVICE_SELECT_FAILED trial=%" PRIu32
      " result=%d\n",
      trial, result
    );

    goto cleanup_catalog;
  }

  if (effective_api != minimum_api)
  {
    fprintf (stderr, "TRANSFER_DEVICE_API_MISMATCH\n");

    goto cleanup_catalog;
  }

  if (vulkan_core10_transfer_session_open (
    &session, &selected, 0u, memory_mode, stdout
  ) != VK_ERROR_INITIALIZATION_FAILED
   || vulkan_core10_transfer_session_open (
    &session, &selected, 3u, memory_mode, stdout
  ) != VK_ERROR_INITIALIZATION_FAILED
   || vulkan_core10_transfer_session_open (
    &session, &selected, 4096u,
    (vulkan_core10_transfer_memory_mode_t) 99u,
    stdout
  ) != VK_ERROR_INITIALIZATION_FAILED
   || vulkan_core10_transfer_session_open (
    &session, &selected, 4096u, memory_mode, NULL
  ) != VK_ERROR_INITIALIZATION_FAILED
   || session.device != VK_NULL_HANDLE)
  {
    fprintf (stderr, "TRANSFER_INVALID_OPEN_CONTROLS_FAILED\n");

    goto cleanup_catalog;
  }

  fprintf (
    stdout,
    "TRANSFER_INVALID_OPEN trial=%" PRIu32
    " rejected=7 owned=0\n",
    trial
  );

  result = vulkan_core10_transfer_session_open (
    &session, &selected, 4096u, memory_mode, stdout
  );

  if (result == VK_ERROR_FEATURE_NOT_PRESENT
   && memory_mode == VULKAN_CORE10_TRANSFER_REQUIRE_NONCOHERENT
   && session.device == VK_NULL_HANDLE
   && session.opened == VK_FALSE)
  {
    fprintf (
      stdout,
      "TRANSFER_UNSUPPORTED_TRIAL trial=%" PRIu32
      " mode=noncoherent result=-8 owned=0\n",
      trial
    );

    status = 3;

    goto cleanup_catalog;
  }

  if (result != VK_SUCCESS || session.opened != VK_TRUE)
  {
    fprintf (
      stderr,
      "TRANSFER_OPEN_ERROR trial=%" PRIu32 " result=%d\n",
      trial, result
    );

    goto cleanup_session;
  }

  if (vulkan_core10_transfer_session_open (
    &session, &selected, 4096u, memory_mode, stdout
  ) != VK_ERROR_INITIALIZATION_FAILED
   || vulkan_core10_transfer_session_copy (
    &session, source, output, 0u
  ) != VK_ERROR_INITIALIZATION_FAILED
   || vulkan_core10_transfer_session_copy (
    &session, source, output, 3u
  ) != VK_ERROR_INITIALIZATION_FAILED
   || vulkan_core10_transfer_session_copy (
    &session, source, output, 4100u
  ) != VK_ERROR_INITIALIZATION_FAILED
   || vulkan_core10_transfer_session_copy (
    &session, NULL, output, 4u
  ) != VK_ERROR_INITIALIZATION_FAILED
   || vulkan_core10_transfer_session_copy (
    &session, source, NULL, 4u
  ) != VK_ERROR_INITIALIZATION_FAILED
   || session.completed_copies != 0u)
  {
    fprintf (stderr, "TRANSFER_INVALID_ACTIVE_CONTROLS_FAILED\n");

    goto cleanup_session;
  }

  fprintf (
    stdout,
    "TRANSFER_INVALID_ACTIVE trial=%" PRIu32
    " rejected=6 copies=0\n",
    trial
  );

  uint32_t verified_words = 0u;

  for (uint32_t round = 0u; round < 5u; round++)
  {
    const uint32_t words = words_each[round];
    const VkDeviceSize bytes = (VkDeviceSize) words * 4u;

    for (uint32_t i = 0u; i < 1024u; i++)
    {
      source[i] = (UINT32_C (0x6a09e667) ^ (i * UINT32_C (0x01020408)))
                + ((round + 1u) * UINT32_C (0x00110011));
      output[i] = UINT32_C (0xdeadbeef);
    }

    result = vulkan_core10_transfer_session_copy (
      &session, source, output, bytes
    );

    if (result != VK_SUCCESS)
    {
      fprintf (
        stderr,
        "TRANSFER_GPU_COPY_FAILED trial=%" PRIu32
        " round=%" PRIu32 " result=%d\n",
        trial, round, result
      );

      goto cleanup_session;
    }

    for (uint32_t i = 0u; i < 1024u; i++)
    {
      const uint32_t expected =
        (i < words) ? source[i] : UINT32_C (0xdeadbeef);

      if (output[i] != expected)
      {
        fprintf (
          stderr,
          "TRANSFER_WORD_MISMATCH trial=%" PRIu32
          " round=%" PRIu32 " index=%" PRIu32
          " observed=0x%08" PRIx32
          " expected=0x%08" PRIx32 "\n",
          trial, round, i, output[i], expected
        );

        goto cleanup_session;
      }
    }

    verified_words += words;

    fprintf (
      stdout,
      "TRANSFER_VERIFIED trial=%" PRIu32
      " round=%" PRIu32
      " words=%" PRIu32
      " bytes=%" PRIu64
      " valid=1\n",
      trial, round, words, (uint64_t) bytes
    );
  }

  if (session.completed_copies != 5u
   || session.copied_bytes != 5412u
   || verified_words != 1353u)
  {
    fprintf (stderr, "TRANSFER_COMPLETION_COUNTS_FAILED\n");

    goto cleanup_session;
  }

  fprintf (
    stdout,
    "TRANSFER_TRIAL_PASS trial=%" PRIu32
    " mode=%" PRIu32
    " copies=5 verified_words=1353"
    " copied_bytes=5412 invalid=13 shaders=0\n",
    trial, (uint32_t) memory_mode
  );

  status = 0;

cleanup_session:
  result = vulkan_core10_transfer_session_close (&session);

  if (result != VK_SUCCESS || session.device != VK_NULL_HANDLE
   || session.opened != VK_FALSE)
  {
    fprintf (stderr, "TRANSFER_DEVICE_CLOSE_FAILED\n");

    status = 1;
  }

  if (vulkan_core10_transfer_session_close (&session) != VK_SUCCESS)
  {
    fprintf (stderr, "TRANSFER_SECOND_DEVICE_CLOSE_FAILED\n");

    status = 1;
  }

cleanup_catalog:
  result = vulkan_core10_device_catalog_close (&catalog);

  if (result != VK_SUCCESS || catalog.opened != VK_FALSE)
  {
    fprintf (stderr, "TRANSFER_INSTANCE_CLOSE_FAILED\n");

    status = 1;
  }

  return status;
}

/**
 * Run two real GPU buffer-transfer sessions with explicit API and memory mode.
 *
 * Native exit 0 means ten fence-completed buffer copies were verified.
 * Native exit 3 explicitly indicates two driver refusals for requested
 * HOST_VISIBLE noncoherent memory. Exit 1 means driver/test failure;
 * exit 2 rejects malformed command-line input.
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
      "Usage: %s --minimum 1.0|1.1 --memory prefer|noncoherent\n",
      argv[0]
    );

    return 2;
  }

  const uint32_t api = strcmp (argv[2], "1.0") == 0
    ? VK_API_VERSION_1_0 : VK_API_VERSION_1_1;

  const vulkan_core10_transfer_memory_mode_t memory_mode =
    strcmp (argv[4], "prefer") == 0
      ? VULKAN_CORE10_TRANSFER_PREFER_COHERENT
      : VULKAN_CORE10_TRANSFER_REQUIRE_NONCOHERENT;

  int previous_status = -1;

  for (uint32_t trial = 0u; trial < 2u; trial++)
  {
    const int result = verify_transfer_trial (
      trial, api, memory_mode
    );

    if (result != 0 && result != 3) return 1;

    if (previous_status != -1 && result != previous_status)
    {
      fprintf (stderr, "TRANSFER_INCONSISTENT_DRIVER_MEMORY_SUPPORT\n");

      return 1;
    }

    previous_status = result;
  }

  if (previous_status == 3)
  {
    fprintf (
      stdout,
      "TRANSFER_DIAGNOSTIC_UNSUPPORTED api=%s"
      " mode=%" PRIu32
      " trials=2 refusals=2 copied_bytes=0 shaders=0\n",
      argv[2], (uint32_t) memory_mode
    );

    return 3;
  }

  fprintf (
    stdout,
    "TRANSFER_DIAGNOSTIC_PASS api=%s"
    " mode=%" PRIu32
    " trials=2 copies=10 verified_words=2706"
    " copied_bytes=10824 invalid=26 shaders=0\n",
    argv[2], (uint32_t) memory_mode
  );

  return 0;
}
