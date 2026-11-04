/**
 * Author......: See docs/credits.txt
 * License.....: MIT
 */

#ifndef HC_BACKEND_VULKAN_H
#define HC_BACKEND_VULKAN_H

/**
 * Manage opt-in Vulkan 1.0 discovery and an isolated Compute device session.
 *
 * backend_ctx_t owns the physical-device catalogue and at most one
 * logical-device session. The catalogue owns VkInstance and its indexed
 * physical devices; the session owns VkDevice, queue, command pool,
 * independent transfer and Compute command/fence pairs and reusable
 * CPU-accessible storage slots.
 * A slot maps Compute memory directly when compatible or pairs Compute memory
 * with a host-visible transfer allocation. A session validates its queue
 * using a single empty submission before storage is reserved. Staging
 * transfers retain exclusive ownership across timed or unlimited fence waits.
 * Compute programs borrow its logical device, queue and retained storage.
 * A session-backed
 * public Compute job also borrows cached pipelines, stage-count descriptor
 * sets and a reusable command buffer while retaining ownership of its
 * temporary host arrays. Every borrowed resource remains session-owned.
 *
 * Vulkan devices are never inserted into devices_param or scheduled for
 * password/hash computation. The session closes before the catalogue.
 */

#include "types.h"
#include "vulkan-core10-device-catalog.h"

#include <stdint.h>
#include <stdio.h>

/**
 * Open one Vulkan catalogue and record its enumerated device count.
 *
 * Requires a backend context without an active Vulkan catalogue and one
 * exact minimum core API: 1.0 or 1.1. On success backend_ctx owns the
 * allocated catalogue, publishes the physical-device count, and records
 * VK_SUCCESS in rc_vulkan_init.
 *
 * On failure the allocation is released, vulkan_devices_cnt stays zero
 * and rc_vulkan_init contains the exact VkResult. Other GPU backends and
 * backend_ctx->devices_param are not modified.
 */
VkResult backend_vulkan_catalog_open (
  backend_ctx_t *backend_ctx,
  uint32_t minimum_api,
  FILE *diagnostics);

/**
 * Validate one explicitly indexed Vulkan device for a public Compute task.
 *
 * Requires an opened backend-owned catalogue and exact caller workload
 * metadata. Returns a driver-validated physical device and queue-family
 * selection without opening VkDevice or adding a hashcat kernel device.
 * Vulkan failures are propagated directly to the caller.
 */
VkResult backend_vulkan_catalog_select (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  const vulkan_auto_compute_requirements_t *workload,
  vulkan_auto_device_t *selected,
  uint32_t *effective_api);

/**
 * Destroy the backend-owned Vulkan physical-device catalogue.
 *
 * Requires no active backend-owned Vulkan logical-device session.
 * Frees the catalogue after closing its VkInstance and owned handle
 * array, then clears its pointer, device count and result status.
 * Closing an empty catalogue succeeds. An active session returns
 * VK_ERROR_INITIALIZATION_FAILED and retains all Vulkan ownership.
 * The ordinary CUDA, HIP, Metal and OpenCL contexts remain untouched.
 */
VkResult backend_vulkan_catalog_close (
  backend_ctx_t *backend_ctx);

/**
 * Open one exact-index Vulkan 1.0 Compute device session in backend_ctx_t.
 *
 * Requires an active backend-owned catalogue, an explicit compatible
 * physical-device index and exact public workload requirements. Checks
 * the requested device against driver limits, creates VkDevice with one
 * Compute queue and no enabled optional features or extensions, creates
 * a transfer command pool, primary buffer and signaled fence, then submits
 * one empty command buffer and waits for its fence. The separate Compute
 * command/fence pair is allocated on first program acquisition.
 *
 * On success backend_ctx owns the complete session until close. On
 * failure every partially created Vulkan resource is released and
 * backend_ctx->vulkan_session remains NULL. No hashcat hashing device,
 * password data, shader, descriptor set or GPU kernel is involved.
 */
VkResult backend_vulkan_session_open (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  const vulkan_auto_compute_requirements_t *workload,
  FILE *diagnostics);

