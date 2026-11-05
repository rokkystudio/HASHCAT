/**
 * Author......: See docs/credits.txt
 * License.....: MIT
 */

#ifndef HC_BACKEND_VULKAN_DEVICE_H
#define HC_BACKEND_VULKAN_DEVICE_H

#include "types.h"

#if defined (WITH_VULKAN_DISCOVERY)

#include <vulkan/vulkan.h>

#include <stdint.h>

/**
 * Complete Vulkan state of one registered hashcat device.
 *
 * Only this layer owns the structure. types.h keeps a bare pointer to it so
 * the generic device table stays free of Vulkan headers and layouts.
 *
 * M1 stored the Vulkan device type, vendor id, vendor name and API version in
 * the reused OpenCL metadata slots of hc_device_param_t. Since M2 all of that
 * lives here; the OpenCL slots stay NULL/zero for a Vulkan device and every
 * reader goes through device_param->vulkan after checking is_vulkan.
 *
 * The runtime handles are created lazily by backend_vulkan_devices_memory_init
 * and released by backend_vulkan_devices_term. A structure with device equal
 * to VK_NULL_HANDLE is metadata-only, which is what the registration pass
 * produces before the diagnostic chain runs.
 */

#define HC_VULKAN_MAX_BUFFERS        40
#define HC_VULKAN_MAX_PIPELINES       4
#define HC_VULKAN_NAME_MAX          256
#define HC_VULKAN_VENDOR_MAX         64
#define HC_VULKAN_BUFFER_NAME_MAX    48
#define HC_VULKAN_PUSH_CONSTANT_MAX 128

/**
 * Fixed device-memory slots of the hashcat buffer set.
 *
 * The order is part of the layout: the descriptor bindings in
 * backend_vulkan_bindings.h address buffers by these indices, and
 * backend_vulkan_devices_memory_init fills them in this exact order.
 */

typedef enum hc_vulkan_buffer_index
{
  HC_VK_BUFFER_PWS = 0,
  HC_VK_BUFFER_PWS_AMP,
  HC_VK_BUFFER_PWS_COMP,
  HC_VK_BUFFER_PWS_IDX,
  HC_VK_BUFFER_RULES,
  HC_VK_BUFFER_RULES_C,
  HC_VK_BUFFER_COMBS,
  HC_VK_BUFFER_COMBS_C,
  HC_VK_BUFFER_BFS,
  HC_VK_BUFFER_BFS_C,
  HC_VK_BUFFER_TM_C,
  HC_VK_BUFFER_BITMAP_S1_A,
  HC_VK_BUFFER_BITMAP_S1_B,
  HC_VK_BUFFER_BITMAP_S1_C,
  HC_VK_BUFFER_BITMAP_S1_D,
  HC_VK_BUFFER_BITMAP_S2_A,
  HC_VK_BUFFER_BITMAP_S2_B,
  HC_VK_BUFFER_BITMAP_S2_C,
  HC_VK_BUFFER_BITMAP_S2_D,
  HC_VK_BUFFER_PLAIN,
  HC_VK_BUFFER_DIGESTS,
  HC_VK_BUFFER_DIGESTS_SHOWN,
  HC_VK_BUFFER_SALTS,
  HC_VK_BUFFER_ESALTS,
  HC_VK_BUFFER_TMPS,
  HC_VK_BUFFER_HOOKS,
  HC_VK_BUFFER_RESULT,
  HC_VK_BUFFER_EXTRA0,
  HC_VK_BUFFER_EXTRA1,
  HC_VK_BUFFER_EXTRA2,
  HC_VK_BUFFER_EXTRA3,
  HC_VK_BUFFER_ROOT_CSS,
  HC_VK_BUFFER_MARKOV_CSS,
  HC_VK_BUFFER_ST_DIGESTS,
  HC_VK_BUFFER_ST_SALTS,
  HC_VK_BUFFER_ST_ESALTS,
  HC_VK_BUFFER_KERNEL_PARAM,
  HC_VK_BUFFER_COUNT
} hc_vulkan_buffer_index_t;

