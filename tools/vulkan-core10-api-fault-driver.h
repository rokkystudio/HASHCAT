#ifndef VULKAN_CORE10_API_FAULT_DRIVER_H
#define VULKAN_CORE10_API_FAULT_DRIVER_H

/**
 * Inject a test-only VkResult at the genuine Vulkan 1.0 call boundary.
 *
 * Only dedicated Android diagnostic executables link the vkQueueSubmit
 * and vkWaitForFences symbols through GNU ld.lld --wrap. When no test
 * is armed, calls pass directly to the real Vulkan loader. An armed
 * VkQueue/VkDevice receives one explicitly selected negative result
 * without invoking that operation in the driver. Other Vulkan handles
 * remain unaffected.
 *
 * This test facility never deliberately resets, hangs or loses a GPU.
 * No HASHCAT backend, shader or password-processing code uses it.
 */

#include <vulkan/vulkan.h>

#include <stdint.h>
#include <stdio.h>

/**
 * Select the exact Vulkan function whose result is test-controlled.
 *
 * SUBMIT interrupts vkQueueSubmit before submitting its command.
 * WAIT interrupts vkWaitForFences after vkQueueSubmit has succeeded,
 * leaving the real queued work for vkDeviceWaitIdle during teardown.
 */
typedef enum vulkan_core10_api_fault_site
{
  VULKAN_CORE10_API_FAULT_SUBMIT = 1,
  VULKAN_CORE10_API_FAULT_WAIT = 2
} vulkan_core10_api_fault_site_t;

/**
 * Report the specific API dispatches made while one fault is armed.
 *
 * real_submissions counts calls to __real_vkQueueSubmit, whereas
 * submit_attempts counts matching intercepted submissions. A WAIT
 * injection has one real submission and no real fence wait; a SUBMIT
 * injection invokes neither real operation. blocked counts matching
 * post-failure calls denied before entering the real driver.
 */
typedef struct vulkan_core10_api_fault_counts
{
  uint32_t submit_attempts;
  uint32_t real_submissions;
  uint32_t wait_attempts;
  uint32_t real_waits;
  uint32_t injections;
  uint32_t blocked;
  VkResult first_error;
} vulkan_core10_api_fault_counts_t;

/**
 * Arm one explicit negative VkResult for one live Vulkan queue/device.
 *
 * Requires a non-null existing queue and device, declared SUBMIT/WAIT
 * site, and exactly VK_ERROR_DEVICE_LOST or
 * VK_ERROR_OUT_OF_DEVICE_MEMORY. A second arm while active is rejected.
 * The function stores the target handles and performs no Vulkan calls.
 * This diagnostic supports one Vulkan logical device at a time.
 */
VkResult vulkan_core10_api_fault_arm (
  VkQueue queue,
  VkDevice device,
  vulkan_core10_api_fault_site_t site,
  VkResult result,
  FILE *diagnostics);

/**
 * Read the exact counts of intercepted real and injected API calls.
 *
 * Requires an armed test and non-NULL output. This method does not
 * modify Vulkan resources or issue GPU work.
 */
VkResult vulkan_core10_api_fault_snapshot (
  vulkan_core10_api_fault_counts_t *counts);

/**
 * Disable interception and release the target queue/device references.
 *
 * Reports the complete per-test API call counts, then passes all new
 * Vulkan calls through to the genuine loader. The owning VkDevice
 * must still be alive when this function is called. No Vulkan object
 * is destroyed or recreated by this method.
 */
VkResult vulkan_core10_api_fault_disarm (void);

#endif