/**
 * Describe borrowed handles of an active Vulkan Compute session.
 *
 * The parent backend context retains ownership of VkDevice and its queue.
 * device_index and queue_family identify the exact physical device and
 * selected queue family; effective_api records the requested core API.
 * Borrowed handles remain valid until backend_vulkan_session_close.
 */
typedef struct backend_vulkan_device_view
{
  VkDevice device;
  VkQueue queue;
  uint32_t device_index;
  uint32_t queue_family;
  uint32_t effective_api;
} backend_vulkan_device_view_t;

/**
 * Borrow an exact indexed Vulkan session device for synchronous Compute.
 *
 * Requires an active session for device_index and a non-NULL output.
 * Returns the owned VkDevice, queue and API metadata without allocating
 * another logical device or transferring ownership. Invalid requests
 * return VK_ERROR_INITIALIZATION_FAILED and leave output unchanged.
 * The caller completes its GPU submissions before the session closes.
 */
VkResult backend_vulkan_session_device (
  const backend_ctx_t *backend_ctx,
  uint32_t device_index,
  backend_vulkan_device_view_t *view);

/**
 * Describe the last host-observed ownership and completion of one GPU session.
 *
 * All pending and cancellation flags describe only recorded host state.
 * They do not poll GPU fences, imply that queued work has finished or
 * authorize releasing memory. Managed Compute and staging submissions
 * retain pending=true until their respective completion methods succeed.
 * Completed/canceled counters are monotonically increasing per session.
 */
typedef struct backend_vulkan_session_status
{
  VkBool32 compute_leased;
  VkBool32 compute_pending;
  VkBool32 compute_cancel_requested;
  VkBool32 compute_failed;
  VkBool32 descriptors_leased;
  VkBool32 transfer_pending;
  VkBool32 transfer_cancel_requested;
  VkBool32 transfer_failed;
  uint32_t compute_submissions;
  uint32_t compute_completions;
  uint32_t compute_cancellations;
  uint32_t transfer_completions;
  uint32_t transfer_cancellations;
} backend_vulkan_session_status_t;

/**
 * Read exact host-side ownership and completion counters of one Vulkan session.
 *
 * Requires an active matching device index and a non-NULL output.
 * Copies current session flags and counters without waiting, submitting
 * commands or reading GPU memory. Invalid inputs leave output unchanged.
 * The caller serializes this read with all other session operations;
 * Vulkan fence completion must still be checked through the matching API.
 * No other GPU backend is accessed or selected.
 */
VkResult backend_vulkan_session_status (
  const backend_ctx_t *backend_ctx,
  uint32_t device_index,
  backend_vulkan_session_status_t *status);

/**
 * Describe one CPU-accessible storage slot borrowed from the active session.
 *
 * buffer and memory identify the Compute storage allocation. host_buffer and
 * host_memory identify the CPU-visible allocation; they equal the Compute
 * handles in direct mode and identify a separate transfer buffer in staging
 * mode. mapped always addresses host_memory. bytes is the requested logical
 * descriptor range and capacity_bytes is the retained slot capacity.
 * plan describes host-memory cache synchronization. The view remains valid
 * until a later reserve grows or replaces that slot, or the session closes.
 */
typedef struct backend_vulkan_storage_view
{
  VkBuffer buffer;
  VkDeviceMemory memory;
  VkBuffer host_buffer;
  VkDeviceMemory host_memory;
  void *mapped;
  VkDeviceSize bytes;
  VkDeviceSize capacity_bytes;
  VkBool32 staging;
  vulkan_auto_memory_plan_t plan;
} backend_vulkan_storage_view_t;

