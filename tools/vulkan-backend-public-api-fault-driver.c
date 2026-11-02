/**
 * Intercept selected Vulkan calls only in the public-executor diagnostic.
 *
 * ld.lld --wrap redirects the linked production executor's references to the
 * functions below. Every unarmed or non-selected call reaches libvulkan.so.
 * The selected one-based attempt returns the declared synthetic VkResult
 * without calling the driver at that site. vkDeviceWaitIdle is never failed:
 * it is observed and forwarded so cleanup of a submitted command remains real.
 */

#include "vulkan-backend-public-api-fault-driver.h"

#include <inttypes.h>
#include <string.h>

VKAPI_ATTR VkResult VKAPI_CALL __real_vkCreateBuffer (
  VkDevice device,
  const VkBufferCreateInfo *create_info,
  const VkAllocationCallbacks *allocator,
  VkBuffer *buffer);

VKAPI_ATTR VkResult VKAPI_CALL __real_vkCreateShaderModule (
  VkDevice device,
  const VkShaderModuleCreateInfo *create_info,
  const VkAllocationCallbacks *allocator,
  VkShaderModule *shader_module);

VKAPI_ATTR VkResult VKAPI_CALL __real_vkCreateComputePipelines (
  VkDevice device,
  VkPipelineCache pipeline_cache,
  uint32_t create_info_count,
  const VkComputePipelineCreateInfo *create_infos,
  const VkAllocationCallbacks *allocator,
  VkPipeline *pipelines);

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

VKAPI_ATTR VkResult VKAPI_CALL __real_vkDeviceWaitIdle (
  VkDevice device);

/**
 * Own one single-threaded test interception state.
 *
 * The dedicated diagnostic executes one production four-stage job at a time.
 * Disarm occurs after that job has completed its ordinary cleanup and before
 * the next healthy recovery job starts.
 */
typedef struct backend_vulkan_public_api_fault_state
{
  backend_vulkan_public_api_fault_site_t site;
  uint32_t fail_on_attempt;
  VkResult result;
  backend_vulkan_public_api_fault_counts_t counts;
  VkBool32 armed;
  FILE *diagnostics;
} backend_vulkan_public_api_fault_state_t;

static backend_vulkan_public_api_fault_state_t
  backend_vulkan_public_api_fault_state = { 0 };

/**
 * Return the stable diagnostic name for a supported wrapped Vulkan call.
 */
static const char *backend_vulkan_public_api_fault_site_name (
  backend_vulkan_public_api_fault_site_t site)
{
  switch (site)
  {
    case BACKEND_VULKAN_PUBLIC_API_FAULT_CREATE_BUFFER:
      return "create_buffer";
    case BACKEND_VULKAN_PUBLIC_API_FAULT_CREATE_SHADER_MODULE:
      return "create_shader_module";
    case BACKEND_VULKAN_PUBLIC_API_FAULT_CREATE_COMPUTE_PIPELINE:
      return "create_compute_pipeline";
    case BACKEND_VULKAN_PUBLIC_API_FAULT_QUEUE_SUBMIT:
      return "queue_submit";
    case BACKEND_VULKAN_PUBLIC_API_FAULT_FENCE_WAIT:
      return "fence_wait";
  }

  return "invalid";
}

/**
 * Decide whether one selected wrapped call returns a synthetic result.
 *
 * Calls before fail_on_attempt are counted as real selected-site calls and
 * continue to the Vulkan loader. The selected attempt latches the configured
 * error. Any unexpected later selected-site call remains fail-closed and
 * returns the same error without entering the driver.
 */
static VkBool32 backend_vulkan_public_api_fault_intercept (
  backend_vulkan_public_api_fault_site_t site,
  VkResult *result)
{
  backend_vulkan_public_api_fault_state_t *state =
    &backend_vulkan_public_api_fault_state;

  if (state->armed != VK_TRUE || state->site != site)
  {
    return VK_FALSE;
  }

  state->counts.attempts++;

  if (state->counts.injections != 0u)
  {
    state->counts.blocked++;

    *result = state->counts.first_error;

    fprintf (
      state->diagnostics,
      "BACKEND_API_FAULT_BLOCK"
      " site=%s"
      " attempt=%" PRIu32
      " result=%d"
      " real_driver_called=0\n",
      backend_vulkan_public_api_fault_site_name (site),
      state->counts.attempts,
      *result
    );

    return VK_TRUE;
  }

  if (state->counts.attempts == state->fail_on_attempt)
  {
    state->counts.injections = 1u;
    state->counts.first_error = state->result;

    *result = state->result;

    fprintf (
      state->diagnostics,
      "BACKEND_API_FAULT_INJECT"
      " site=%s"
      " attempt=%" PRIu32
      " result=%d"
      " real_driver_called=0"
      " real_driver_failure=0\n",
      backend_vulkan_public_api_fault_site_name (site),
      state->counts.attempts,
      *result
    );

    return VK_TRUE;
  }

  state->counts.real_calls++;

  return VK_FALSE;
}

