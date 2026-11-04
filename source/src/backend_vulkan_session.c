/**
 * Author......: See docs/credits.txt
 * License.....: MIT
 */

/**
 * Own one Vulkan 1.0 Compute queue session in the parent backend context.
 *
 * The device catalogue supplies one physical device and Compute queue
 * family. The session retains a logical device, command pool, dedicated
 * transfer command/fence pair, reusable Compute command/fence pair, immutable
 * shader pipelines, descriptor sets and direct/staged storage slots.
 * A fence-completed empty queue submission verifies the session before
 * use. An incomplete Compute submission poisons the command pair until
 * explicit recovery waits for device idle and replaces that pair.
 */

#include "backend_vulkan.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/**
 * Hold the Vulkan logical-device resources of one backend-owned session.
 *
 * Physical-device handles and VkInstance belong to the separate catalogue.
 * The diagnostics stream belongs to the caller and remains borrowed.
 * storage_buffers retain direct or staged CPU-accessible allocations between
 * programs; allocation and reuse counters describe the session lifetime.
 * cached_pipelines retain exact immutable SPIR-V module identities,
 * compiled Compute pipelines and one shared descriptor/pipeline layout.
 * descriptor_pools retain stage-count-specific descriptor sets; an exclusive
 * descriptor lease protects buffer rebinding until GPU work completes.
 * compute_command and compute_fence are reserved for sequential Compute jobs
 * and are independent of the staging-transfer command buffer and fence.
 * Managed Compute submissions retain their command lease until a real fence
 * wait succeeds. Cancellation discards their result but never revokes GPU
 * queue work or permits premature descriptor and storage reuse.
 * An unsuccessful job poisons this pair until explicit driver-verified
 * recovery replaces both objects after a successful device-idle wait.
 * completed_transfers counts fence-completed host/GPU copies.
 * transfer_pending retains one queue submission until explicit completion.
 * transfer_cancel_requested discards the pending result only after its real
 * fence signals; it does not abort work already submitted to Vulkan.
 * A driver failure poisons this pair until idle-verified recovery replaces
 * its command and fence without touching the independent Compute pair.
 * empty_submissions counts successful, fence-completed empty queue submits.
 */
typedef struct backend_vulkan_session_storage_buffer
{
  VkBuffer buffer;
  VkDeviceMemory memory;
  VkBuffer host_buffer;
  VkDeviceMemory host_memory;
  void *mapped;
  VkDeviceSize capacity_bytes;
  VkBool32 staging;
  vulkan_auto_memory_plan_t plan;
} backend_vulkan_session_storage_buffer_t;

/**
 * Own one immutable shader identity and its compiled Compute pipeline.
 *
 * A pipeline is keyed by the full SPIR-V word sequence, never by a shader
 * label or hash approximation. Every entry remains session-owned and can
 * be borrowed by repeated sequential public Compute programs.
 */
typedef struct backend_vulkan_session_pipeline
{
  uint32_t *spirv;
  uint32_t spirv_words;
  VkPipeline pipeline;
} backend_vulkan_session_pipeline_t;

/**
 * Retain the descriptor pool and stage sets for one exact program stage count.
 *
 * All sets share the session's two-storage-buffer descriptor layout.
 * Their bindings are rewritten on each exclusive acquisition to reflect
 * the current physical storage buffers and requested descriptor ranges.
 */
typedef struct backend_vulkan_session_descriptor_pool
{
  VkDescriptorPool pool;
  VkDescriptorSet *sets;
  uint32_t stage_count;
} backend_vulkan_session_descriptor_pool_t;

struct backend_vulkan_compute_session
{
  VkDevice device;
  VkQueue queue;
  VkCommandPool command_pool;
  VkCommandBuffer command_buffer;
  VkFence fence;
  VkCommandBuffer compute_command;
  VkFence compute_fence;
  VkBool32 compute_command_leased;
  VkBool32 compute_command_failed;
  VkBool32 compute_submission_pending;
  VkBool32 compute_submission_consumed;
  VkBool32 compute_cancel_requested;
  uint32_t compute_managed_submissions;
  uint32_t compute_managed_completed;
  uint32_t compute_managed_cancelled;
  uint32_t compute_command_acquisitions;
  uint32_t compute_command_reuses;
  uint32_t compute_command_recoveries;
  VkDescriptorSetLayout cached_descriptor_layout;
  VkPipelineLayout cached_pipeline_layout;
  backend_vulkan_session_descriptor_pool_t *descriptor_pools;
  uint32_t descriptor_pool_count;
  uint32_t descriptor_pool_reuses;
  uint32_t descriptor_acquisitions;
  uint32_t descriptor_writes;
  VkBool32 descriptor_leased;
  backend_vulkan_session_pipeline_t *cached_pipelines;
  uint32_t cached_pipeline_count;
  uint32_t cached_pipeline_hits;
  backend_vulkan_session_storage_buffer_t *storage_buffers;
  uint32_t storage_buffer_count;
  uint32_t storage_allocations;
  uint32_t storage_reuses;
  uint32_t device_index;
  uint32_t queue_family;
  uint32_t effective_api;
  uint32_t empty_submissions;
  uint32_t completed_transfers;
  uint32_t cancelled_transfers;
  uint32_t transfer_recoveries;
  uint32_t transfer_abandoned;
  uint32_t pending_buffer_index;
  VkDeviceSize pending_bytes;
  VkBool32 pending_download;
  VkBool32 transfer_pending;
  VkBool32 transfer_cancel_requested;
  VkBool32 transfer_failed;
  FILE *diagnostics;
};

/**
 * Release one session-owned direct or staged storage allocation.
 *
 * The parent logical device remains live. mapped always belongs to
 * host_memory. Direct mode aliases host handles to the Compute handles;
 * staging mode destroys the separate host transfer allocation first.
 * A zero-initialized slot is valid and all metadata is cleared on return.
 */
static void backend_vulkan_session_storage_buffer_release (
  VkDevice device,
  backend_vulkan_session_storage_buffer_t *buffer)
{
  if (buffer == NULL || device == VK_NULL_HANDLE) return;

  if (buffer->mapped != NULL && buffer->host_memory != VK_NULL_HANDLE)
  {
    vkUnmapMemory (device, buffer->host_memory);
  }

  if (buffer->host_buffer != VK_NULL_HANDLE
   && buffer->host_buffer != buffer->buffer)
  {
    vkDestroyBuffer (device, buffer->host_buffer, NULL);
  }

  if (buffer->host_memory != VK_NULL_HANDLE
   && buffer->host_memory != buffer->memory)
  {
    vkFreeMemory (device, buffer->host_memory, NULL);
  }

  if (buffer->buffer != VK_NULL_HANDLE)
  {
    vkDestroyBuffer (device, buffer->buffer, NULL);
  }

  if (buffer->memory != VK_NULL_HANDLE)
  {
    vkFreeMemory (device, buffer->memory, NULL);
  }

  memset (buffer, 0, sizeof (*buffer));
}

/**
 * Wait for idle and release all already acquired VkDevice resources.
 *
 * The caller retains the VkInstance while this function executes.
 * Cached descriptor pools/sets are released after GPU idle, followed by
 * retained mappings, buffers and memory. Cached Compute pipelines/layouts,
 * the reusable Compute command/fence pair and the separate transfer pair
 * are released before their shared command pool and VkDevice.
 * A successfully created VkDevice is destroyed even when vkDeviceWaitIdle
 * reports an error; the first wait error is returned after cleanup.
 */
static VkResult backend_vulkan_session_release (
  struct backend_vulkan_compute_session *session)
{
  if (session->device == VK_NULL_HANDLE) return VK_SUCCESS;

  const VkResult result = vkDeviceWaitIdle (session->device);

  for (uint32_t i = 0u; i < session->descriptor_pool_count; i++)
  {
    backend_vulkan_session_descriptor_pool_t *pool =
      &session->descriptor_pools[i];

    if (pool->pool != VK_NULL_HANDLE)
    {
      vkDestroyDescriptorPool (
        session->device, pool->pool, NULL
      );
    }

    free (pool->sets);
  }

  free (session->descriptor_pools);

  session->descriptor_pools = NULL;
  session->descriptor_pool_count = 0u;
  session->descriptor_leased = VK_FALSE;

  for (uint32_t i = 0u; i < session->storage_buffer_count; i++)
  {
    backend_vulkan_session_storage_buffer_release (
      session->device, &session->storage_buffers[i]
    );
  }

  free (session->storage_buffers);

  session->storage_buffers = NULL;
  session->storage_buffer_count = 0u;

  for (uint32_t i = 0u; i < session->cached_pipeline_count; i++)
  {
    backend_vulkan_session_pipeline_t *entry =
      &session->cached_pipelines[i];

    if (entry->pipeline != VK_NULL_HANDLE)
    {
      vkDestroyPipeline (session->device, entry->pipeline, NULL);
    }

    free (entry->spirv);
  }

  free (session->cached_pipelines);

  session->cached_pipelines = NULL;
  session->cached_pipeline_count = 0u;

  if (session->cached_pipeline_layout != VK_NULL_HANDLE)
  {
    vkDestroyPipelineLayout (
      session->device, session->cached_pipeline_layout, NULL
    );

    session->cached_pipeline_layout = VK_NULL_HANDLE;
  }

  if (session->cached_descriptor_layout != VK_NULL_HANDLE)
  {
    vkDestroyDescriptorSetLayout (
      session->device, session->cached_descriptor_layout, NULL
    );

    session->cached_descriptor_layout = VK_NULL_HANDLE;
  }

  if (session->compute_fence != VK_NULL_HANDLE)
  {
    vkDestroyFence (session->device, session->compute_fence, NULL);

    session->compute_fence = VK_NULL_HANDLE;
  }