/**
 * Reserve reusable direct or staged storage buffers on the active session device.
 *
 * Requires the exact selected physical device and queue metadata used by the
 * session, positive per-buffer logical sizes and caller storage for one view
 * per buffer. Existing slots with sufficient capacity are retained. Direct
 * mode maps compatible storage memory. When storage memory is not host-visible,
 * or force_staging is true, the slot owns a separate host-visible transfer
 * buffer while the Compute buffer uses a compatible device memory type.
 *
 * Smaller slots are replaced only after their new Compute and optional staging
 * resources are complete. Allocation is rejected while descriptors or
 * Compute commands are leased, a transfer is pending, or either the
 * transfer path or Compute command pair is poisoned. A GPU submission
 * failure therefore blocks all buffer replacement until idle-verified
 * command recovery. Any allocation failure preserves already valid slots
 * and returns the driver VkResult. The caller does not destroy or unmap views.
 */
VkResult backend_vulkan_session_storage_reserve (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  const vulkan_auto_device_t *selected,
  uint32_t buffer_count,
  const VkDeviceSize *buffer_bytes,
  VkBool32 force_staging,
  backend_vulkan_storage_view_t *views,
  FILE *diagnostics);

/**
 * Transfer one exact CPU-visible staging range and wait for completion.
 *
 * Validates the active session, exact device and staging slot, copy-aligned
 * positive byte count and requested direction. Flushes noncoherent upload
 * memory, submits explicit Vulkan 1.0 buffer-copy and memory barriers,
 * waits for the transfer fence, and invalidates downloaded noncoherent
 * bytes before returning. A GPU submission or synchronization failure
 * leaves the transfer pair unavailable until explicit recovery.
 * The routine never destroys or replaces the caller's retained buffers.
 */
VkResult backend_vulkan_session_storage_transfer (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  uint32_t buffer_index,
  VkDeviceSize bytes,
  VkBool32 download);

/**
 * Submit one staging upload or download without waiting for its fence.
 *
 * Reserves the session's transfer command/fence pair exclusively and
 * returns after a real vkQueueSubmit succeeds. The caller must call
 * backend_vulkan_session_storage_transfer_complete or explicitly recover
 * the pending transfer before another transfer or Compute job can begin.
 * Invalid ranges do not submit GPU work and leave the session unchanged.
 */
VkResult backend_vulkan_session_storage_transfer_begin (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  uint32_t buffer_index,
  VkDeviceSize bytes,
  VkBool32 download);

/**
 * Request cancellation of one submitted staging transfer without GPU waiting.
 *
 * Validates an active indexed Vulkan session with one pending transfer,
 * no outstanding Compute/descriptor lease and a healthy transfer pair.
 * Records a discard request for that exact submission and returns
 * immediately; Vulkan 1.0 cannot revoke work already submitted to a queue.
 * Callers must serialize cancellation and completion on the host thread.
 * The command, fence and storage remain exclusively leased until completion
 * observes the real fence or explicit recovery waits for the device.
 *
 * A cancelled download is not published or invalidated for host reads;
 * a submitted upload may still write the destination on the GPU. Callers
 * must not read cancelled download data and must wait for completion or
 * recover before reusing any pending resources. Repeated or invalid
 * cancellation requests return VK_ERROR_INITIALIZATION_FAILED.
 */
VkResult backend_vulkan_session_storage_transfer_cancel (
  backend_ctx_t *backend_ctx,
  uint32_t device_index);

/**
 * Complete a submitted staging transfer with an exact fence timeout.
 *
 * Requires one outstanding transfer on the exact indexed device and no
 * conflicting Compute or descriptor lease. The timeout is measured in
 * nanoseconds and passed unchanged to vkWaitForFences. VK_TIMEOUT leaves
 * all transfer ownership, pending metadata and mapped buffer lifetimes
 * intact; callers may poll again or explicitly recover the pending pair.
 *
 * VK_SUCCESS invalidates noncoherent download memory before publishing
 * completion and releasing the transfer pair. Other driver errors poison
 * the transfer pair and preserve the pending submission for recovery.
 * The method never selects a different GPU backend.
 */
VkResult backend_vulkan_session_storage_transfer_complete_timeout (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  uint64_t timeout_ns);

/**
 * Complete one submitted staging transfer with an unlimited fence wait.
 *
 * Forwards to the timed completion method with UINT64_MAX. Preserves the
 * synchronous caller contract and returns the exact Vulkan result.
 * Successfully completed copies release the transfer pair for reuse;
 * driver failures require explicit recovery or session closure.
 */
