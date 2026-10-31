#ifndef VULKAN_CORE10_RUNTIME_H
#define VULKAN_CORE10_RUNTIME_H

/**
 * Own a portable six-buffer Vulkan 1.0 three-stage Compute execution context.
 *
 * This standalone infrastructure accepts public shader modules and exact
 * caller-specified resource requirements. It uses only Vulkan 1.0 commands.
 * The caller selects the physical GPU and checks the workload's limits.
 *
 * All Vulkan handles belong to one runtime object and are released on
 * creation failure or explicit destruction. No hashcat functionality,
 * cryptographic operation or GPU-vendor selection policy is present.
 */

#include "vulkan-auto-config.h"
#include "vulkan-multistage-memory-plan.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define VULKAN_CORE10_RUNTIME_BUFFERS 6u
#define VULKAN_CORE10_RUNTIME_GPU_BUFFERS 4u
#define VULKAN_CORE10_RUNTIME_STAGES 3u

/**
 * Declare the full size, access and memory requirements of one Vulkan buffer.
 *
 * A GPU storage buffer must occupy indices 0..3 and have STORAGE_BUFFER
 * usage. HOST_VISIBLE staging buffers occupy indices 4 and 5, require
 * explicit CPU mapping, and carry their specified transfer usage.
 * The caller declares all requirements; no buffer sizes are inferred.
 */
typedef struct vulkan_core10_runtime_buffer_spec
{
  const char *label;
  VkDeviceSize size;
  VkBufferUsageFlags usage;
  vulkan_heap_buffer_role_t role;
  VkMemoryPropertyFlags forbidden_flags;
  VkBool32 map;
} vulkan_core10_runtime_buffer_spec_t;

/**
 * Declare a SPIR-V 1.0 compute shader whose source words remain caller-owned.
 */
typedef struct vulkan_core10_runtime_shader
{
  const uint32_t *words;
  size_t bytes;
} vulkan_core10_runtime_shader_t;

/**
 * Hold every Vulkan handle and completed-submission state for one runtime.
 *
 * Device and command resources are available after runtime_create succeeds.
 * Shader and buffer specifications are borrowed only while creating the
 * resources. Mapped memory remains valid until runtime_destroy. The
 * application owns VkInstance.
 */
typedef struct vulkan_core10_runtime
{
  VkDevice device;
  VkQueue queue;
  VkBuffer buffers[VULKAN_CORE10_RUNTIME_BUFFERS];
  VkDeviceMemory memory[VULKAN_CORE10_RUNTIME_BUFFERS];
  void *mapped[VULKAN_CORE10_RUNTIME_BUFFERS];
  VkMemoryRequirements requirements[VULKAN_CORE10_RUNTIME_BUFFERS];
  vulkan_multistage_memory_plan_t memory_plan;
  VkDescriptorSetLayout descriptor_layout;
  VkPipelineLayout pipeline_layout;
  VkShaderModule shaders[VULKAN_CORE10_RUNTIME_STAGES];
  VkPipeline pipelines[VULKAN_CORE10_RUNTIME_STAGES];
  VkDescriptorPool descriptor_pool;
  VkDescriptorSet descriptor_set;
  VkCommandPool command_pool;
  VkCommandBuffer command;
  VkFence fence;
  VkPhysicalDeviceLimits limits;
  uint32_t queue_family;
  uint32_t acquired;
  uint32_t completed_submissions;
  VkBool32 commands_recording;
  VkBool32 commands_recorded;
  VkBool32 submission_pending;
  FILE *diagnostics;
} vulkan_core10_runtime_t;

/**
 * Create six Vulkan buffers, mapped staging, descriptors, pipelines and fence.
 *
 * Resource selection uses exact vkGetBufferMemoryRequirements output and a
 * joint heap planner. Compatible GPU memory prefers DEVICE_LOCAL and staging
 * requires HOST_VISIBLE. Buffer allocations are independent and bind at 0.
 * Descriptor binding N refers to GPU buffer N for N in [0,4).
 *
 * fault_after is zero for normal execution. A nonzero value deliberately
 * returns VK_ERROR_INITIALIZATION_FAILED immediately after acquiring the
 * corresponding one-based Vulkan resource, then exercises complete cleanup.
 * Fault injection is intended only for this standalone synthetic diagnostic.
 *
 * On failure, releases every acquired Vulkan object. If input validation
 * fails before resource acquisition, the device handle remains NULL and
 * runtime_destroy clears the owner. A success requires runtime_destroy.
 */
VkResult vulkan_core10_runtime_create (
  vulkan_core10_runtime_t *runtime,
  const vulkan_auto_device_t *selected,
  const vulkan_core10_runtime_buffer_spec_t
    buffers[VULKAN_CORE10_RUNTIME_BUFFERS],
  const vulkan_core10_runtime_shader_t
    shaders[VULKAN_CORE10_RUNTIME_STAGES],
  uint32_t fault_after,
  FILE *diagnostics);

/**
 * Begin recording the reusable primary Vulkan 1.0 command buffer.
 *
 * The buffer is recorded exactly once for this runtime. The caller writes
 * explicit transfer/Compute/barrier commands into runtime->command before
 * completing the recording with vulkan_core10_runtime_end_commands.
 */
VkResult vulkan_core10_runtime_begin_commands (
  vulkan_core10_runtime_t *runtime);

/**
 * Finalize a reusable command buffer before the first queue submission.
 *
 * The buffer uses no ONE_TIME_SUBMIT flag. The same completed command buffer
 * may be submitted again only after the previous fence signals completion.
 */
VkResult vulkan_core10_runtime_end_commands (
  vulkan_core10_runtime_t *runtime);

/**
 * Submit the recorded command buffer and wait for its Vulkan 1.0 fence.
 *
 * A previously signaled fence is reset before the next submission. The
 * caller must flush CPU writes before submitting, and invalidate noncoherent
 * GPU readback memory only after this method returns VK_SUCCESS.
 */
VkResult vulkan_core10_runtime_submit_and_wait (
  vulkan_core10_runtime_t *runtime);

/**
 * Synchronize one currently mapped staging buffer on a Vulkan 1.0 device.
 *
 * For noncoherent memory calls vkFlushMappedMemoryRanges on CPU-to-GPU
 * upload, or vkInvalidateMappedMemoryRanges after a successful GPU fence.
 * Uses the entire allocation with offset 0 and VK_WHOLE_SIZE. Coherent
 * memory returns VK_SUCCESS without a driver cache-management operation.
 */
VkResult vulkan_core10_runtime_sync_staging (
  vulkan_core10_runtime_t *runtime,
  uint32_t index,
  VkBool32 invalidate);

/**
 * Release every Vulkan resource owned by a completed or partial runtime.
 *
 * Waits for the device to become idle, unmaps CPU allocations, destroys
 * command, descriptor and pipeline objects, destroys buffers before freeing
 * their memory, then destroys the logical device. The runtime is reset to
 * zero and may be safely destroyed again. Returns the device-idle result.
 */
VkResult vulkan_core10_runtime_destroy (
  vulkan_core10_runtime_t *runtime);

#endif