  if (session->compute_command != VK_NULL_HANDLE)
  {
    vkFreeCommandBuffers (
      session->device, session->command_pool, 1u,
      &session->compute_command
    );

    session->compute_command = VK_NULL_HANDLE;
  }

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
 * Borrow the active session's logical device and Compute queue.
 *
 * The caller supplies the exact physical-device index and receives handles
 * only after the active session has completed its initial fence submission.
 * Session ownership and destruction order remain unchanged, and a failed
 * lookup leaves the caller's output untouched.
 */
VkResult backend_vulkan_session_device (
  const backend_ctx_t *backend_ctx,
  uint32_t device_index,
  backend_vulkan_device_view_t *view)
{
  if (backend_ctx == NULL || view == NULL
   || backend_ctx->vulkan_session == NULL)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  const struct backend_vulkan_compute_session *session =
    backend_ctx->vulkan_session;

  if (session->device_index != device_index
   || session->device == VK_NULL_HANDLE
   || session->queue == VK_NULL_HANDLE
   || session->empty_submissions == 0u)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  *view = (backend_vulkan_device_view_t)
  {
    .device = session->device,
    .queue = session->queue,
    .device_index = session->device_index,
    .queue_family = session->queue_family,
    .effective_api = session->effective_api
  };

  return VK_SUCCESS;
}

/**
 * Snapshot exact host-recorded ownership and completion of a Vulkan session.
 *
 * Validates the active exact device index without changing any resources.
 * All fields are copied from the session's host-side state; no Vulkan fence
 * is polled, so a pending flag stays true until the corresponding completion
 * API observes real GPU completion. The caller serializes session access.
 * Invalid inputs leave the caller's output untouched.
 */
VkResult backend_vulkan_session_status (
  const backend_ctx_t *backend_ctx,
  uint32_t device_index,
  backend_vulkan_session_status_t *status)
{
  if (backend_ctx == NULL || backend_ctx->vulkan_session == NULL
   || status == NULL)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  const struct backend_vulkan_compute_session *session =
    backend_ctx->vulkan_session;

  if (session->device == VK_NULL_HANDLE
   || session->queue == VK_NULL_HANDLE
   || session->device_index != device_index
   || session->empty_submissions == 0u)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  *status = (backend_vulkan_session_status_t)
  {
    .compute_leased = session->compute_command_leased,
    .compute_pending = session->compute_submission_pending,
    .compute_cancel_requested = session->compute_cancel_requested,
    .compute_failed = session->compute_command_failed,
    .descriptors_leased = session->descriptor_leased,
    .transfer_pending = session->transfer_pending,
    .transfer_cancel_requested = session->transfer_cancel_requested,
    .transfer_failed = session->transfer_failed,
    .compute_submissions = session->compute_managed_submissions,
    .compute_completions = session->compute_managed_completed,
    .compute_cancellations = session->compute_managed_cancelled,
    .transfer_completions = session->completed_transfers,
    .transfer_cancellations = session->cancelled_transfers
  };

  return VK_SUCCESS;
}

/**
 * Select compatible Compute-buffer memory from actual device properties.
 *
 * DEVICE_LOCAL memory is preferred when permitted by the buffer's exact
 * memoryTypeBits. If no compatible local type exists, the first compatible
 * type is selected. No device identity or driver-version rule participates.
 */
static VkResult backend_vulkan_session_select_compute_memory (
  const VkPhysicalDeviceMemoryProperties *properties,
  uint32_t compatible_bits,
  uint32_t *index)
{
  if (properties == NULL || index == NULL
   || properties->memoryTypeCount == 0u
   || properties->memoryTypeCount > 32u)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  for (uint32_t pass = 0u; pass < 2u; pass++)
  {
    for (uint32_t candidate = 0u;
         candidate < properties->memoryTypeCount;
         candidate++)
    {
      const uint32_t bit = UINT32_C (1) << candidate;

      if ((compatible_bits & bit) == 0u) continue;

      const VkMemoryPropertyFlags flags =
        properties->memoryTypes[candidate].propertyFlags;

      if (pass == 0u
       && (flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == 0u)
      {
        continue;
      }

      *index = candidate;

      return VK_SUCCESS;
    }
  }

  return VK_ERROR_FEATURE_NOT_PRESENT;
}

/**
 * Reserve reusable direct or staging storage buffers on the active device.
 *
 * Direct mode uses a HOST_VISIBLE memory type compatible with the Compute
 * buffer. If no such type exists, or force_staging is true, the Compute
 * buffer uses compatible device memory and a separate TRANSFER_SRC/DST
 * buffer owns the CPU-visible mapping. Existing capacity is retained unless
 * it is too small or explicit staging requires replacement of a direct slot.
 *
 * A replacement becomes visible only after all Vulkan objects, memory
 * bindings and host mappings succeed. Concurrent descriptor/Compute command
 * leases and incomplete/poisoned Compute submissions block storage
 * replacement so cached descriptors and in-flight GPU work never reference
 * a buffer destroyed by a new allocation. Allocation failures preserve the
 * previous complete slot.
 */
VkResult backend_vulkan_session_storage_reserve (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  const vulkan_auto_device_t *selected,
  uint32_t buffer_count,
  const VkDeviceSize *buffer_bytes,
  VkBool32 force_staging,
  backend_vulkan_storage_view_t *views,
  FILE *diagnostics)
{
  if (backend_ctx == NULL || selected == NULL || buffer_bytes == NULL
   || views == NULL || diagnostics == NULL || buffer_count == 0u
   || backend_ctx->vulkan_session == NULL
   || (force_staging != VK_FALSE && force_staging != VK_TRUE))
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  size_t allocation_bytes = 0u;

  if (__builtin_mul_overflow (
        (size_t) buffer_count,
        sizeof (backend_vulkan_session_storage_buffer_t),
        &allocation_bytes
      ))
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  struct backend_vulkan_compute_session *session =
    backend_ctx->vulkan_session;

  if (session->device == VK_NULL_HANDLE
   || session->device_index != device_index
   || selected->index != device_index
   || selected->compute_queue_family != session->queue_family
   || session->descriptor_leased == VK_TRUE
   || session->compute_command_leased == VK_TRUE
   || session->compute_command_failed == VK_TRUE
   || session->compute_submission_pending == VK_TRUE
   || session->transfer_pending == VK_TRUE
   || session->transfer_failed == VK_TRUE)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  for (uint32_t i = 0u; i < buffer_count; i++)
  {
    if (buffer_bytes[i] == 0u
     || buffer_bytes[i] > selected->properties.limits.maxStorageBufferRange)
    {
      return VK_ERROR_INITIALIZATION_FAILED;
    }
  }

  if (session->storage_buffer_count < buffer_count)
  {
    const uint32_t previous_count = session->storage_buffer_count;

    backend_vulkan_session_storage_buffer_t *expanded =
      (backend_vulkan_session_storage_buffer_t *) realloc (
        session->storage_buffers,
        allocation_bytes
      );

    if (expanded == NULL) return VK_ERROR_OUT_OF_HOST_MEMORY;

    session->storage_buffers = expanded;

    memset (
      &session->storage_buffers[previous_count], 0,
      (size_t) (buffer_count - previous_count)
        * sizeof (*session->storage_buffers)
    );

    session->storage_buffer_count = buffer_count;
  }

  for (uint32_t i = 0u; i < buffer_count; i++)
  {
    backend_vulkan_session_storage_buffer_t *buffer =
      &session->storage_buffers[i];

    if (buffer->buffer != VK_NULL_HANDLE
     && buffer->memory != VK_NULL_HANDLE
     && buffer->host_buffer != VK_NULL_HANDLE
     && buffer->host_memory != VK_NULL_HANDLE
     && buffer->mapped != NULL
     && buffer->capacity_bytes >= buffer_bytes[i]
     && (force_staging == VK_FALSE || buffer->staging == VK_TRUE))
    {
      session->storage_reuses++;

      views[i] = (backend_vulkan_storage_view_t)
      {
        .buffer = buffer->buffer,
        .memory = buffer->memory,
        .host_buffer = buffer->host_buffer,
        .host_memory = buffer->host_memory,
        .mapped = buffer->mapped,
        .bytes = buffer_bytes[i],
        .capacity_bytes = buffer->capacity_bytes,
        .staging = buffer->staging,
        .plan = buffer->plan
      };

      fprintf (
        diagnostics,
        "BACKEND_VULKAN_STORAGE_REUSE"
        " index=%" PRIu32 " slot=%" PRIu32
        " bytes=%" PRIu64 " capacity=%" PRIu64
        " staging=%u coherent=%u reuses=%" PRIu32 "\n",
        device_index, i,
        (uint64_t) buffer_bytes[i],
        (uint64_t) buffer->capacity_bytes,
        buffer->staging, buffer->plan.coherent,
        session->storage_reuses
      );

      continue;
    }

    backend_vulkan_session_storage_buffer_t replacement = { 0 };

    const VkBufferCreateInfo compute_info =
    {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = buffer_bytes[i],
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
             | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
             | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };

    VkResult result = vkCreateBuffer (
      session->device, &compute_info, NULL, &replacement.buffer
    );

    if (result != VK_SUCCESS) return result;

    VkMemoryRequirements compute_requirements = { 0 };

    vkGetBufferMemoryRequirements (
      session->device, replacement.buffer, &compute_requirements
    );

    uint32_t direct_type = 0u;
    VkBool32 direct_coherent = VK_FALSE;

    VkResult direct_result = VK_ERROR_FEATURE_NOT_PRESENT;

    if (force_staging == VK_FALSE)
    {
      direct_result = vulkan_auto_select_memory_type (
        &selected->memory_properties,
        compute_requirements.memoryTypeBits,
        &direct_type,
        &direct_coherent
      );

      if (direct_result != VK_SUCCESS
       && direct_result != VK_ERROR_FEATURE_NOT_PRESENT)
      {
        backend_vulkan_session_storage_buffer_release (
          session->device, &replacement
        );

        return direct_result;
      }
    }

    const VkBool32 use_staging =
      force_staging == VK_TRUE || direct_result != VK_SUCCESS;

    uint32_t compute_type = direct_type;

    if (use_staging == VK_TRUE)
    {
      result = backend_vulkan_session_select_compute_memory (
        &selected->memory_properties,
        compute_requirements.memoryTypeBits,
        &compute_type
      );

      if (result != VK_SUCCESS)
      {
        backend_vulkan_session_storage_buffer_release (
          session->device, &replacement
        );

        return result;
      }
    }

    const VkMemoryAllocateInfo compute_allocation =
    {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = compute_requirements.size,
      .memoryTypeIndex = compute_type
    };

    result = vkAllocateMemory (
      session->device, &compute_allocation, NULL, &replacement.memory
    );

    if (result != VK_SUCCESS)
    {
      backend_vulkan_session_storage_buffer_release (
        session->device, &replacement
      );

      return result;
    }

    result = vkBindBufferMemory (
      session->device, replacement.buffer, replacement.memory, 0u
    );

    if (result != VK_SUCCESS)
    {
      backend_vulkan_session_storage_buffer_release (
        session->device, &replacement
      );

      return result;
    }

    uint32_t host_type = compute_type;
    VkBool32 host_coherent = direct_coherent;
    VkMemoryRequirements host_requirements = compute_requirements;

    if (use_staging == VK_FALSE)
    {
      replacement.host_buffer = replacement.buffer;
      replacement.host_memory = replacement.memory;

      result = vkMapMemory (
        session->device, replacement.host_memory,
        0u, VK_WHOLE_SIZE, 0u, &replacement.mapped
      );

      if (result != VK_SUCCESS)
      {
        backend_vulkan_session_storage_buffer_release (
          session->device, &replacement
        );

        return result;
      }
    }
    else
    {
      const VkBufferCreateInfo host_info =
      {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = buffer_bytes[i],
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT
               | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
      };

      result = vkCreateBuffer (
        session->device, &host_info, NULL, &replacement.host_buffer
      );

      if (result != VK_SUCCESS)
      {
        backend_vulkan_session_storage_buffer_release (
          session->device, &replacement
        );

        return result;
      }

      vkGetBufferMemoryRequirements (
        session->device, replacement.host_buffer, &host_requirements
      );

      result = vulkan_auto_select_memory_type (
        &selected->memory_properties,
        host_requirements.memoryTypeBits,
        &host_type,
        &host_coherent
      );

      if (result != VK_SUCCESS)
      {
        backend_vulkan_session_storage_buffer_release (
          session->device, &replacement
        );

        return result;
      }

      const VkMemoryAllocateInfo host_allocation =
      {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = host_requirements.size,
        .memoryTypeIndex = host_type
      };

      result = vkAllocateMemory (
        session->device, &host_allocation, NULL,
        &replacement.host_memory
      );

      if (result != VK_SUCCESS)
      {
        backend_vulkan_session_storage_buffer_release (
          session->device, &replacement
        );

        return result;
      }

      result = vkBindBufferMemory (
        session->device, replacement.host_buffer,
        replacement.host_memory, 0u
      );

      if (result != VK_SUCCESS)
      {
        backend_vulkan_session_storage_buffer_release (
          session->device, &replacement
        );

        return result;
      }

      result = vkMapMemory (
        session->device, replacement.host_memory,
        0u, VK_WHOLE_SIZE, 0u, &replacement.mapped
      );

      if (result != VK_SUCCESS)
      {
        backend_vulkan_session_storage_buffer_release (
          session->device, &replacement
        );

        return result;
      }
    }

    replacement.capacity_bytes = buffer_bytes[i];
    replacement.staging = use_staging;
    replacement.plan = (vulkan_auto_memory_plan_t)
    {
      .allocation_bytes = host_requirements.size,
      .memory_type_index = host_type,
      .compatible_memory_type_bits = host_requirements.memoryTypeBits,
      .coherent = host_coherent,
      .non_coherent_atom_size =
        selected->properties.limits.nonCoherentAtomSize
    };

    backend_vulkan_session_storage_buffer_release (
      session->device, buffer
    );

    *buffer = replacement;

    session->storage_allocations++;

    views[i] = (backend_vulkan_storage_view_t)
    {
      .buffer = buffer->buffer,
      .memory = buffer->memory,
      .host_buffer = buffer->host_buffer,
      .host_memory = buffer->host_memory,
      .mapped = buffer->mapped,
      .bytes = buffer_bytes[i],
      .capacity_bytes = buffer->capacity_bytes,
      .staging = buffer->staging,
      .plan = buffer->plan
    };

    fprintf (
      diagnostics,
      "BACKEND_VULKAN_STORAGE_ALLOC"
      " index=%" PRIu32 " slot=%" PRIu32
      " bytes=%" PRIu64
      " compute_allocation=%" PRIu64
      " host_allocation=%" PRIu64
      " compute_memory_type=%" PRIu32
      " host_memory_type=%" PRIu32
      " staging=%u coherent=%u"
      " allocations=%" PRIu32 "\n",
      device_index, i,
      (uint64_t) buffer_bytes[i],
      (uint64_t) compute_requirements.size,
      (uint64_t) host_requirements.size,
      compute_type, host_type,
      buffer->staging, buffer->plan.coherent,
      session->storage_allocations
    );
  }

  return VK_SUCCESS;
}

/**
 * Submit one staging copy and retain ownership until its fence is observed.
 *
 * Validates an exact Vulkan storage slot, transfer direction and byte range,
 * flushes noncoherent input, and records explicit HOST/TRANSFER/COMPUTE
 * memory barriers before queue submission. A successful vkQueueSubmit
 * marks the transfer pending. A Vulkan operation error poisons the transfer
 * pair until explicit driver-verified recovery.
 */
VkResult backend_vulkan_session_storage_transfer_begin (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  uint32_t buffer_index,
  VkDeviceSize bytes,
  VkBool32 download)
{
  if (backend_ctx == NULL || backend_ctx->vulkan_session == NULL
   || bytes == 0u || (bytes & 3u) != 0u
   || (download != VK_FALSE && download != VK_TRUE))
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  struct backend_vulkan_compute_session *session =
    backend_ctx->vulkan_session;

  if (session->device == VK_NULL_HANDLE
   || session->queue == VK_NULL_HANDLE
   || session->device_index != device_index
   || session->transfer_failed == VK_TRUE
   || session->transfer_pending == VK_TRUE
   || session->compute_command_failed == VK_TRUE
   || session->descriptor_leased == VK_TRUE
   || session->compute_command_leased == VK_TRUE
   || session->completed_transfers == UINT32_MAX
   || buffer_index >= session->storage_buffer_count)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  const backend_vulkan_session_storage_buffer_t *buffer =
    &session->storage_buffers[buffer_index];

  if (buffer->staging != VK_TRUE
   || buffer->buffer == VK_NULL_HANDLE
   || buffer->host_buffer == VK_NULL_HANDLE
   || buffer->host_memory == VK_NULL_HANDLE
   || buffer->mapped == NULL
   || bytes > buffer->capacity_bytes)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  VkResult result = vkWaitForFences (
    session->device, 1u, &session->fence, VK_TRUE, UINT64_MAX
  );

  if (result != VK_SUCCESS) goto poison;

  if (download == VK_FALSE)
  {
    result = vulkan_auto_sync_mapped_memory (
      session->device, buffer->host_memory, &buffer->plan, VK_FALSE
    );

    if (result != VK_SUCCESS) goto poison;
  }

  result = vkResetCommandBuffer (
    session->command_buffer, 0u
  );

  if (result != VK_SUCCESS) goto poison;

  const VkCommandBufferBeginInfo begin =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
  };

  result = vkBeginCommandBuffer (
    session->command_buffer, &begin
  );

  if (result != VK_SUCCESS) goto poison;

  const VkBufferMemoryBarrier before_copy =
  {
    .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
    .srcAccessMask = download == VK_TRUE
      ? VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT
      : VK_ACCESS_HOST_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .buffer = download == VK_TRUE
      ? buffer->buffer : buffer->host_buffer,
    .offset = 0u,
    .size = bytes
  };

  vkCmdPipelineBarrier (
    session->command_buffer,
    download == VK_TRUE
      ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT
      : VK_PIPELINE_STAGE_HOST_BIT,
    VK_PIPELINE_STAGE_TRANSFER_BIT,
    0u,
    0u, NULL,
    1u, &before_copy,
    0u, NULL
  );

  const VkBufferCopy copy =
  {
    .srcOffset = 0u,
    .dstOffset = 0u,
    .size = bytes
  };

  vkCmdCopyBuffer (
    session->command_buffer,
    download == VK_TRUE ? buffer->buffer : buffer->host_buffer,
    download == VK_TRUE ? buffer->host_buffer : buffer->buffer,
    1u, &copy
  );

  const VkBufferMemoryBarrier after_copy =
  {
    .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
    .dstAccessMask = download == VK_TRUE
      ? VK_ACCESS_HOST_READ_BIT
      : VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .buffer = download == VK_TRUE
      ? buffer->host_buffer : buffer->buffer,
    .offset = 0u,
    .size = bytes
  };

  vkCmdPipelineBarrier (
    session->command_buffer,
    VK_PIPELINE_STAGE_TRANSFER_BIT,
    download == VK_TRUE
      ? VK_PIPELINE_STAGE_HOST_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
    0u,
    0u, NULL,
    1u, &after_copy,
    0u, NULL
  );

  result = vkEndCommandBuffer (session->command_buffer);

  if (result != VK_SUCCESS) goto poison;

  result = vkResetFences (
    session->device, 1u, &session->fence
  );

  if (result != VK_SUCCESS) goto poison;

  const VkSubmitInfo submit =
  {
    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    .commandBufferCount = 1u,
    .pCommandBuffers = &session->command_buffer
  };

  result = vkQueueSubmit (
    session->queue, 1u, &submit, session->fence
  );

  if (result != VK_SUCCESS) goto poison;

  session->pending_buffer_index = buffer_index;
  session->pending_bytes = bytes;
  session->pending_download = download;
  session->transfer_pending = VK_TRUE;
  session->transfer_cancel_requested = VK_FALSE;

  fprintf (
    session->diagnostics,
    "BACKEND_VULKAN_STAGING_SUBMIT"
    " index=%" PRIu32 " slot=%" PRIu32
    " direction=%s bytes=%" PRIu64
    " pending=1\n",
    device_index, buffer_index,
    download == VK_TRUE ? "download" : "upload",
    (uint64_t) bytes
  );

  return VK_SUCCESS;

poison:
  session->transfer_failed = VK_TRUE;

  fprintf (
    session->diagnostics,
    "BACKEND_VULKAN_STAGING_ERROR"
    " index=%" PRIu32 " pending=%u result=%d\n",
    device_index, session->transfer_pending, result
  );

  return result;
}

/**
 * Mark a submitted staging copy for discard without waiting for the GPU.
 *
 * Requires one healthy pending transfer and exclusive ownership of the
 * session's Compute and descriptor leases. The actual Vulkan queue job
 * continues running: the host merely withdraws interest in its result.
 * The transfer command, fence and storage remain pending until a real
 * fence completion or device-idle-verified recovery. The host caller must
 * serialize cancellation and completion; this session is not thread-safe.
 * Cancelling twice
 * or cancelling an already retired submission is rejected.
 */
VkResult backend_vulkan_session_storage_transfer_cancel (
  backend_ctx_t *backend_ctx,
  uint32_t device_index)
{
  if (backend_ctx == NULL || backend_ctx->vulkan_session == NULL)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  struct backend_vulkan_compute_session *session =
    backend_ctx->vulkan_session;

  if (session->device == VK_NULL_HANDLE
   || session->device_index != device_index
   || session->transfer_pending != VK_TRUE
   || session->transfer_failed == VK_TRUE
   || session->transfer_cancel_requested == VK_TRUE
   || session->cancelled_transfers == UINT32_MAX
   || session->compute_command_leased == VK_TRUE
   || session->descriptor_leased == VK_TRUE)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  session->transfer_cancel_requested = VK_TRUE;

  fprintf (
    session->diagnostics,
    "BACKEND_VULKAN_STAGING_CANCEL_REQUEST"
    " index=%" PRIu32
    " slot=%" PRIu32
    " direction=%s"
    " bytes=%" PRIu64
    " pending=1\n",
    device_index, session->pending_buffer_index,
    session->pending_download == VK_TRUE ? "download" : "upload",
    (uint64_t) session->pending_bytes
  );

  return VK_SUCCESS;
}

/**
 * Complete one pending staging submission with an exact fence timeout.
 *
 * Waits for the session-owned transfer fence using timeout_ns. VK_TIMEOUT
 * leaves the pending submission and its storage/command ownership unchanged,
 * permitting subsequent polling, completion or explicit recovery.
 * VK_SUCCESS invalidates noncoherent downloaded host memory only for
 * uncancelled downloads. A cancelled submission is retired without
 * publishing a host result, and its transfer pair becomes reusable.
 * Other Vulkan driver or host-cache errors poison the transfer pair
 * without clearing the pending submission record.
 */
VkResult backend_vulkan_session_storage_transfer_complete_timeout (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  uint64_t timeout_ns)
{
  if (backend_ctx == NULL || backend_ctx->vulkan_session == NULL)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  struct backend_vulkan_compute_session *session =
    backend_ctx->vulkan_session;

  if (session->device == VK_NULL_HANDLE
   || session->device_index != device_index
   || session->transfer_pending != VK_TRUE
   || session->transfer_failed == VK_TRUE
   || session->descriptor_leased == VK_TRUE
   || session->compute_command_leased == VK_TRUE
   || session->pending_buffer_index >= session->storage_buffer_count)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  const backend_vulkan_session_storage_buffer_t *buffer =
    &session->storage_buffers[session->pending_buffer_index];

  VkResult result = vkWaitForFences (
    session->device, 1u, &session->fence, VK_TRUE, timeout_ns
  );

  if (result == VK_TIMEOUT)
  {
    fprintf (
      session->diagnostics,
      "BACKEND_VULKAN_STAGING_WAIT_PENDING"
      " index=%" PRIu32
      " timeout_ns=%" PRIu64
      " pending=1 result=%d\n",
      device_index, timeout_ns, result
    );

    return VK_TIMEOUT;
  }

  if (result == VK_SUCCESS && session->pending_download == VK_TRUE
   && session->transfer_cancel_requested != VK_TRUE)
  {
    result = vulkan_auto_sync_mapped_memory (
      session->device, buffer->host_memory, &buffer->plan, VK_TRUE
    );
  }

  if (result != VK_SUCCESS)
  {
    session->transfer_failed = VK_TRUE;

    fprintf (
      session->diagnostics,
      "BACKEND_VULKAN_STAGING_ERROR"
      " index=%" PRIu32 " pending=1 result=%d\n",
      device_index, result
    );

    return result;
  }

  session->transfer_pending = VK_FALSE;

  if (session->transfer_cancel_requested == VK_TRUE)
  {
    session->transfer_cancel_requested = VK_FALSE;
    session->cancelled_transfers++;

    fprintf (
      session->diagnostics,
      "BACKEND_VULKAN_STAGING_CANCEL_COMPLETE"
      " index=%" PRIu32
      " slot=%" PRIu32
      " direction=%s"
      " bytes=%" PRIu64
      " cancelled=%" PRIu32
      " pending=0\n",
      device_index, session->pending_buffer_index,
      session->pending_download == VK_TRUE ? "download" : "upload",
      (uint64_t) session->pending_bytes,
      session->cancelled_transfers
    );

    return VK_SUCCESS;
  }

  session->completed_transfers++;

  fprintf (
    session->diagnostics,
    "BACKEND_VULKAN_STAGING_TRANSFER_PASS"
    " index=%" PRIu32 " slot=%" PRIu32
    " direction=%s bytes=%" PRIu64
    " coherent=%u completed=%" PRIu32 "\n",
    device_index, session->pending_buffer_index,
    session->pending_download == VK_TRUE ? "download" : "upload",
    (uint64_t) session->pending_bytes, buffer->plan.coherent,
    session->completed_transfers
  );

  return VK_SUCCESS;
}

/**
 * Complete a submitted staging transfer with an unlimited fence wait.
 *
 * Retains the synchronous completion contract by forwarding UINT64_MAX
 * to the timed Vulkan fence wait. Any transient VK_TIMEOUT from another
 * caller leaves the submission pending for this subsequent blocking wait.
 */
VkResult backend_vulkan_session_storage_transfer_complete (
  backend_ctx_t *backend_ctx,
  uint32_t device_index)
{
  return backend_vulkan_session_storage_transfer_complete_timeout (
    backend_ctx, device_index, UINT64_MAX
  );
}

/**
 * Submit and complete an existing synchronous staging transfer.
 *
 * The transfer remains exclusive between queue submission and successful
 * fence wait. Pending or driver-poisoned transfers are rejected until
 * recovery, while direct storage and legacy Compute backends are unchanged.
 */
VkResult backend_vulkan_session_storage_transfer (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  uint32_t buffer_index,
  VkDeviceSize bytes,
  VkBool32 download)
{
  const VkResult result = backend_vulkan_session_storage_transfer_begin (
    backend_ctx, device_index, buffer_index, bytes, download
  );

  if (result != VK_SUCCESS) return result;

  return backend_vulkan_session_storage_transfer_complete (
    backend_ctx, device_index
  );
}

/**
 * Restore an abandoned or failed staging submission after real device idle.
 *
 * Requires a pending or driver-poisoned transfer and no outstanding
 * Compute/descriptor lease. A successful vkDeviceWaitIdle precedes
 * allocation of a replacement primary transfer command and fence.
 * The old pair is freed only after the replacement pair is complete;
 * retained storage and cached Compute resources remain owned by the same
 * logical device. Failed recovery never enables further transfers.
 */
VkResult backend_vulkan_session_storage_recover (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  FILE *diagnostics)
{
  if (backend_ctx == NULL || backend_ctx->vulkan_session == NULL
   || diagnostics == NULL)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  struct backend_vulkan_compute_session *session =
    backend_ctx->vulkan_session;

  if (session->device == VK_NULL_HANDLE
   || session->device_index != device_index
   || session->command_pool == VK_NULL_HANDLE
   || session->command_buffer == VK_NULL_HANDLE
   || session->fence == VK_NULL_HANDLE
   || (session->transfer_failed != VK_TRUE
    && session->transfer_pending != VK_TRUE)
   || session->descriptor_leased == VK_TRUE
   || session->compute_command_leased == VK_TRUE
   || session->transfer_recoveries == UINT32_MAX
   || (session->transfer_pending == VK_TRUE
    && session->transfer_abandoned == UINT32_MAX))
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  VkResult result = vkDeviceWaitIdle (session->device);

  if (result != VK_SUCCESS)
  {
    session->transfer_failed = VK_TRUE;

    fprintf (
      diagnostics,
      "BACKEND_VULKAN_STAGING_RECOVER"
      " index=%" PRIu32
      " wait_result=%d recreated=0 recovered=0\n",
      device_index, result
    );

    return result;
  }

  const VkCommandBufferAllocateInfo command_info =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
    .commandPool = session->command_pool,
    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
    .commandBufferCount = 1u
  };

