/**
 * Manage synchronous, caller-owned public GPU jobs on a Vulkan 1.0 graph.
 *
 * Uploads, uniform parameters and per-stage outputs cross a host-facing
 * task boundary, with explicit Vulkan cache synchronization and reuse of
 * one command buffer. The port does not own any GPU resource or kernel.
 */

#include "vulkan-core10-job-port.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/**
 * Record the caller's exact graph and copy its immutable readback indices.
 *
 * Failure leaves an empty port. The caller's Vulkan owner remains valid
 * and is responsible for its own resource cleanup.
 */
VkResult vulkan_core10_job_port_open (
  vulkan_core10_job_port_t *port,
  vulkan_core10_multiset_t *runtime,
  const vulkan_core10_multiset_buffer_spec_t *buffer_specs,
  uint32_t buffer_count,
  const vulkan_core10_multiset_graph_spec_t *graph,
  FILE *diagnostics)
{
  if (port == NULL || runtime == NULL || buffer_specs == NULL
   || graph == NULL || diagnostics == NULL
   || port->ready == VK_TRUE || port->runtime != NULL
   || port->readback_indices != NULL
   || graph->stage_count == 0u
   || graph->transfer_bytes > (VkDeviceSize) SIZE_MAX
   || graph->uniform_buffer_index >= buffer_count)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

#if SIZE_MAX <= UINT32_MAX
  if (graph->stage_count > SIZE_MAX / sizeof (uint32_t))
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
#endif

  const VkDeviceSize uniform_bytes =
    buffer_specs[graph->uniform_buffer_index].size;

  if (uniform_bytes == 0u
   || uniform_bytes > (VkDeviceSize) SIZE_MAX)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  uint32_t *indices = (uint32_t *) calloc (
    graph->stage_count, sizeof (*indices)
  );

  if (indices == NULL)
  {
    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }

  VkResult result = vulkan_core10_multiset_graph_record (
    runtime, buffer_specs, buffer_count, graph, diagnostics
  );

  if (result != VK_SUCCESS)
  {
    free (indices);

    return result;
  }

  memcpy (
    indices, graph->readback_buffer_indices,
    graph->stage_count * sizeof (*indices)
  );

  port->runtime = runtime;
  port->readback_indices = indices;
  port->stage_count = graph->stage_count;
  port->upload_index = graph->upload_buffer_index;
  port->uniform_index = graph->uniform_buffer_index;
  port->input_bytes = graph->transfer_bytes;
  port->uniform_bytes = uniform_bytes;
  port->jobs_completed = 0u;
  port->ready = VK_TRUE;
  port->failed = VK_FALSE;
  port->diagnostics = diagnostics;

  fprintf (
    diagnostics,
    "JOB_PORT_READY sets=%" PRIu32 " stages=%" PRIu32
    " upload=%" PRIu32 " uniform=%" PRIu32
    " input_bytes=%" PRIu64 " uniform_bytes=%" PRIu64 "\n",
    runtime->set_count, port->stage_count,
    port->upload_index, port->uniform_index,
    (uint64_t) port->input_bytes, (uint64_t) port->uniform_bytes
  );

  return VK_SUCCESS;
}

/**
 * Validate one host task, run it on the already recorded Vulkan command
 * buffer and copy all fully synchronized stage outputs back to the caller.
 */
