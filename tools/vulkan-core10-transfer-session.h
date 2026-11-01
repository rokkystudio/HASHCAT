#ifndef VULKAN_CORE10_TRANSFER_SESSION_H
#define VULKAN_CORE10_TRANSFER_SESSION_H

/**
 * Own standalone Vulkan 1.0 buffer memory and transfer-queue resources.
 *
 * This component is independent of the HASHCAT backend, algorithms and
 * scheduler. It uses only public host-provided byte arrays, real Vulkan
 * buffer-memory requirements and explicit fence synchronization. No
 * shader module, compute pipeline, descriptor set or hash processing
 * is created.
 */

#include "vulkan-auto-config.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/**
 * Declare how exact Vulkan memory property flags are selected.
 *
 * PREFER_COHERENT chooses compatible HOST_VISIBLE|HOST_COHERENT
 * memory when available and a compatible HOST_VISIBLE type otherwise.
 * REQUIRE_NONCOHERENT accepts only a compatible HOST_VISIBLE memory
 * type lacking HOST_COHERENT; absence returns FEATURE_NOT_PRESENT
 * without silently choosing another mode.
 */
typedef enum vulkan_core10_transfer_memory_mode
{
  VULKAN_CORE10_TRANSFER_PREFER_COHERENT = 0,
  VULKAN_CORE10_TRANSFER_REQUIRE_NONCOHERENT = 1
} vulkan_core10_transfer_memory_mode_t;

/**
 * Own one transfer-compatible Vulkan buffer and its mapped allocation.
 *
 * buffer and memory are destroyed by the parent transfer session. Each
 * memory allocation is mapped at offset zero for VK_WHOLE_SIZE.
 * The driver-provided allocation size is kept in plan to synchronize
 * noncoherent mappings over an entire valid mapped allocation.
 */
typedef struct vulkan_core10_transfer_buffer
{
  VkBuffer buffer;
  VkDeviceMemory memory;
  void *mapped;
  vulkan_auto_memory_plan_t plan;
} vulkan_core10_transfer_buffer_t;

/**
 * Own one Vulkan logical device, queue, command resources and two buffers.
 *
 * VkInstance and VkPhysicalDevice remain owned by the caller. Buffer
 * index zero is the transfer source and index one is the destination.
 * A successful transfer writes caller-owned output only after a
 * completed Vulkan fence and mapped-memory invalidation.
 */
typedef struct vulkan_core10_transfer_session
{
  VkDevice device;
  VkQueue queue;
  VkCommandPool command_pool;
  VkCommandBuffer command;
  VkFence fence;
  vulkan_core10_transfer_buffer_t buffers[2];
  VkDeviceSize buffer_bytes;
  uint32_t queue_family;
  uint32_t device_index;
  uint32_t memory_mode;
  uint32_t completed_copies;
  uint64_t copied_bytes;
  uint32_t flushes;
  uint32_t invalidates;
  VkBool32 opened;
  FILE *diagnostics;
} vulkan_core10_transfer_session_t;

/**
 * Open independent, host-mapped Vulkan 1.0 transfer/storage buffers.
 *
 * Requires an empty zero-initialized session, a capability-checked
 * physical device and Compute queue selection, a positive byte size
 * divisible by four and within maxStorageBufferRange, and one declared
 * memory mode. Creates VkDevice with no extensions/optional features,
 * one queue, a resettable primary command buffer and fence, then two
 * separately allocated and mapped STORAGE_BUFFER|TRANSFER buffers.
 *
 * Returns VK_SUCCESS only when every resource is owned. Any driver
 * failure releases all partial resources and leaves the session empty.
 * Memory type indices come from actual VkMemoryRequirements bitmasks;
 * no vendor/device-name selection is performed.
 */
VkResult vulkan_core10_transfer_session_open (
  vulkan_core10_transfer_session_t *session,
  const vulkan_auto_device_t *selected,
  VkDeviceSize buffer_bytes,
  vulkan_core10_transfer_memory_mode_t memory_mode,
  FILE *diagnostics);

/**
 * Copy an exact public byte range through the real Vulkan GPU queue.
 *
 * Requires an opened session, non-NULL host buffers and a positive
 * 4-byte-aligned range at most buffer_bytes and SIZE_MAX. Flushes
 * noncoherent source writes, records vkCmdCopyBuffer and a
 * transfer-to-host-read dependency, waits for a real Vulkan fence,
 * invalidates noncoherent destination writes and copies bytes into
 * caller-owned output. The output remains untouched on failures
 * before fence completion and memory visibility synchronization.
 *
 * Each successful copy advances completed_copies and copied_bytes.
 * No shader, descriptor, password candidate or cryptographic kernel
 * is executed by this method.
 */
VkResult vulkan_core10_transfer_session_copy (
  vulkan_core10_transfer_session_t *session,
  const void *input,
  void *output,
  VkDeviceSize bytes);

/**
 * Destroy all owned transfer resources while VkInstance is still live.
 *
 * Waits for VkDevice idle, unmaps memory, destroys both VkBuffers,
 * frees both VkDeviceMemory allocations, releases fence/command/pool
 * resources and finally destroys VkDevice. Clears the complete
 * session struct after cleanup; closing an empty session is valid.
 * The first vkDeviceWaitIdle error is propagated after resource
 * release.
 */
VkResult vulkan_core10_transfer_session_close (
  vulkan_core10_transfer_session_t *session);

#endif
