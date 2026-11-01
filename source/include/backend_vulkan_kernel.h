/**
 * Author......: See docs/credits.txt
 * License.....: MIT
 */

#ifndef HC_BACKEND_VULKAN_KERNEL_H
#define HC_BACKEND_VULKAN_KERNEL_H

/**
 * Execute an opt-in public-data SPIR-V 1.0 kernel in the HASHCAT backend.
 *
 * The production-owned backend_ctx supplies an already enumerated Vulkan
 * physical device and VkInstance. The kernel test opens an independent
 * logical Compute device, allocates two host-visible storage buffers,
 * builds one descriptor set and Compute pipeline from embedded SPIR-V,
 * dispatches two 32-lane workgroups, waits for a real fence, and verifies
 * 64 unsigned integer outputs against exact CPU reference values.
 *
 * The temporary logical device and all pipeline/memory objects are
 * destroyed before returning; the backend_ctx catalogue remains live.
 * This method is intended only for hashcat -I information mode. No
 * hashcat password candidate, cryptographic kernel or hashing scheduler
 * is read, dispatched or modified.
 */

#include "types.h"

#include <vulkan/vulkan.h>

#include <stdint.h>
#include <stdio.h>

/**
 * Verify one real Vulkan 1.0 public Compute shader and its output.
 *
 * Requires an active backend-owned Vulkan catalogue, exact physical
 * device index and non-NULL diagnostics. Rejects devices lacking the
 * declared 32x2 workgroup limits or two 256-byte storage buffers.
 * Creates the Vulkan Compute pipeline, binds both storage descriptors,
 * executes vkCmdDispatch(2,1,1), makes shader writes host-visible,
 * waits for the actual queue fence and compares every output word.
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

#endif // HC_BACKEND_VULKAN_KERNEL_H
