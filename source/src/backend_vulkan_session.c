/**
 * Author......: See docs/credits.txt
 * License.....: MIT
 */

/**
 * Own a single Vulkan 1.0 Compute queue session in hashcat's backend_ctx.
 *
 * The owning catalogue supplies a real physical device and Compute queue
 * family. The session owns one VkDevice, VkCommandPool, VkCommandBuffer and
 * VkFence. One empty command buffer is submitted to validate queue/fence
 * operation. No descriptors, shaders, hashing data or kernel dispatch
 * are created by this component.
 */

#include "backend_vulkan.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>

/**
 * Hold the Vulkan logical-device resources of one backend-owned session.
 *
 * Physical-device handles and VkInstance belong to the separate catalogue.
 * The diagnostics stream belongs to the caller and remains borrowed.
 * empty_submissions counts successful, fence-completed empty queue submits.
 */
struct backend_vulkan_compute_session
{
  VkDevice device;
  VkQueue queue;
  VkCommandPool command_pool;
  VkCommandBuffer command_buffer;
  VkFence fence;
  uint32_t device_index;
  uint32_t queue_family;
  uint32_t effective_api;
  uint32_t empty_submissions;
  FILE *diagnostics;
};

/**
 * Wait for idle and release all already acquired VkDevice resources.
 *
 * The caller retains the VkInstance while this function executes.
 * The command buffer is freed before its pool. A successfully created
 * VkDevice is always destroyed, even when vkDeviceWaitIdle reports an
 * error. The first wait error is returned after completing cleanup.
 */
static VkResult backend_vulkan_session_release (
  struct backend_vulkan_compute_session *session)
{
  if (session->device == VK_NULL_HANDLE) return VK_SUCCESS;

  const VkResult result = vkDeviceWaitIdle (session->device);

  if (session->fence != VK_NULL_HANDLE)
  {
    vkDestroyFence (session->device, session->fence, NULL);

    session->fence = VK_NULL_HANDLE;
  }

  if (session->command_buffer != VK_NULL_HANDLE)
  {
    vkFreeCommandBuffers (
      session->device, session->command_pool, 1u,
      &session->command_buffer
    );

    session->command_buffer = VK_NULL_HANDLE;
  }

  if (session->command_pool != VK_NULL_HANDLE)
  {
    vkDestroyCommandPool (
      session->device, session->command_pool, NULL
    );

    session->command_pool = VK_NULL_HANDLE;
  }

  vkDestroyDevice (session->device, NULL);

  session->device = VK_NULL_HANDLE;
  session->queue = VK_NULL_HANDLE;

  return result;
}

/**
 * Create and validate a standalone Vulkan Compute queue session.
 *
 * Uses the caller's exact physical-device index and public workload
 * declaration. Creates no shader/descriptor resources and submits only
 * a zero-command primary buffer. On every failure, releases all
 * partially acquired Vulkan ownership before returning its VkResult.
 */