VkResult backend_vulkan_session_storage_transfer_complete (
  backend_ctx_t *backend_ctx,
  uint32_t device_index);

/**
 * Recover an interrupted or failed staging transfer on the same device.
 *
 * Rejects healthy sessions, mismatched indices and outstanding Compute
 * or descriptor leases. Uses vkDeviceWaitIdle to confirm that any pending
 * GPU transfer has finished before allocating a new transfer command
 * buffer and fence in the existing command pool. Replaces both objects
 * only after allocation succeeds, preserving retained buffers, pipelines,
 * descriptors and the independent Compute command pair.
 *
 * Any driver error or device loss leaves this transfer path unavailable;
 * recovery never claims success without a real idle wait. A pending
 * download abandoned by recovery must be resubmitted before host readback.
 */
VkResult backend_vulkan_session_storage_recover (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  FILE *diagnostics);

/**
 * Describe a cached Compute pipeline borrowed from the active session.
 *
 * descriptor_layout provides two Vulkan 1.0 storage-buffer bindings shared
 * by all public stages. pipeline_layout and pipeline remain owned by the
 * session until its logical device closes. Descriptor sets are borrowed
 * separately from the stage-count session cache.
 */
typedef struct backend_vulkan_pipeline_view
{
  VkDescriptorSetLayout descriptor_layout;
  VkPipelineLayout pipeline_layout;
  VkPipeline pipeline;
} backend_vulkan_pipeline_view_t;

/**
 * Reuse or create one immutable SPIR-V Compute pipeline in the session.
 *
 * Requires an active matching logical device, an exact nonempty SPIR-V
 * module, a non-NULL output and a diagnostics stream. Compares module
 * words byte-for-byte, retaining a private copy as the cache identity.
 * Misses compile one VkPipeline using the common two-storage-buffer layout;
 * hits borrow the same pipeline without creating driver objects. The
 * session owns the layout, pipelines and module identities until close.
 */
VkResult backend_vulkan_session_pipeline_acquire (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  const uint32_t *spirv,
  uint32_t spirv_words,
  backend_vulkan_pipeline_view_t *view,
  FILE *diagnostics);

/**
 * Declare one stage's storage binding inputs for a session descriptor set.
 *
 * Each source/destination buffer belongs to the active session and remains
 * valid for the current synchronous program. Byte ranges are explicit and
 * refer to the logical descriptor sizes, not the buffer capacities.
 */
typedef struct backend_vulkan_descriptor_binding
{
  VkBuffer source_buffer;
  VkBuffer destination_buffer;
  VkDeviceSize source_bytes;
  VkDeviceSize destination_bytes;
} backend_vulkan_descriptor_binding_t;

/**
 * Borrow descriptor sets allocated in a session-owned stage-count pool.
 *
 * Sets points into the session cache and is valid until the descriptor
 * lease is returned or the session closes. The caller must not free
 * descriptor sets, reset their pool or retain this view across jobs.
 */
typedef struct backend_vulkan_descriptor_view
{
  const VkDescriptorSet *sets;
  uint32_t stage_count;
} backend_vulkan_descriptor_view_t;

/**
 * Bind a declared sequence of storage ranges to reusable descriptor sets.
 *
 * Validates all bindings and the exact physical device before modifying
 * Vulkan objects. Each stage count owns one descriptor pool with exactly
 * the requested number of sets and two storage descriptors per set.
 * Subsequent acquisitions of that stage count reuse the same handles
 * and rebind both exact VkBuffer ranges for every stage.
 *
 * One synchronous program holds the exclusive descriptor lease until
 * backend_vulkan_session_descriptors_release is called. Invalid requests
 * return VK_ERROR_INITIALIZATION_FAILED without acquiring a lease.
 */
VkResult backend_vulkan_session_descriptors_acquire (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  const backend_vulkan_descriptor_binding_t *bindings,
  uint32_t stage_count,
  backend_vulkan_descriptor_view_t *view,
  FILE *diagnostics);