/**
 * Arm one site-specific synthetic error without issuing Vulkan work.
 */
VkResult backend_vulkan_public_api_fault_arm (
  backend_vulkan_public_api_fault_site_t site,
  uint32_t fail_on_attempt,
  VkResult result,
  FILE *diagnostics)
{
  backend_vulkan_public_api_fault_state_t *state =
    &backend_vulkan_public_api_fault_state;

  const VkBool32 resource_site =
    site == BACKEND_VULKAN_PUBLIC_API_FAULT_CREATE_BUFFER
    || site == BACKEND_VULKAN_PUBLIC_API_FAULT_CREATE_SHADER_MODULE
    || site == BACKEND_VULKAN_PUBLIC_API_FAULT_CREATE_COMPUTE_PIPELINE;

  const VkBool32 submission_site =
    site == BACKEND_VULKAN_PUBLIC_API_FAULT_QUEUE_SUBMIT
    || site == BACKEND_VULKAN_PUBLIC_API_FAULT_FENCE_WAIT;

  if (state->armed == VK_TRUE
   || (resource_site == VK_FALSE && submission_site == VK_FALSE)
   || fail_on_attempt == 0u
   || diagnostics == NULL
   || (resource_site == VK_TRUE
    && result != VK_ERROR_OUT_OF_DEVICE_MEMORY)
   || (submission_site == VK_TRUE
    && result != VK_ERROR_DEVICE_LOST))
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  memset (state, 0, sizeof (*state));

  state->site = site;
  state->fail_on_attempt = fail_on_attempt;
  state->result = result;
  state->armed = VK_TRUE;
  state->diagnostics = diagnostics;

  fprintf (
    diagnostics,
    "BACKEND_API_FAULT_ARM"
    " site=%s"
    " fail_on_attempt=%" PRIu32
    " result=%d"
    " origin=test_linker_wrap"
    " real_driver_failure=0\n",
    backend_vulkan_public_api_fault_site_name (site),
    fail_on_attempt,
    result
  );

  return VK_SUCCESS;
}

/**
 * Return a snapshot of all wrapped-call counters for the active job.
 */
VkResult backend_vulkan_public_api_fault_snapshot (
  backend_vulkan_public_api_fault_counts_t *counts)
{
  if (counts == NULL
   || backend_vulkan_public_api_fault_state.armed != VK_TRUE)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  *counts = backend_vulkan_public_api_fault_state.counts;

  return VK_SUCCESS;
}

/**
 * Report and clear the process-local interception state.
 */