/**
 * One Vulkan storage allocation and its host access path.
 *
 * In direct mode memory is host-visible, mapped and host_buffer equals buffer.
 * In staging mode memory is not host-visible: buffer stays device-local and
 * host_buffer/host_memory/host_mapped identify the CPU-visible copy that is
 * transferred explicitly around each GPU submission.
 */

typedef struct hc_vulkan_buffer
{
  char           name[HC_VULKAN_BUFFER_NAME_MAX];

  VkBuffer       buffer;
  VkDeviceMemory memory;

  VkBuffer       host_buffer;
  VkDeviceMemory host_memory;
  void          *host_mapped;

  VkDeviceSize   requested_size;
  VkDeviceSize   size;

  VkBool32       host_visible;
  VkBool32       coherent;
  VkBool32       staging;
} hc_vulkan_buffer_t;

struct vk_program;

typedef struct hc_vulkan_pipeline
{
  const struct vk_program *program;

  VkShaderModule shader_module;
  VkPipeline     pipeline;
} hc_vulkan_pipeline_t;

typedef struct hc_vulkan_device
{
  u32 index;

  VkPhysicalDevice                 physical;
  VkPhysicalDeviceProperties       properties;
  VkPhysicalDeviceMemoryProperties memory_properties;

  u32  vendor_id;
  u32  device_type;
  u32  api_version;
  char name[HC_VULKAN_NAME_MAX];
  char vendor[HC_VULKAN_VENDOR_MAX];
  char api_version_string[32];

  u32             queue_family;
  VkDevice        device;
  VkQueue         queue;
  VkCommandPool   command_pool;
  VkCommandBuffer command_buffer;
  VkFence         fence;

  VkDescriptorPool      descriptor_pool;
  VkDescriptorSet       descriptor_set;
  VkDescriptorSetLayout descriptor_set_layout;
  VkPipelineLayout      pipeline_layout;
  u32                   descriptor_binding_count;

  hc_vulkan_buffer_t buffers[HC_VULKAN_MAX_BUFFERS];
  u32                buffer_count;

  hc_vulkan_pipeline_t pipelines[HC_VULKAN_MAX_PIPELINES];
  u32                  pipeline_count;

  u32 push_constant_bytes;
  u8  push_constants[HC_VULKAN_PUSH_CONSTANT_MAX];

  u64 dispatch_groups_total;
  u64 dispatch_groups_largest;
  u32 dispatch_splits;

  bool device_ready;
  bool memory_ready;

  /**
   * Set only after the fixed-vector mode 0 self-check passed on this physical
   * device in this process. It is the schedulability gate for
   * --backend-info-json and for accepting work.
   */

  bool schedulable;
} hc_vulkan_device_t;

/**
 * Register the physical devices of the already opened Vulkan catalogue as
 * ordinary device table entries.
 *
 * The entries are appended after every schedulable backend so that existing
 * -d identifiers stay stable. They are visible in -I and --backend-info-json
 * and can be selected with -d, but they are not schedulable until Vulkan
 * hashing is implemented.
 */

void backend_vulkan_devices_init (hashcat_ctx_t *hashcat_ctx);

/**
 * Decide whether registered Vulkan devices may receive work in this run.
 *
 * other_active is the number of non-Vulkan devices that are already
 * schedulable. A Vulkan device is activated when it was selected explicitly
 * with -d/--backend-device or when no other device is left (the documented
 * --backend-ignore-opencl path). It becomes schedulable only after the fixed
 * MD5 straight self-check passed; a failed check leaves it skipped.
 *
 * need_info forces the self-check even when the device will not run work, so
 * --backend-info-json can report a truthful schedulable flag.
 */

void backend_vulkan_devices_activate (hashcat_ctx_t *hashcat_ctx, const int other_active, const int need_info);

/**
 * Open the runtime with the real hashconfig buffer sizes and upload the
 * immutable per-run data (digests, salts, esalts) into the Vulkan buffers.
 * Called from backend_session_begin() for an active Vulkan device.
 */