VkResult backend_vulkan_session_open (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  const vulkan_auto_compute_requirements_t *workload,
  FILE *diagnostics)
{
  if (backend_ctx == NULL || diagnostics == NULL
   || backend_ctx->vulkan_catalog == NULL
   || backend_ctx->vulkan_session != NULL
   || workload == NULL
   || device_index >= backend_ctx->vulkan_devices_cnt)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  vulkan_auto_device_t selected = { 0 };
  uint32_t effective_api = 0u;

  VkResult result = backend_vulkan_catalog_select (
    backend_ctx, device_index, workload, &selected, &effective_api
  );

  if (result != VK_SUCCESS) return result;

  struct backend_vulkan_compute_session *session =
    (struct backend_vulkan_compute_session *) calloc (
      1u, sizeof (*session)
    );

  if (session == NULL) return VK_ERROR_OUT_OF_HOST_MEMORY;

  session->device_index = device_index;
  session->queue_family = selected.compute_queue_family;
  session->effective_api = effective_api;
  session->diagnostics = diagnostics;

  const float priority = 1.0f;

  const VkDeviceQueueCreateInfo queue_info =
  {
    .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
    .queueFamilyIndex = selected.compute_queue_family,
    .queueCount = 1u,
    .pQueuePriorities = &priority
  };

  const VkDeviceCreateInfo device_info =
  {
    .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
    .queueCreateInfoCount = 1u,
    .pQueueCreateInfos = &queue_info,
    .enabledLayerCount = 0u,
    .enabledExtensionCount = 0u,
    .pEnabledFeatures = NULL
  };

  result = vkCreateDevice (
    selected.physical, &device_info, NULL, &session->device
  );

  if (result != VK_SUCCESS) goto cleanup;

  vkGetDeviceQueue (
    session->device, selected.compute_queue_family, 0u,
    &session->queue
  );

  if (session->queue == VK_NULL_HANDLE)
  {
    result = VK_ERROR_INITIALIZATION_FAILED;

    goto cleanup;
  }

  const VkCommandPoolCreateInfo pool_info =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
    .queueFamilyIndex = selected.compute_queue_family,
    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT
  };

  result = vkCreateCommandPool (
    session->device, &pool_info, NULL, &session->command_pool
  );

  if (result != VK_SUCCESS) goto cleanup;

  const VkCommandBufferAllocateInfo command_info =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
    .commandPool = session->command_pool,
    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
    .commandBufferCount = 1u
  };

  result = vkAllocateCommandBuffers (
    session->device, &command_info, &session->command_buffer
  );

  if (result != VK_SUCCESS) goto cleanup;

  const VkFenceCreateInfo fence_info =
  {
    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
  };

  result = vkCreateFence (
    session->device, &fence_info, NULL, &session->fence
  );

  if (result != VK_SUCCESS) goto cleanup;

  const VkCommandBufferBeginInfo begin_info =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
  };

  result = vkBeginCommandBuffer (
    session->command_buffer, &begin_info
  );

  if (result != VK_SUCCESS) goto cleanup;

  result = vkEndCommandBuffer (session->command_buffer);

  if (result != VK_SUCCESS) goto cleanup;

  const VkSubmitInfo submit_info =
  {
    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    .commandBufferCount = 1u,
    .pCommandBuffers = &session->command_buffer
  };

  result = vkQueueSubmit (
    session->queue, 1u, &submit_info, session->fence
  );

  if (result != VK_SUCCESS) goto cleanup;

  result = vkWaitForFences (
    session->device, 1u, &session->fence,
    VK_TRUE, UINT64_MAX
  );

  if (result != VK_SUCCESS) goto cleanup;

  session->empty_submissions = 1u;
  backend_ctx->vulkan_session = session;

  fprintf (
    diagnostics,
    "BACKEND_VULKAN_SESSION_READY index=%" PRIu32
    " queue_family=%" PRIu32
    " effective_api=%" PRIu32 ".%" PRIu32
    " command_buffers=1 empty_submissions=1"
    " features=0 extensions=0 scheduled=0\n",
    device_index,
    session->queue_family,
    VK_API_VERSION_MAJOR (effective_api),
    VK_API_VERSION_MINOR (effective_api)
  );

  return VK_SUCCESS;

cleanup:
  fprintf (
    diagnostics,
    "BACKEND_VULKAN_SESSION_OPEN_FAILED index=%" PRIu32
    " result=%d\n",
    device_index, result
  );

  backend_vulkan_session_release (session);

  free (session);

  return result;
}

/**
 * Close one backend-owned Vulkan Compute queue session and clear its owner.
 *
 * Destroys the logical device and its subordinate resources while the
 * backend-owned physical-device catalogue is still active. Emits a
 * complete ownership-close event for an active session; closing an
 * empty session is successful and does not emit a duplicate event.
 */
VkResult backend_vulkan_session_close (
  backend_ctx_t *backend_ctx)
{
  if (backend_ctx == NULL) return VK_ERROR_INITIALIZATION_FAILED;

  struct backend_vulkan_compute_session *session =
    backend_ctx->vulkan_session;

  if (session == NULL) return VK_SUCCESS;

  const uint32_t device_index = session->device_index;
  const uint32_t queue_family = session->queue_family;
  const uint32_t submissions = session->empty_submissions;
  FILE *diagnostics = session->diagnostics;

  const VkResult result = backend_vulkan_session_release (session);

  backend_ctx->vulkan_session = NULL;

  free (session);

  fprintf (
    diagnostics,
    "BACKEND_VULKAN_SESSION_CLOSE index=%" PRIu32
    " queue_family=%" PRIu32
    " empty_submissions=%" PRIu32
    " wait_result=%d\n",
    device_index, queue_family, submissions, result
  );

  return result;
}
