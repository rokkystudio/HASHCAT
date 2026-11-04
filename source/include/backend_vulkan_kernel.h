/**
 * Author......: See docs/credits.txt
 * License.....: MIT
 */

#ifndef HC_BACKEND_VULKAN_KERNEL_H
#define HC_BACKEND_VULKAN_KERNEL_H

/**
 * Execute dynamically owned Vulkan SPIR-V 1.0 Compute programs.
 *
 * A real backend_ctx catalogue supplies the selected Vulkan GPU and
 * VkInstance. Each method selects an explicit backend_vulkan_program_t,
 * validates its SPIR-V headers, buffer graph and dispatch topology against
 * real VkPhysicalDeviceLimits, then borrows an active matching session or
 * creates its own temporary VkDevice resources. The exported diagnostics
 * verify one, two or four public arithmetic stages and one cryptographic
 * MD5 stage over fixed public known-answer vectors. A separate explicit
 * CPU-reference diagnostic verifies error handling after real GPU completion.
 * Host ownership arrays are sized from program stage/buffer counts; stages
 * declare exact SPIR-V modules, storage dependencies and dispatch topology.
 * Job-owned Vulkan resources are released before returning, while borrowed
 * pipeline, command, descriptor and storage objects remain session-owned.
 *
 * These methods run only in hashcat -I information mode. The MD5 diagnostic
 * uses immutable public messages; no HASHCAT candidates, target digests,
 * salts, attack modes or hashing scheduler state are read or modified.
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
 * Verify post-fence CPU validation failure without poisoning a healthy GPU.
 *
 * Requires a healthy session-owned Vulkan Compute queue and diagnostic
 * stream. Runs the public four-stage arithmetic program with one deliberately
 * corrupted host-only reference word after the real GPU fence and cache
 * synchronization. That CPU comparison must return the expected validation
 * error while retaining a healthy, reusable command/fence and descriptor
 * cache. Executes the same program again with an unchanged CPU reference
 * and requires exact equality and clean session ownership.
 *
 * Returns VK_SUCCESS only when both GPU submissions finish, the injected
 * comparison fails, the retry succeeds and no session recovery is needed.
 * This is an explicit information-mode test and never accesses password
 * candidates, target digests or production scheduling.
 */
VkResult backend_vulkan_kernel_verify_chain4_post_fence_cpu_error (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  FILE *diagnostics);

/**
 * Verify safe cleanup of a host abort after GPU queue submission.
 *
 * Requires a healthy session-owned Vulkan Compute queue. Submits the public
 * four-stage arithmetic program, returns a simulated CPU failure before
 * fence waiting, and uses the normal cleanup to wait on the true submitted
 * fence, return descriptors and retain the healthy command/fence pair.
 * Executes the same program again and requires real GPU completion, exact
 * output equality and no command recovery. Returns VK_SUCCESS only if the
 * original CPU error and the second healthy completion are observed.
 * This diagnostic does not access production workload or backend selection.
 */
VkResult backend_vulkan_kernel_verify_chain4_post_submit_abort (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  FILE *diagnostics);

/**
 * Verify reuse of a successfully recorded but unsubmitted Compute command.
 *
 * Requires a healthy session-owned Vulkan Compute device. The public
 * four-stage arithmetic program emits a simulated CPU error after command
 * recording and before queue submission. Cleanup returns its descriptor
 * and command leases without marking the unsubmitted GPU work as failed.
 * A second ordinary four-stage GPU execution must submit, complete on its
 * real fence, verify exact CPU output and retain a healthy session.
 * VK_SUCCESS requires no command submission for the injected error,
 * exactly one completed submission for the retry and no device recovery.
 */
VkResult backend_vulkan_kernel_verify_chain4_pre_submit_abort (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  FILE *diagnostics);

/**
 * Verify one real MD5 SPIR-V Compute stage on four public known-answer vectors.
 *
 * Requires an active backend Vulkan catalogue, valid physical-device index
 * and non-NULL diagnostics. The declared program allocates two 256-byte
 * storage buffers and one descriptor/shader/pipeline set. Four Compute
 * invocations consume canonical padded blocks for "", "a", "abc" and
 * "message digest" and write four 128-bit MD5 states.
 *
 * VK_SUCCESS requires every digest word to match its standard known answer,
 * all unwritten destination words to retain their sentinel and all temporary
 * Vulkan resources to be released. The method does not read HASHCAT password
 * candidates, target digests, salts, attack modes or scheduler state.
 */
VkResult backend_vulkan_kernel_verify_md5 (
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

/**
 * Observe a real VK_TIMEOUT on an unsignaled fence after GPU submission.
 *
 * Requires an open backend Vulkan catalogue, a valid device index and
 * non-NULL diagnostics. The full four-stage public program is submitted
 * using VK_NULL_HANDLE as its submission fence; the separately owned fence
 * remains unsignaled. vkWaitForFences with zero timeout returns the actual
 * driver status, which must be VK_TIMEOUT for a successful diagnostic.
 * The standard cleanup waits for actual device idle before freeing all
 * dynamically owned Vulkan resources. No password workload is scheduled.
 *
 * VK_TIMEOUT indicates successful observation and complete cleanup.
 * Any other Vulkan or cleanup error is returned directly.
 */
VkResult backend_vulkan_kernel_verify_chain4_fence_timeout (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  FILE *diagnostics);

#endif // HC_BACKEND_VULKAN_KERNEL_H