int backend_vulkan_devices_session_begin (hashcat_ctx_t *hashcat_ctx, hc_device_param_t *device_param);

/**
 * Refuse a workload the registered program cannot execute.
 *
 * Supported: -m 0 -a 0, no rules, no -O, no --increment, no brain, no -S,
 * no benchmark. Everything else returns -1 with an explanatory message.
 */

int backend_vulkan_validate_workload (hashcat_ctx_t *hashcat_ctx, hc_device_param_t *device_param);

/**
 * Release every Vulkan resource owned by the registered devices.
 *
 * Destroys buffers, pipelines, descriptor objects, command pool, fence and
 * VkDevice, then releases the hc_vulkan_device allocation. The catalogue
 * itself is owned by backend_vulkan_catalog_close and is left untouched.
 * Calling this function without registered devices, or twice, is harmless.
 */

void backend_vulkan_devices_term (hashcat_ctx_t *hashcat_ctx);

/**
 * Report the 1-based device id when the user explicitly selected a Vulkan
 * device through -d / --backend-device, otherwise zero.
 */

u32 backend_vulkan_device_selected (hashcat_ctx_t *hashcat_ctx);

/**
 * Open the logical device and allocate the complete hashcat buffer set.
 *
 * Requires a registered Vulkan device_param. When hashcat already computed
 * the per-device sizes for an active hashconfig they are reused unchanged;
 * otherwise the fixed diagnostic layout is used. Each buffer prefers a
 * host-visible coherent memory type, falls back to host-visible
 * non-coherent with explicit cache maintenance, and uses a device-local
 * allocation plus a host-visible staging buffer only when the driver exposes
 * no host-visible type. A buffer larger than maxStorageBufferRange is
 * refused before any Vulkan allocation. On failure every partially created
 * object is released.
 */

int backend_vulkan_devices_memory_init (hashcat_ctx_t *hashcat_ctx, hc_device_param_t *device_param);

/**
 * Release the logical device and every buffer but keep the metadata.
 *
 * Safe on a device that was never opened and on a partially initialized
 * runtime. The hc_vulkan_device allocation itself stays owned by
 * backend_vulkan_devices_term.
 */

int backend_vulkan_devices_memory_term (hashcat_ctx_t *hashcat_ctx, hc_device_param_t *device_param);

/**
 * Validate the memory path without a shader.
 *
 * Uploads a deterministic 4 KiB pattern into pws_buf through the normal
 * transfer entry point and compares the download byte-for-byte. Returns zero
 * only on exact equality.
 */

int backend_vulkan_device_memory_selftest (hashcat_ctx_t *hashcat_ctx, hc_device_param_t *device_param);

/**
 * Report the current process RSS in KiB from /proc/self/status, or zero when
 * the file is unavailable. Used by the repeated init/term diagnostic.
 */

u64 backend_vulkan_process_rss_kib (void);

/**
 * Upload bytes into one fixed buffer slot.
 *
 * The whole range must fit the requested slot size. Non-coherent mappings are
 * flushed before the GPU sees the data; staged slots are copied through their
 * host-visible allocation and the copy is fence-completed before return.
 */

int backend_vulkan_device_upload (hashcat_ctx_t *hashcat_ctx, hc_device_param_t *device_param, const u32 buffer_idx, const void *source, const VkDeviceSize bytes);

/**
 * Download bytes from one fixed buffer slot.
 *
 * Staged slots are copied back first; non-coherent mappings are invalidated
 * before the host reads them. The caller must ensure the producing GPU
 * submission already completed.
 */

int backend_vulkan_device_download (hashcat_ctx_t *hashcat_ctx, hc_device_param_t *device_param, const u32 buffer_idx, void *destination, const VkDeviceSize bytes);

/**
 * Fill one fixed buffer slot with a byte value.
 */

int backend_vulkan_device_memset (hashcat_ctx_t *hashcat_ctx, hc_device_param_t *device_param, const u32 buffer_idx, const int value, const VkDeviceSize bytes);

