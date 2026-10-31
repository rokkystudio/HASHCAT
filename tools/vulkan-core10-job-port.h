#ifndef VULKAN_CORE10_JOB_PORT_H
#define VULKAN_CORE10_JOB_PORT_H

/**
 * Expose synchronous, noncryptographic GPU jobs through a Vulkan 1.0 port.
 *
 * The port accepts caller-owned input bytes, uniform bytes and one output
 * destination for each declared Compute stage. It records the existing
 * multi-set graph once and submits the same command buffer for each job.
 * Vulkan buffers, pipelines, command objects and the device remain owned
 * by vulkan_core10_multiset_t.
 *
 * This is a GPU task boundary for public integer Compute. It does not
 * discover hashcat workloads, implement any hash algorithm or integrate
 * with the production hashcat dispatch scheduler.
 */

#include "vulkan-core10-multiset-graph.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/**
 * Track a fully recorded sequential Vulkan 1.0 Compute job port.
 *
 * The port owns only the copy of per-stage readback indices. It borrows
 * the Vulkan runtime and leaves its objects unchanged during closure.
 * The caller maintains the runtime until job_port_close completes.
 *
 * failed becomes VK_TRUE after a Vulkan synchronization or submission
 * error, preventing later submissions using an uncertain GPU state.
 */
typedef struct vulkan_core10_job_port
{
  vulkan_core10_multiset_t *runtime;
  uint32_t *readback_indices;
  uint32_t stage_count;
  uint32_t upload_index;
  uint32_t uniform_index;
  VkDeviceSize input_bytes;
  VkDeviceSize uniform_bytes;
  uint32_t jobs_completed;
  VkBool32 ready;
  VkBool32 failed;
  FILE *diagnostics;
} vulkan_core10_job_port_t;

/**
 * Validate the graph, record one reusable command buffer and open the port.
 *
 * Requires an empty port, a successfully created multiset runtime, exact
 * matching immutable buffer specifications, a declared stage dependency
 * graph and a diagnostic stream. The graph recorder validates all indices,
 * declared buffer usages, dispatch limits and stage dependencies.
 *
 * On success, the port owns a copy of all readback indices and borrows
 * the runtime. The caller closes the port before destroying the runtime.
 * On failure, the port remains zero initialized and its host allocation
 * is released. VkResult errors from the graph recorder are propagated.
 */
VkResult vulkan_core10_job_port_open (
  vulkan_core10_job_port_t *port,
  vulkan_core10_multiset_t *runtime,
  const vulkan_core10_multiset_buffer_spec_t *buffer_specs,
  uint32_t buffer_count,
  const vulkan_core10_multiset_graph_spec_t *graph,
  FILE *diagnostics);

/**
 * Execute one synchronous GPU job using caller-owned input/output arrays.
 *
 * Requires exact declared input, uniform, output sizes and one non-null,
 * distinct output pointer per stage. All metadata is validated before
 * writing a mapped GPU allocation. CPU data is copied into the existing
 * mapped upload/uniform buffers and flushed if noncoherent.
 *
 * Submits the pre-recorded command buffer, waits for fence completion,
 * invalidates every mapped readback when noncoherent, then copies each
 * complete stage output to the caller. Updates jobs_completed only when
 * every GPU operation and readback succeeds. A Vulkan execution error
 * marks the port failed and blocks subsequent submits until closure.
 */
VkResult vulkan_core10_job_port_submit (
  vulkan_core10_job_port_t *port,
  const void *input,
  VkDeviceSize input_bytes,
  const void *uniform,
  VkDeviceSize uniform_bytes,
  void *const *outputs,
  uint32_t output_count,
  VkDeviceSize output_bytes);

/**
 * Release the port's copied indices and reset all CPU-side job state.
 *
 * The Vulkan runtime, descriptor sets and GPU resources stay owned by
 * the original caller. Closing an empty port succeeds, while a NULL port
 * returns VK_ERROR_INITIALIZATION_FAILED.
 */
VkResult vulkan_core10_job_port_close (
  vulkan_core10_job_port_t *port);

#endif