  VkCommandBuffer replacement_command = VK_NULL_HANDLE;

  result = vkAllocateCommandBuffers (
    session->device, &command_info, &replacement_command
  );

  if (result != VK_SUCCESS)
  {
    session->transfer_failed = VK_TRUE;

    return result;
  }

  const VkFenceCreateInfo fence_info =
  {
    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
    .flags = VK_FENCE_CREATE_SIGNALED_BIT
  };

  VkFence replacement_fence = VK_NULL_HANDLE;

  result = vkCreateFence (
    session->device, &fence_info, NULL, &replacement_fence
  );

  if (result != VK_SUCCESS)
  {
    vkFreeCommandBuffers (
      session->device, session->command_pool, 1u,
      &replacement_command
    );

    session->transfer_failed = VK_TRUE;

    return result;
  }

  vkDestroyFence (session->device, session->fence, NULL);

  vkFreeCommandBuffers (
    session->device, session->command_pool, 1u,
    &session->command_buffer
  );

  const VkBool32 abandoned = session->transfer_pending;

  session->fence = replacement_fence;
  session->command_buffer = replacement_command;
  session->transfer_failed = VK_FALSE;
  session->transfer_pending = VK_FALSE;
  session->transfer_cancel_requested = VK_FALSE;
  session->pending_bytes = 0u;
  session->pending_download = VK_FALSE;
  session->transfer_recoveries++;

