/**
 * Author......: See docs/credits.txt
 * License.....: MIT
 */

#ifndef HC_BACKEND_VULKAN_KERNEL_H
#define HC_BACKEND_VULKAN_KERNEL_H

/**
 * Execute dynamically owned public-data Vulkan SPIR-V 1.0 Compute programs.
 *
 * A real backend_ctx catalogue supplies the selected Vulkan GPU and
 * VkInstance. Each method selects an explicit backend_vulkan_program_t,
 * validates its SPIR-V headers, buffer graph and dispatch topology against
 * real VkPhysicalDeviceLimits, then creates temporary VkDevice resources.
 * The exported diagnostics verify one, two, or four ordered public stages.
 * Host ownership arrays are sized from program stage/buffer counts, while
 * each stage declares its exact SPIR-V module, storage dependency and
 * dispatch topology. All temporary Vulkan resources are released before
 * returning.
 *
 * These methods run only in hashcat -I information mode. No HASHCAT
 * candidates, salts, digests, cryptographic kernels or hashing scheduler
 * are read or modified.
 */

#include "types.h"

#include <vulkan/vulkan.h>

#include <stdint.h>
#include <stdio.h>

/**
 * Verify one real Vulkan 1.0 public Compute shader and its output.
 *
 * Requires an active backend-owned Vulkan catalogue, exact physical
 * device index and non-NULL diagnostics. The one-stage program declaration
 * supplies the immutable SPIR-V module, buffer indices, local size and
 * dispatch groups. The declaration is validated against the physical GPU
 * before VkDevice creation. The executor then builds its pipeline/set,
 * executes the declared dispatch, waits on a real fence and verifies output.
 *
 * Returns VK_SUCCESS only for exact GPU/CPU output equality and
 * complete resource cleanup. Other VkResults propagate immediately;
 * a mismatch returns VK_ERROR_INITIALIZATION_FAILED. The ordinary
 * OpenCL/CUDA/HIP/Metal device list and scheduling remain untouched.
 */
VkResult backend_vulkan_kernel_verify (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  FILE *diagnostics);

/**
 * Verify one ordered pair of public SPIR-V 1.0 Compute shader stages.
 *
 * Requires a live backend Vulkan catalogue, valid physical-device index
 * and diagnostic stream. The two-stage backend_vulkan_program_t explicitly
 * declares both SPIR-V modules, buffer 0->1->2 dependencies and dispatch
 * topology. After validating that graph against real device limits, the
 * executor creates three buffers, two sets/pipelines and records the required
 * COMPUTE write-to-read dependency before the second declared dispatch.
 * A real fence precedes independent CPU comparison of all 128 stage values.
 *
 * VK_SUCCESS requires complete GPU/CPU equality and successful cleanup.
 * Any Vulkan error or mismatch propagates without scheduling HASHCAT
 * hashing work or adding Vulkan to the ordinary backend device list.
 */
VkResult backend_vulkan_kernel_verify_chain (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  FILE *diagnostics);

/**
 * Verify four ordered public Compute stages with dynamically sized ownership.
 *
 * Requires the same live backend Vulkan catalogue and diagnostic stream.
 * Allocates five storage-buffer records plus four descriptor sets, shader
 * modules and pipelines from the declared program counts. Stage zero runs
 * public_u32_transform; stages one through three reuse public_u32_rotate
 * through explicit 0->1->2->3->4 storage dependencies. Three Compute
 * write-to-read barriers order the stages, and a real fence precedes CPU
 * verification of all 256 stage-output words.
 *
 * VK_SUCCESS requires exact GPU/CPU equality and complete dynamic cleanup.
 * HASHCAT candidates, target hashes and the ordinary hashing scheduler are
 * never read or modified.
 */
VkResult backend_vulkan_kernel_verify_chain4 (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  FILE *diagnostics);

/**
 * Validate four-stage Vulkan resource cleanup at one explicit fault point.
 *
 * Requires a live backend catalogue, a valid physical-device index,
 * non-NULL diagnostics and controlled_fault_point in the range 1..37.
 * The method creates actual Vulkan objects, then returns a simulated
 * VK_ERROR_INITIALIZATION_FAILED at the selected checkpoint and runs
 * the standard resource cleanup path. It logs exact owned resources,
 * device-idle status and host-array cleanup completion.
 *
 * Checkpoints 1..35 occur after resource acquisition. Checkpoint 36
 * aborts before any queue submission. Checkpoint 37 aborts after a
 * successful real vkQueueSubmit and relies on vkDeviceWaitIdle during
 * cleanup. No Vulkan driver return is replaced or fabricated, and no
 * actual device-loss event is induced.
 *
 * The method is dedicated to synthetic public-integer diagnostics.
 * It never reads or schedules HASHCAT password/hash workloads.
 */
VkResult backend_vulkan_kernel_verify_chain4_fault (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  FILE *diagnostics,
  uint32_t controlled_fault_point);

#endif // HC_BACKEND_VULKAN_KERNEL_H
