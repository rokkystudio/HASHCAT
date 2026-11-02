#ifndef VULKAN_BACKEND_PUBLIC_API_FAULT_DRIVER_H
#define VULKAN_BACKEND_PUBLIC_API_FAULT_DRIVER_H

/**
 * Inject one test-only Vulkan API error into the production public executor.
 *
 * Dedicated Android diagnostics link selected Vulkan symbols through
 * ld.lld --wrap. Unarmed calls and calls for non-selected sites reach the
 * real Vulkan loader unchanged. The selected call returns one explicit
 * synthetic VkResult at a declared attempt number, allowing the unchanged
 * backend_vulkan_kernel implementation to exercise its normal error and
 * cleanup path without inducing a physical GPU failure.
 *
 * The production HASHCAT binary does not link this driver.
 */

#include <vulkan/vulkan.h>

#include <stdint.h>
#include <stdio.h>

/**
 * Identify the exact Vulkan call whose result is test-controlled.
 *
 * Resource-creation sites interrupt the fifth buffer or fourth stage object
 * after earlier resources have been acquired successfully. Submission sites
 * exercise the fully created four-stage command path.
 */
typedef enum backend_vulkan_public_api_fault_site
{
  BACKEND_VULKAN_PUBLIC_API_FAULT_CREATE_BUFFER = 1,
  BACKEND_VULKAN_PUBLIC_API_FAULT_CREATE_SHADER_MODULE = 2,
  BACKEND_VULKAN_PUBLIC_API_FAULT_CREATE_COMPUTE_PIPELINE = 3,
  BACKEND_VULKAN_PUBLIC_API_FAULT_QUEUE_SUBMIT = 4,
  BACKEND_VULKAN_PUBLIC_API_FAULT_FENCE_WAIT = 5
} backend_vulkan_public_api_fault_site_t;

/**
 * Record calls observed while one API fault is armed.
 *
 * attempts counts calls to the selected site. real_calls counts selected-site
 * calls forwarded to the Vulkan loader before the injected call. Queue and
 * fence counters track real submission/wait operations independently of the
 * selected site. device_wait_idle_calls and wait_idle_result describe the
 * production cleanup barrier executed while the synthetic failure is armed.
 */
typedef struct backend_vulkan_public_api_fault_counts
{
  uint32_t attempts;
  uint32_t real_calls;
  uint32_t real_submissions;
  uint32_t real_waits;
  uint32_t device_wait_idle_calls;
  uint32_t injections;
  uint32_t blocked;
  VkResult first_error;
  VkResult wait_idle_result;
} backend_vulkan_public_api_fault_counts_t;

/**
 * Arm one explicit negative Vulkan result at one selected call attempt.
 *
 * Requires a declared site, a one-based fail_on_attempt, non-NULL diagnostics
 * and the site-specific result used by this diagnostic: resource creation
 * returns VK_ERROR_OUT_OF_DEVICE_MEMORY while queue submission and fence wait
 * return VK_ERROR_DEVICE_LOST. No Vulkan function is called by this method.
 */
VkResult backend_vulkan_public_api_fault_arm (
  backend_vulkan_public_api_fault_site_t site,
  uint32_t fail_on_attempt,
  VkResult result,
  FILE *diagnostics);

/**
 * Copy the current API-fault counters without issuing a Vulkan operation.
 *
 * Requires an armed diagnostic and non-NULL output. The returned values
 * describe only the current production executor attempt and its cleanup.
 */
VkResult backend_vulkan_public_api_fault_snapshot (
  backend_vulkan_public_api_fault_counts_t *counts);

/**
 * Disable the test-only interception after the production executor returns.
 *
 * Reports the complete call counters, clears all process-local test state and
 * makes subsequent wrapped Vulkan calls pass directly to the real loader.
 * Vulkan objects are owned and released by the production executor itself.
 */
VkResult backend_vulkan_public_api_fault_disarm (void);

#endif
