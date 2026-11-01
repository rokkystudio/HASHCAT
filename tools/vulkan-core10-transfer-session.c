/**
 * Own standalone Vulkan 1.0 mapped buffers and a real transfer queue.
 *
 * A caller-supplied physical device and Compute queue determine all Vulkan
 * memory compatibility. Two mapped storage/transfer buffers are connected
 * only through vkCmdCopyBuffer. No compute shader, descriptor set, hashcat
 * kernel, password input or algorithm is loaded or executed.
 */

#include "vulkan-core10-transfer-session.h"

#include <inttypes.h>
#include <stdint.h>
#include <string.h>

/**
 * Select memory flags according to one explicitly requested mode.
 *
 * PREFER_COHERENT uses the shared Vulkan memory type selector. The
 * REQUIRE_NONCOHERENT mode accepts only exact driver-compatible
 * HOST_VISIBLE types with HOST_COHERENT unset. Both paths return a
 * Vulkan feature error when the required memory does not exist.
 */
static VkResult vulkan_core10_transfer_select_memory (
  const VkPhysicalDeviceMemoryProperties *properties,
  uint32_t compatible_bits,
  vulkan_core10_transfer_memory_mode_t memory_mode,
  uint32_t *index,
  VkBool32 *coherent)
{
  if (properties == NULL || index == NULL || coherent == NULL
   || properties->memoryTypeCount > 32u)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  if (memory_mode == VULKAN_CORE10_TRANSFER_PREFER_COHERENT)
  {
    return vulkan_auto_select_memory_type (
      properties, compatible_bits, index, coherent
    );
  }

  if (memory_mode != VULKAN_CORE10_TRANSFER_REQUIRE_NONCOHERENT)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  for (uint32_t candidate = 0u;
       candidate < properties->memoryTypeCount; candidate++)
  {
    const uint32_t bit = UINT32_C (1) << candidate;
    const VkMemoryPropertyFlags flags =
      properties->memoryTypes[candidate].propertyFlags;

    if ((compatible_bits & bit) != 0u
     && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0u
     && (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0u)
    {
      *index = candidate;
      *coherent = VK_FALSE;

      return VK_SUCCESS;
    }
  }

  return VK_ERROR_FEATURE_NOT_PRESENT;
}

/**
 * Release every partially or fully acquired Vulkan resource in order.
 *
 * Waits once for VkDevice idle, releases mappings before their memory,
 * destroys buffers before memory allocations, and destroys VkDevice last.
 * A caller-owned VkInstance is never affected. Zeroes the entire session
 * only after resource release, including the cached diagnostics pointer.
 */
VkResult vulkan_core10_transfer_session_close (
  vulkan_core10_transfer_session_t *session)
{
  if (session == NULL) return VK_ERROR_INITIALIZATION_FAILED;

  if (session->device == VK_NULL_HANDLE)
  {
    memset (session, 0, sizeof (*session));

    return VK_SUCCESS;
  }

  const VkResult wait_result = vkDeviceWaitIdle (session->device);

  if (session->diagnostics != NULL)
  {
    fprintf (
      session->diagnostics,
      "TRANSFER_CLOSE index=%" PRIu32
      " bytes=%" PRIu64
      " copies=%" PRIu32
      " copied_bytes=%" PRIu64
      " flushes=%" PRIu32
      " invalidates=%" PRIu32
      " wait_result=%d\n",
      session->device_index,
      (uint64_t) session->buffer_bytes,
      session->completed_copies,
      session->copied_bytes,
      session->flushes,
      session->invalidates,
      wait_result
    );
  }

  for (uint32_t i = 0u; i < 2u; i++)
  {
    vulkan_core10_transfer_buffer_t *buffer = &session->buffers[i];

    if (buffer->mapped != NULL)
    {
      vkUnmapMemory (session->device, buffer->memory);

      buffer->mapped = NULL;
    }

    if (buffer->buffer != VK_NULL_HANDLE)
    {
      vkDestroyBuffer (session->device, buffer->buffer, NULL);

      buffer->buffer = VK_NULL_HANDLE;
    }

    if (buffer->memory != VK_NULL_HANDLE)
    {
      vkFreeMemory (session->device, buffer->memory, NULL);

      buffer->memory = VK_NULL_HANDLE;
    }
  }

  if (session->fence != VK_NULL_HANDLE)
  {
    vkDestroyFence (session->device, session->fence, NULL);

    session->fence = VK_NULL_HANDLE;
  }

  if (session->command != VK_NULL_HANDLE)
  {
    vkFreeCommandBuffers (
      session->device, session->command_pool, 1u, &session->command
    );

    session->command = VK_NULL_HANDLE;
  }

  if (session->command_pool != VK_NULL_HANDLE)
  {
    vkDestroyCommandPool (
      session->device, session->command_pool, NULL
    );

    session->command_pool = VK_NULL_HANDLE;
  }

  vkDestroyDevice (session->device, NULL);

  memset (session, 0, sizeof (*session));

  return wait_result;
}

/**
 * Acquire a Vulkan 1.0 queue and two exactly mapped transfer buffers.
 *
 * Input validation happens before driver allocation. The physical device
 * must have already passed the caller's Vulkan API and Compute capability
 * checks. Each independent VkBuffer uses its own actual memoryTypeBits
 * and VkMemoryRequirements.size, with no vendor-specific assumptions.
 * A failed acquisition closes all previous resources.
 */
VkResult vulkan_core10_transfer_session_open (
  vulkan_core10_transfer_session_t *session,
  const vulkan_auto_device_t *selected,
  VkDeviceSize buffer_bytes,
  vulkan_core10_transfer_memory_mode_t memory_mode,
  FILE *diagnostics)
{
  if (session == NULL || selected == NULL || diagnostics == NULL
   || selected->physical == VK_NULL_HANDLE
   || session->device != VK_NULL_HANDLE
   || session->opened != VK_FALSE
   || session->buffers[0].buffer != VK_NULL_HANDLE
   || session->buffers[1].buffer != VK_NULL_HANDLE
   || buffer_bytes == 0u || (buffer_bytes % 4u) != 0u
   || buffer_bytes > SIZE_MAX
   || buffer_bytes > selected->properties.limits.maxStorageBufferRange
   || selected->memory_properties.memoryTypeCount == 0u
   || selected->memory_properties.memoryTypeCount > 32u
   || (memory_mode != VULKAN_CORE10_TRANSFER_PREFER_COHERENT
    && memory_mode != VULKAN_CORE10_TRANSFER_REQUIRE_NONCOHERENT))
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  session->device_index = selected->index;
  session->queue_family = selected->compute_queue_family;
  session->memory_mode = (uint32_t) memory_mode;
  session->diagnostics = diagnostics;

  VkResult result = VK_SUCCESS;

  const float priority = 1.0f;

  const VkDeviceQueueCreateInfo queue_info =
  {
    .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
    .queueFamilyIndex = selected->compute_queue_family,
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
    selected->physical, &device_info, NULL, &session->device
  );

  if (result != VK_SUCCESS) goto failed;

  vkGetDeviceQueue (
    session->device, selected->compute_queue_family, 0u,
    &session->queue
  );

  if (session->queue == VK_NULL_HANDLE)
  {
    result = VK_ERROR_INITIALIZATION_FAILED;

    goto failed;
  }

  const VkCommandPoolCreateInfo pool_info =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
    .queueFamilyIndex = selected->compute_queue_family,
    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT
  };

  result = vkCreateCommandPool (
    session->device, &pool_info, NULL, &session->command_pool
  );

  if (result != VK_SUCCESS) goto failed;

  const VkCommandBufferAllocateInfo command_info =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
    .commandPool = session->command_pool,
    .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
    .commandBufferCount = 1u
  };

  result = vkAllocateCommandBuffers (
    session->device, &command_info, &session->command
  );

  if (result != VK_SUCCESS) goto failed;

  const VkFenceCreateInfo fence_info =
  {
    .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO
  };

  result = vkCreateFence (
    session->device, &fence_info, NULL, &session->fence
  );

  if (result != VK_SUCCESS) goto failed;

  for (uint32_t i = 0u; i < 2u; i++)
  {
    vulkan_core10_transfer_buffer_t *buffer = &session->buffers[i];

    const VkBufferCreateInfo buffer_info =
    {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = buffer_bytes,
      .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
             | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
             | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };

    result = vkCreateBuffer (
      session->device, &buffer_info, NULL, &buffer->buffer
    );

    if (result != VK_SUCCESS) goto failed;

    VkMemoryRequirements requirements = { 0 };

    vkGetBufferMemoryRequirements (
      session->device, buffer->buffer, &requirements
    );

    uint32_t memory_type = UINT32_MAX;
    VkBool32 coherent = VK_FALSE;

    result = vulkan_core10_transfer_select_memory (
      &selected->memory_properties,
      requirements.memoryTypeBits, memory_mode,
      &memory_type, &coherent
    );

    if (result != VK_SUCCESS)
    {
      fprintf (
        diagnostics,
        "TRANSFER_MEMORY_UNAVAILABLE index=%" PRIu32
        " buffer=%" PRIu32 " compatible=0x%08" PRIx32
        " mode=%" PRIu32 " result=%d\n",
        selected->index, i,
        requirements.memoryTypeBits, (uint32_t) memory_mode,
        result
      );

      goto failed;
    }

    const VkMemoryAllocateInfo allocation_info =
    {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = requirements.size,
      .memoryTypeIndex = memory_type
    };

    result = vkAllocateMemory (
      session->device, &allocation_info, NULL, &buffer->memory
    );

    if (result != VK_SUCCESS) goto failed;

    result = vkBindBufferMemory (
      session->device, buffer->buffer, buffer->memory, 0u
    );

    if (result != VK_SUCCESS) goto failed;

    result = vkMapMemory (
      session->device, buffer->memory, 0u, VK_WHOLE_SIZE,
      0u, &buffer->mapped
    );

    if (result != VK_SUCCESS) goto failed;

    buffer->plan = (vulkan_auto_memory_plan_t)
    {
      .allocation_bytes = requirements.size,
      .memory_type_index = memory_type,
      .compatible_memory_type_bits = requirements.memoryTypeBits,
      .coherent = coherent,
      .non_coherent_atom_size =
        selected->properties.limits.nonCoherentAtomSize
    };
  }

  session->buffer_bytes = buffer_bytes;
  session->opened = VK_TRUE;

  fprintf (
    diagnostics,
    "TRANSFER_OPEN index=%" PRIu32
    " queue_family=%" PRIu32
    " bytes=%" PRIu64
    " mode=%" PRIu32
    " src_type=%" PRIu32
    " dst_type=%" PRIu32
    " coherent_src=%u"
    " coherent_dst=%u"
    " atom=%" PRIu64
    " shaders=0 features=0 extensions=0\n",
    session->device_index,
    session->queue_family,
    (uint64_t) session->buffer_bytes,
    session->memory_mode,
    session->buffers[0].plan.memory_type_index,
    session->buffers[1].plan.memory_type_index,
    session->buffers[0].plan.coherent,
    session->buffers[1].plan.coherent,
    (uint64_t) session->buffers[0].plan.non_coherent_atom_size
  );

  return VK_SUCCESS;