VkResult vulkan_core10_job_port_submit (
  vulkan_core10_job_port_t *port,
  const void *input,
  VkDeviceSize input_bytes,
  const void *uniform,
  VkDeviceSize uniform_bytes,
  void *const *outputs,
  uint32_t output_count,
  VkDeviceSize output_bytes)
{
  if (port == NULL || port->ready != VK_TRUE
   || port->failed == VK_TRUE || port->runtime == NULL
   || port->runtime->device == VK_NULL_HANDLE
   || port->runtime->recorded != VK_TRUE
   || port->runtime->pending == VK_TRUE
   || port->readback_indices == NULL
   || input == NULL || uniform == NULL || outputs == NULL
   || input_bytes != port->input_bytes
   || uniform_bytes != port->uniform_bytes
   || output_count != port->stage_count
   || output_bytes != port->input_bytes
   || port->jobs_completed == UINT32_MAX)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  for (uint32_t stage = 0u; stage < output_count; stage++)
  {
    if (outputs[stage] == NULL)
    {
      return VK_ERROR_INITIALIZATION_FAILED;
    }

    for (uint32_t prior = 0u; prior < stage; prior++)
    {
      if (outputs[stage] == outputs[prior])
      {
        return VK_ERROR_INITIALIZATION_FAILED;
      }
    }
  }

  vulkan_core10_multiset_t *runtime = port->runtime;
  const uint32_t upload = port->upload_index;
  const uint32_t uniforms = port->uniform_index;

  if (runtime->buffers[upload].mapped == NULL
   || runtime->buffers[uniforms].mapped == NULL)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  for (uint32_t stage = 0u; stage < output_count; stage++)
  {
    if (runtime->buffers[port->readback_indices[stage]].mapped == NULL)
    {
      return VK_ERROR_INITIALIZATION_FAILED;
    }
  }

  memcpy (
    runtime->buffers[upload].mapped, input, (size_t) input_bytes
  );

  memcpy (
    runtime->buffers[uniforms].mapped, uniform, (size_t) uniform_bytes
  );

  VkResult result = vulkan_core10_multiset_sync (
    runtime, upload, VK_FALSE
  );

  if (result != VK_SUCCESS) goto failed;

  result = vulkan_core10_multiset_sync (
    runtime, uniforms, VK_FALSE
  );

  if (result != VK_SUCCESS) goto failed;

  result = vulkan_core10_multiset_submit_and_wait (runtime);

  if (result != VK_SUCCESS) goto failed;

  for (uint32_t stage = 0u; stage < output_count; stage++)
  {
    result = vulkan_core10_multiset_sync (
      runtime, port->readback_indices[stage], VK_TRUE
    );

    if (result != VK_SUCCESS) goto failed;
  }

  for (uint32_t stage = 0u; stage < output_count; stage++)
  {
    memcpy (
      outputs[stage],
      runtime->buffers[port->readback_indices[stage]].mapped,
      (size_t) output_bytes
    );
  }

  port->jobs_completed++;

  fprintf (
    port->diagnostics,
    "JOB_PORT_SUBMIT completed=%" PRIu32
    " stages=%" PRIu32 " input_bytes=%" PRIu64
    " uniform_bytes=%" PRIu64 " output_bytes=%" PRIu64
    " reused=%u\n",
    port->jobs_completed, port->stage_count,
    (uint64_t) input_bytes, (uint64_t) uniform_bytes,
    (uint64_t) output_bytes,
    (port->jobs_completed > 1u) ? 1u : 0u
  );

  return VK_SUCCESS;

failed:
  port->failed = VK_TRUE;

  fprintf (
    port->diagnostics,
    "JOB_PORT_EXECUTION_FAILED result=%d completed=%" PRIu32 "\n",
    result, port->jobs_completed
  );

  return result;
}

/**
 * Discard the port's host-owned readback metadata and zero its state.
 *
 * Every underlying Vulkan object continues to belong to the shared
 * runtime, which the caller destroys after closing this port.
 */
VkResult vulkan_core10_job_port_close (
  vulkan_core10_job_port_t *port)
{
  if (port == NULL) return VK_ERROR_INITIALIZATION_FAILED;

  if (port->ready == VK_TRUE && port->diagnostics != NULL)
  {
    fprintf (
      port->diagnostics,
      "JOB_PORT_CLOSE completed=%" PRIu32 " stages=%" PRIu32
      " failed=%u\n",
      port->jobs_completed, port->stage_count, port->failed
    );
  }

  free (port->readback_indices);

  memset (port, 0, sizeof (*port));

  return VK_SUCCESS;
}
