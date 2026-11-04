/**
 * Verify Vulkan 1.0 logical-device ownership in an actual backend_ctx_t.
 *
 * Verifies backend-owned Vulkan 1.0/1.1 devices, staging transfers,
 * immutable public Compute pipelines and reusable command ownership.
 * Two independent baseline sessions exercise resource reuse and recovery;
 * two additional sessions exercise cancellation-aware shutdown of queued
 * Compute work and staging transfers through actual GPU fences.
 * Both cached public shaders operate only on 32-bit diagnostic data.
 * Invalid device, buffer, submission and lease requests are rejected.
 */

#include "backend_vulkan.h"
#include "backend_vulkan_public_spirv.h"
#include "backend_vulkan_public_stage2_spirv.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * Verify reusable staging buffers and explicit bidirectional GPU transfers.
 *
 * Converts two existing slots to separate host/Compute allocations and
 * returns borrowed views of the retained slots on successful completion.
 * Uploads and downloads four deterministic public byte ranges, verifies
 * exact readback after overwriting the host mapping, and rejects five
 * invalid transfers without submitting GPU work.
 */
static int verify_staging_transfer_trial (
  backend_ctx_t *backend_ctx,
  const vulkan_auto_device_t *selected,
  uint32_t trial,
  backend_vulkan_storage_view_t *staging_views)
{
  if (staging_views == NULL) return 1;

  const VkDeviceSize capacities[2] = { 256u, 96u };
  const VkDeviceSize lengths[2][2] =
  {
    { 64u, 256u },
    { 32u, 96u }
  };
  backend_vulkan_storage_view_t staged[2] = { 0 };

  VkResult result = backend_vulkan_session_storage_reserve (
    backend_ctx, 0u, selected, 2u,
    capacities, VK_TRUE, staged, stdout
  );

  if (result != VK_SUCCESS
   || staged[0].staging != VK_TRUE
   || staged[1].staging != VK_TRUE
   || staged[0].host_buffer == staged[0].buffer
   || staged[1].host_buffer == staged[1].buffer
   || staged[0].mapped == NULL
   || staged[1].mapped == NULL)
  {
    fprintf (stderr, "BACKEND_SESSION_STAGING_RESERVE_FAILED result=%d\n", result);

    return 1;
  }

  if (backend_vulkan_session_storage_transfer (
    backend_ctx, 0u, 0u, 0u, VK_FALSE
  ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_session_storage_transfer (
    backend_ctx, 0u, 0u, 6u, VK_FALSE
  ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_session_storage_transfer (
    backend_ctx, 0u, 1u, 100u, VK_FALSE
  ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_session_storage_transfer (
    backend_ctx, 0u, 2u, 32u, VK_FALSE
  ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_session_storage_transfer (
    backend_ctx, backend_ctx->vulkan_devices_cnt, 0u, 32u, VK_FALSE
  ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (stderr, "BACKEND_SESSION_STAGING_CONTROLS_FAILED\n");

    return 1;
  }

  uint32_t verified_bytes = 0u;

  for (uint32_t slot = 0u; slot < 2u; slot++)
  {
    for (uint32_t pass = 0u; pass < 2u; pass++)
    {
      const VkDeviceSize bytes = lengths[slot][pass];
      uint8_t expected[256] = { 0 };

      for (uint32_t i = 0u; i < bytes; i++)
      {
        expected[i] = (uint8_t) (
          i + slot * UINT32_C (31) + pass * UINT32_C (73)
        );
      }

      memcpy (staged[slot].mapped, expected, (size_t) bytes);

      result = backend_vulkan_session_storage_transfer (
        backend_ctx, 0u, slot, bytes, VK_FALSE
      );

      if (result != VK_SUCCESS)
      {
        fprintf (
          stderr,
          "BACKEND_SESSION_STAGING_UPLOAD_FAILED slot=%" PRIu32
          " pass=%" PRIu32 " result=%d\n",
          slot, pass, result
        );

        return 1;
      }

      memset (staged[slot].mapped, 0, (size_t) bytes);

      result = backend_vulkan_session_storage_transfer (
        backend_ctx, 0u, slot, bytes, VK_TRUE
      );

      if (result != VK_SUCCESS
       || memcmp (staged[slot].mapped, expected, (size_t) bytes) != 0)
      {
        fprintf (
          stderr,
          "BACKEND_SESSION_STAGING_READBACK_FAILED slot=%" PRIu32
          " pass=%" PRIu32 " result=%d\n",
          slot, pass, result
        );

        return 1;
      }

      verified_bytes += (uint32_t) bytes;
    }
  }

  staging_views[0] = staged[0];
  staging_views[1] = staged[1];

  fprintf (
    stdout,
    "BACKEND_SESSION_STAGING_TRANSFER_PASS trial=%" PRIu32
    " uploads=4 downloads=4 verified_bytes=%" PRIu32
    " rejected=5 staging_buffers=2\n",
    trial, verified_bytes
  );

  return 0;
}

/**
 * Validate exact pipeline identity, descriptor rebinding and command leases.
 *
 * Two immutable public shaders create distinct cached pipelines. One-stage
 * descriptors bind the first storage pair and are then reacquired with the
 * reversed pair using the identical descriptor handle. A two-stage program
 * owns a separate descriptor pool and two sets. Invalid descriptor requests
 * and duplicate leases are rejected before the same command/fence pair is
 * acquired twice. The method never destroys session-owned Vulkan handles.
 */
static int verify_cached_compute_resources_trial (
  backend_ctx_t *backend_ctx,
  const backend_vulkan_storage_view_t *staging_views,
  uint32_t trial)
{
  if (staging_views == NULL) return 1;

  const uint32_t first_words =
    (uint32_t) (sizeof (backend_vulkan_public_spirv)
      / sizeof (uint32_t));

  const uint32_t second_words =
    (uint32_t) (sizeof (backend_vulkan_public_stage2_spirv)
      / sizeof (uint32_t));

  backend_vulkan_pipeline_view_t first = { 0 };
  backend_vulkan_pipeline_view_t second = { 0 };
  backend_vulkan_pipeline_view_t repeated = { 0 };

  VkResult result = backend_vulkan_session_pipeline_acquire (
    backend_ctx, 0u, backend_vulkan_public_spirv,
    first_words, &first, stdout
  );

  if (result != VK_SUCCESS
   || first.pipeline == VK_NULL_HANDLE
   || first.descriptor_layout == VK_NULL_HANDLE
   || first.pipeline_layout == VK_NULL_HANDLE)
  {
    fprintf (stderr, "BACKEND_SESSION_PIPELINE_FIRST_FAILED result=%d\n", result);

    return 1;
  }

  result = backend_vulkan_session_pipeline_acquire (
    backend_ctx, 0u, backend_vulkan_public_stage2_spirv,
    second_words, &second, stdout
  );

  if (result != VK_SUCCESS
   || second.pipeline == VK_NULL_HANDLE
   || second.pipeline == first.pipeline
   || second.pipeline_layout != first.pipeline_layout
   || second.descriptor_layout != first.descriptor_layout)
  {
    fprintf (stderr, "BACKEND_SESSION_PIPELINE_SECOND_FAILED result=%d\n", result);

    return 1;
  }

  result = backend_vulkan_session_pipeline_acquire (
    backend_ctx, 0u, backend_vulkan_public_spirv,
    first_words, &repeated, stdout
  );

  if (result != VK_SUCCESS
   || repeated.pipeline != first.pipeline
   || repeated.pipeline_layout != first.pipeline_layout
   || repeated.descriptor_layout != first.descriptor_layout)
  {
    fprintf (stderr, "BACKEND_SESSION_PIPELINE_CACHE_FAILED result=%d\n", result);

    return 1;
  }

  if (backend_vulkan_session_pipeline_acquire (
    backend_ctx, 0u, NULL, first_words, &repeated, stdout
  ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_session_pipeline_acquire (
    backend_ctx, 0u, backend_vulkan_public_spirv,
    0u, &repeated, stdout
  ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_session_pipeline_acquire (
    backend_ctx, backend_ctx->vulkan_devices_cnt,
    backend_vulkan_public_spirv, first_words, &repeated, stdout
  ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (stderr, "BACKEND_SESSION_PIPELINE_CONTROLS_FAILED\n");

    return 1;
  }

  const backend_vulkan_descriptor_binding_t bindings[2] =
  {
    {
      .source_buffer = staging_views[0].buffer,
      .destination_buffer = staging_views[1].buffer,
      .source_bytes = staging_views[0].bytes,
      .destination_bytes = staging_views[1].bytes
    },
    {
      .source_buffer = staging_views[1].buffer,
      .destination_buffer = staging_views[0].buffer,
      .source_bytes = staging_views[1].bytes,
      .destination_bytes = staging_views[0].bytes
    }
  };

  backend_vulkan_descriptor_view_t first_descriptors = { 0 };
  backend_vulkan_descriptor_view_t repeated_descriptors = { 0 };
  backend_vulkan_descriptor_view_t second_descriptors = { 0 };

  result = backend_vulkan_session_descriptors_acquire (
    backend_ctx, 0u, bindings, 1u, &first_descriptors, stdout
  );

  if (result != VK_SUCCESS
   || first_descriptors.sets == NULL
   || first_descriptors.sets[0] == VK_NULL_HANDLE
   || first_descriptors.stage_count != 1u)
  {
    fprintf (stderr, "BACKEND_SESSION_DESCRIPTOR_FIRST_FAILED result=%d\n", result);

    return 1;
  }

  backend_vulkan_descriptor_binding_t invalid_binding = bindings[0];

  invalid_binding.source_buffer = VK_NULL_HANDLE;

  if (backend_vulkan_session_descriptors_acquire (
    backend_ctx, 0u, bindings, 1u, &repeated_descriptors, stdout
  ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_session_descriptors_acquire (
    backend_ctx, backend_ctx->vulkan_devices_cnt,
    bindings, 1u, &repeated_descriptors, stdout
  ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_session_descriptors_acquire (
    backend_ctx, 0u, bindings, 0u, &repeated_descriptors, stdout
  ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_session_descriptors_acquire (
    backend_ctx, 0u, &invalid_binding, 1u,
    &repeated_descriptors, stdout
  ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_session_descriptors_acquire (
    backend_ctx, 0u, bindings, 1u, NULL, stdout
  ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_session_storage_transfer (
    backend_ctx, 0u, 0u, 32u, VK_FALSE
  ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (stderr, "BACKEND_SESSION_DESCRIPTOR_CONTROLS_FAILED\n");

    return 1;
  }

  result = backend_vulkan_session_descriptors_release (
    backend_ctx, 0u
  );

  if (result != VK_SUCCESS
   || backend_vulkan_session_descriptors_release (
        backend_ctx, 0u
      ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (stderr, "BACKEND_SESSION_DESCRIPTOR_RELEASE_FAILED result=%d\n", result);

    return 1;
  }

  result = backend_vulkan_session_descriptors_acquire (
    backend_ctx, 0u, &bindings[1], 1u,
    &repeated_descriptors, stdout
  );

  if (result != VK_SUCCESS
   || repeated_descriptors.sets == NULL
   || repeated_descriptors.sets[0] != first_descriptors.sets[0]
   || repeated_descriptors.stage_count != 1u)
  {
    fprintf (stderr, "BACKEND_SESSION_DESCRIPTOR_REBIND_FAILED result=%d\n", result);

    return 1;
  }

  result = backend_vulkan_session_descriptors_release (
    backend_ctx, 0u
  );

  if (result != VK_SUCCESS)
  {
    fprintf (stderr, "BACKEND_SESSION_DESCRIPTOR_SECOND_RELEASE_FAILED result=%d\n", result);

    return 1;
  }

  result = backend_vulkan_session_descriptors_acquire (
    backend_ctx, 0u, bindings, 2u, &second_descriptors, stdout
  );

  if (result != VK_SUCCESS
   || second_descriptors.sets == NULL
   || second_descriptors.sets[0] == VK_NULL_HANDLE
   || second_descriptors.sets[1] == VK_NULL_HANDLE
   || second_descriptors.stage_count != 2u)
  {
    fprintf (stderr, "BACKEND_SESSION_DESCRIPTOR_MULTISTAGE_FAILED result=%d\n", result);

    return 1;
  }

  result = backend_vulkan_session_descriptors_release (
    backend_ctx, 0u
  );

  if (result != VK_SUCCESS)
  {
    fprintf (stderr, "BACKEND_SESSION_DESCRIPTOR_FINAL_RELEASE_FAILED result=%d\n", result);

    return 1;
  }

  fprintf (
    stdout,
    "BACKEND_SESSION_DESCRIPTOR_CACHE_PASS"
    " trial=%" PRIu32
    " pools=2 pool_reuses=1 acquisitions=3"
    " bindings=8 invalid=7\n",
    trial
  );

  backend_vulkan_command_view_t first_command = { 0 };
  backend_vulkan_command_view_t second_command = { 0 };

  result = backend_vulkan_session_command_acquire (
    backend_ctx, 0u, &first_command, stdout
  );

  if (result != VK_SUCCESS
   || first_command.command == VK_NULL_HANDLE
   || first_command.fence == VK_NULL_HANDLE)
  {
    fprintf (stderr, "BACKEND_SESSION_COMMAND_FIRST_FAILED result=%d\n", result);

    return 1;
  }

  if (backend_vulkan_session_command_acquire (
    backend_ctx, 0u, &second_command, stdout
  ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_session_command_acquire (
    backend_ctx, backend_ctx->vulkan_devices_cnt,
    &second_command, stdout
  ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_session_storage_transfer (
    backend_ctx, 0u, 0u, 32u, VK_FALSE
  ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (stderr, "BACKEND_SESSION_COMMAND_LEASE_CONTROLS_FAILED\n");

    return 1;
  }

  result = backend_vulkan_session_command_release (
    backend_ctx, 0u, VK_TRUE
  );

  if (result != VK_SUCCESS
   || backend_vulkan_session_command_release (
        backend_ctx, 0u, VK_TRUE
      ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (stderr, "BACKEND_SESSION_COMMAND_RELEASE_FAILED result=%d\n", result);

    return 1;
  }

  result = backend_vulkan_session_command_acquire (
    backend_ctx, 0u, &second_command, stdout
  );

  if (result != VK_SUCCESS
   || second_command.command != first_command.command
   || second_command.fence != first_command.fence)
  {
    fprintf (stderr, "BACKEND_SESSION_COMMAND_REUSE_FAILED result=%d\n", result);

    return 1;
  }

  result = backend_vulkan_session_command_release (
    backend_ctx, 0u, VK_TRUE
  );

  if (result != VK_SUCCESS)
  {
    fprintf (stderr, "BACKEND_SESSION_COMMAND_FINAL_RELEASE_FAILED result=%d\n", result);

    return 1;
  }

  fprintf (
    stdout,
    "BACKEND_SESSION_COMPUTE_RESOURCES_PASS"
    " trial=%" PRIu32
    " cached_pipelines=2 cache_hits=1"
    " command_acquisitions=2 command_reuses=1"
    " invalid=7\n",
    trial
  );

  return 0;
}

/**
 * Recover one truly submitted but deliberately unobserved Compute queue job.
 *
 * Records and submits a real empty Compute command, releases its exclusive
 * lease without waiting for the command fence and verifies that subsequent
 * command acquisition is blocked. A successful VkDeviceWaitIdle permits
 * the session to replace the poisoned command/fence pair while retaining
 * a descriptor lease. That lease cannot be returned to its pool until
 * idle-verified recovery. The replacement command is submitted and
 * fence-verified before a retained staging allocation is read back.
 *
 * Invalid recovery requests preserve the active and failed session states.
 * This diagnostic injects a host-side incomplete lease only; Vulkan driver
 * failures and VK_ERROR_DEVICE_LOST are never fabricated.
 */
static int verify_command_recovery_trial (
  backend_ctx_t *backend_ctx,
  const backend_vulkan_storage_view_t *staging_views,
  uint32_t trial)
{
  if (backend_ctx == NULL || staging_views == NULL
   || staging_views[0].mapped == NULL)
  {
    return 1;
  }

  backend_vulkan_device_view_t device = { 0 };

  VkResult result = backend_vulkan_session_device (
    backend_ctx, 0u, &device
  );

  if (result != VK_SUCCESS
   || device.device == VK_NULL_HANDLE
   || device.queue == VK_NULL_HANDLE)
  {
    fprintf (stderr, "BACKEND_SESSION_RECOVERY_DEVICE_FAILED result=%d\n", result);

    return 1;
  }

  if (backend_vulkan_session_command_recover (
    backend_ctx, 0u, stdout
  ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (stderr, "BACKEND_SESSION_RECOVERY_HEALTHY_ACCEPTED\n");

    return 1;
  }

  const backend_vulkan_descriptor_binding_t bindings =
  {
    .source_buffer = staging_views[0].buffer,
    .destination_buffer = staging_views[1].buffer,
    .source_bytes = staging_views[0].bytes,
    .destination_bytes = staging_views[1].bytes
  };

  backend_vulkan_descriptor_view_t descriptors = { 0 };

  result = backend_vulkan_session_descriptors_acquire (
    backend_ctx, 0u, &bindings, 1u, &descriptors, stdout
  );

  if (result != VK_SUCCESS
   || descriptors.sets == NULL
   || descriptors.sets[0] == VK_NULL_HANDLE)
  {
    fprintf (stderr, "BACKEND_SESSION_RECOVERY_DESCRIPTOR_BIND_FAILED result=%d\n", result);

    return 1;
  }

  backend_vulkan_command_view_t before = { 0 };

  result = backend_vulkan_session_command_acquire (
    backend_ctx, 0u, &before, stdout
  );

  if (result != VK_SUCCESS
   || before.command == VK_NULL_HANDLE
   || before.fence == VK_NULL_HANDLE)
  {
    fprintf (stderr, "BACKEND_SESSION_RECOVERY_FIRST_ACQUIRE_FAILED result=%d\n", result);

    return 1;
  }

  const VkCommandBufferBeginInfo begin =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
  };

  result = vkBeginCommandBuffer (before.command, &begin);

  if (result != VK_SUCCESS) return 1;

  result = vkEndCommandBuffer (before.command);

  if (result != VK_SUCCESS) return 1;

  result = vkResetFences (
    device.device, 1u, &before.fence
  );

  if (result != VK_SUCCESS) return 1;

  const VkSubmitInfo submission =
  {
    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    .commandBufferCount = 1u,
    .pCommandBuffers = &before.command
  };

  result = vkQueueSubmit (
    device.queue, 1u, &submission, before.fence
  );

  if (result != VK_SUCCESS)
  {
    fprintf (stderr, "BACKEND_SESSION_RECOVERY_SUBMIT_FAILED result=%d\n", result);

    return 1;
  }

  if (backend_vulkan_session_command_recover (
    backend_ctx, 0u, stdout
  ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (stderr, "BACKEND_SESSION_RECOVERY_ACTIVE_LEASE_ACCEPTED\n");

    return 1;
  }

  result = backend_vulkan_session_command_release (
    backend_ctx, 0u, VK_FALSE
  );

  if (result != VK_SUCCESS
   || backend_vulkan_session_descriptors_release (
        backend_ctx, 0u
      ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (stderr, "BACKEND_SESSION_RECOVERY_DESCRIPTOR_EARLY_RELEASE_ACCEPTED result=%d\n", result);

    return 1;
  }

  backend_vulkan_session_status_t recovery_status = { 0 };

  if (backend_vulkan_session_status (
    backend_ctx, 0u, &recovery_status
  ) != VK_SUCCESS
   || recovery_status.compute_leased != VK_FALSE
   || recovery_status.compute_pending != VK_FALSE
   || recovery_status.compute_failed != VK_TRUE
   || recovery_status.descriptors_leased != VK_TRUE
   || recovery_status.transfer_pending != VK_FALSE)
  {
    fprintf (stderr, "BACKEND_SESSION_RECOVERY_POISON_STATUS_FAILED\n");

    return 1;
  }

  if (backend_vulkan_session_close_if_ready (
    backend_ctx, 0u
  ) != VK_NOT_READY)
  {
    fprintf (stderr, "BACKEND_SESSION_RECOVERY_POISON_CLOSE_ACCEPTED\n");

    return 1;
  }

  backend_vulkan_command_view_t after = { 0 };

  if (backend_vulkan_session_command_acquire (
    backend_ctx, 0u, &after, stdout
  ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_session_command_recover (
        backend_ctx, backend_ctx->vulkan_devices_cnt, stdout
      ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_session_command_recover (
        backend_ctx, 0u, NULL
      ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (stderr, "BACKEND_SESSION_RECOVERY_POISON_CONTROLS_FAILED\n");

    return 1;
  }

  result = backend_vulkan_session_command_recover (
    backend_ctx, 0u, stdout
  );

  if (result != VK_SUCCESS)
  {
    fprintf (stderr, "BACKEND_SESSION_RECOVERY_RESET_FAILED result=%d\n", result);

    return 1;
  }

  if (backend_vulkan_session_status (
    backend_ctx, 0u, &recovery_status
  ) != VK_SUCCESS
   || recovery_status.compute_failed != VK_FALSE
   || recovery_status.compute_leased != VK_FALSE
   || recovery_status.descriptors_leased != VK_TRUE
   || recovery_status.transfer_pending != VK_FALSE)
  {
    fprintf (stderr, "BACKEND_SESSION_RECOVERY_READY_STATUS_FAILED\n");

    return 1;
  }

  result = backend_vulkan_session_descriptors_release (
    backend_ctx, 0u
  );

  if (result != VK_SUCCESS)
  {
    fprintf (stderr, "BACKEND_SESSION_RECOVERY_DESCRIPTOR_RELEASE_FAILED result=%d\n", result);

    return 1;
  }

  if (backend_vulkan_session_command_recover (
    backend_ctx, 0u, stdout
  ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (stderr, "BACKEND_SESSION_RECOVERY_DOUBLE_RESET_ACCEPTED\n");

    return 1;
  }

  result = backend_vulkan_session_command_acquire (
    backend_ctx, 0u, &after, stdout
  );

  if (result != VK_SUCCESS
   || after.command == VK_NULL_HANDLE
   || after.fence == VK_NULL_HANDLE)
  {
    fprintf (stderr, "BACKEND_SESSION_RECOVERY_SECOND_ACQUIRE_FAILED result=%d\n", result);

    return 1;
  }

  result = vkBeginCommandBuffer (after.command, &begin);

  if (result != VK_SUCCESS) return 1;

  result = vkEndCommandBuffer (after.command);

  if (result != VK_SUCCESS) return 1;

  result = vkResetFences (
    device.device, 1u, &after.fence
  );

  if (result != VK_SUCCESS) return 1;

  const VkSubmitInfo verified_submission =
  {
    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    .commandBufferCount = 1u,
    .pCommandBuffers = &after.command
  };

  result = vkQueueSubmit (
    device.queue, 1u, &verified_submission, after.fence
  );

  if (result != VK_SUCCESS)
  {
    fprintf (stderr, "BACKEND_SESSION_RECOVERY_RESUBMIT_FAILED result=%d\n", result);

    return 1;
  }

  result = vkWaitForFences (
    device.device, 1u, &after.fence, VK_TRUE, UINT64_MAX
  );

  if (result != VK_SUCCESS)
  {
    fprintf (stderr, "BACKEND_SESSION_RECOVERY_RESUBMIT_WAIT_FAILED result=%d\n", result);

    return 1;
  }

  result = backend_vulkan_session_command_release (
    backend_ctx, 0u, VK_TRUE
  );

  if (result != VK_SUCCESS)
  {
    fprintf (stderr, "BACKEND_SESSION_RECOVERY_FINAL_RELEASE_FAILED result=%d\n", result);

    return 1;
  }

  const uint32_t expected = UINT32_C (0x6b68f293) ^ trial;

  memcpy (staging_views[0].mapped, &expected, sizeof (expected));

  result = backend_vulkan_session_storage_transfer (
    backend_ctx, 0u, 0u, sizeof (expected), VK_FALSE
  );

  if (result != VK_SUCCESS) return 1;

  memset (staging_views[0].mapped, 0, sizeof (expected));

  result = backend_vulkan_session_storage_transfer (
    backend_ctx, 0u, 0u, sizeof (expected), VK_TRUE
  );

  if (result != VK_SUCCESS
   || memcmp (staging_views[0].mapped, &expected, sizeof (expected)) != 0)
  {
    fprintf (stderr, "BACKEND_SESSION_RECOVERY_STAGING_FAILED result=%d\n", result);

    return 1;
  }

  fprintf (
    stdout,
    "BACKEND_SESSION_COMMAND_RECOVERY_PASS"
    " trial=%" PRIu32
    " submissions=2"
    " recovered=1"
    " rejected=6"
    " verified_bytes=4\n",
    trial
  );

  return 0;
}

/**
 * Verify a real in-flight Compute cancellation and reusable fence lifecycle.
 *
 * Records and submits an empty Vulkan Compute command to the session-owned
 * queue. A cancellation request discards that submission's result without
 * releasing the borrowed command or storage resources before the real fence
 * completes. A zero-timeout wait may report actual VK_TIMEOUT or success.
 * Reacquiring the same command and fence submits a second ordinary job,
 * followed by an exact storage round trip on the original logical device.
 *
 * Each command holds a real stage-count descriptor lease. Returning
 * descriptors during a pending or canceled-but-unfinished submission is
 * rejected, while returning them after real completion permits exact reuse.
 * Invalid indices, repeated cancellation, duplicate submission, premature
 * release and double completion are rejected without invented driver errors
 * or changes to other GPU backend scheduling state.
 */
static int verify_managed_compute_cancellation_trial (
  backend_ctx_t *backend_ctx,
  const backend_vulkan_storage_view_t *staging_views,
  uint32_t trial)
{
  if (backend_ctx == NULL || staging_views == NULL
   || staging_views[0].staging != VK_TRUE
   || staging_views[0].mapped == NULL
   || staging_views[0].bytes < sizeof (uint32_t))
  {
    return 1;
  }

  backend_vulkan_device_view_t device = { 0 };

  VkResult result = backend_vulkan_session_device (
    backend_ctx, 0u, &device
  );

  if (result != VK_SUCCESS || device.device == VK_NULL_HANDLE)
  {
    fprintf (stderr, "BACKEND_SESSION_MANAGED_DEVICE_FAILED result=%d\n", result);

    return 1;
  }

  if (backend_vulkan_session_command_submit (
    backend_ctx, 0u
  ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_session_command_cancel (
        backend_ctx, 0u
      ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_session_command_complete_timeout (
        backend_ctx, 0u, 0u
      ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (stderr, "BACKEND_SESSION_MANAGED_HEALTHY_CONTROLS_FAILED\n");

    return 1;
  }

  const backend_vulkan_descriptor_binding_t bindings =
  {
    .source_buffer = staging_views[0].buffer,
    .destination_buffer = staging_views[1].buffer,
    .source_bytes = staging_views[0].bytes,
    .destination_bytes = staging_views[1].bytes
  };

  backend_vulkan_descriptor_view_t descriptor = { 0 };

  result = backend_vulkan_session_descriptors_acquire (
    backend_ctx, 0u, &bindings, 1u, &descriptor, stdout
  );

  if (result != VK_SUCCESS
   || descriptor.sets == NULL
   || descriptor.sets[0] == VK_NULL_HANDLE)
  {
    fprintf (stderr, "BACKEND_SESSION_MANAGED_DESCRIPTOR_FIRST_FAILED result=%d\n", result);

    return 1;
  }

  backend_vulkan_command_view_t first = { 0 };

  result = backend_vulkan_session_command_acquire (
    backend_ctx, 0u, &first, stdout
  );

  if (result != VK_SUCCESS
   || first.command == VK_NULL_HANDLE
   || first.fence == VK_NULL_HANDLE)
  {
    fprintf (stderr, "BACKEND_SESSION_MANAGED_ACQUIRE_FAILED result=%d\n", result);

    return 1;
  }

  const VkCommandBufferBeginInfo begin =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
  };

  result = vkBeginCommandBuffer (first.command, &begin);

  if (result != VK_SUCCESS) return 1;

  result = vkEndCommandBuffer (first.command);

  if (result != VK_SUCCESS) return 1;

  if (backend_vulkan_session_command_cancel (
    backend_ctx, 0u
  ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_session_command_submit (
        backend_ctx, backend_ctx->vulkan_devices_cnt
      ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (stderr, "BACKEND_SESSION_MANAGED_UNSUBMITTED_CONTROLS_FAILED\n");

    return 1;
  }

  result = backend_vulkan_session_command_submit (
    backend_ctx, 0u
  );

  if (result != VK_SUCCESS)
  {
    fprintf (stderr, "BACKEND_SESSION_MANAGED_SUBMIT_FAILED result=%d\n", result);

    return 1;
  }

  if (backend_vulkan_session_close_if_ready (
    backend_ctx, 0u
  ) != VK_NOT_READY)
  {
    fprintf (stderr, "BACKEND_SESSION_MANAGED_PENDING_CLOSE_ACCEPTED\n");

    return 1;
  }

  backend_vulkan_session_status_t command_status = { 0 };

  if (backend_vulkan_session_status (
    backend_ctx, 0u, &command_status
  ) != VK_SUCCESS
   || command_status.compute_leased != VK_TRUE
   || command_status.compute_pending != VK_TRUE
   || command_status.compute_cancel_requested != VK_FALSE
   || command_status.descriptors_leased != VK_TRUE
   || command_status.transfer_pending != VK_FALSE
   || command_status.compute_submissions != 1u
   || command_status.compute_completions != 0u
   || command_status.compute_cancellations != 0u)
  {
    fprintf (stderr, "BACKEND_SESSION_MANAGED_SUBMITTED_STATUS_FAILED\n");

    return 1;
  }

  if (backend_vulkan_session_command_submit (
    backend_ctx, 0u
  ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_session_command_release (
        backend_ctx, 0u, VK_TRUE
      ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_session_command_cancel (
        backend_ctx, backend_ctx->vulkan_devices_cnt
      ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_session_descriptors_release (
        backend_ctx, 0u
      ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (stderr, "BACKEND_SESSION_MANAGED_PENDING_CONTROLS_FAILED\n");

    return 1;
  }

  result = backend_vulkan_session_command_cancel (
    backend_ctx, 0u
  );

  if (result != VK_SUCCESS
   || backend_vulkan_session_command_cancel (
        backend_ctx, 0u
      ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (stderr, "BACKEND_SESSION_MANAGED_CANCEL_CONTROLS_FAILED result=%d\n", result);

    return 1;
  }

  if (backend_vulkan_session_status (
    backend_ctx, 0u, &command_status
  ) != VK_SUCCESS
   || command_status.compute_leased != VK_TRUE
   || command_status.compute_pending != VK_TRUE
   || command_status.compute_cancel_requested != VK_TRUE
   || command_status.descriptors_leased != VK_TRUE
   || command_status.compute_cancellations != 0u)
  {
    fprintf (stderr, "BACKEND_SESSION_MANAGED_CANCEL_STATUS_FAILED\n");

    return 1;
  }

  if (backend_vulkan_session_close_if_ready (
    backend_ctx, 0u
  ) != VK_NOT_READY)
  {
    fprintf (stderr, "BACKEND_SESSION_MANAGED_CANCEL_CLOSE_ACCEPTED\n");

    return 1;
  }

  backend_vulkan_command_view_t second = { 0 };

  if (backend_vulkan_session_command_acquire (
    backend_ctx, 0u, &second, stdout
  ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_session_storage_transfer (
        backend_ctx, 0u, 0u, 4u, VK_FALSE
      ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (stderr, "BACKEND_SESSION_MANAGED_CANCEL_LEASE_FAILED\n");

    return 1;
  }

  const VkResult cancelled_poll =
    backend_vulkan_session_command_complete_timeout (
      backend_ctx, 0u, 0u
    );

  if (cancelled_poll != VK_SUCCESS && cancelled_poll != VK_TIMEOUT)
  {
    fprintf (stderr, "BACKEND_SESSION_MANAGED_CANCEL_WAIT_FAILED result=%d\n", cancelled_poll);

    return 1;
  }

  if (cancelled_poll == VK_TIMEOUT)
  {
    if (backend_vulkan_session_command_release (
      backend_ctx, 0u, VK_TRUE
    ) != VK_ERROR_INITIALIZATION_FAILED
     || backend_vulkan_session_descriptors_release (
          backend_ctx, 0u
        ) != VK_ERROR_INITIALIZATION_FAILED)
    {
      fprintf (stderr, "BACKEND_SESSION_MANAGED_TIMEOUT_RELEASE_FAILED\n");

      return 1;
    }

    result = backend_vulkan_session_command_complete (
      backend_ctx, 0u
    );

    if (result != VK_SUCCESS) return 1;
  }

  if (backend_vulkan_session_command_complete_timeout (
    backend_ctx, 0u, 0u
  ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (stderr, "BACKEND_SESSION_MANAGED_CANCEL_DOUBLE_WAIT_ACCEPTED\n");

    return 1;
  }

  if (backend_vulkan_session_status (
    backend_ctx, 0u, &command_status
  ) != VK_SUCCESS
   || command_status.compute_leased != VK_TRUE
   || command_status.compute_pending != VK_FALSE
   || command_status.compute_cancel_requested != VK_FALSE
   || command_status.descriptors_leased != VK_TRUE
   || command_status.compute_submissions != 1u
   || command_status.compute_cancellations != 1u
   || command_status.compute_completions != 0u)
  {
    fprintf (stderr, "BACKEND_SESSION_MANAGED_CANCEL_RETIRED_STATUS_FAILED\n");

    return 1;
  }

  if (backend_vulkan_session_close_if_ready (
    backend_ctx, 0u
  ) != VK_NOT_READY)
  {
    fprintf (stderr, "BACKEND_SESSION_MANAGED_LEASE_CLOSE_ACCEPTED\n");

    return 1;
  }

  result = backend_vulkan_session_descriptors_release (
    backend_ctx, 0u
  );

  if (result != VK_SUCCESS) return 1;

  result = backend_vulkan_session_command_release (
    backend_ctx, 0u, VK_TRUE
  );

  if (result != VK_SUCCESS) return 1;

  result = backend_vulkan_session_descriptors_acquire (
    backend_ctx, 0u, &bindings, 1u, &descriptor, stdout
  );

  if (result != VK_SUCCESS
   || descriptor.sets == NULL
   || descriptor.sets[0] == VK_NULL_HANDLE)
  {
    fprintf (stderr, "BACKEND_SESSION_MANAGED_DESCRIPTOR_REUSE_FAILED result=%d\n", result);

    return 1;
  }

  result = backend_vulkan_session_command_acquire (
    backend_ctx, 0u, &second, stdout
  );

  if (result != VK_SUCCESS
   || second.command != first.command
   || second.fence != first.fence)
  {
    fprintf (stderr, "BACKEND_SESSION_MANAGED_REUSE_FAILED result=%d\n", result);

    return 1;
  }

  result = vkBeginCommandBuffer (second.command, &begin);

  if (result != VK_SUCCESS) return 1;

  result = vkEndCommandBuffer (second.command);

  if (result != VK_SUCCESS) return 1;

  result = backend_vulkan_session_command_submit (
    backend_ctx, 0u
  );

  if (result != VK_SUCCESS) return 1;

  const VkResult normal_poll =
    backend_vulkan_session_command_complete_timeout (
      backend_ctx, 0u, 0u
    );

  if (normal_poll != VK_SUCCESS && normal_poll != VK_TIMEOUT)
  {
    fprintf (stderr, "BACKEND_SESSION_MANAGED_NORMAL_WAIT_FAILED result=%d\n", normal_poll);

    return 1;
  }

  if (normal_poll == VK_TIMEOUT)
  {
    result = backend_vulkan_session_command_complete (
      backend_ctx, 0u
    );

    if (result != VK_SUCCESS) return 1;
  }

  if (backend_vulkan_session_command_cancel (
    backend_ctx, 0u
  ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_session_command_submit (
        backend_ctx, 0u
      ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (stderr, "BACKEND_SESSION_MANAGED_FINISHED_CONTROLS_FAILED\n");

    return 1;
  }

  if (backend_vulkan_session_status (
    backend_ctx, 0u, &command_status
  ) != VK_SUCCESS
   || command_status.compute_leased != VK_TRUE
   || command_status.compute_pending != VK_FALSE
   || command_status.compute_cancel_requested != VK_FALSE
   || command_status.descriptors_leased != VK_TRUE
   || command_status.compute_submissions != 2u
   || command_status.compute_completions != 1u
   || command_status.compute_cancellations != 1u)
  {
    fprintf (stderr, "BACKEND_SESSION_MANAGED_COMPLETED_STATUS_FAILED\n");

    return 1;
  }

  result = backend_vulkan_session_descriptors_release (
    backend_ctx, 0u
  );

  if (result != VK_SUCCESS) return 1;

  result = backend_vulkan_session_command_release (
    backend_ctx, 0u, VK_TRUE
  );

  if (result != VK_SUCCESS) return 1;

  const uint32_t payload = UINT32_C (0x51ca9703) ^ trial;

  memcpy (staging_views[0].mapped, &payload, sizeof (payload));

  result = backend_vulkan_session_storage_transfer (
    backend_ctx, 0u, 0u, sizeof (payload), VK_FALSE
  );

  if (result != VK_SUCCESS) return 1;

  memset (staging_views[0].mapped, 0, sizeof (payload));

  result = backend_vulkan_session_storage_transfer (
    backend_ctx, 0u, 0u, sizeof (payload), VK_TRUE
  );

  if (result != VK_SUCCESS
   || memcmp (staging_views[0].mapped, &payload, sizeof (payload)) != 0)
  {
    fprintf (stderr, "BACKEND_SESSION_MANAGED_STORAGE_FAILED result=%d\n", result);

    return 1;
  }

  fprintf (
    stdout,
    "BACKEND_SESSION_MANAGED_COMPUTE_PASS"
    " trial=%" PRIu32
    " submissions=2 cancelled=1 completed=1"
    " command_reuses=2"
    " cancelled_poll=%d normal_poll=%d"
    " verified_bytes=4\n",
    trial, cancelled_poll, normal_poll
  );

  return 0;
}

/**
 * Verify an abandoned real staging submission and explicit recovery.
 *
 * Submits a four-byte upload without awaiting its fence. The pending
 * transfer prevents another staging submission or Compute command lease.
 * Recovery waits for real device idle, replaces its transfer command/fence
 * pair, and preserves the mapped staging slot and independent Compute
 * resources. Readback verifies the abandoned upload reached GPU memory.
 * A second asynchronous upload and download use zero-timeout Vulkan
 * fence polls, each accepting actual VK_SUCCESS or VK_TIMEOUT. Timed-out
 * submissions retain their exclusive leases and complete through the
 * unchanged blocking API. Readback checks both values and verifies that
 * a finished transfer cannot be completed twice. No driver error is forged.
 */
static int verify_staging_recovery_trial (
  backend_ctx_t *backend_ctx,
  const backend_vulkan_storage_view_t *staging_views,
  uint32_t trial)
{
  if (backend_ctx == NULL || staging_views == NULL
   || staging_views[0].mapped == NULL
   || staging_views[0].staging != VK_TRUE
   || staging_views[0].bytes < sizeof (uint32_t))
  {
    return 1;
  }

  const uint32_t initial = UINT32_C (0x765ae19d) ^ trial;
  const uint32_t final = UINT32_C (0x319e2b7f) ^ trial;

  if (backend_vulkan_session_storage_recover (
    backend_ctx, 0u, stdout
  ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_session_storage_transfer_complete (
    backend_ctx, 0u
  ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (stderr, "BACKEND_SESSION_STAGING_RECOVERY_HEALTHY_FAILED\n");

    return 1;
  }

  memcpy (staging_views[0].mapped, &initial, sizeof (initial));

  VkResult result = backend_vulkan_session_storage_transfer_begin (
    backend_ctx, 0u, 0u, sizeof (initial), VK_FALSE
  );

  if (result != VK_SUCCESS)
  {
    fprintf (stderr, "BACKEND_SESSION_STAGING_RECOVERY_SUBMIT_FAILED result=%d\n", result);

    return 1;
  }

  backend_vulkan_command_view_t command = { 0 };

  if (backend_vulkan_session_storage_transfer_begin (
    backend_ctx, 0u, 0u, sizeof (initial), VK_FALSE
  ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_session_storage_transfer (
        backend_ctx, 0u, 0u, sizeof (initial), VK_TRUE
      ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_session_command_acquire (
        backend_ctx, 0u, &command, stdout
      ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_session_storage_transfer_complete (
        backend_ctx, backend_ctx->vulkan_devices_cnt
      ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_session_storage_recover (
        backend_ctx, backend_ctx->vulkan_devices_cnt, stdout
      ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_session_storage_recover (
        backend_ctx, 0u, NULL
      ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (stderr, "BACKEND_SESSION_STAGING_RECOVERY_CONTROLS_FAILED\n");

    return 1;
  }

  result = backend_vulkan_session_storage_recover (
    backend_ctx, 0u, stdout
  );

  if (result != VK_SUCCESS)
  {
    fprintf (stderr, "BACKEND_SESSION_STAGING_RECOVERY_FAILED result=%d\n", result);

    return 1;
  }

  if (backend_vulkan_session_storage_recover (
    backend_ctx, 0u, stdout
  ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_session_storage_transfer_complete (
        backend_ctx, 0u
      ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (stderr, "BACKEND_SESSION_STAGING_RECOVERY_DOUBLE_ACCEPTED\n");

    return 1;
  }

  memset (staging_views[0].mapped, 0, sizeof (initial));

  result = backend_vulkan_session_storage_transfer (
    backend_ctx, 0u, 0u, sizeof (initial), VK_TRUE
  );

  if (result != VK_SUCCESS
   || memcmp (staging_views[0].mapped, &initial, sizeof (initial)) != 0)
  {
    fprintf (stderr, "BACKEND_SESSION_STAGING_RECOVERY_READBACK_FAILED result=%d\n", result);

    return 1;
  }

  memcpy (staging_views[0].mapped, &final, sizeof (final));

  result = backend_vulkan_session_storage_transfer_begin (
    backend_ctx, 0u, 0u, sizeof (final), VK_FALSE
  );

  if (result != VK_SUCCESS) return 1;

  const VkResult upload_poll =
    backend_vulkan_session_storage_transfer_complete_timeout (
      backend_ctx, 0u, 0u
    );

  if (upload_poll != VK_SUCCESS && upload_poll != VK_TIMEOUT)
  {
    fprintf (stderr, "BACKEND_SESSION_STAGING_UPLOAD_POLL_FAILED result=%d\n", upload_poll);

    return 1;
  }

  if (upload_poll == VK_TIMEOUT)
  {
    if (backend_vulkan_session_command_acquire (
      backend_ctx, 0u, &command, stdout
    ) != VK_ERROR_INITIALIZATION_FAILED)
    {
      fprintf (stderr, "BACKEND_SESSION_STAGING_UPLOAD_POLL_LEASE_FAILED\n");

      return 1;
    }

    result = backend_vulkan_session_storage_transfer_complete (
      backend_ctx, 0u
    );

    if (result != VK_SUCCESS) return 1;
  }

  memset (staging_views[0].mapped, 0, sizeof (final));

  result = backend_vulkan_session_storage_transfer_begin (
    backend_ctx, 0u, 0u, sizeof (final), VK_TRUE
  );

  if (result != VK_SUCCESS) return 1;

  const VkResult download_poll =
    backend_vulkan_session_storage_transfer_complete_timeout (
      backend_ctx, 0u, 0u
    );

  if (download_poll != VK_SUCCESS && download_poll != VK_TIMEOUT)
  {
    fprintf (stderr, "BACKEND_SESSION_STAGING_DOWNLOAD_POLL_FAILED result=%d\n", download_poll);

    return 1;
  }

  if (download_poll == VK_TIMEOUT)
  {
    if (backend_vulkan_session_storage_transfer_begin (
      backend_ctx, 0u, 0u, sizeof (final), VK_FALSE
    ) != VK_ERROR_INITIALIZATION_FAILED)
    {
      fprintf (stderr, "BACKEND_SESSION_STAGING_DOWNLOAD_POLL_LEASE_FAILED\n");

      return 1;
    }

    result = backend_vulkan_session_storage_transfer_complete (
      backend_ctx, 0u
    );

    if (result != VK_SUCCESS) return 1;
  }

  if (memcmp (staging_views[0].mapped, &final, sizeof (final)) != 0)
  {
    fprintf (stderr, "BACKEND_SESSION_STAGING_RECOVERY_FINAL_FAILED\n");

    return 1;
  }

  if (backend_vulkan_session_storage_transfer_complete_timeout (
    backend_ctx, 0u, 0u
  ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (stderr, "BACKEND_SESSION_STAGING_DOUBLE_COMPLETION_ACCEPTED\n");

    return 1;
  }

  fprintf (
    stdout,
    "BACKEND_SESSION_STAGING_POLL_PASS"
    " trial=%" PRIu32
    " timeout_ns=0"
    " upload_result=%d"
    " download_result=%d"
    " completed=2 verified_bytes=4\n",
    trial, upload_poll, download_poll
  );

  fprintf (
    stdout,
    "BACKEND_SESSION_STAGING_RECOVERY_PASS"
    " trial=%" PRIu32
    " abandoned=1 recovered=1"
    " completed=3 verified_bytes=8"
    " rejected=10\n",
    trial
  );

  return 0;
}

/**
 * Verify cooperative cancellation of real queued upload and download copies.
 *
 * An upload is submitted to the GPU, marked unwanted without waiting and
 * completed through the real transfer fence. The destination is read back
 * normally to prove cancellation never pretends to revoke a queue submit.
 * The independent download cancellation also retires only after its fence;
 * a subsequent normal readback validates the live mapped storage slot.
 *
 * The test rejects cancellation without a pending transfer, repeat
 * cancellation requests, mismatched indices, conflicting new submissions
 * and Compute command acquisition while the cancelled copy is in flight.
 * No artificial driver errors or GPU device loss are introduced.
 */
static int verify_staging_cancellation_trial (
  backend_ctx_t *backend_ctx,
  const backend_vulkan_storage_view_t *staging_views,
  uint32_t trial)
{
  if (backend_ctx == NULL || staging_views == NULL
   || staging_views[0].staging != VK_TRUE
   || staging_views[0].mapped == NULL
   || staging_views[0].bytes < sizeof (uint32_t))
  {
    return 1;
  }

  const uint32_t payload = UINT32_C (0x4ae7310c) ^ trial;

  if (backend_vulkan_session_storage_transfer_cancel (
    backend_ctx, 0u
  ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (stderr, "BACKEND_SESSION_STAGING_CANCEL_HEALTHY_ACCEPTED\n");

    return 1;
  }

  memcpy (staging_views[0].mapped, &payload, sizeof (payload));

  VkResult result = backend_vulkan_session_storage_transfer_begin (
    backend_ctx, 0u, 0u, sizeof (payload), VK_FALSE
  );

  if (result != VK_SUCCESS)
  {
    fprintf (stderr, "BACKEND_SESSION_STAGING_CANCEL_UPLOAD_FAILED result=%d\n", result);

    return 1;
  }

  backend_vulkan_command_view_t command = { 0 };

  if (backend_vulkan_session_storage_transfer_cancel (
    backend_ctx, backend_ctx->vulkan_devices_cnt
  ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_session_storage_transfer_cancel (
        backend_ctx, 0u
      ) != VK_SUCCESS
   || backend_vulkan_session_storage_transfer_cancel (
        backend_ctx, 0u
      ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_session_storage_transfer_begin (
        backend_ctx, 0u, 0u, sizeof (payload), VK_FALSE
      ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_session_command_acquire (
        backend_ctx, 0u, &command, stdout
      ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (stderr, "BACKEND_SESSION_STAGING_CANCEL_UPLOAD_CONTROLS_FAILED\n");

    return 1;
  }

  backend_vulkan_session_status_t transfer_status = { 0 };

  if (backend_vulkan_session_status (
    backend_ctx, 0u, &transfer_status
  ) != VK_SUCCESS
   || transfer_status.transfer_pending != VK_TRUE
   || transfer_status.transfer_cancel_requested != VK_TRUE
   || transfer_status.transfer_failed != VK_FALSE
   || transfer_status.compute_leased != VK_FALSE
   || transfer_status.descriptors_leased != VK_FALSE
   || transfer_status.transfer_cancellations != 0u)
  {
    fprintf (stderr, "BACKEND_SESSION_STAGING_CANCEL_REQUEST_STATUS_FAILED\n");

    return 1;
  }

  if (backend_vulkan_session_close_if_ready (
    backend_ctx, 0u
  ) != VK_NOT_READY)
  {
    fprintf (stderr, "BACKEND_SESSION_STAGING_PENDING_CLOSE_ACCEPTED\n");

    return 1;
  }

  const VkResult upload_poll =
    backend_vulkan_session_storage_transfer_complete_timeout (
      backend_ctx, 0u, 0u
    );

  if (upload_poll != VK_SUCCESS && upload_poll != VK_TIMEOUT)
  {
    fprintf (stderr, "BACKEND_SESSION_STAGING_CANCEL_UPLOAD_POLL_FAILED result=%d\n", upload_poll);

    return 1;
  }

  if (upload_poll == VK_TIMEOUT)
  {
    if (backend_vulkan_session_command_acquire (
      backend_ctx, 0u, &command, stdout
    ) != VK_ERROR_INITIALIZATION_FAILED)
    {
      fprintf (stderr, "BACKEND_SESSION_STAGING_CANCEL_TIMEOUT_LEASE_FAILED\n");

      return 1;
    }

    result = backend_vulkan_session_storage_transfer_complete (
      backend_ctx, 0u
    );

    if (result != VK_SUCCESS) return 1;
  }

  if (backend_vulkan_session_storage_transfer_cancel (
    backend_ctx, 0u
  ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (stderr, "BACKEND_SESSION_STAGING_CANCEL_UPLOAD_REPEAT_ACCEPTED\n");

    return 1;
  }

  memset (staging_views[0].mapped, 0, sizeof (payload));

  result = backend_vulkan_session_storage_transfer (
    backend_ctx, 0u, 0u, sizeof (payload), VK_TRUE
  );

  if (result != VK_SUCCESS
   || memcmp (staging_views[0].mapped, &payload, sizeof (payload)) != 0)
  {
    fprintf (stderr, "BACKEND_SESSION_STAGING_CANCEL_UPLOAD_READBACK_FAILED result=%d\n", result);

    return 1;
  }

  result = backend_vulkan_session_storage_transfer_begin (
    backend_ctx, 0u, 0u, sizeof (payload), VK_TRUE
  );

  if (result != VK_SUCCESS) return 1;

  if (backend_vulkan_session_storage_transfer_cancel (
    backend_ctx, 0u
  ) != VK_SUCCESS)
  {
    fprintf (stderr, "BACKEND_SESSION_STAGING_CANCEL_DOWNLOAD_REQUEST_FAILED\n");

    return 1;
  }

  const VkResult download_poll =
    backend_vulkan_session_storage_transfer_complete_timeout (
      backend_ctx, 0u, 0u
    );

  if (download_poll != VK_SUCCESS && download_poll != VK_TIMEOUT)
  {
    fprintf (stderr, "BACKEND_SESSION_STAGING_CANCEL_DOWNLOAD_POLL_FAILED result=%d\n", download_poll);

    return 1;
  }

  if (download_poll == VK_TIMEOUT)
  {
    result = backend_vulkan_session_storage_transfer_complete (
      backend_ctx, 0u
    );

    if (result != VK_SUCCESS) return 1;
  }

  if (backend_vulkan_session_storage_transfer_complete (
    backend_ctx, 0u
  ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (stderr, "BACKEND_SESSION_STAGING_CANCEL_DOUBLE_COMPLETION_ACCEPTED\n");

    return 1;
  }

  memset (staging_views[0].mapped, 0, sizeof (payload));

  result = backend_vulkan_session_storage_transfer (
    backend_ctx, 0u, 0u, sizeof (payload), VK_TRUE
  );

  if (result != VK_SUCCESS
   || memcmp (staging_views[0].mapped, &payload, sizeof (payload)) != 0)
  {
    fprintf (stderr, "BACKEND_SESSION_STAGING_CANCEL_DOWNLOAD_READBACK_FAILED result=%d\n", result);

    return 1;
  }

  if (backend_vulkan_session_status (
    backend_ctx, 0u, &transfer_status
  ) != VK_SUCCESS
   || transfer_status.transfer_pending != VK_FALSE
   || transfer_status.transfer_cancel_requested != VK_FALSE
   || transfer_status.transfer_failed != VK_FALSE
   || transfer_status.transfer_cancellations != 2u
   || transfer_status.compute_submissions != 2u
   || transfer_status.compute_completions != 1u
   || transfer_status.compute_cancellations != 1u)
  {
    fprintf (stderr, "BACKEND_SESSION_STAGING_CANCEL_RETIRED_STATUS_FAILED\n");

    return 1;
  }

  fprintf (
    stdout,
    "BACKEND_SESSION_STAGING_CANCEL_PASS"
    " trial=%" PRIu32
    " cancelled=2"
    " verified_downloads=2"
    " upload_poll=%d"
    " download_poll=%d"
    " preserved_storage=1\n",
    trial, upload_poll, download_poll
  );

  return 0;
}

/**
 * Verify two independently owned Vulkan Compute sessions and instances.
 *
 * Each session uses an explicit physical-device index and public
 * 1x1x1 workload declaration. Invalid host calls and a physical-device
 * limit refusal must leave the owner empty before a successful open.
 * An active session also prevents premature VkInstance destruction.
 */
static int verify_session_trial (
  backend_ctx_t *backend_ctx,
  uint32_t minimum_api,
  uint32_t trial)
{
  const vulkan_auto_compute_requirements_t workload =
  {
    .local_size = { 1u, 1u, 1u },
    .group_count = { 1u, 1u, 1u },
    .storage_buffer_count = 1u,
    .largest_storage_buffer_bytes = 4u,
    .shared_memory_bytes = 0u
  };

  if (backend_vulkan_session_open (
    backend_ctx, 0u, &workload, stdout
  ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (stderr, "BACKEND_SESSION_UNOPENED_ACCEPTED\n");

    return 1;
  }

  if (backend_vulkan_session_close (backend_ctx) != VK_SUCCESS)
  {
    fprintf (stderr, "BACKEND_SESSION_EMPTY_CLOSE_FAILED\n");

    return 1;
  }

  VkResult result = backend_vulkan_catalog_open (
    backend_ctx, minimum_api, stdout
  );

  if (result != VK_SUCCESS)
  {
    fprintf (stderr, "BACKEND_SESSION_CATALOG_OPEN_FAILED result=%d\n", result);

    return 1;
  }

  int status = 1;

  if (backend_ctx->vulkan_catalog == NULL
   || backend_ctx->vulkan_devices_cnt == 0u
   || backend_ctx->vulkan_session != NULL)
  {
    fprintf (stderr, "BACKEND_SESSION_CATALOG_STATE_INVALID\n");

    goto cleanup_catalog;
  }

  const VkResult reject_index = backend_vulkan_session_open (
    backend_ctx, backend_ctx->vulkan_devices_cnt, &workload, stdout
  );
  const VkResult reject_workload = backend_vulkan_session_open (
    backend_ctx, 0u, NULL, stdout
  );
  const VkResult reject_diagnostics = backend_vulkan_session_open (
    backend_ctx, 0u, &workload, NULL
  );

  if (reject_index != VK_ERROR_INITIALIZATION_FAILED
   || reject_workload != VK_ERROR_INITIALIZATION_FAILED
   || reject_diagnostics != VK_ERROR_INITIALIZATION_FAILED
   || backend_ctx->vulkan_session != NULL)
  {
    fprintf (stderr, "BACKEND_SESSION_INVALID_CONTROLS_FAILED\n");

    goto cleanup_catalog;
  }

  vulkan_auto_compute_requirements_t unsupported = workload;

  unsupported.local_size[0] = UINT32_MAX;

  result = backend_vulkan_session_open (
    backend_ctx, 0u, &unsupported, stdout
  );

  if (result != VK_ERROR_FEATURE_NOT_PRESENT
   || backend_ctx->vulkan_session != NULL)
  {
    fprintf (stderr, "BACKEND_SESSION_LIMIT_CONTROL_FAILED result=%d\n", result);

    goto cleanup_catalog;
  }

  fprintf (
    stdout,
    "BACKEND_SESSION_INVALID_CONTROLS trial=%" PRIu32
    " invalid=4 unsupported=1 active=0\n",
    trial
  );

  result = backend_vulkan_session_open (
    backend_ctx, 0u, &workload, stdout
  );

  if (result != VK_SUCCESS || backend_ctx->vulkan_session == NULL)
  {
    fprintf (stderr, "BACKEND_SESSION_OPEN_FAILED result=%d\n", result);

    goto cleanup_catalog;
  }

  backend_vulkan_device_view_t view = { 0 };

  const VkResult borrow_result = backend_vulkan_session_device (
    backend_ctx, 0u, &view
  );

  if (borrow_result != VK_SUCCESS
   || view.device == VK_NULL_HANDLE
   || view.queue == VK_NULL_HANDLE
   || view.device_index != 0u
   || view.effective_api != minimum_api)
  {
    fprintf (stderr, "BACKEND_SESSION_DEVICE_BORROW_FAILED result=%d\n", borrow_result);

    goto cleanup_session;
  }

  const backend_vulkan_device_view_t original_view = view;

  const VkResult reject_borrow_index = backend_vulkan_session_device (
    backend_ctx, backend_ctx->vulkan_devices_cnt, &view
  );
  const VkResult reject_borrow_output = backend_vulkan_session_device (
    backend_ctx, 0u, NULL
  );

  if (reject_borrow_index != VK_ERROR_INITIALIZATION_FAILED
   || reject_borrow_output != VK_ERROR_INITIALIZATION_FAILED
   || memcmp (&view, &original_view, sizeof (view)) != 0)
  {
    fprintf (stderr, "BACKEND_SESSION_DEVICE_BORROW_CONTROLS_FAILED\n");

    goto cleanup_session;
  }

  backend_vulkan_session_status_t snapshot =
  {
    .compute_pending = VK_TRUE,
    .transfer_pending = VK_TRUE
  };

  const backend_vulkan_session_status_t invalid_snapshot = snapshot;

  if (backend_vulkan_session_status (
    backend_ctx, backend_ctx->vulkan_devices_cnt, &snapshot
  ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_session_status (
        backend_ctx, 0u, NULL
      ) != VK_ERROR_INITIALIZATION_FAILED
   || memcmp (&snapshot, &invalid_snapshot, sizeof (snapshot)) != 0
   || backend_vulkan_session_status (
        backend_ctx, 0u, &snapshot
      ) != VK_SUCCESS
   || snapshot.compute_leased != VK_FALSE
   || snapshot.compute_pending != VK_FALSE
   || snapshot.compute_cancel_requested != VK_FALSE
   || snapshot.compute_failed != VK_FALSE
   || snapshot.descriptors_leased != VK_FALSE
   || snapshot.transfer_pending != VK_FALSE
   || snapshot.transfer_cancel_requested != VK_FALSE
   || snapshot.transfer_failed != VK_FALSE
   || snapshot.compute_submissions != 0u
   || snapshot.compute_completions != 0u
   || snapshot.compute_cancellations != 0u
   || snapshot.transfer_completions != 0u
   || snapshot.transfer_cancellations != 0u)
  {
    fprintf (stderr, "BACKEND_SESSION_STATUS_INITIAL_FAILED\n");

    goto cleanup_session;
  }

  fprintf (
    stdout,
    "BACKEND_SESSION_DEVICE_BORROW_PASS trial=%" PRIu32
    " index=%" PRIu32 " queue_family=%" PRIu32
    " rejected=2 owned=1\n",
    trial, view.device_index, view.queue_family
  );

  vulkan_auto_device_t selected = { 0 };
  uint32_t effective_api = 0u;

  result = backend_vulkan_catalog_select (
    backend_ctx, 0u, &workload, &selected, &effective_api
  );

  if (result != VK_SUCCESS || effective_api != minimum_api)
  {
    fprintf (stderr, "BACKEND_SESSION_STORAGE_DEVICE_FAILED result=%d\n", result);

    goto cleanup_session;
  }

  const VkDeviceSize initial_bytes[2] = { 64u, 128u };
  const VkDeviceSize reused_bytes[2] = { 32u, 96u };
  const VkDeviceSize grown_bytes[2] = { 256u, 96u };
  backend_vulkan_storage_view_t initial[2] = { 0 };
  backend_vulkan_storage_view_t reused[2] = { 0 };
  backend_vulkan_storage_view_t grown[2] = { 0 };

  result = backend_vulkan_session_storage_reserve (
    backend_ctx, 0u, &selected, 2u, initial_bytes, VK_FALSE, initial, stdout
  );

  if (result != VK_SUCCESS
   || initial[0].buffer == VK_NULL_HANDLE
   || initial[1].buffer == VK_NULL_HANDLE
   || initial[0].mapped == NULL
   || initial[1].mapped == NULL)
  {
    fprintf (stderr, "BACKEND_SESSION_STORAGE_ALLOC_FAILED result=%d\n", result);

    goto cleanup_session;
  }

  result = backend_vulkan_session_storage_reserve (
    backend_ctx, 0u, &selected, 2u, reused_bytes, VK_FALSE, reused, stdout
  );

  if (result != VK_SUCCESS
   || reused[0].buffer != initial[0].buffer
   || reused[1].buffer != initial[1].buffer
   || reused[0].bytes != 32u
   || reused[1].bytes != 96u
   || reused[0].capacity_bytes != 64u
   || reused[1].capacity_bytes != 128u)
  {
    fprintf (stderr, "BACKEND_SESSION_STORAGE_REUSE_FAILED result=%d\n", result);

    goto cleanup_session;
  }

  result = backend_vulkan_session_storage_reserve (
    backend_ctx, 0u, &selected, 2u, grown_bytes, VK_FALSE, grown, stdout
  );

  if (result != VK_SUCCESS
   || grown[0].capacity_bytes != 256u
   || grown[1].buffer != initial[1].buffer
   || grown[1].capacity_bytes != 128u)
  {
    fprintf (stderr, "BACKEND_SESSION_STORAGE_GROW_FAILED result=%d\n", result);

    goto cleanup_session;
  }

  const VkDeviceSize invalid_bytes[2] = { 0u, 96u };

  if (backend_vulkan_session_storage_reserve (
    backend_ctx, 0u, &selected, 2u, invalid_bytes, VK_FALSE, grown, stdout
  ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_session_storage_reserve (
    backend_ctx, backend_ctx->vulkan_devices_cnt,
    &selected, 2u, grown_bytes, VK_FALSE, grown, stdout
  ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (stderr, "BACKEND_SESSION_STORAGE_CONTROLS_FAILED\n");

    goto cleanup_session;
  }

  fprintf (
    stdout,
    "BACKEND_SESSION_STORAGE_PASS trial=%" PRIu32
    " allocations=3 reuses=3 growth=1 invalid=2\n",
    trial
  );

  backend_vulkan_storage_view_t staging_views[2] = { 0 };

  if (verify_staging_transfer_trial (
    backend_ctx, &selected, trial, staging_views
  ) != 0)
  {
    goto cleanup_session;
  }

  if (verify_cached_compute_resources_trial (
    backend_ctx, staging_views, trial
  ) != 0)
  {
    goto cleanup_session;
  }

  if (verify_command_recovery_trial (
    backend_ctx, staging_views, trial
  ) != 0)
  {
    goto cleanup_session;
  }

  if (verify_managed_compute_cancellation_trial (
    backend_ctx, staging_views, trial
  ) != 0)
  {
    goto cleanup_session;
  }

  if (verify_staging_recovery_trial (
    backend_ctx, staging_views, trial
  ) != 0)
  {
    goto cleanup_session;
  }

  if (verify_staging_cancellation_trial (
    backend_ctx, staging_views, trial
  ) != 0)
  {
    goto cleanup_session;
  }

  if (backend_vulkan_session_status (
    backend_ctx, 0u, &snapshot
  ) != VK_SUCCESS
   || snapshot.compute_leased != VK_FALSE
   || snapshot.compute_pending != VK_FALSE
   || snapshot.compute_cancel_requested != VK_FALSE
   || snapshot.compute_failed != VK_FALSE
   || snapshot.descriptors_leased != VK_FALSE
   || snapshot.transfer_pending != VK_FALSE
   || snapshot.transfer_cancel_requested != VK_FALSE
   || snapshot.transfer_failed != VK_FALSE
   || snapshot.compute_submissions != 2u
   || snapshot.compute_completions != 1u
   || snapshot.compute_cancellations != 1u
   || snapshot.transfer_completions != 17u
   || snapshot.transfer_cancellations != 2u)
  {
    fprintf (stderr, "BACKEND_SESSION_STATUS_FINAL_FAILED\n");

    goto cleanup_session;
  }

  fprintf (
    stdout,
    "BACKEND_SESSION_STATUS_PASS"
    " trial=%" PRIu32
    " pending=0 leased=0 poisoned=0"
    " compute_submitted=%" PRIu32
    " compute_completed=%" PRIu32
    " compute_cancelled=%" PRIu32
    " transfer_completed=%" PRIu32
    " transfer_cancelled=%" PRIu32 "\n",
    trial,
    snapshot.compute_submissions,
    snapshot.compute_completions,
    snapshot.compute_cancellations,
    snapshot.transfer_completions,
    snapshot.transfer_cancellations
  );

  if (backend_vulkan_session_close_if_ready (
    backend_ctx, backend_ctx->vulkan_devices_cnt
  ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_ctx->vulkan_session == NULL)
  {
    fprintf (stderr, "BACKEND_SESSION_CLOSE_INDEX_ACCEPTED\n");

    goto cleanup_session;
  }

  const VkResult reject_reopen = backend_vulkan_session_open (
    backend_ctx, 0u, &workload, stdout
  );
  const VkResult reject_catalog_close = backend_vulkan_catalog_close (
    backend_ctx
  );

  if (reject_reopen != VK_ERROR_INITIALIZATION_FAILED
   || reject_catalog_close != VK_ERROR_INITIALIZATION_FAILED
   || backend_ctx->vulkan_session == NULL
   || backend_ctx->vulkan_catalog == NULL)
  {
    fprintf (stderr, "BACKEND_SESSION_OWNER_ORDER_FAILED\n");

    goto cleanup_session;
  }

  fprintf (
    stdout,
    "BACKEND_SESSION_OWNER_CONTROLS trial=%" PRIu32
    " rejects=2 active=1\n", trial
  );

  if (backend_ctx->backend_devices_cnt != 0
   || backend_ctx->devices_param != NULL
   || backend_ctx->enabled != false)
  {
    fprintf (stderr, "BACKEND_SESSION_SCHEDULER_STATE_FAILED\n");

    goto cleanup_session;
  }

  status = 0;

cleanup_session:
  result = status == 0
    ? backend_vulkan_session_close_if_ready (backend_ctx, 0u)
    : backend_vulkan_session_close (backend_ctx);

  if (result != VK_SUCCESS || backend_ctx->vulkan_session != NULL)
  {
    fprintf (stderr, "BACKEND_SESSION_DEVICE_CLOSE_FAILED result=%d\n", result);

    status = 1;
  }

  if (backend_vulkan_session_close (backend_ctx) != VK_SUCCESS
   || backend_vulkan_session_close_if_ready (
        backend_ctx, 0u
      ) != VK_SUCCESS)
  {
    fprintf (stderr, "BACKEND_SESSION_SECOND_CLOSE_FAILED\n");

    status = 1;
  }

  if (status == 0)
  {
    fprintf (
      stdout,
      "BACKEND_SESSION_CLOSE_GUARD_PASS"
      " trial=%" PRIu32 " deferred=5 ready=1 idempotent=1\n",
      trial
    );
  }

cleanup_catalog:
  if (backend_vulkan_catalog_close (backend_ctx) != VK_SUCCESS
   || backend_ctx->vulkan_catalog != NULL
   || backend_ctx->vulkan_devices_cnt != 0u)
  {
    fprintf (stderr, "BACKEND_SESSION_INSTANCE_CLOSE_FAILED\n");

    status = 1;
  }

  if (status == 0)
  {
    fprintf (
      stdout,
      "BACKEND_SESSION_TRIAL_PASS trial=%" PRIu32
      " invalid=4 owner_rejects=2 empty_submissions=1"
      " storage_allocations=5 storage_reuses=3 storage_growth=1"
      " staging_transfers=17 staging_rejected=5"
      " staging_recoveries=1 staging_abandoned=1"
      " staging_cancellations=2"
      " cached_pipelines=2 cache_hits=1"
      " descriptor_pools=2 descriptor_reuses=4 descriptor_writes=14"
      " command_acquisitions=6 command_reuses=4"
      " managed_submissions=2 managed_completed=1 managed_cancelled=1"
      " command_recoveries=1"
      " scheduled=0\n",
      trial
    );
  }

  return status;
}

/**
 * Verify graceful shutdown against a real outstanding Compute or transfer.
 *
 * Each trial opens an independent logical-device session. Unsubmitted
 * borrowed Compute resources reject closure without being destroyed.
 * Pending Compute and staging work are cooperatively canceled, retaining
 * their leases through an actual fence wait; a zero-timeout pending result
 * is completed by the next unlimited poll. The third trial completes a
 * managed Compute submission before requesting shutdown, confirming that
 * its still-borrowed command and descriptors are retired without a second
 * GPU wait. Absent sessions accept repeated closure without driver calls.
 */
static int verify_shutdown_trial (
  backend_ctx_t *backend_ctx,
  uint32_t minimum_api,
  uint32_t trial)
{
  const vulkan_auto_compute_requirements_t workload =
  {
    .local_size = { 1u, 1u, 1u },
    .group_count = { 1u, 1u, 1u },
    .storage_buffer_count = 2u,
    .largest_storage_buffer_bytes = 16u,
    .shared_memory_bytes = 0u
  };

  VkResult result = backend_vulkan_catalog_open (
    backend_ctx, minimum_api, stdout
  );

  if (result != VK_SUCCESS) return 1;

  int status = 1;

  result = backend_vulkan_session_open (
    backend_ctx, 0u, &workload, stdout
  );

  if (result != VK_SUCCESS)
  {
    fprintf (stderr, "BACKEND_SESSION_SHUTDOWN_OPEN_FAILED result=%d\n", result);

    goto cleanup;
  }

  if (backend_vulkan_session_shutdown_poll (
    backend_ctx, backend_ctx->vulkan_devices_cnt, 0u
  ) != VK_ERROR_INITIALIZATION_FAILED
   || backend_vulkan_session_shutdown_poll (
        NULL, 0u, 0u
      ) != VK_ERROR_INITIALIZATION_FAILED)
  {
    fprintf (stderr, "BACKEND_SESSION_SHUTDOWN_INVALID_ACCEPTED\n");

    goto cleanup;
  }

  vulkan_auto_device_t selected = { 0 };
  uint32_t effective_api = 0u;

  result = backend_vulkan_catalog_select (
    backend_ctx, 0u, &workload, &selected, &effective_api
  );

  if (result != VK_SUCCESS || effective_api != minimum_api)
  {
    fprintf (stderr, "BACKEND_SESSION_SHUTDOWN_SELECT_FAILED result=%d\n", result);

    goto cleanup;
  }

  const VkDeviceSize bytes[2] = { 16u, 16u };
  backend_vulkan_storage_view_t views[2] = { 0 };

  result = backend_vulkan_session_storage_reserve (
    backend_ctx, 0u, &selected, 2u, bytes,
    trial == 1u ? VK_TRUE : VK_FALSE, views, stdout
  );

  if (result != VK_SUCCESS)
  {
    fprintf (stderr, "BACKEND_SESSION_SHUTDOWN_STORAGE_FAILED result=%d\n", result);

    goto cleanup;
  }

  if (trial != 1u)
  {
    backend_vulkan_pipeline_view_t pipeline = { 0 };
    const uint32_t words = (uint32_t) (
      sizeof (backend_vulkan_public_spirv) / sizeof (uint32_t)
    );

    result = backend_vulkan_session_pipeline_acquire (
      backend_ctx, 0u, backend_vulkan_public_spirv,
      words, &pipeline, stdout
    );

    if (result != VK_SUCCESS) goto cleanup;

    const backend_vulkan_descriptor_binding_t binding =
    {
      .source_buffer = views[0].buffer,
      .destination_buffer = views[1].buffer,
      .source_bytes = views[0].bytes,
      .destination_bytes = views[1].bytes
    };

    backend_vulkan_descriptor_view_t descriptors = { 0 };

    result = backend_vulkan_session_descriptors_acquire (
      backend_ctx, 0u, &binding, 1u, &descriptors, stdout
    );

    if (result != VK_SUCCESS) goto cleanup;

    backend_vulkan_command_view_t command = { 0 };

    result = backend_vulkan_session_command_acquire (
      backend_ctx, 0u, &command, stdout
    );

    if (result != VK_SUCCESS) goto cleanup;

    if (backend_vulkan_session_shutdown_poll (
      backend_ctx, 0u, 0u
    ) != VK_NOT_READY)
    {
      fprintf (stderr, "BACKEND_SESSION_SHUTDOWN_UNSUBMITTED_ACCEPTED\n");

      goto cleanup;
    }

    backend_vulkan_session_status_t unsubmitted = { 0 };

    if (backend_vulkan_session_status (
      backend_ctx, 0u, &unsubmitted
    ) != VK_SUCCESS
     || unsubmitted.compute_leased != VK_TRUE
     || unsubmitted.descriptors_leased != VK_TRUE
     || unsubmitted.compute_pending != VK_FALSE)
    {
      fprintf (stderr, "BACKEND_SESSION_SHUTDOWN_UNSUBMITTED_STATE_FAILED\n");

      goto cleanup;
    }

    const VkCommandBufferBeginInfo begin =
    {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };

    result = vkBeginCommandBuffer (command.command, &begin);

    if (result != VK_SUCCESS) goto cleanup;

    result = vkEndCommandBuffer (command.command);

    if (result != VK_SUCCESS) goto cleanup;

    result = backend_vulkan_session_command_submit (
      backend_ctx, 0u
    );

    if (result != VK_SUCCESS) goto cleanup;

    if (trial == 2u)
    {
      result = backend_vulkan_session_command_complete (
        backend_ctx, 0u
      );

      if (result != VK_SUCCESS
       || backend_vulkan_session_close_if_ready (
            backend_ctx, 0u
          ) != VK_NOT_READY)
      {
        fprintf (stderr, "BACKEND_SESSION_SHUTDOWN_PRECOMPLETED_WAIT_FAILED result=%d\n", result);

        goto cleanup;
      }

      backend_vulkan_session_status_t completed = { 0 };

      if (backend_vulkan_session_status (
        backend_ctx, 0u, &completed
      ) != VK_SUCCESS
       || completed.compute_pending != VK_FALSE
       || completed.compute_leased != VK_TRUE
       || completed.descriptors_leased != VK_TRUE
       || completed.compute_completions != 1u
       || completed.compute_cancellations != 0u)
      {
        fprintf (stderr, "BACKEND_SESSION_SHUTDOWN_PRECOMPLETED_STATUS_FAILED\n");

        goto cleanup;
      }
    }
  }
  else
  {
    const uint32_t payload = UINT32_C (0x7e2b0c51);

    memcpy (views[0].mapped, &payload, sizeof (payload));

    result = backend_vulkan_session_storage_transfer_begin (
      backend_ctx, 0u, 0u, sizeof (payload), VK_FALSE
    );

    if (result != VK_SUCCESS) goto cleanup;
  }

  const VkResult first_poll =
    backend_vulkan_session_shutdown_poll (
      backend_ctx, 0u, 0u
    );

  if ((first_poll != VK_SUCCESS && first_poll != VK_NOT_READY)
   || (trial == 2u && first_poll != VK_SUCCESS))
  {
    fprintf (stderr, "BACKEND_SESSION_SHUTDOWN_POLL_FAILED result=%d\n", first_poll);

    goto cleanup;
  }

  if (first_poll == VK_NOT_READY)
  {
    backend_vulkan_session_status_t pending = { 0 };

    if (backend_vulkan_session_status (
      backend_ctx, 0u, &pending
    ) != VK_SUCCESS
     || (trial == 0u
       ? (pending.compute_pending != VK_TRUE
       || pending.compute_cancel_requested != VK_TRUE
       || pending.descriptors_leased != VK_TRUE)
       : (pending.transfer_pending != VK_TRUE
       || pending.transfer_cancel_requested != VK_TRUE)))
    {
      fprintf (stderr, "BACKEND_SESSION_SHUTDOWN_PENDING_STATE_FAILED\n");

      goto cleanup;
    }

    result = backend_vulkan_session_shutdown_poll (
      backend_ctx, 0u, UINT64_MAX
    );

    if (result != VK_SUCCESS)
    {
      fprintf (stderr, "BACKEND_SESSION_SHUTDOWN_WAIT_FAILED result=%d\n", result);

      goto cleanup;
    }
  }

  if (backend_ctx->vulkan_session != NULL
   || backend_vulkan_session_shutdown_poll (
        backend_ctx, 0u, 0u
      ) != VK_SUCCESS)
  {
    fprintf (stderr, "BACKEND_SESSION_SHUTDOWN_IDEMPOTENCE_FAILED\n");

    goto cleanup;
  }

  fprintf (
    stdout,
    "BACKEND_SESSION_SHUTDOWN_PASS"
    " trial=%" PRIu32
    " kind=%s"
    " first_poll=%d"
    " cancelled=%u"
    " completed=%u"
    " closed=1 idempotent=1\n",
    trial, trial == 1u ? "transfer" : "compute",
    first_poll, trial != 2u, trial == 2u
  );

  status = 0;

cleanup:
  if (backend_vulkan_session_close (backend_ctx) != VK_SUCCESS)
  {
    status = 1;
  }

  if (backend_vulkan_catalog_close (backend_ctx) != VK_SUCCESS)
  {
    status = 1;
  }

  return status;
}

/**
 * Run five independent Vulkan 1.0/1.1 sessions with GPU-backed checks.
 *
 * Two baseline sessions verify storage, exact descriptor/pipeline caches,
 * zero-timeout fence polls, canceled Compute and staging submissions,
 * driver-verified recovery and resource reuse. Three shutdown sessions
 * verify cancellation of pending Compute/staging work and retirement of
 * already completed managed work before closing each logical device.
 * Returns 0 only after all five device/instance lifetimes succeed,
 * 1 on a GPU or ownership error, and 2 for malformed arguments.
 */
int main (int argc, char **argv)
{
  if (argc != 3 || strcmp (argv[1], "--minimum") != 0
   || (strcmp (argv[2], "1.0") != 0
    && strcmp (argv[2], "1.1") != 0))
  {
    fprintf (stderr, "Usage: %s --minimum 1.0|1.1\n", argv[0]);

    return 2;
  }

  const uint32_t minimum_api =
    strcmp (argv[2], "1.0") == 0
      ? VK_API_VERSION_1_0 : VK_API_VERSION_1_1;

  backend_ctx_t *backend_ctx = (backend_ctx_t *) calloc (
    1u, sizeof (*backend_ctx)
  );

  if (backend_ctx == NULL) return 1;

  int status = 0;

  for (uint32_t trial = 0u; trial < 2u; trial++)
  {
    if (verify_session_trial (
      backend_ctx, minimum_api, trial
    ) != 0)
    {
      status = 1;

      break;
    }
  }

  if (status == 0)
  {
    for (uint32_t trial = 0u; trial < 3u; trial++)
    {
      if (verify_shutdown_trial (
        backend_ctx, minimum_api, trial
      ) != 0)
      {
        status = 1;

        break;
      }
    }
  }

  if (backend_vulkan_session_close (backend_ctx) != VK_SUCCESS)
  {
    status = 1;
  }

  if (backend_vulkan_catalog_close (backend_ctx) != VK_SUCCESS)
  {
    status = 1;
  }

  free (backend_ctx);

  if (status == 0)
  {
    fprintf (
      stdout,
      "BACKEND_SESSION_DIAGNOSTIC_PASS api=%" PRIu32 ".%" PRIu32
      " trials=2 invalid=8 owner_rejects=4"
      " unsupported=2 empty_submissions=2"
      " storage_allocations=10 storage_reuses=6 storage_growth=2"
      " staging_transfers=34 staging_rejected=10"
      " staging_recoveries=2 staging_abandoned=2"
      " staging_cancellations=4"
      " command_submissions=8 command_recoveries=2"
      " managed_submissions=4 managed_completed=2 managed_cancelled=2"
      " shutdown_trials=3 scheduled=0\n",
      VK_API_VERSION_MAJOR (minimum_api),
      VK_API_VERSION_MINOR (minimum_api)
    );
  }

  return status;
}