  if (abandoned == VK_TRUE)
  {
    session->transfer_abandoned++;
  }

  fprintf (
    diagnostics,
    "BACKEND_VULKAN_STAGING_RECOVER"
    " index=%" PRIu32
    " wait_result=0 recreated=1"
    " recovered=%" PRIu32
    " abandoned=%" PRIu32 "\n",
    device_index,
    session->transfer_recoveries, session->transfer_abandoned
  );

  return VK_SUCCESS;
}

/**
 * Borrow an exact cached SPIR-V Compute pipeline from the active session.
 *
 * The first successful request establishes one two-storage-buffer descriptor
 * layout and pipeline layout for all public stages. Each distinct complete
 * SPIR-V module creates one VkPipeline and one private identity copy. Later
 * requests for the exact same words borrow that pipeline with no new shader,
 * pipeline or layout allocation. Every acquired Vulkan object remains owned
 * by the session and is destroyed before its VkDevice.
 */
VkResult backend_vulkan_session_pipeline_acquire (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  const uint32_t *spirv,
  uint32_t spirv_words,
  backend_vulkan_pipeline_view_t *view,
  FILE *diagnostics)
{
  if (backend_ctx == NULL || backend_ctx->vulkan_session == NULL
   || spirv == NULL || spirv_words < 5u
   || view == NULL || diagnostics == NULL)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  struct backend_vulkan_compute_session *session =
    backend_ctx->vulkan_session;

  if (session->device == VK_NULL_HANDLE
   || session->device_index != device_index
   || session->compute_command_failed == VK_TRUE
   || session->transfer_pending == VK_TRUE
   || session->transfer_failed == VK_TRUE)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  size_t code_bytes = 0u;

  if (__builtin_mul_overflow (
        (size_t) spirv_words, sizeof (*spirv), &code_bytes
      ))
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  for (uint32_t i = 0u; i < session->cached_pipeline_count; i++)
  {
    const backend_vulkan_session_pipeline_t *entry =
      &session->cached_pipelines[i];

    if (entry->spirv_words != spirv_words
     || memcmp (entry->spirv, spirv, code_bytes) != 0)
    {
      continue;
    }

    session->cached_pipeline_hits++;

    *view = (backend_vulkan_pipeline_view_t)
    {
      .descriptor_layout = session->cached_descriptor_layout,
      .pipeline_layout = session->cached_pipeline_layout,
      .pipeline = entry->pipeline
    };

    fprintf (
      diagnostics,
      "BACKEND_VULKAN_PIPELINE_CACHE_HIT"
      " index=%" PRIu32 " words=%" PRIu32
      " entries=%" PRIu32 " hits=%" PRIu32 "\n",
      device_index, spirv_words,
      session->cached_pipeline_count,
      session->cached_pipeline_hits
    );

    return VK_SUCCESS;
  }

  if (session->cached_descriptor_layout == VK_NULL_HANDLE)
  {
    const VkDescriptorSetLayoutBinding bindings[2] =
    {
      {
        .binding = 0u,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1u,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
      },
      {
        .binding = 1u,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1u,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT
      }
    };

    const VkDescriptorSetLayoutCreateInfo descriptor_info =
    {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 2u,
      .pBindings = bindings
    };

    const VkResult result = vkCreateDescriptorSetLayout (
      session->device, &descriptor_info, NULL,
      &session->cached_descriptor_layout
    );

    if (result != VK_SUCCESS) return result;
  }

  if (session->cached_pipeline_layout == VK_NULL_HANDLE)
  {
    const VkPipelineLayoutCreateInfo layout_info =
    {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1u,
      .pSetLayouts = &session->cached_descriptor_layout
    };

    const VkResult result = vkCreatePipelineLayout (
      session->device, &layout_info, NULL,
      &session->cached_pipeline_layout
    );

    if (result != VK_SUCCESS) return result;
  }

  if (session->cached_pipeline_count == UINT32_MAX)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  size_t array_bytes = 0u;

  if (__builtin_mul_overflow (
        (size_t) (session->cached_pipeline_count + 1u),
        sizeof (*session->cached_pipelines), &array_bytes
      ))
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  uint32_t *code_copy = (uint32_t *) malloc (code_bytes);

  if (code_copy == NULL) return VK_ERROR_OUT_OF_HOST_MEMORY;

  memcpy (code_copy, spirv, code_bytes);

  const VkShaderModuleCreateInfo shader_info =
  {
    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
    .codeSize = code_bytes,
    .pCode = code_copy
  };

  VkShaderModule shader = VK_NULL_HANDLE;

  VkResult result = vkCreateShaderModule (
    session->device, &shader_info, NULL, &shader
  );

  if (result != VK_SUCCESS)
  {
    free (code_copy);

    return result;
  }

  const VkComputePipelineCreateInfo pipeline_info =
  {
    .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
    .stage =
    {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
      .module = shader,
      .pName = "main"
    },
    .layout = session->cached_pipeline_layout
  };

  VkPipeline pipeline = VK_NULL_HANDLE;

  result = vkCreateComputePipelines (
    session->device, VK_NULL_HANDLE, 1u,
    &pipeline_info, NULL, &pipeline
  );

  vkDestroyShaderModule (session->device, shader, NULL);

  if (result != VK_SUCCESS)
  {
    if (pipeline != VK_NULL_HANDLE)
    {
      vkDestroyPipeline (session->device, pipeline, NULL);
    }

    free (code_copy);

    return result;
  }

  backend_vulkan_session_pipeline_t *expanded =
    (backend_vulkan_session_pipeline_t *) realloc (
      session->cached_pipelines, array_bytes
    );

  if (expanded == NULL)
  {
    vkDestroyPipeline (session->device, pipeline, NULL);

    free (code_copy);

    return VK_ERROR_OUT_OF_HOST_MEMORY;
  }

  session->cached_pipelines = expanded;

  session->cached_pipelines[session->cached_pipeline_count] =
    (backend_vulkan_session_pipeline_t)
    {
      .spirv = code_copy,
      .spirv_words = spirv_words,
      .pipeline = pipeline
    };

  session->cached_pipeline_count++;

  *view = (backend_vulkan_pipeline_view_t)
  {
    .descriptor_layout = session->cached_descriptor_layout,
    .pipeline_layout = session->cached_pipeline_layout,
    .pipeline = pipeline
  };

  fprintf (
    diagnostics,
    "BACKEND_VULKAN_PIPELINE_CACHE_BUILD"
    " index=%" PRIu32 " words=%" PRIu32
    " entries=%" PRIu32 "\n",
    device_index, spirv_words, session->cached_pipeline_count
  );

  return VK_SUCCESS;
}