failed:
  fprintf (
    diagnostics,
    "TRANSFER_OPEN_FAILED index=%" PRIu32
    " mode=%" PRIu32
    " result=%d\n",
    selected->index, (uint32_t) memory_mode, result
  );

  vulkan_core10_transfer_session_close (session);

  return result;
}

/**
 * Transfer public data from host to host through Vulkan buffer memory.
 *
 * A successful transfer performs an actual vkQueueSubmit, completes a
 * fence, then makes the destination host-visible before copying to the
 * caller. The command's TRANSFER_WRITE to HOST_READ dependency pairs
 * with full-allocation cache invalidation on noncoherent memory.
 */
VkResult vulkan_core10_transfer_session_copy (
  vulkan_core10_transfer_session_t *session,
  const void *input,
  void *output,
  VkDeviceSize bytes)
{
  if (session == NULL || session->opened != VK_TRUE
   || input == NULL || output == NULL
   || bytes == 0u || (bytes % 4u) != 0u
   || bytes > SIZE_MAX || bytes > session->buffer_bytes)
  {
    return VK_ERROR_INITIALIZATION_FAILED;
  }

  memcpy (
    session->buffers[0].mapped, input, (size_t) bytes
  );

  VkResult result = vulkan_auto_sync_mapped_memory (
    session->device, session->buffers[0].memory,
    &session->buffers[0].plan, VK_FALSE
  );

  if (result != VK_SUCCESS) return result;

  if (session->buffers[0].plan.coherent == VK_FALSE)
  {
    session->flushes++;
  }

  result = vkResetCommandBuffer (session->command, 0u);

  if (result != VK_SUCCESS) return result;

  const VkCommandBufferBeginInfo begin_info =
  {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
  };

  result = vkBeginCommandBuffer (session->command, &begin_info);

  if (result != VK_SUCCESS) return result;

  const VkBufferCopy region =
  {
    .srcOffset = 0u,
    .dstOffset = 0u,
    .size = bytes
  };

  vkCmdCopyBuffer (
    session->command, session->buffers[0].buffer,
    session->buffers[1].buffer, 1u, &region
  );

  const VkBufferMemoryBarrier host_barrier =
  {
    .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
    .buffer = session->buffers[1].buffer,
    .offset = 0u,
    .size = bytes
  };

  vkCmdPipelineBarrier (
    session->command,
    VK_PIPELINE_STAGE_TRANSFER_BIT,
    VK_PIPELINE_STAGE_HOST_BIT,
    0u,
    0u, NULL,
    1u, &host_barrier,
    0u, NULL
  );

  result = vkEndCommandBuffer (session->command);

  if (result != VK_SUCCESS) return result;

  result = vkResetFences (
    session->device, 1u, &session->fence
  );

  if (result != VK_SUCCESS) return result;

  const VkSubmitInfo submit_info =
  {
    .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
    .commandBufferCount = 1u,
    .pCommandBuffers = &session->command
  };

  result = vkQueueSubmit (
    session->queue, 1u, &submit_info, session->fence
  );

  if (result != VK_SUCCESS) return result;

  result = vkWaitForFences (
    session->device, 1u, &session->fence,
    VK_TRUE, UINT64_MAX
  );

  if (result != VK_SUCCESS) return result;

  result = vulkan_auto_sync_mapped_memory (
    session->device, session->buffers[1].memory,
    &session->buffers[1].plan, VK_TRUE
  );

  if (result != VK_SUCCESS) return result;

  if (session->buffers[1].plan.coherent == VK_FALSE)
  {
    session->invalidates++;
  }

  memcpy (
    output, session->buffers[1].mapped, (size_t) bytes
  );

  session->completed_copies++;
  session->copied_bytes += (uint64_t) bytes;

  fprintf (
    session->diagnostics,
    "TRANSFER_COPY index=%" PRIu32
    " bytes=%" PRIu64
    " copies=%" PRIu32
    " flushes=%" PRIu32
    " invalidates=%" PRIu32
    " fence=signaled shaders=0\n",
    session->device_index,
    (uint64_t) bytes,
    session->completed_copies,
    session->flushes,
    session->invalidates
  );

  return VK_SUCCESS;
}
