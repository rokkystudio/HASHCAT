/**
 * Intercept only selected Vulkan call sites in an Android test executable.
 *
 * ld.lld --wrap redirects references to vkQueueSubmit/vkWaitForFences
 * in the standalone transfer source to these exact functions. Every
 * unarmed or non-targeted call invokes its real Vulkan loader symbol.
 * An armed injection returns a deliberately test-generated negative
 * VkResult before the selected driver call and latches that error.
 * Subsequent calls to the same targeted queue/device are denied
 * without entering the driver, including zero-command submissions.
 */

#include "vulkan-core10-api-fault-driver.h"

#include <inttypes.h>
#include <string.h>

/**
 * Declare the true Vulkan entry points exposed by the test-only linker.
 *
 * ld.lld supplies __real_vkQueueSubmit and __real_vkWaitForFences from
 * the dynamically loaded Vulkan 1.0 library whenever the test binary
 * is linked with the corresponding --wrap options.
 */
VKAPI_ATTR VkResult VKAPI_CALL __real_vkQueueSubmit (
  VkQueue queue,
  uint32_t submit_count,
  const VkSubmitInfo *submits,
  VkFence fence);

VKAPI_ATTR VkResult VKAPI_CALL __real_vkWaitForFences (
  VkDevice device,
  uint32_t fence_count,
  const VkFence *fences,
  VkBool32 wait_all,
  uint64_t timeout);

/**
 * Own the single-threaded, single-target driver interception state.
 *
 * A test invocation executes one Vulkan session at a time. The state
 * is process-local and does not reference a HASHCAT backend. The
 * caller disarms interception before destroying the targeted VkDevice.
 */
typedef struct vulkan_core10_api_fault_state
{
  VkQueue target_queue;
  VkDevice target_device;
  vulkan_core10_api_fault_site_t site;
  VkResult result;
  vulkan_core10_api_fault_counts_t counts;
  VkBool32 armed;
  FILE *diagnostics;
} vulkan_core10_api_fault_state_t;

static vulkan_core10_api_fault_state_t vulkan_core10_api_fault_state = { 0 };

/**
 * Return an explicit stable diagnostic name for the selected API site.
 *
 * Only queue_submit and fence_wait are accepted when arming a test.
 */
static const char *vulkan_core10_api_fault_site_name (
  vulkan_core10_api_fault_site_t site)
{
  switch (site)
  {
    case VULKAN_CORE10_API_FAULT_SUBMIT: return "queue_submit";
    case VULKAN_CORE10_API_FAULT_WAIT:   return "fence_wait";
  }

  return "invalid";
}

/**
 * Arm the selected true Vulkan 1.0 call boundary for one live device.
 *
 * A test-selectable terminal error is stored, but no Vulkan API call
 * is made. The first matching selected API call receives this value
 * without reaching the driver. All later matching queue or wait
 * attempts are terminal until the caller explicitly disarms.
 */
VkResult vulkan_core10_api_fault_arm (
  VkQueue queue,
  VkDevice device,
  vulkan_core10_api_fault_site_t site,
  VkResult result,
  FILE *diagnostics)
{
  vulkan_core10_api_fault_state_t *state =
    &vulkan_core10_api_fault_state;

  if (state->armed == VK_TRUE
   || queue == VK_NULL_HANDLE
   || device == VK_NULL_HANDLE
   || (site != VULKAN_CORE10_API_FAULT_SUBMIT
    && site != VULKAN_CORE10_API_FAULT_WAIT)
   || (result != VK_ERROR_DEVICE_LOST
    && result != VK_ERROR_OUT_OF_DEVICE_MEMORY)
   || diagnostics == NULL)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  memset (state, 0, sizeof (*state));

  state->target_queue = queue;
  state->target_device = device;
  state->site = site;
  state->result = result;
  state->armed = VK_TRUE;
  state->diagnostics = diagnostics;

  fprintf (
    diagnostics,
    "API_FAULT_ARM site=%s result=%d"
    " origin=test_linker_wrap real_device_loss=0\n",
    vulkan_core10_api_fault_site_name (site), result
  );

  return VK_SUCCESS;
}