/**
 * Bind exact session-owned storage ranges to cached descriptor stage sets.
 *
 * Validates the complete binding sequence and active device before creating
 * or modifying descriptor objects. Pools are keyed only by the exact number
 * of stages. Each pool contains exactly that many descriptor sets and two
 * storage-buffer descriptors per set. Reacquisition rewrites both bindings
 * to the current VkBuffer handles and logical ranges, even when the stage
 * count matches a prior job. The lease remains exclusive until release.
 */
VkResult backend_vulkan_session_descriptors_acquire (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  const backend_vulkan_descriptor_binding_t *bindings,
  uint32_t stage_count,
  backend_vulkan_descriptor_view_t *view,
  FILE *diagnostics)
{
  if (backend_ctx == NULL || backend_ctx->vulkan_session == NULL
   || bindings == NULL || stage_count == 0u
   || stage_count > UINT32_MAX / 2u
   || view == NULL || diagnostics == NULL)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  struct backend_vulkan_compute_session *session =
    backend_ctx->vulkan_session;

  if (session->device == VK_NULL_HANDLE
   || session->device_index != device_index
   || session->cached_descriptor_layout == VK_NULL_HANDLE
   || session->descriptor_leased == VK_TRUE
   || session->transfer_pending == VK_TRUE
   || session->compute_command_leased == VK_TRUE
   || session->compute_command_failed == VK_TRUE
   || session->transfer_failed == VK_TRUE)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  for (uint32_t stage = 0u; stage < stage_count; stage++)
  {
    const backend_vulkan_descriptor_binding_t *binding =
      &bindings[stage];

    if (binding->source_buffer == VK_NULL_HANDLE
     || binding->destination_buffer == VK_NULL_HANDLE
     || binding->source_bytes == 0u
     || binding->destination_bytes == 0u)
    {
      return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkBool32 source_found = VK_FALSE;
    VkBool32 destination_found = VK_FALSE;

    for (uint32_t slot = 0u; slot < session->storage_buffer_count; slot++)
    {
      const backend_vulkan_session_storage_buffer_t *buffer =
        &session->storage_buffers[slot];

      if (buffer->buffer == binding->source_buffer
       && buffer->capacity_bytes >= binding->source_bytes)
      {
        source_found = VK_TRUE;
      }

      if (buffer->buffer == binding->destination_buffer
       && buffer->capacity_bytes >= binding->destination_bytes)
      {
        destination_found = VK_TRUE;
      }
    }

    if (source_found != VK_TRUE || destination_found != VK_TRUE)
    {
      return VK_ERROR_INITIALIZATION_FAILED;
    }
  }

  if (session->descriptor_acquisitions == UINT32_MAX
   || session->descriptor_writes > UINT32_MAX - stage_count * 2u)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  backend_vulkan_session_descriptor_pool_t *entry = NULL;
  uint32_t pool_index = 0u;

  for (uint32_t i = 0u; i < session->descriptor_pool_count; i++)
  {
    if (session->descriptor_pools[i].stage_count == stage_count)
    {
      entry = &session->descriptor_pools[i];
      pool_index = i;

      break;
    }
  }

  if (entry != NULL)
  {
    if (session->descriptor_pool_reuses == UINT32_MAX)
    {
      return VK_ERROR_INITIALIZATION_FAILED;
    }

    session->descriptor_pool_reuses++;

    fprintf (
      diagnostics,
      "BACKEND_VULKAN_DESCRIPTOR_POOL_REUSE"
      " index=%" PRIu32
      " stages=%" PRIu32
      " reuses=%" PRIu32 "\n",
      device_index, stage_count, session->descriptor_pool_reuses
    );
  }
  else
  {
    if (session->descriptor_pool_count == UINT32_MAX)
    {
      return VK_ERROR_INITIALIZATION_FAILED;
    }

    size_t set_bytes = 0u;
    size_t pool_bytes = 0u;

    if (__builtin_mul_overflow (
          (size_t) stage_count, sizeof (VkDescriptorSet),
          &set_bytes
        )
     || __builtin_mul_overflow (
          (size_t) stage_count, sizeof (VkDescriptorSetLayout),
          &pool_bytes
        ))
    {
      return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkDescriptorSet *sets = (VkDescriptorSet *) calloc (
      1u, set_bytes
    );

    VkDescriptorSetLayout *layouts =
      (VkDescriptorSetLayout *) malloc (pool_bytes);

    if (sets == NULL || layouts == NULL)
    {
      free (layouts);
      free (sets);

      return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    for (uint32_t i = 0u; i < stage_count; i++)
    {
      layouts[i] = session->cached_descriptor_layout;
    }

    const VkDescriptorPoolSize pool_size =
    {
      .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
      .descriptorCount = stage_count * 2u
    };

    const VkDescriptorPoolCreateInfo pool_info =
    {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = stage_count,
      .poolSizeCount = 1u,
      .pPoolSizes = &pool_size
    };

    VkDescriptorPool pool = VK_NULL_HANDLE;

    VkResult result = vkCreateDescriptorPool (
      session->device, &pool_info, NULL, &pool
    );

    if (result == VK_SUCCESS)
    {
      const VkDescriptorSetAllocateInfo sets_info =
      {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool,
        .descriptorSetCount = stage_count,
        .pSetLayouts = layouts
      };

      result = vkAllocateDescriptorSets (
        session->device, &sets_info, sets
      );
    }

    free (layouts);

    if (result != VK_SUCCESS)
    {
      if (pool != VK_NULL_HANDLE)
      {
        vkDestroyDescriptorPool (session->device, pool, NULL);
      }

      free (sets);

      return result;
    }

    size_t array_bytes = 0u;

    if (__builtin_mul_overflow (
          (size_t) (session->descriptor_pool_count + 1u),
          sizeof (*session->descriptor_pools), &array_bytes
        ))
    {
      vkDestroyDescriptorPool (session->device, pool, NULL);
      free (sets);

      return VK_ERROR_INITIALIZATION_FAILED;
    }

    backend_vulkan_session_descriptor_pool_t *expanded =
      (backend_vulkan_session_descriptor_pool_t *) realloc (
        session->descriptor_pools, array_bytes
      );

    if (expanded == NULL)
    {
      vkDestroyDescriptorPool (session->device, pool, NULL);
      free (sets);

      return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    session->descriptor_pools = expanded;
    pool_index = session->descriptor_pool_count;

    session->descriptor_pools[pool_index] =
      (backend_vulkan_session_descriptor_pool_t)
      {
        .pool = pool,
        .sets = sets,
        .stage_count = stage_count
      };

    entry = &session->descriptor_pools[pool_index];

    session->descriptor_pool_count++;

    fprintf (
      diagnostics,
      "BACKEND_VULKAN_DESCRIPTOR_POOL_BUILD"
      " index=%" PRIu32
      " stages=%" PRIu32
      " pools=%" PRIu32 "\n",
      device_index, stage_count, session->descriptor_pool_count
    );
  }

  for (uint32_t stage = 0u; stage < stage_count; stage++)
  {
    const backend_vulkan_descriptor_binding_t *binding =
      &bindings[stage];

    const VkDescriptorBufferInfo buffer_info[2] =
    {
      {
        .buffer = binding->source_buffer,
        .offset = 0u,
        .range = binding->source_bytes
      },
      {
        .buffer = binding->destination_buffer,
        .offset = 0u,
        .range = binding->destination_bytes
      }
    };

    const VkWriteDescriptorSet writes[2] =
    {
      {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = entry->sets[stage],
        .dstBinding = 0u,
        .descriptorCount = 1u,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &buffer_info[0]
      },
      {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = entry->sets[stage],
        .dstBinding = 1u,
        .descriptorCount = 1u,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &buffer_info[1]
      }
    };

    vkUpdateDescriptorSets (
      session->device, 2u, writes, 0u, NULL
    );
  }

  session->descriptor_leased = VK_TRUE;
  session->descriptor_acquisitions++;
  session->descriptor_writes += stage_count * 2u;

  *view = (backend_vulkan_descriptor_view_t)
  {
    .sets = entry->sets,
    .stage_count = stage_count
  };

  fprintf (
    diagnostics,
    "BACKEND_VULKAN_DESCRIPTOR_BIND"
    " index=%" PRIu32
    " stages=%" PRIu32
    " writes=%" PRIu32
    " acquisitions=%" PRIu32 "\n",
    device_index, stage_count, stage_count * 2u,
    session->descriptor_acquisitions
  );

  return VK_SUCCESS;
}

/**
 * Return the exclusive descriptor-set lease to the owning session.
 *
 * Only the active exact device index with one outstanding lease can
 * complete the operation. A submitted Compute command retains descriptor
 * ownership until the real fence confirms completion. A poisoned command
 * pair also retains descriptors until device-idle-verified recovery.
 * The cached descriptor sets and pool stay allocated for later reuse.
 */
VkResult backend_vulkan_session_descriptors_release (
  backend_ctx_t *backend_ctx,
  uint32_t device_index)
{
  if (backend_ctx == NULL || backend_ctx->vulkan_session == NULL)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  struct backend_vulkan_compute_session *session =
    backend_ctx->vulkan_session;

  if (session->device_index != device_index
   || session->descriptor_leased != VK_TRUE
   || session->compute_submission_pending == VK_TRUE
   || session->compute_command_failed == VK_TRUE)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  session->descriptor_leased = VK_FALSE;

  return VK_SUCCESS;
}

/**
 * Borrow one resettable command buffer and fence for a synchronous job.
 *
 * Uses the existing session command pool but allocates a separate primary
 * buffer and fence from the transfer pair. On subsequent acquisitions the
 * exact handles are returned after a reset of the previously completed
 * command buffer. At most one caller holds the Compute lease at a time.
 */
VkResult backend_vulkan_session_command_acquire (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  backend_vulkan_command_view_t *view,
  FILE *diagnostics)
{
  if (backend_ctx == NULL || backend_ctx->vulkan_session == NULL
   || view == NULL || diagnostics == NULL)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  struct backend_vulkan_compute_session *session =
    backend_ctx->vulkan_session;

  if (session->device == VK_NULL_HANDLE
   || session->device_index != device_index
   || session->compute_command_leased == VK_TRUE
   || session->compute_command_failed == VK_TRUE
   || session->compute_submission_pending == VK_TRUE
   || session->transfer_pending == VK_TRUE
   || session->transfer_failed == VK_TRUE)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  const VkBool32 reused =
    session->compute_command != VK_NULL_HANDLE;

  if (session->compute_command == VK_NULL_HANDLE)
  {
    const VkCommandBufferAllocateInfo info =
    {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = session->command_pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1u
    };

    VkResult result = vkAllocateCommandBuffers (
      session->device, &info, &session->compute_command
    );

    if (result != VK_SUCCESS) return result;

    const VkFenceCreateInfo fence_info =
    {
      .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
    };

    result = vkCreateFence (
      session->device, &fence_info, NULL, &session->compute_fence
    );

    if (result != VK_SUCCESS)
    {
      vkFreeCommandBuffers (
        session->device, session->command_pool, 1u,
        &session->compute_command
      );

      session->compute_command = VK_NULL_HANDLE;

      return result;
    }
  }

  const VkResult reset_result = vkResetCommandBuffer (
    session->compute_command, 0u
  );

  if (reset_result != VK_SUCCESS)
  {
    session->compute_command_failed = VK_TRUE;

    return reset_result;
  }

  session->compute_command_leased = VK_TRUE;
  session->compute_submission_consumed = VK_FALSE;
  session->compute_cancel_requested = VK_FALSE;
  session->compute_command_acquisitions++;

  if (reused == VK_TRUE)
  {
    session->compute_command_reuses++;
  }

  *view = (backend_vulkan_command_view_t)
  {
    .command = session->compute_command,
    .fence = session->compute_fence
  };

  fprintf (
    diagnostics,
    "BACKEND_VULKAN_COMMAND_REUSE"
    " index=%" PRIu32 " acquisition=%" PRIu32
    " reused=%u reuses=%" PRIu32 "\n",
    device_index,
    session->compute_command_acquisitions,
    reused, session->compute_command_reuses
  );

  return VK_SUCCESS;
}

/**
 * Submit one session-owned primary Compute command through its retained fence.
 *
 * Requires an active exclusive command lease and an already ended command
 * recording. Resets the retained fence before queue submission, then marks
 * the resulting queue work pending. Each lease permits only one queue
 * submission, even if a previous fence already signaled; callers must
 * release and reacquire to reset the command buffer for another recording.
 * Real submission failures poison the command pair without releasing
 * its lease or other cached GPU resources.
 * The caller must release a failed lease as incomplete before recovery.
 */
VkResult backend_vulkan_session_command_submit (
  backend_ctx_t *backend_ctx,
  uint32_t device_index)
{
  if (backend_ctx == NULL || backend_ctx->vulkan_session == NULL)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  struct backend_vulkan_compute_session *session =
    backend_ctx->vulkan_session;

  if (session->device == VK_NULL_HANDLE
   || session->queue == VK_NULL_HANDLE
   || session->device_index != device_index
   || session->compute_command == VK_NULL_HANDLE
   || session->compute_fence == VK_NULL_HANDLE
   || session->compute_command_leased != VK_TRUE
   || session->compute_command_failed == VK_TRUE
   || session->compute_submission_pending == VK_TRUE
   || session->compute_submission_consumed == VK_TRUE
   || session->transfer_pending == VK_TRUE
   || session->transfer_failed == VK_TRUE
   || session->compute_managed_submissions == UINT32_MAX
   || session->compute_managed_completed == UINT32_MAX
   || session->compute_managed_cancelled == UINT32_MAX)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  VkResult result = vkResetFences (
    session->device, 1u, &session->compute_fence
  );

  if (result != VK_SUCCESS) goto poison;

  const VkSubmitInfo submit =
  {
    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    .commandBufferCount = 1u,
    .pCommandBuffers = &session->compute_command
  };

  result = vkQueueSubmit (
    session->queue, 1u, &submit, session->compute_fence
  );

  if (result != VK_SUCCESS) goto poison;

  session->compute_submission_pending = VK_TRUE;
  session->compute_submission_consumed = VK_TRUE;
  session->compute_cancel_requested = VK_FALSE;
  session->compute_managed_submissions++;

  fprintf (
    session->diagnostics,
    "BACKEND_VULKAN_MANAGED_COMMAND_SUBMIT"
    " index=%" PRIu32
    " pending=1"
    " submitted=%" PRIu32 "\n",
    device_index, session->compute_managed_submissions
  );

  return VK_SUCCESS;

poison:
  session->compute_command_failed = VK_TRUE;

  fprintf (
    session->diagnostics,
    "BACKEND_VULKAN_MANAGED_COMMAND_ERROR"
    " index=%" PRIu32 " result=%d\n",
    device_index, result
  );

  return result;
}

/**
 * Discard one pending Compute submission's output without waiting for GPU idle.
 *
 * Vulkan queue work already submitted continues to execute; the borrowed
 * command/fence pair and any descriptors remain leased until a successful
 * fence completion. Repeated cancellation or a missing pending submission
 * returns VK_ERROR_INITIALIZATION_FAILED without changing ownership.
 */
VkResult backend_vulkan_session_command_cancel (
  backend_ctx_t *backend_ctx,
  uint32_t device_index)
{
  if (backend_ctx == NULL || backend_ctx->vulkan_session == NULL)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  struct backend_vulkan_compute_session *session =
    backend_ctx->vulkan_session;

  if (session->device == VK_NULL_HANDLE
   || session->device_index != device_index
   || session->compute_command_leased != VK_TRUE
   || session->compute_command_failed == VK_TRUE
   || session->compute_submission_pending != VK_TRUE
   || session->compute_cancel_requested == VK_TRUE
   || session->compute_managed_cancelled == UINT32_MAX)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  session->compute_cancel_requested = VK_TRUE;

  fprintf (
    session->diagnostics,
    "BACKEND_VULKAN_MANAGED_COMMAND_CANCEL_REQUEST"
    " index=%" PRIu32 " pending=1\n",
    device_index
  );

  return VK_SUCCESS;
}

/**
 * Observe completion of one submitted session Compute command.
 *
 * Calls vkWaitForFences with the caller's exact nanosecond timeout.
 * VK_TIMEOUT retains exclusive command and descriptor ownership. On a
 * successful fence wait, one completed or cancelled submission is recorded,
 * while the command lease remains held until explicit normal release.
 * Driver errors poison the command pair for idle-verified recovery.
 */
VkResult backend_vulkan_session_command_complete_timeout (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  uint64_t timeout_ns)
{
  if (backend_ctx == NULL || backend_ctx->vulkan_session == NULL)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  struct backend_vulkan_compute_session *session =
    backend_ctx->vulkan_session;

  if (session->device == VK_NULL_HANDLE
   || session->device_index != device_index
   || session->compute_command_leased != VK_TRUE
   || session->compute_command_failed == VK_TRUE
   || session->compute_submission_pending != VK_TRUE
   || session->compute_fence == VK_NULL_HANDLE)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  const VkResult result = vkWaitForFences (
    session->device, 1u, &session->compute_fence, VK_TRUE, timeout_ns
  );

  if (result == VK_TIMEOUT)
  {
    fprintf (
      session->diagnostics,
      "BACKEND_VULKAN_MANAGED_COMMAND_WAIT_PENDING"
      " index=%" PRIu32
      " timeout_ns=%" PRIu64
      " pending=1 result=%d\n",
      device_index, timeout_ns, result
    );

    return result;
  }

  if (result != VK_SUCCESS)
  {
    session->compute_command_failed = VK_TRUE;

    fprintf (
      session->diagnostics,
      "BACKEND_VULKAN_MANAGED_COMMAND_ERROR"
      " index=%" PRIu32 " result=%d\n",
      device_index, result
    );

    return result;
  }

  session->compute_submission_pending = VK_FALSE;

  if (session->compute_cancel_requested == VK_TRUE)
  {
    session->compute_cancel_requested = VK_FALSE;
    session->compute_managed_cancelled++;

    fprintf (
      session->diagnostics,
      "BACKEND_VULKAN_MANAGED_COMMAND_CANCEL_COMPLETE"
      " index=%" PRIu32
      " cancelled=%" PRIu32
      " pending=0\n",
      device_index, session->compute_managed_cancelled
    );

    return VK_SUCCESS;
  }

  session->compute_managed_completed++;

  fprintf (
    session->diagnostics,
    "BACKEND_VULKAN_MANAGED_COMMAND_COMPLETE"
    " index=%" PRIu32
    " completed=%" PRIu32
    " pending=0\n",
    device_index, session->compute_managed_completed
  );

  return VK_SUCCESS;
}

/**
 * Complete a session Compute submission using an unlimited fence wait.
 *
 * Delegates to the timeout-aware method with UINT64_MAX. Completed work
 * remains leased until a matching command_release with completed=VK_TRUE.
 */
VkResult backend_vulkan_session_command_complete (
  backend_ctx_t *backend_ctx,
  uint32_t device_index)
{
  return backend_vulkan_session_command_complete_timeout (
    backend_ctx, device_index, UINT64_MAX
  );
}

/**
 * Restore a poisoned Compute command/fence pair after GPU work is idle.
 *
 * Requires an inactive command lease, a retained VkDevice and a failed
 * prior command. Any descriptor lease stays held across recovery so its
 * Vulkan objects cannot be rebound or released before GPU work is idle.
 * A real vkDeviceWaitIdle proves that submitted work has completed.
 * Replacement primary command and fence objects are allocated before the
 * old pair is destroyed, preserving the failed pair on allocation failure.
 * Storage, descriptors, pipelines and the logical device remain retained.
 * Device loss or any driver error keeps the session poisoned.
 */
VkResult backend_vulkan_session_command_recover (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  FILE *diagnostics)
{
  if (backend_ctx == NULL || backend_ctx->vulkan_session == NULL
   || diagnostics == NULL)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  struct backend_vulkan_compute_session *session =
    backend_ctx->vulkan_session;

  if (session->device_index != device_index
   || session->device == VK_NULL_HANDLE
   || session->command_pool == VK_NULL_HANDLE
   || session->compute_command == VK_NULL_HANDLE
   || session->compute_fence == VK_NULL_HANDLE
   || session->compute_command_failed != VK_TRUE
   || session->compute_command_leased == VK_TRUE
   || session->transfer_pending == VK_TRUE
   || session->transfer_failed == VK_TRUE
   || session->compute_command_recoveries == UINT32_MAX)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  VkResult result = vkDeviceWaitIdle (session->device);

  if (result != VK_SUCCESS)
  {
    fprintf (
      diagnostics,
      "BACKEND_VULKAN_COMMAND_RECOVER"
      " index=%" PRIu32
      " wait_result=%d"
      " recreated=0"
      " recovered=0\n",
      device_index, result
    );

    return result;
  }

  const VkCommandBufferAllocateInfo command_info =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
    .commandPool = session->command_pool,
    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
    .commandBufferCount = 1u
  };

  VkCommandBuffer replacement_command = VK_NULL_HANDLE;

  result = vkAllocateCommandBuffers (
    session->device, &command_info, &replacement_command
  );

  if (result != VK_SUCCESS) return result;

  const VkFenceCreateInfo fence_info =
  {
    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
  };

  VkFence replacement_fence = VK_NULL_HANDLE;

  result = vkCreateFence (
    session->device, &fence_info, NULL, &replacement_fence
  );

  if (result != VK_SUCCESS)
  {
    vkFreeCommandBuffers (
      session->device, session->command_pool, 1u,
      &replacement_command
    );

    return result;
  }

  vkDestroyFence (
    session->device, session->compute_fence, NULL
  );

  vkFreeCommandBuffers (
    session->device, session->command_pool, 1u,
    &session->compute_command
  );

  session->compute_fence = replacement_fence;
  session->compute_command = replacement_command;
  session->compute_command_failed = VK_FALSE;
  session->compute_submission_pending = VK_FALSE;
  session->compute_submission_consumed = VK_FALSE;
  session->compute_cancel_requested = VK_FALSE;
  session->compute_command_recoveries++;

  fprintf (
    diagnostics,
    "BACKEND_VULKAN_COMMAND_RECOVER"
    " index=%" PRIu32
    " wait_result=0"
    " recreated=1"
    " recovered=%" PRIu32 "\n",
    device_index, session->compute_command_recoveries
  );

  return VK_SUCCESS;
}

/**
 * Return one exclusively leased Compute command pair after job cleanup.
 *
 * A submitted job returns the lease for reuse only after the real
 * fence has completed. A successfully ended command that was never
 * submitted also permits completed=VK_TRUE: it has no pending GPU work
 * and will be reset on the next acquisition. The caller must verify its
 * recording and submission state before claiming completion. An incomplete
 * recording, failed queue submission or unresolved GPU synchronization
 * poisons the command pair when returned incomplete. Recovery then
 * requires verified device idle. Cached resources remain session-owned.
 */
VkResult backend_vulkan_session_command_release (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  VkBool32 completed)
{
  if (backend_ctx == NULL || backend_ctx->vulkan_session == NULL
   || (completed != VK_FALSE && completed != VK_TRUE))
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  struct backend_vulkan_compute_session *session =
    backend_ctx->vulkan_session;

  if (session->device_index != device_index
   || session->compute_command_leased != VK_TRUE
   || (completed == VK_TRUE
    && (session->compute_submission_pending == VK_TRUE
     || session->compute_command_failed == VK_TRUE)))
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  session->compute_command_leased = VK_FALSE;

  if (completed != VK_TRUE)
  {
    session->compute_command_failed = VK_TRUE;

    fprintf (
      session->diagnostics,
      "BACKEND_VULKAN_COMMAND_POISON"
      " index=%" PRIu32
      " leased=0"
      " recoverable_after_idle=1\n",
      device_index
    );
  }

  return VK_SUCCESS;
}

/**
 * Close an exact Vulkan device session only when host ownership is quiescent.
 *
 * A live Compute/descriptor lease, pending queue submission, or poisoned
 * synchronization path requires an explicit normal completion, release
 * or driver-verified recovery before this nonblocking preflight succeeds.
 * VK_NOT_READY retains every resource and does not issue a Vulkan driver
 * call. When no borrowed or pending objects remain, the normal closure
 * waits for device idle before destroying its session-owned handles.
 * An empty session remains idempotent. The caller serializes all API use.
 */
VkResult backend_vulkan_session_close_if_ready (
  backend_ctx_t *backend_ctx,
  uint32_t device_index)
{
  if (backend_ctx == NULL) return VK_ERROR_INITIALIZATION_FAILED;

  struct backend_vulkan_compute_session *session =
    backend_ctx->vulkan_session;

  if (session == NULL) return VK_SUCCESS;

  if (session->device == VK_NULL_HANDLE
   || session->device_index != device_index)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  if (session->compute_command_leased == VK_TRUE
   || session->descriptor_leased == VK_TRUE
   || session->compute_submission_pending == VK_TRUE
   || session->compute_cancel_requested == VK_TRUE
   || session->compute_command_failed == VK_TRUE
   || session->transfer_pending == VK_TRUE
   || session->transfer_cancel_requested == VK_TRUE
   || session->transfer_failed == VK_TRUE)
  {
    fprintf (
      session->diagnostics,
      "BACKEND_VULKAN_CLOSE_DEFERRED"
      " index=%" PRIu32
      " compute_leased=%u"
      " descriptors_leased=%u"
      " compute_pending=%u"
      " compute_failed=%u"
      " transfer_pending=%u"
      " transfer_failed=%u"
      " result=%d\n",
      device_index, session->compute_command_leased,
      session->descriptor_leased, session->compute_submission_pending,
      session->compute_command_failed, session->transfer_pending,
      session->transfer_failed, VK_NOT_READY
    );

    return VK_NOT_READY;
  }

  return backend_vulkan_session_close (backend_ctx);
}

/**
 * Advance graceful shutdown by canceling and retiring pending Vulkan work.
 *
 * Only session-managed Compute and staging submissions are eligible for
 * cooperative cancellation. Vulkan does not revoke submitted commands,
 * so their command, descriptor and storage leases remain owned until a
 * real vkWaitForFences success. VK_TIMEOUT returns VK_NOT_READY without
 * modifying those leases. A session-managed Compute submission whose
 * fence was confirmed by a prior caller is also eligible for retirement.
 * Descriptor and command leases are released after confirmed completion.
 *
 * Unsubmitted borrowed work or poisoned synchronization requires action
 * by the original owner and is never forcibly destroyed by this method.
 * When all leases are free, close_if_ready performs the final device-idle
 * wait and normal teardown. The host serializes all session operations.
 */
VkResult backend_vulkan_session_shutdown_poll (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  uint64_t timeout_ns)
{
  if (backend_ctx == NULL) return VK_ERROR_INITIALIZATION_FAILED;

  struct backend_vulkan_compute_session *session =
    backend_ctx->vulkan_session;

  if (session == NULL) return VK_SUCCESS;

  if (session->device == VK_NULL_HANDLE
   || session->device_index != device_index)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  if (session->compute_command_failed == VK_TRUE
   || session->transfer_failed == VK_TRUE)
  {
    return VK_NOT_READY;
  }

  if (session->compute_submission_pending == VK_TRUE)
  {
    if (session->compute_command_leased != VK_TRUE)
    {
      return VK_NOT_READY;
    }

    if (session->compute_cancel_requested != VK_TRUE)
    {
      const VkResult cancel_result =
        backend_vulkan_session_command_cancel (
          backend_ctx, device_index
        );

      if (cancel_result != VK_SUCCESS) return cancel_result;
    }

    const VkResult wait_result =
      backend_vulkan_session_command_complete_timeout (
        backend_ctx, device_index, timeout_ns
      );

    if (wait_result == VK_TIMEOUT)
    {
      fprintf (
        session->diagnostics,
        "BACKEND_VULKAN_SHUTDOWN_PENDING"
        " index=%" PRIu32
        " kind=compute"
        " timeout_ns=%" PRIu64
        " result=%d\n",
        device_index, timeout_ns, VK_NOT_READY
      );

      return VK_NOT_READY;
    }

    if (wait_result != VK_SUCCESS) return wait_result;
  }

  if (session->compute_submission_pending != VK_TRUE
   && session->compute_submission_consumed == VK_TRUE
   && session->compute_command_leased == VK_TRUE)
  {
    if (session->descriptor_leased == VK_TRUE)
    {
      const VkResult descriptor_result =
        backend_vulkan_session_descriptors_release (
          backend_ctx, device_index
        );

      if (descriptor_result != VK_SUCCESS) return descriptor_result;
    }

    const VkResult release_result =
      backend_vulkan_session_command_release (
        backend_ctx, device_index, VK_TRUE
      );

    if (release_result != VK_SUCCESS) return release_result;
  }

  if (session->transfer_pending == VK_TRUE)
  {
    if (session->transfer_cancel_requested != VK_TRUE)
    {
      const VkResult cancel_result =
        backend_vulkan_session_storage_transfer_cancel (
          backend_ctx, device_index
        );

      if (cancel_result != VK_SUCCESS) return cancel_result;
    }

    const VkResult wait_result =
      backend_vulkan_session_storage_transfer_complete_timeout (
        backend_ctx, device_index, timeout_ns
      );

    if (wait_result == VK_TIMEOUT)
    {
      fprintf (
        session->diagnostics,
        "BACKEND_VULKAN_SHUTDOWN_PENDING"
        " index=%" PRIu32
        " kind=transfer"
        " timeout_ns=%" PRIu64
        " result=%d\n",
        device_index, timeout_ns, VK_NOT_READY
      );

      return VK_NOT_READY;
    }

    if (wait_result != VK_SUCCESS) return wait_result;
  }

  if (session->compute_command_leased == VK_TRUE
   || session->descriptor_leased == VK_TRUE
   || session->compute_command_failed == VK_TRUE
   || session->transfer_failed == VK_TRUE)
  {
    return VK_NOT_READY;
  }

  fprintf (
    session->diagnostics,
    "BACKEND_VULKAN_SHUTDOWN_COMPLETE"
    " index=%" PRIu32
    " compute_cancelled=%" PRIu32
    " transfer_cancelled=%" PRIu32 "\n",
    device_index,
    session->compute_managed_cancelled,
    session->cancelled_transfers
  );

  return backend_vulkan_session_close_if_ready (
    backend_ctx, device_index
  );
}

/**
 * Close one backend-owned Vulkan Compute queue session and clear its owner.
 *
 * Waits for GPU idle and releases cached descriptor pools/sets, pipelines
 * and layouts, retained storage, both command/fence pairs, command pool and
 * logical device before the backend-owned physical-device catalogue closes.
 * Reports the final descriptor rebinding, pipeline cache and command reuse
 * counts. Closing an empty session succeeds without emitting duplicate logs.
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
  const uint32_t storage_buffers = session->storage_buffer_count;
  const uint32_t storage_allocations = session->storage_allocations;
  const uint32_t storage_reuses = session->storage_reuses;
  const uint32_t completed_transfers = session->completed_transfers;
  const uint32_t cancelled_transfers = session->cancelled_transfers;
  const uint32_t transfer_recoveries = session->transfer_recoveries;
  const uint32_t transfer_abandoned = session->transfer_abandoned;
  const uint32_t cached_pipelines = session->cached_pipeline_count;
  const uint32_t pipeline_hits = session->cached_pipeline_hits;
  const uint32_t descriptor_pools = session->descriptor_pool_count;
  const uint32_t descriptor_reuses = session->descriptor_pool_reuses;
  const uint32_t descriptor_acquisitions =
    session->descriptor_acquisitions;
  const uint32_t descriptor_writes = session->descriptor_writes;
  const uint32_t command_acquisitions =
    session->compute_command_acquisitions;
  const uint32_t command_reuses = session->compute_command_reuses;
  const uint32_t command_recoveries =
    session->compute_command_recoveries;
  const uint32_t managed_submissions =
    session->compute_managed_submissions;
  const uint32_t managed_completed =
    session->compute_managed_completed;
  const uint32_t managed_cancelled =
    session->compute_managed_cancelled;
  FILE *diagnostics = session->diagnostics;

  const VkResult result = backend_vulkan_session_release (session);

  backend_ctx->vulkan_session = NULL;

  free (session);

  fprintf (
    diagnostics,
    "BACKEND_VULKAN_DESCRIPTORS_CLOSE"
    " index=%" PRIu32
    " pools=%" PRIu32
    " pool_reuses=%" PRIu32
    " acquisitions=%" PRIu32
    " writes=%" PRIu32
    " released=1 wait_result=%d\n",
    device_index, descriptor_pools, descriptor_reuses,
    descriptor_acquisitions, descriptor_writes, result
  );

  fprintf (
    diagnostics,
    "BACKEND_VULKAN_RESOURCES_CLOSE"
    " index=%" PRIu32
    " pipelines=%" PRIu32
    " pipeline_hits=%" PRIu32
    " command_acquisitions=%" PRIu32
    " command_reuses=%" PRIu32
    " released=1 wait_result=%d\n",
    device_index, cached_pipelines, pipeline_hits,
    command_acquisitions, command_reuses, result
  );

  fprintf (
    diagnostics,
    "BACKEND_VULKAN_STAGING_CANCEL_CLOSE"
    " index=%" PRIu32
    " cancelled=%" PRIu32
    " released=1 wait_result=%d\n",
    device_index, cancelled_transfers, result
  );

  fprintf (
    diagnostics,
    "BACKEND_VULKAN_STAGING_RECOVERY_CLOSE"
    " index=%" PRIu32
    " recoveries=%" PRIu32
    " abandoned=%" PRIu32
    " released=1 wait_result=%d\n",
    device_index, transfer_recoveries, transfer_abandoned, result
  );

  fprintf (
    diagnostics,
    "BACKEND_VULKAN_MANAGED_COMMAND_CLOSE"
    " index=%" PRIu32
    " submitted=%" PRIu32
    " completed=%" PRIu32
    " cancelled=%" PRIu32
    " released=1 wait_result=%d\n",
    device_index, managed_submissions, managed_completed,
    managed_cancelled, result
  );

  fprintf (
    diagnostics,
    "BACKEND_VULKAN_COMMAND_RECOVERY_CLOSE"
    " index=%" PRIu32
    " recoveries=%" PRIu32
    " released=1 wait_result=%d\n",
    device_index, command_recoveries, result
  );

  fprintf (
    diagnostics,
    "BACKEND_VULKAN_SESSION_CLOSE index=%" PRIu32
    " queue_family=%" PRIu32
    " empty_submissions=%" PRIu32
    " storage_buffers=%" PRIu32
    " storage_allocations=%" PRIu32
    " storage_reuses=%" PRIu32
    " completed_transfers=%" PRIu32
    " wait_result=%d\n",
    device_index, queue_family, submissions,
    storage_buffers, storage_allocations, storage_reuses,
    completed_transfers, result
  );

  return result;
}