VkResult backend_vulkan_public_api_fault_disarm (void)
{
  backend_vulkan_public_api_fault_state_t *state =
    &backend_vulkan_public_api_fault_state;

  if (state->armed != VK_TRUE)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  fprintf (
    state->diagnostics,
    "BACKEND_API_FAULT_DISARM"
    " site=%s"
    " attempts=%" PRIu32
    " real_calls=%" PRIu32
    " real_submissions=%" PRIu32
    " real_waits=%" PRIu32
    " wait_idle_calls=%" PRIu32
    " wait_idle_result=%d"
    " injected=%" PRIu32
    " blocked=%" PRIu32
    " first_error=%d"
    " real_driver_failure=0\n",
    backend_vulkan_public_api_fault_site_name (state->site),
    state->counts.attempts,
    state->counts.real_calls,
    state->counts.real_submissions,
    state->counts.real_waits,
    state->counts.device_wait_idle_calls,
    state->counts.wait_idle_result,
    state->counts.injections,
    state->counts.blocked,
    state->counts.first_error
  );

  memset (state, 0, sizeof (*state));

  return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateBuffer (
  VkDevice device,
  const VkBufferCreateInfo *create_info,
  const VkAllocationCallbacks *allocator,
  VkBuffer *buffer)
{
  VkResult injected = VK_SUCCESS;

  if (backend_vulkan_public_api_fault_intercept (
      BACKEND_VULKAN_PUBLIC_API_FAULT_CREATE_BUFFER,
      &injected) == VK_TRUE)
  {
    if (buffer != NULL) *buffer = VK_NULL_HANDLE;

    return injected;
  }

  return __real_vkCreateBuffer (
    device, create_info, allocator, buffer
  );
}

VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateShaderModule (
  VkDevice device,
  const VkShaderModuleCreateInfo *create_info,
  const VkAllocationCallbacks *allocator,
  VkShaderModule *shader_module)
{
  VkResult injected = VK_SUCCESS;

  if (backend_vulkan_public_api_fault_intercept (
      BACKEND_VULKAN_PUBLIC_API_FAULT_CREATE_SHADER_MODULE,
      &injected) == VK_TRUE)
  {
    if (shader_module != NULL) *shader_module = VK_NULL_HANDLE;

    return injected;
  }

  return __real_vkCreateShaderModule (
    device, create_info, allocator, shader_module
  );
}

VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkCreateComputePipelines (
  VkDevice device,
  VkPipelineCache pipeline_cache,
  uint32_t create_info_count,
  const VkComputePipelineCreateInfo *create_infos,
  const VkAllocationCallbacks *allocator,
  VkPipeline *pipelines)
{
  VkResult injected = VK_SUCCESS;

  if (backend_vulkan_public_api_fault_intercept (
      BACKEND_VULKAN_PUBLIC_API_FAULT_CREATE_COMPUTE_PIPELINE,
      &injected) == VK_TRUE)
  {
    if (pipelines != NULL)
    {
      for (uint32_t i = 0u; i < create_info_count; i++)
      {
        pipelines[i] = VK_NULL_HANDLE;
      }
    }

    return injected;
  }

  return __real_vkCreateComputePipelines (
    device, pipeline_cache, create_info_count,
    create_infos, allocator, pipelines
  );
}

VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkQueueSubmit (
  VkQueue queue,
  uint32_t submit_count,
  const VkSubmitInfo *submits,
  VkFence fence)
{
  backend_vulkan_public_api_fault_state_t *state =
    &backend_vulkan_public_api_fault_state;

  VkResult injected = VK_SUCCESS;

  if (backend_vulkan_public_api_fault_intercept (
      BACKEND_VULKAN_PUBLIC_API_FAULT_QUEUE_SUBMIT,
      &injected) == VK_TRUE)
  {
    return injected;
  }

  if (state->armed == VK_TRUE)
  {
    state->counts.real_submissions++;
  }

  return __real_vkQueueSubmit (
    queue, submit_count, submits, fence
  );
}

VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkWaitForFences (
  VkDevice device,
  uint32_t fence_count,
  const VkFence *fences,
  VkBool32 wait_all,
  uint64_t timeout)
{
  backend_vulkan_public_api_fault_state_t *state =
    &backend_vulkan_public_api_fault_state;

  VkResult injected = VK_SUCCESS;

  if (backend_vulkan_public_api_fault_intercept (
      BACKEND_VULKAN_PUBLIC_API_FAULT_FENCE_WAIT,
      &injected) == VK_TRUE)
  {
    return injected;
  }

  if (state->armed == VK_TRUE)
  {
    state->counts.real_waits++;
  }

  return __real_vkWaitForFences (
    device, fence_count, fences, wait_all, timeout
  );
}

VKAPI_ATTR VkResult VKAPI_CALL __wrap_vkDeviceWaitIdle (
  VkDevice device)
{
  backend_vulkan_public_api_fault_state_t *state =
    &backend_vulkan_public_api_fault_state;

  if (state->armed != VK_TRUE)
  {
    return __real_vkDeviceWaitIdle (device);
  }

  state->counts.device_wait_idle_calls++;

  const VkResult result = __real_vkDeviceWaitIdle (device);

  state->counts.wait_idle_result = result;

  fprintf (
    state->diagnostics,
    "BACKEND_API_FAULT_WAIT_IDLE"
    " call=%" PRIu32
    " result=%d"
    " real_driver_called=1\n",
    state->counts.device_wait_idle_calls,
    result
  );

  return result;
}
