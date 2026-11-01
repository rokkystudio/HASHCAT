/**
 * Author......: See docs/credits.txt
 * License.....: MIT
 */

#ifndef HC_BACKEND_VULKAN_KERNEL_H
#define HC_BACKEND_VULKAN_KERNEL_H

/**
 * Execute one or two opt-in public-data Vulkan SPIR-V 1.0 Compute stages.
 *
 * A real backend_ctx catalogue supplies the selected Vulkan GPU and
 * VkInstance. Each method selects an explicit backend_vulkan_program_t,
 * validates its SPIR-V headers, buffer graph and dispatch topology against
 * real VkPhysicalDeviceLimits, then creates temporary VkDevice resources.
 * The first method verifies 64 unsigned integers; the two-stage method
 * verifies both 64-word intermediate and 64-word final outputs after
 * an explicit shader-write-to-shader-read barrier. Both methods release
 * all temporary Vulkan resources before returning.
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

#endif // HC_BACKEND_VULKAN_KERNEL_H