/**
 * Intercept each queue submission at the actual linked Vulkan API boundary.
 *
 * Unarmed and non-targeted queues pass straight to the real driver.
 * At SUBMIT injection, the caller receives the negative test result
 * without a real queue submission. At WAIT injection, the first
 * queue submission is real and its later fence wait is intercepted.
 * Once an error is latched, further submissions are rejected before
 * calling the loader and counted separately as blocked attempts.
 */
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkQueueSubmit (
  VkQueue queue,
  uint32_t submit_count,
  const VkSubmitInfo *submits,
  VkFence fence)
{
  vulkan_core10_api_fault_state_t *state =
    &vulkan_core10_api_fault_state;

  if (state->armed != VK_TRUE || queue != state->target_queue)
  {
    return __real_vkQueueSubmit (
      queue, submit_count, submits, fence
    );
  }

  state->counts.submit_attempts++;

  if (state->counts.injections != 0u)
  {
    state->counts.blocked++;

    fprintf (
      state->diagnostics,
      "API_FAULT_BLOCK site=queue_submit"
      " result=%d real_driver_called=0"
      " count=%" PRIu32 "\n",
      state->counts.first_error, state->counts.blocked
    );

    return state->counts.first_error;
  }

  if (state->site == VULKAN_CORE10_API_FAULT_SUBMIT)
  {
    state->counts.injections = 1u;
    state->counts.first_error = state->result;

    fprintf (
      state->diagnostics,
      "API_FAULT_INJECT site=queue_submit"
      " result=%d real_driver_called=0"
      " real_submissions=0\n",
      state->result
    );

    return state->result;
  }

  state->counts.real_submissions++;

  const VkResult result = __real_vkQueueSubmit (
    queue, submit_count, submits, fence
  );

  fprintf (
    state->diagnostics,
    "API_FAULT_REAL_SUBMIT result=%d"
    " real_submissions=%" PRIu32 "\n",
    result, state->counts.real_submissions
  );

  return result;
}

/**
 * Intercept fence waits at the actual linked Vulkan 1.0 API boundary.
 *
 * Unarmed and non-targeted VkDevices pass through unchanged. At WAIT
 * injection, the real queue submission has already occurred, but the
 * wait is deliberately not called. The caller must close the test
 * session with vkDeviceWaitIdle, which waits for its real pending work
 * before freeing memory. After an injected failure, additional waits
 * on the selected device are also denied without driver interaction.
 */
VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkWaitForFences (
  VkDevice device,
  uint32_t fence_count,
  const VkFence *fences,
  VkBool32 wait_all,
  uint64_t timeout)
{
  vulkan_core10_api_fault_state_t *state =
    &vulkan_core10_api_fault_state;

  if (state->armed != VK_TRUE || device != state->target_device)
  {
    return __real_vkWaitForFences (
      device, fence_count, fences, wait_all, timeout
    );
  }

  state->counts.wait_attempts++;

  if (state->counts.injections != 0u)
  {
    state->counts.blocked++;

    fprintf (
      state->diagnostics,
      "API_FAULT_BLOCK site=fence_wait"
      " result=%d real_driver_called=0"
      " count=%" PRIu32 "\n",
      state->counts.first_error, state->counts.blocked
    );

    return state->counts.first_error;
  }

  if (state->site == VULKAN_CORE10_API_FAULT_WAIT)
  {
    state->counts.injections = 1u;
    state->counts.first_error = state->result;

    fprintf (
      state->diagnostics,
      "API_FAULT_INJECT site=fence_wait"
      " result=%d real_driver_called=0"
      " prior_real_submission=%" PRIu32 "\n",
      state->result, state->counts.real_submissions
    );

    return state->result;
  }

  state->counts.real_waits++;

  const VkResult result = __real_vkWaitForFences (
    device, fence_count, fences, wait_all, timeout
  );

  fprintf (
    state->diagnostics,
    "API_FAULT_REAL_WAIT result=%d"
    " real_waits=%" PRIu32 "\n",
    result, state->counts.real_waits
  );

  return result;
}

/**
 * Return the exact intercepted operation counts for an armed test.
 *
 * All counters reflect function calls at the linked Vulkan boundary.
 * A reported injection is explicitly test-generated and is never
 * classified as an error returned by the real Vulkan loader.
 */
VkResult vulkan_core10_api_fault_snapshot (
  vulkan_core10_api_fault_counts_t *counts)
{
  if (counts == NULL
   || vulkan_core10_api_fault_state.armed != VK_TRUE)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  *counts = vulkan_core10_api_fault_state.counts;

  return VK_SUCCESS;
}

/**
 * Finish the test-only interception before destroying its VkDevice.
 *
 * Reports exact attempted, real, injected and blocked Vulkan calls.
 * Disarms and clears all target handles without touching the GPU.
 * vkDeviceWaitIdle and VkDevice cleanup are performed by the owner.
 */
VkResult vulkan_core10_api_fault_disarm (void)
{
  vulkan_core10_api_fault_state_t *state =
    &vulkan_core10_api_fault_state;

  if (state->armed != VK_TRUE)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  fprintf (
    state->diagnostics,
    "API_FAULT_DISARM site=%s result=%d"
    " submit_attempts=%" PRIu32
    " real_submissions=%" PRIu32
    " wait_attempts=%" PRIu32
    " real_waits=%" PRIu32
    " injected=%" PRIu32
    " blocked=%" PRIu32
    " real_device_loss=0\n",
    vulkan_core10_api_fault_site_name (state->site),
    state->result,
    state->counts.submit_attempts,
    state->counts.real_submissions,
    state->counts.wait_attempts,
    state->counts.real_waits,
    state->counts.injections,
    state->counts.blocked
  );

  memset (state, 0, sizeof (*state));

  return VK_SUCCESS;
}