/**
 * Copy one fixed buffer slot into another through the host-visible path.
 */

int backend_vulkan_device_copy (hashcat_ctx_t *hashcat_ctx, hc_device_param_t *device_param, const u32 source_idx, const u32 destination_idx, const VkDeviceSize bytes);

/**
 * Look up one immutable program in the host registry by its stable name.
 * Returns NULL for an unknown or NULL name.
 */

const struct vk_program *backend_vulkan_program_by_name (const char *name);

/**
 * Enumerate the immutable program registry.
 *
 * program_count() is the number of registered programs; program_at() returns
 * NULL outside the valid range. The mode 0 straight entry is exposed by its
 * stable name so the scheduler does not depend on registry order.
 */

u32 backend_vulkan_program_count (void);

const struct vk_program *backend_vulkan_program_at (const u32 index);

const struct vk_program *backend_vulkan_program_md5_straight (void);

/**
 * Validate a registry program against the v1 ABI and the physical device,
 * then create and cache its Compute pipeline.
 *
 * Rejects a bindings-version mismatch, a binding table that differs from
 * v1, malformed SPIR-V, a local size above the device limits and a
 * push-constant block above 128 bytes before any pipeline object exists.
 * Repeated calls for the same program reuse the cached pipeline.
 */

int backend_vulkan_device_program_prepare (hashcat_ctx_t *hashcat_ctx, hc_device_param_t *device_param, const struct vk_program *program);

/**
 * Dispatch one program over group_count workgroups.
 *
 * group_count is the total workgroup count (kernel_power divided by the
 * registry local size). Counts above maxComputeWorkGroupCount[0] are split
 * into several vkCmdDispatch calls inside one command buffer with a
 * Compute-to-Compute barrier between chunks. One fence covers the whole
 * submission and is waited on with a fixed timeout. Optional push constants
 * are taken from the caller and must not exceed the program block.
 */

int backend_vulkan_device_dispatch (hashcat_ctx_t *hashcat_ctx, hc_device_param_t *device_param, const struct vk_program *program, const u64 group_count, const void *push_constants, const u32 push_constant_bytes);

/**
 * Download the complete result buffer after a dispatch.
 */

int backend_vulkan_device_readback_result (hashcat_ctx_t *hashcat_ctx, hc_device_param_t *device_param, void *destination, const VkDeviceSize bytes);

/**
 * Run the M2 data-path check and compare every workgroup with a CPU
 * reference.
 *
 * Fills pws_buf through the normal upload path, dispatches the registered
 * check program with more than 65535 workgroups (diagnostic layout), reads
 * the result buffer back and verifies per-group sums and processed-element
 * counts. Returns zero only when the GPU output matches the reference
 * exactly and every input element was processed once.
 */

int backend_vulkan_device_run_check (hashcat_ctx_t *hashcat_ctx, hc_device_param_t *device_param);

/**
 * Run the fixed-vector mode 0 / -a 0 check through the registered program.
 *
 * Uploads three known plaintexts and two known MD5 digests, dispatches the
 * real straight kernel, and compares the plain_t records against the CPU
 * reference. Requires an open runtime; used as the schedulability gate.
 */

int backend_vulkan_device_md5_straight_check (hashcat_ctx_t *hashcat_ctx, hc_device_param_t *device_param);

/**
 * Execute one mode 0 / -a 0 candidate batch on the Vulkan device.
 *
 * Candidates are expanded from the host pws_comp/pws_idx arrays into pw_t
 * records and uploaded; the kernel salts, digests and offsets come from
 * device_param->kernel_param. Candidates above the one-block limit are
 * refused. The result buffer is pre-filled before the dispatch; the caller
 * reads it through check_cracked().
 */

int backend_vulkan_device_run_md5_straight (hashcat_ctx_t *hashcat_ctx, hc_device_param_t *device_param, const u64 pws_pos, const u64 pws_cnt);

#endif // WITH_VULKAN_DISCOVERY

#endif // HC_BACKEND_VULKAN_DEVICE_H