/**
 * Return the exclusive descriptor lease after GPU execution completes.
 *
 * Requires the exact active device index and an outstanding lease.
 * A pending managed Compute submission or poisoned command pair prevents
 * descriptor release until its actual fence completes or an explicit
 * device-idle-verified command recovery succeeds. A canceled submission
 * retains descriptors until its fence signals. Once safe, the pool and
 * sets remain owned by the session for later programs of the same size.
 */
VkResult backend_vulkan_session_descriptors_release (
  backend_ctx_t *backend_ctx,
  uint32_t device_index);

/**
 * Describe a reusable command buffer and fence borrowed from the session.
 *
 * Both handles belong to the parent VkDevice and remain valid until session
 * closure. The caller records a synchronous Compute program, waits for the
 * submitted fence, then returns the lease before another acquisition.
 */
typedef struct backend_vulkan_command_view
{
  VkCommandBuffer command;
  VkFence fence;
} backend_vulkan_command_view_t;

/**
 * Acquire the single reusable Compute command/fence pair for a session job.
 *
 * Requires an active matching device without an outstanding command lease,
 * pending/poisoned staging transfer or failed Compute command pair.
 * Lazily allocates a separate primary command buffer and fence, retaining
 * them across sequential jobs. Each acquisition resets the command buffer
 * for a new recording and returns the borrowed handles. A rejected request
 * never transfers ownership or creates an additional job command pair.
 */
VkResult backend_vulkan_session_command_acquire (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  backend_vulkan_command_view_t *view,
  FILE *diagnostics);

/**
 * Submit the session-owned, previously recorded Compute command.
 *
 * Requires an exclusive active command lease, the exact session device,
 * a healthy command pair and no outstanding transfer. The caller records
 * and ends the borrowed primary command buffer before submission.
 * Resets its retained fence, submits the command to the session queue,
 * and retains the command lease and any descriptor lease until completion.
 * A failed Vulkan submission poisons the Compute command pair; callers
 * return the lease as incomplete and recover after verified device idle.
 * This operation never selects or replaces another compute backend.
 */
VkResult backend_vulkan_session_command_submit (
  backend_ctx_t *backend_ctx,
  uint32_t device_index);

/**
 * Request cancellation of one submitted session Compute command.
 *
 * Requires a pending, healthy submission and its active command lease.
 * Marks the output as unwanted without blocking or revoking Vulkan queue
 * work. The command buffer, fence, descriptors and storage remain leased
 * until completion observes the real driver fence. An already canceled,
 * completed, unsubmitted or mismatched request is rejected. Caller access
 * to cancelled output is not permitted until a fresh normal submission.
 */
VkResult backend_vulkan_session_command_cancel (
  backend_ctx_t *backend_ctx,
  uint32_t device_index);

/**
 * Observe completion of a session-submitted Compute command.
 *
 * Passes timeout_ns unchanged to vkWaitForFences. VK_TIMEOUT retains
 * pending state and all borrowed resources without poisoning the session.
 * VK_SUCCESS retires the submission, records completed or canceled status,
 * and allows the caller to explicitly return the command lease with
 * completed=VK_TRUE. No GPU output is read back for a canceled submission.
 * A genuine driver failure poisons the pair; the caller releases its
 * command lease as incomplete before explicit device-idle recovery.
 */
VkResult backend_vulkan_session_command_complete_timeout (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  uint64_t timeout_ns);

/**
 * Complete a session-submitted Compute command with an unlimited wait.
 *
 * Forwards UINT64_MAX to the timeout-aware completion operation. Existing
 * caller-managed Compute queue jobs are not required to use this interface.
 */
VkResult backend_vulkan_session_command_complete (
  backend_ctx_t *backend_ctx,
  uint32_t device_index);

/**
 * Recover the command submission pair after an unsuccessful Compute job.
 *
 * Requires an active exact device session with a poisoned command pair
 * and no outstanding command lease. An outstanding descriptor lease is
 * retained, and vkDeviceWaitIdle proves all GPU commands have completed
 * before its release becomes possible. The old Compute command buffer
 * and fence are replaced in the existing command pool. Storage, pipelines,
 * descriptor pools and the logical device remain allocated. Successful
 * recovery permits another command acquisition without a new VkDevice.
 *
 * If vkDeviceWaitIdle or Vulkan object creation fails, returns that driver
 * VkResult and keeps the command path unavailable. Device loss cannot be
 * recovered through this method and requires destruction of the session.
 * A healthy session, invalid index or outstanding lease is rejected.
 */
VkResult backend_vulkan_session_command_recover (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  FILE *diagnostics);

/**
 * Return one synchronous Compute command lease to its owning session.
 *
 * Requires the exact device index and an outstanding lease. A managed
 * submission cannot be released with completed=VK_TRUE until the real
 * fence has completed, including after a cancellation request. A command
 * that was successfully recorded but never submitted may also be returned
 * complete because no GPU work references it; the caller must establish
 * this state explicitly. A failed recording or unresolved submission
 * poisons the pair when released incomplete until device-idle-verified
 * recovery or session closure. Pinned descriptors remain leased until
 * recovery proves GPU idle. Other GPU backends are unaffected.
 */
VkResult backend_vulkan_session_command_release (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  VkBool32 completed);

/**
 * Close one exact Vulkan session only when all session-owned work is released.
 *
 * Requires the active session to match device_index. Returns VK_NOT_READY
 * without waiting or modifying ownership if any Compute command or
 * descriptor lease, managed Compute submission, staging transfer, or
 * poisoned command/transfer pair remains. An unowned session returns
 * VK_SUCCESS, matching the existing idempotent close contract.
 *
 * Once ready, delegates to normal session closure, which waits for the
 * logical device to idle and releases all retained Vulkan resources.
 * The caller must serialize this operation with other session APIs.
 * This does not alter the existing unconditional close or any other
 * GPU backend.
 */
VkResult backend_vulkan_session_close_if_ready (
  backend_ctx_t *backend_ctx,
  uint32_t device_index);

/**
 * Request graceful cancellation and advance shutdown of one Vulkan session.
 *
 * Requires the exact indexed device and a host-serialized call. An active
 * managed Compute submission or staging transfer is marked canceled and
 * polled using the exact timeout_ns passed to its existing fence wait.
 * VK_TIMEOUT returns VK_NOT_READY while preserving queue ownership and
 * all borrowed resources. A submitted Compute command whose fence has
 * already been confirmed by an earlier completion call also returns its
 * retained descriptor and command leases without another fence wait.
 * Pending work is retired only after real fence confirmation, and a
 * successful staging completion releases its transfer lease. A later
 * call continues the same pending cancellation without resubmission.
 *
 * A borrowed but unsubmitted command, standalone descriptor lease or
 * poisoned command/transfer pair returns VK_NOT_READY until its owner
 * releases it or explicitly recovers. Genuine Vulkan errors are returned
 * unchanged and do not trigger forced cleanup or backend selection.
 * Once the session is quiescent, normal device-idle-verified close occurs.
 * timeout_ns only bounds the pending fence wait, not the final idle wait.
 * An absent session is idempotently successful; invalid indices are rejected.
 */
VkResult backend_vulkan_session_shutdown_poll (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  uint64_t timeout_ns);

/**
 * Destroy the backend-owned Vulkan Compute session before the catalogue.
 *
 * Waits for the logical device to idle, releases retained descriptor
 * pools/sets, storage mappings, buffers, memory, cached pipelines/layouts,
 * both command/fence pairs and their shared pool, then destroys VkDevice
 * and clears the owned session pointer.
 * Closing an empty session succeeds. A NULL backend context returns
 * VK_ERROR_INITIALIZATION_FAILED. Any VkDeviceWaitIdle error is
 * returned after the remaining session resources are released.
 */
VkResult backend_vulkan_session_close (
  backend_ctx_t *backend_ctx);

#endif // HC_BACKEND_VULKAN_H
