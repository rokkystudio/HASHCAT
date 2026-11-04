# Vulkan 1.0 Compute queue session owned by HASHCAT backend_ctx

Date: 2026-11-01

## Healthy release of recorded commands before GPU submission (2026-11-04)

A session-backed public arithmetic command records a separate
`command_recorded` state after a successful `vkEndCommandBuffer`.
A command that was recorded but never submitted can return its
command and descriptor leases as complete without a GPU fence or a
device-idle wait. A failed or incomplete recording cannot claim the
same state. A real queue-submit failure still poisons the command pair
and requires explicit idle-verified recovery.

An explicit information-only test
(`HASHCAT_VULKAN_PRE_SUBMIT_ABORT_TEST=1`) returns a simulated CPU error
after recording the public four-stage command but before submitting it.
The test validates zero GPU submissions and zero completions for the
failed invocation, clean lease release, and a successful real GPU
retry using the existing device, descriptor sets, pipeline and storage.
The normal GPU backend and standalone diagnostic submissions are not
redirected by this path.

A real Mali ARM64 Vulkan 1.0 device passed normal information mode,
the pre-submit abort with direct GPU memory and the pre-submit abort
with forced staging. Both injected runs logged zero queued commands for
the failed invocation, one healthy subsequent GPU submission/fence
completion, clean resource reuse and no command poison or recovery.
The previous post-submit abort and post-fence CPU-reference regressions
passed on the same native build. The C sources compiled for ARM64 and
ARMv7; the diagnostic ARM64 executable built without APK packaging.

## Fence-verified retirement after a submitted host abort (2026-11-04)

A session-backed public arithmetic diagnostic can now be interrupted
after a successful real Vulkan queue submission but before the caller
waits on its fence or reads GPU output. The optional, exact
`HASHCAT_VULKAN_POST_SUBMIT_ABORT_TEST=1` information-mode control
triggers this host-only error on one four-stage diagnostic command.
The GPU still completes the previously submitted work.

The normal job cleanup waits on that session's submitted Compute
fence using the existing session completion API, then returns the
descriptor and command leases without requesting a device-wide idle
wait or poisoning the command pair. A genuine fence error propagates;
it cannot be treated as successful completion. The original simulated
CPU error is retained after successful cleanup. A second unmodified
four-stage job must then complete on the same cached GPU resources,
and status accounting must show two actual managed submissions and
two fence completions with no pending or poisoned state.

Standalone fault-point diagnostics retain their own VkDeviceWaitIdle
cleanup and unsignaled-fence timeout probe. The new host-abort path
is opt-in, executes only in backend-information diagnostics and does
not schedule production GPU work or change any legacy backend.

A real ARM64 Vulkan 1.0 device passed ordinary information mode,
the post-submit host abort with direct memory, and the same test with
staging buffers. Both injected runs confirmed one simulated host
abort before fence polling, one real managed fence retirement, five
managed Compute completions, a healthy four-stage retry and zero
command poison or recovery. The earlier post-fence CPU-reference
regression also passed on the new build. Incompatible optional fault
modes are rejected explicitly. ARM64 and ARMv7 sources compiled,
and the native ARM64 executable built and ran without APK packaging.

## Post-fence CPU-reference failure and healthy GPU retry (2026-11-04)

An explicit information-mode diagnostic can be selected with
`HASHCAT_VULKAN_CPU_REFERENCE_TEST=1`. It executes the existing
four-stage public arithmetic shader using a real Vulkan queue and
fence. Only after GPU completion and host cache synchronization does
the test invert one bit in an independent CPU reference word; no GPU
input, shader output or Vulkan driver return is altered. The regular
CPU comparison detects the deliberate mismatch and returns
`VK_ERROR_INITIALIZATION_FAILED`.

The owning cleanup uses the confirmed submission and fence state to
release its descriptor and command leases as complete. It preserves
the CPU validation error without poisoning or recovering the Vulkan
command pair. The diagnostic then executes the same four-stage graph
normally on the retained device, pipeline/descriptor cache and storage,
requiring a successful readback. Exact host-side session counters
must show two real submissions and completions, no cancellation,
no outstanding leases and no command poison after both operations.
The optional test is reached only through backend-information mode;
normal information mode and the pre-existing production GPU backend
are unaffected.

A real ARM64 Vulkan 1.0 device completed three verification runs:
ordinary information mode without injection, the injected CPU error
with direct memory, and the same error with forced staging memory.
Both injected runs logged one explicit simulated CPU-reference fault,
one corresponding CPU mismatch, one successful retry and one clean
session closure. Each injected run recorded five managed Compute
completions (three normal diagnostics plus the injected and retry
commands), with zero unexpected command recoveries. All seven
information-mode GPU jobs completed their actual fences and all
remaining known-answer checks passed. ARM64 and ARMv7 source
compilation and the ARM64 native build succeeded. No APK rebuild was
required for this diagnostic-only integration.

## Separation of GPU completion and host-side validation errors (2026-11-04)

The public diagnostic resource cleanup now returns a borrowed session
Compute command as successfully completed only when a real queue
submission has a previously confirmed fence and resource cleanup has
succeeded. This decision depends on actual GPU synchronization rather
than the CPU's later reference-output validation status. A mismatch
or host-side cache/validation failure after the fence remains the
caller's original error, but cannot by itself poison a healthy command
pair and force needless device-idle recovery. A job with no submitted
command, an unfinished GPU submission, or a driver synchronization
failure does not acquire a false completed status.

Descriptor and command leases are still returned separately, after
the same real fence confirmation. The existing standalone diagnostic
cleanup and fault probes retain their own device-idle guarantees.
The original production runtime and its scheduling remain independent.
ARM64 and ARMv7 source compilation checks cover this resource-lifetime
change. Physical-GPU information-mode regression verifies normal
completion, exact resource reuse and a deliberate host-only reference
mismatch after the GPU fence, followed by a successful retry.

## Real zero-timeout fence observation in public arithmetic diagnostics (2026-11-04)

The public one-, two- and four-stage arithmetic programs submit one
session-owned Compute command each, then call
`backend_vulkan_session_command_complete_timeout()` with an exact
zero-nanosecond timeout. An already signaled fence completes the
submission immediately. A real `VK_TIMEOUT` retains the command,
descriptor and storage leases and is followed by the existing unlimited
fence completion on that same submission. Real driver errors propagate
without publishing incomplete GPU output. No second fence wait occurs
after `VK_SUCCESS`. Output invalidation and CPU value checking execute
only after successful real fence completion.

The diagnostic log includes one exact zero-timeout observation per
stage count, including the observed Vulkan return code and pending
flag. Real Mali ARM64 Vulkan 1.0 checks passed with direct and
forced-staging memory. Each run made three polls, received three
actual VK_TIMEOUT results and completed all three submitted commands
through subsequent fence waits. Each timeout had a matching
session-owned pending-state record; all five GPU diagnostics and
their cache/resource checks passed without unexpected recovery.
The previous standalone unsignaled fence probe, cryptographic
known-answer diagnostics and production backend remain independent.
Only native ARM64 outputs were built, without APK packaging.

## Shared session submission for public arithmetic Compute (2026-11-04)

The declared one-, two- and four-stage public 32-bit arithmetic
diagnostics now submit borrowed Compute command buffers through
`backend_vulkan_session_command_submit()` and finish through
`backend_vulkan_session_command_complete()`. Session ownership
records each queue submission and its exact fence-confirmed
completion. The existing command lease, descriptor lease, cached
pipelines, storage slots and stage barriers are unchanged, and normal
job cleanup explicitly returns both borrowed leases after completion.

Independent standalone public diagnostics still use their own
`vkQueueSubmit` and `vkWaitForFences` operations. In particular,
the unsignaled-fence timeout probe and controlled pre-/post-submit
resource-failure tests retain their original driver-facing behavior.
The fixed-vector and message-batch cryptographic diagnostics also
retain their separate existing submission path. No production
workload routing, candidate search, target comparison or non-Vulkan
GPU backend is added by this change.

The diagnostic runtime regression checks exactly three managed
submissions, three managed completions and one clean session close,
alongside the existing five completed jobs, storage reuse and
descriptor/pipeline cache invariants. A real Mali ARM64 information
run passed all checks with zero unexpected resource recovery. The
updated ARM64 native frontend builds and links successfully; both
ARM native source variants compile with warnings treated as errors.
No Android APK was built for this native-only step.

## Cooperative cancellation and progressive session shutdown (2026-11-04)

The explicit `backend_vulkan_session_shutdown_poll()` operation
advances an exact Vulkan session toward closure without discarding
outstanding GPU work. If a managed Compute command or staging copy
has been submitted, it requests cancellation only once, then polls
the existing fence using the supplied nanosecond timeout. A real
`VK_TIMEOUT` becomes `VK_NOT_READY`: the pending submission,
mapped buffers and descriptor/command ownership remain intact.
The next call continues waiting on that same work. Vulkan 1.0 cannot
revoke a command that is already in the queue.

Once the fence signals, a canceled Compute submission returns its
descriptor and command leases to the session, or a canceled staging
transfer retires its transfer lease. If a caller already confirmed
completion of a session-managed Compute command but retained its
command/descriptor leases, the same shutdown operation releases those
leases directly without repeating the per-command fence wait. The
previous completion remains recorded as normal rather than being
retroactively counted as canceled. The operation then invokes the
existing guarded close, including the final `vkDeviceWaitIdle` and
normal destruction of retained resources. The supplied timeout
bounds only pending fence waits, not this final device-idle wait.
An unsubmitted borrowed command, separate descriptor lease or
poisoned synchronization pair returns `VK_NOT_READY` until the
original owner releases or recovers it. Vulkan driver errors are
returned without a forced teardown or alternate backend selection.
An invalid device index is rejected; closing an absent session is
idempotent. All calls remain serialized by their host owner.

Three additional real ARM64 Vulkan 1.0 logical-device sessions exercise
the progression for a canceled Compute command, a canceled staging
upload and a previously completed Compute command with retained
leases. The first two observed a zero-timeout pending fence and
subsequently closed after an unlimited completion wait. The third
verified a successful Compute fence wait before shutdown, confirmed
that an outstanding lease prevents normal guarded closure and then
closed successfully through the shutdown API without a second
per-command wait. Premature shutdown with an unsubmitted Compute
lease, mismatched indices and repeated closing after success were
checked. Both original diagnostic sessions, resource reuse and
staging readbacks still pass. Both ARM64 and ARMv7 standalone native
executables build successfully, without rebuilding the Android APK.

## Deferred Vulkan session closure with explicit ownership checks (2026-11-04)

A new optional `backend_vulkan_session_close_if_ready()` entry point
uses the exact selected Vulkan device index to close a session only
when its host-owned resources are quiescent. An active Compute command
lease, descriptor lease, managed queue submission, staging transfer,
pending cancellation or poisoned synchronization pair returns
`VK_NOT_READY`. No device-driver call, implicit queue wait, resource
destruction or state mutation occurs on that return path. An invalid
device index returns `VK_ERROR_INITIALIZATION_FAILED`, while an
already absent session returns `VK_SUCCESS` for idempotent teardown.

Once all work has been completed and released, the guarded entry
point delegates to the existing closure function. The latter retains
its historical unconditional cleanup contract and waits for
`vkDeviceWaitIdle` before destroying descriptor pools, storage,
pipelines, synchronization objects and the logical device. Forced
closure remains available for error cleanup; the guarded path is
suited to lifecycle requests that must defer active GPU work.
Callers serialize all session operations on the host.

The standalone Vulkan 1.0 ARM64 hardware diagnostic exercised two
independent logical-device lifetimes and ten actual deferred-close
checks: running and canceled Compute commands, a completed but still
leased command/descriptor pair, a poisoned command pair awaiting
explicit recovery, and a pending staging transfer, each once per
lifetime. Neither catalogue nor resources were destroyed by any
deferred attempt. Both fully released sessions subsequently closed
through the guarded API and accepted idempotent repeated closure.
The exact device-index rejection was tested as well. ARM64 and ARMv7
native builds and the single-device GPU regression passed without
rebuilding an Android APK.

## Exact host-side session status and reserve safety (2026-11-04)

A new read-only `backend_vulkan_session_status()` API exposes the
session's host-observed Compute command lease, pending/canceled/failed
submission state, descriptor lease, pending/canceled/failed transfer
state and separate completed/canceled counters. The view is copied
without queue submission, driver calls or waiting on GPU fences. It
does **not** infer that a pending command is finished just because the
GPU may have signaled asynchronously. Only the existing completion
functions retire GPU work and update their associated host state.
The caller must serialize state reads with all session operations.

The API rejects a missing session, wrong device index and null output
without modifying the output buffer. A failed or incomplete Compute
command also prevents new storage reservation, even if its command
lease has been returned: the session may not replace buffers until
idle-verified command recovery clears the poisoned state. Pending
Compute commands and active descriptor leases retain their previous
resource ownership guarantees. No other backend is inspected or
selected.

The standalone device test exercises the exact snapshot before
any submission, after managed queue submission, immediately after
cancellation, after real fence completion, while command recovery is
required and after verified recovery. It also verifies staging
cancellation and all end-of-session counters. A real Mali ARM64
Vulkan 1.0 test passed two independent sessions, each containing
two managed Compute submits (one completed, one canceled), seventeen
completed staging transfers and two canceled transfers. The
diagnostic completion summary reports the actual seventeen
completed transfers per session, including the two final readbacks.
Both ARM64 and ARMv7 native binaries were linked successfully.
This isolated native change did not require an APK build.

## Fence-verified descriptor lease retention (2026-11-04)

A session-managed Compute submission holds its descriptor-set lease as
well as its command/fence pair until the real GPU fence completes. The
descriptor release API returns VK_ERROR_INITIALIZATION_FAILED while a
managed command is pending, including when its cancellation has already
been requested. It also refuses to release descriptors when the
command pair is poisoned by a failed or abandoned submission, even if
that command lease was returned as incomplete. This prevents cached
descriptor sets from being rebound to new storage while the GPU may
still reference their previous resources.

Explicit Compute command recovery accepts a retained descriptor lease,
waits for actual VkDeviceWaitIdle, and replaces the poisoned command/fence
pair before descriptors become releasable. A failed wait or replacement
leaves the session poisoned and descriptors pinned; no device-loss
success is fabricated. On a normally completed or canceled submission,
descriptors may be returned after fence completion while the command
lease is still held; the command lease is then released separately.
The original synchronous GPU backend, queue selection and job paths
are not redirected.

The standalone native diagnostic now acquires six descriptor leases per
independent session, including a lease kept across host-side incomplete
Compute submission and device-idle recovery, and two leases held during
normal/canceled managed submissions. A real Mali ARM64 Vulkan 1.0
test passed two independent sessions, twelve exact descriptor binds,
eight descriptor-pool reuses and two idle-verified recovery operations.
All early-release attempts were rejected, and both managed commands
retained their fence/cancellation/readback guarantees. Both Android
ARM targets compile with warnings treated as errors. The physical GPU
test runner now checks exact descriptor binding and reuse counts.
No APK rebuild was needed for this native-only resource-lifetime work.

## Session-managed Compute command cancellation (2026-11-04)

The independent Vulkan session now exposes explicit submission,
cancellation, zero-timeout polling and blocking completion for a
borrowed primary Compute command. The caller records and ends the
command buffer acquired through the existing session API, then
`backend_vulkan_session_command_submit()` resets its retained fence
and submits that exact primary command through the session queue.
Each command lease allows one real submission; re-recording requires
release and reacquisition of the cached command/fence pair.

`backend_vulkan_session_command_cancel()` marks one in-flight
submission's output as unwanted and returns without blocking. Vulkan
cannot revoke work already submitted to its queue. The command and
any retained resource leases remain unavailable until
`backend_vulkan_session_command_complete_timeout()` observes an actual
fence completion. VK_TIMEOUT preserves pending state and is not treated
as a driver failure. Successful completion separately counts ordinary
and canceled submissions. Command release with completed=VK_TRUE is
rejected while managed GPU work remains pending, even when cancellation
was requested. A driver failure still requires an incomplete lease
release followed by explicit device-idle-verified recovery.

The existing caller-managed command submission and release contract
continues to work independently: production jobs are not migrated
implicitly. This API does not change the original GPU backend or
create a production workload scheduler.

The standalone ARM64 and ARMv7 session diagnostic now records two
real Compute submissions per logical-device trial. The first is
canceled, confirmed through its real fence, and discarded. The second
completes normally on the exact same command buffer and fence.
Invalid/duplicate submits, premature release, duplicate cancels
and concurrent staging operations are rejected. A subsequent
four-byte staging upload/download confirms retained storage is
still valid. A physical Mali ARM64 Vulkan 1.0 test passed four
real managed submits across two independent sessions: two canceled
and two ordinarily completed, plus both follow-up readbacks.

## Cooperative cancellation of an in-flight staging transfer (2026-11-04)

The session exposes `backend_vulkan_session_storage_transfer_cancel()`
as an immediate, nonblocking request to discard a pending transfer's host
result. Vulkan 1.0 does not provide an API to revoke an already submitted
queue command, so cancellation **never** treats in-flight GPU work as
complete. The transfer command buffer, fence and staging storage retain
their exclusive lease until the fence actually signals or the session
undergoes explicit device-idle-verified recovery.

A successful request records `transfer_cancel_requested` on the
specific pending submission. Cancellation with no pending transfer,
wrong physical-device index, a previous cancellation request, poisoned
transfer state or a conflicting Compute/descriptor lease is rejected
without modifying resources. Subsequent zero-timeout waits can still
return the real VK_TIMEOUT while retaining ownership. On actual fence
completion, the canceled submission is retired with a separate
`cancelled_transfers` counter rather than `completed_transfers`.
Canceled downloads skip noncoherent host-memory invalidation and are
never published as valid CPU results. The GPU may still have performed
a canceled upload: callers must not assume that its destination remained
unchanged. Subsequent normal uploads and downloads use the same
allocated Vulkan objects and preserve the original blocking API.

A real Mali ARM64 Vulkan 1.0 smoke test completed two separate
logical-device lifetimes, canceled one upload and one download per
lifetime, retired all four through their actual fences and validated
four follow-up downloads against the expected GPU storage. The test
also rejected repeat cancels and concurrent submissions while the
canceled transfer was pending. Cancellation did not require session
recreation, and neither the OpenCL production runtime nor APK
packaging is involved. The standalone physical-device test runner
checks cancellation and timeout accounting along with prior recovery
evidence.

## Zero-timeout staging completion for event-driven scheduling (2026-11-04)

The backend-owned transfer session exposes
`backend_vulkan_session_storage_transfer_complete_timeout()` for a
caller-selected exact VkFence timeout in nanoseconds. Passing zero
observes current GPU completion without blocking the CPU. If the
driver returns VK_TIMEOUT, the transfer remains pending, its storage
allocation and transfer command/fence pair retain their ownership,
and any conflicting Compute or staging request remains rejected.
The caller may poll the same transfer again, finish it through the
unlimited-wait API or explicitly recover its pending queue work.
VK_TIMEOUT is not interpreted as a driver failure and never poisons
a healthy session.

A successful fence wait invalidates noncoherent downloaded memory
before completion becomes visible to the CPU. Genuine Vulkan errors
continue to poison the transfer pair for driver-verified recovery.
The original synchronous transfer and completion entry points still
perform unlimited waits and keep their existing return semantics.
No published or non-Vulkan GPU backend is selected by this API.

Real Android hardware tests passed on three GPUs with ARM64 and ARMv7:
eight diagnostic processes, sixteen separate logical-device sessions,
and thirty-two real zero-timeout upload/download observations.
Eleven observations returned the actual Vulkan VK_TIMEOUT result.
The remaining twenty-one completed immediately. Each timed-out
transfer completed subsequently through the normal blocking API
without rebuilding its logical device; the readback and original
recovery/lifetime checks passed in all sixteen sessions. Each
returned timeout had one exact pending-state log event.

The new full native ARM64 and ARMv7 frontends also passed twelve
information-mode GPU regressions: six forced-staging sessions, six
direct-memory sessions and two mixed-runtime sessions in the total
set. All sixty Compute program submissions completed using their
own signaled fences, with cached descriptor and pipeline resources
and zero unexpected command or staging recoveries. The standalone
test runner `tools/run-vulkan-backend-session-timed.ps1` accepts an
explicit device serial, ARM ABI and Vulkan 1.0/1.1 requirement,
preserves complete GPU process logs and checks the exact number
of real VK_TIMEOUT observations against pending-state records.

The opt-in dual Android APK contains the newly built native ARM
diagnostic frontends alongside four unchanged published production
ABI executables. SHA-256 comparison validated all six executable
payloads; the default execution backend remains independent and
production workload routing is unchanged.

## Asynchronous staging transfer and recovery (2026-11-04)

Session-owned staging uploads and downloads expose explicit submit and
complete operations. Submission records Vulkan 1.0 transfer/Compute/host
visibility barriers, flushes host writes when required and submits one
real queue command without blocking on its fence. Completion waits for
that fence, invalidates noncoherent downloaded memory, records the
completed byte range and releases the transfer pair for reuse.
The existing synchronous staging transfer API performs these two
operations in sequence and preserves its caller contract.

An outstanding transfer blocks a second transfer, descriptor rebinding,
storage growth and new Compute commands. A failed fence, submission,
command recording or host-memory synchronization leaves the transfer
pair unavailable until explicit recovery. Recovering a pending transfer
requires a successful vkDeviceWaitIdle, then allocates a replacement
transfer command buffer and signaled fence before destroying their old
handles. The independent Compute command/fence pair, cached pipelines,
descriptor sets and storage allocations remain on the same logical
device. Abandoned downloads must be submitted again before host access;
no driver error or device loss is simulated.

The standalone hardware diagnostic deliberately submits a real
four-byte staging upload without completing its fence wait, then
validates pending-state rejection and recovery. Once the GPU is idle,
the test reads back the abandoned upload and executes a second async
upload followed by explicit completion and readback. It verifies both
values and requires one transfer recovery and one abandoned transfer
per session lifetime, in addition to all previous resource checks.

Real-driver verification completed six diagnostic processes with two
independent session lifetimes each: Mali-G715 ARM64 and Adreno 730
ARM64/ARMv7, requesting Vulkan 1.0 and 1.1. All twelve sessions
recovered one abandoned submission, then verified two independent
four-byte readbacks (one abandoned upload and one normally completed
asynchronous upload). Each session completed 13 staging transfers and
rejected ten invalid or conflicting requests. Twelve genuine staging
recoveries and 26 session-trial transfers per API/GPU pair passed.

The regular information-mode regression completed eight further
processes on those devices: six Vulkan-only runs (direct and forced
staging on three ABI/GPU combinations), and two mixed-runtime runs on
Mali ARM64. Each completed five public Compute programs with the
existing cached GPU resources and no unexpected transfer recovery.
These tests do not exercise real Vulkan driver device loss.

An explicit Android APK backend-build selection was verified separately.
Selecting diagnostics includes the tested ARM64/ARMv7 binaries, while
the no-property build still packages the complete original four-ABI
native runtime with identical SHA-256 contents. The original OpenCL
information mode executed successfully on physical Mali ARM64 and
Adreno ARMv7 drivers. Runtime workload-backend switching remains future work;
production GPU execution continues to use the existing scheduler.

## Explicit Compute queue recovery after incomplete submission (2026-11-03)

A failed or incomplete Compute job marks its borrowed command/fence pair
unavailable when returning the command lease. The executor propagates its
original dispatch/verification result into cleanup, preventing a successful
resource cleanup from silently treating the failed job as successful.
Further command acquisitions are rejected while that pair is poisoned.

`backend_vulkan_session_command_recover()` is an explicit operation,
not an automatic retry. It validates the same physical-device index,
requires no outstanding descriptor or command lease and accepts only a
poisoned Compute pair. A real `vkDeviceWaitIdle` must succeed before the
session allocates a replacement command buffer and fence in its existing
command pool. The replacement objects are fully created before the old
pair is destroyed. Retained pipelines, storage and descriptor pools remain
owned by the same Vulkan logical device.

When device idle or resource allocation returns a driver error, recovery
returns that exact VkResult and leaves the command path unavailable.
A lost Vulkan device is not declared recovered: session destruction and
driver-level device reinitialization are separate operations. The
independent staging-transfer command/fence pair is retained during
successful Compute-command recovery and can still be used afterwards.

The standalone Android diagnostic submits a real command and deliberately
returns its command lease before waiting for the associated fence.
It verifies blocked re-acquisition and recovery calls with invalid
contracts, then explicitly waits for device idle through the recovery
method, replaces the command pair and executes a second real submission
with fence completion. An additional staging upload/download validates
the remaining session-owned storage. The diagnostic does not fabricate
VkResult driver failures or device loss.

Physical verification on November 3 completed six native processes:
Mali-G715 ARM64 and Adreno 730 ARM64/ARMv7, each with Vulkan 1.0
and 1.1 requests. All 12 independent session lifetimes submitted one
command without waiting for its fence, rejected six invalid/unsafe
recovery operations, performed one successful device-idle-verified
replacement, submitted another command with a real fence, and verified
a further four-byte staged upload/download. Each lifetime closed
with exactly one successful explicit recovery and ten completed
staging transfers. Driver device loss was not triggered or claimed
to be recoverable.

The same native source completed eight full information-mode GPU
regressions: Mali-G715 and Adreno 730 ARM64/ARMv7 with direct storage
and forced staging, plus mixed-runtime direct/staging sessions.
Each of the forty successful Compute calls finished through its own
fence without requiring command recovery or a redundant device-wide
idle wait. The integration runner also verifies that every healthy
session reports zero command recoveries at final cleanup.

## Fence-completed Compute job cleanup (2026-11-03)

A session-backed Compute program tracks successful GPU queue submission
and the result of its own VkFence wait. On a successful fence wait, all
command-buffer accesses to that program's descriptors and storage have
completed. Job cleanup releases the exclusive descriptor and command
leases without making a redundant device-wide vkDeviceWaitIdle call.
The session-owned logical device, pipelines, descriptors and buffers remain
available for the following synchronous program.

If a submitted program has not established fence completion, cleanup
uses vkDeviceWaitIdle before releasing any dependent resources.
An idle-wait failure marks its borrowed Compute command pair unusable
for future submissions until session closure. Standalone fault-injection
diagnostics continue to wait for VkDevice idle as part of their independent
resource ownership contract. Session closure still performs one final
device-idle wait before destroying shared resources.

The integrated GPU test runner validates exactly five fence-completed
jobs with no per-job device-wide idle wait in each information-mode
session, alongside existing dispatch, descriptor, pipeline, storage,
staging and session-close assertions.

## Persistent descriptor pools and exact buffer rebinding (2026-11-03)

The Vulkan session retains descriptor pools indexed by the exact number
of Compute stages in a validated program. Each pool contains the declared
number of VkDescriptorSet handles with two STORAGE_BUFFER bindings per
stage, using the session's common immutable descriptor layout. One stage
count creates one pool; subsequent programs with the same stage count
reuse the existing pool and every descriptor set.

`backend_vulkan_session_descriptors_acquire()` verifies the physical
device index, stage count, caller output, buffer handles and exact logical
ranges against the session's currently retained storage slots. It then
updates both descriptor bindings on every stage and grants one exclusive
lease. The session refuses simultaneous descriptor leases, buffer
reallocations and staging transfers while descriptors are borrowed.
`backend_vulkan_session_descriptors_release()` returns the lease only
after dependent GPU execution completes; no pool or set is freed until
the owning session closes.

The five information-mode Compute programs have stage counts 1, 2,
4, 1 and 1. They therefore create three descriptor pools, reuse the
one-stage pool twice and perform five complete descriptor acquisitions
with 18 VkWriteDescriptorSet updates. Pipeline, command, storage and
descriptor resources are destroyed in dependency order after a real
device-idle wait. Standalone diagnostics retain their independently
owned temporary descriptor resources and fault checkpoints.

Real Android hardware validation covered six standalone diagnostic processes,
each executing two independent sessions on Mali-G715 ARM64 and Adreno 730
ARM64/ARMv7, requesting Vulkan 1.0 and 1.1. The 12 lifetimes verified two
cached descriptor pools, one pool reuse, three acquisitions, eight descriptor
writes, exclusive lease rejection and one exact buffer rebinding each.
All seven invalid descriptor/storage operations per lifetime were rejected.

Eight full information-mode processes were also validated on those GPU/ABI
combinations: three Vulkan-only direct-memory runs, three Vulkan-only
forced-staging runs, and mixed Vulkan/OpenCL direct/staging runs on Mali ARM64.
Each process completed five Compute programs with three descriptor pools,
two pool reuses, five acquisitions and 18 descriptor writes. Physical GPU
results, pipeline and command reuse, staging copies and descriptor cleanup
all matched the recorded reference values.

## Cached public Compute pipelines and command leases (2026-11-03)

An active Vulkan session owns one descriptor-set layout with two storage-buffer
bindings, one compatible pipeline layout and a cache of Compute pipelines.
`backend_vulkan_session_pipeline_acquire()` compares each complete SPIR-V
word sequence with retained immutable module copies. A matching sequence
borrows the existing VkPipeline; a distinct sequence creates one shader
module and pipeline, releases the temporary shader module and retains its
own module identity. Layouts, pipelines and module copies remain owned
by the session until final device-idle cleanup.

`backend_vulkan_session_command_acquire()` lazily allocates one additional
primary command buffer and fence in the existing session command pool.
This pair serves sequential Compute programs and is independent of the
staging-transfer command buffer and fence. After its completed queue-fence wait, a job
returns its exclusive command lease through
`backend_vulkan_session_command_release()`. The next job records into
the same resettable command buffer. A second concurrent lease, mismatched
device index or failed GPU job prevents unsafe reuse.

The executor borrows stage-count-specific descriptor pools and sets from
the session, rebinding each stage's exact source/destination VkBuffer and
logical byte range before dispatch. A completed synchronous job returns
its descriptor and command leases after GPU completion, freeing only
temporary host arrays. Session-owned descriptor sets, pipeline objects,
storage buffers, command handles and VkDevice remain valid for reuse.

A complete information-mode run requests nine Compute stage pipelines
across five programs, using four distinct immutable SPIR-V modules.
The cache therefore creates four pipelines and records five hits.
Five sequential command acquisitions reuse one command/fence pair four
times. Cache and command ownership counters are recorded at session
close. Independent fault-injection diagnostics continue to own temporary
Vulkan resources.

Standalone Android API-24 diagnostics validated six physical GPU processes:
Mali-G715 ARM64 and Adreno 730 ARM64/ARMv7, each requesting Vulkan 1.0
and 1.1. Every process completed two independent session lifetimes with
two cached public pipelines, one cache hit, two command acquisitions and
one command reuse per lifetime. The 12 device sessions rejected invalid
pipeline requests, duplicate command leases, mismatched device indices and
staging transfers while a Compute command was leased.

Eight full information-mode GPU processes passed on the same ARM64/ARMv7
device set: three Vulkan-only direct-memory, three Vulkan-only forced-staging,
and two mixed-runtime cases on Mali ARM64 (direct and forced-staging).
Each process ran five programs, built four distinct cached pipelines,
recorded five pipeline hits, acquired one command/fence pair five times
and reused that pair four times. Session close reported completed resource
cleanup with VkResult 0. No GPU scheduling workload was run.

## Explicit staging transfer session (2026-11-03)

The native session exposes a synchronous
`backend_vulkan_session_storage_transfer()` API for one staged slot and an
explicit transfer direction. It reuses the session-owned primary command
buffer, Compute queue and fence. No additional device or transfer session
is created for each operation. Each call validates the exact device index,
staging slot, transfer size and four-byte Vulkan copy alignment before GPU
execution.

An upload flushes noncoherent host writes, records HOST_WRITE ->
TRANSFER_READ and TRANSFER_WRITE -> SHADER_READ/WRITE visibility, submits a
staging-to-Compute buffer copy and waits for a real fence. A download records
SHADER_WRITE -> TRANSFER_READ and TRANSFER_WRITE -> HOST_READ barriers,
submits a Compute-to-staging copy, waits for the fence, then invalidates
noncoherent host memory. Host mappings persist across repeated transfers.
The session records every completed transfer and reports the count on close.
After a queue submission or fence failure, further transfer calls are
rejected until the entire session closes.

The standalone diagnostic reserves two staged buffers and performs four
upload/readback pairs using exact 32-, 64-, 96- and 256-byte ranges.
For every pair, the host staging allocation is overwritten before readback,
so the expected bytes can only return through a completed GPU transfer.
Five invalid operations are rejected without submission: zero size,
misaligned size, oversize, invalid buffer index and wrong device index.
All verification runs use public deterministic byte data and test no
workload scheduling.

Real-device verification completed six standalone processes (Mali-G715
ARM64 and Adreno 730 ARM64/ARMv7, each requesting Vulkan 1.0 and 1.1),
12 session lifetimes and 48 upload/readback pairs. Every one of the 96
GPU queue copies completed with a real fence, 5,376 bytes of returned
data matched their host references and 60 invalid transfer requests
were rejected. These runs exercise the explicit session transfer API.

The integrated diagnostic frontend also completed three Vulkan-only
forced-staging processes on those GPU/ABI combinations and one mixed
Vulkan/OpenCL forced-staging process on Mali ARM64. Each process ran five
Compute programs, reporting six staging uploads and nine staging
downloads with correct GPU outputs. Two additional Vulkan-only runs
verified the default direct-memory path on ARM64 and ARMv7. The Android
debug package contains the tested ARM64/ARMv7 binaries with matching
SHA-256 hashes and retains both additional x86 ABIs.

Physical noncoherent-only staging memory, concurrent transfers from
multiple host threads and device-loss recovery remain unverified by
these hardware tests.

## Session storage allocation verification (2026-11-03)

The backend session retains CPU-accessible storage-buffer slots across
consecutive public Compute program lifetimes. Each slot can use direct mapped
Compute memory or a separate mapped transfer allocation when staging is
required. The native session diagnostic validates
two initial allocations of 64 and 128 bytes, reuse with logical ranges of
32 and 96 bytes, and growth of the first slot to 256 bytes. The second
slot retains its original VkBuffer and capacity during the growth request.
Both a zero-length allocation and a mismatched physical-device index
return VK_ERROR_INITIALIZATION_FAILED. Session close waits for device idle
and releases every retained mapping, buffer and memory allocation.

The Android API 24 diagnostic builds for ARM64 and ARMv7 completed
successfully with -Werror. Hardware tests executed two full session
lifetimes per requested API on Mali-G715 ARM64, Adreno 730 ARM64 and
Adreno 730 ARMv7, using both Vulkan 1.0 and 1.1: six native processes,
12 successful storage lifecycle trials, 36 allocations, 36 buffer
reuses, 12 capacity growths and 24 invalid storage requests rejected.
These are direct GPU-driver results, not offline transcript verification.

## Shared logical-device execution (2026-11-03)

The active backend-information Compute path reuses one logical device and
queue across its public one-stage, two-stage and four-stage programs and
the MD5/SHA-256 known-answer diagnostics. The Vulkan catalogue retains
VkInstance and indexed physical devices. The session retains VkDevice,
Compute queue, its initial empty-submission command buffer and fence.

`backend_vulkan_session_device()` returns a read-only view of the
session's Vulkan handles after verifying the exact physical-device
index and successful initial queue submission. The caller does not own
the returned handles. Missing or mismatched sessions return
`VK_ERROR_INITIALIZATION_FAILED` without modifying the view.

The common program executor validates the full declared workload using
the backend catalogue and verifies that the borrowed session's queue
family and API match the selected physical device. The session retains
a growable array of CPU-accessible storage slots. When a Compute buffer
supports compatible HOST_VISIBLE memory, the slot maps it directly.
Otherwise the Compute buffer uses compatible device memory and the slot
owns a separate host-visible TRANSFER_SRC/DST buffer. A program reuses
each slot when its capacity satisfies the requested logical descriptor
range and grows only undersized slots.

Each session-backed program owns its descriptor pool/sets and temporary
host arrays, and borrows cached pipelines, compatible layouts and an
exclusively leased command buffer/fence. Staged inputs are copied into
Compute storage before dispatch and staged outputs return after shader
completion. The job waits for fence completion, releases only its own
resources and returns its command lease. VkDevice, cached pipelines,
commands and retained storage are destroyed when the session closes.
Standalone diagnostics own their independent VkDevice, pipelines,
commands and storage together with their existing fault controls.

The full Android build performs five synchronous Compute program calls
per information-mode invocation, emitting exactly five
`BACKEND_VULKAN_EXECUTOR_SESSION_REUSE` records with
`owns_device=0`. For the current program sequence the retained storage
pool grows to five slots, performs five allocations and nine capacity
reuses. The session closes after the programs and reports the storage
counters together with one completed empty queue submission. The
dedicated integration runner requires all five device-reuse records,
the exact storage lifecycle, successful public GPU validation and a
zero native process exit on both Vulkan-only and mixed-backend runs.
The environment control `HASHCAT_VULKAN_FORCE_STAGING=1` is reserved
for integration diagnostics. It forces every retained slot through the
same transfer-buffer path used when direct storage mapping is unavailable;
it does not alter shader code, dispatch geometry or workload data. Successful
forced-staging runs report completed upload/download counts after GPU result
verification.

This is a shared-resource lifecycle milestone and does not schedule
ordinary password-processing tasks.

## Implementation and ownership

The opt-in Android HASHCAT backend owns one independent Vulkan 1.0
Compute session in addition to its indexed physical-device catalogue.
The session creates a VkDevice, obtains one Compute queue, creates a
resettable command pool, primary transfer command buffer and fence, then
validates the queue with one fence-completed empty command buffer.
Compute calls may reserve mapped direct or staged storage on that same
device. The session retains a separate Compute command/fence pair and
exact-SPIR-V cached pipelines with one shared descriptor/pipeline layout.
All resources are destroyed in ownership order at session closure.

Shader modules used to compile cached pipelines are temporary, and
per-job descriptor pools/sets remain scoped to individual Compute calls.
No password candidate or hashing-scheduler entry belongs to this session.

```text
backend_ctx_t
  |
  +-- vulkan_catalog                 VkInstance
  |      +-- VkPhysicalDevice[]       borrowed physical-device handles
  |
  +-- vulkan_session                 VkDevice
         +-- VkQueue                 borrowed from VkDevice
         +-- VkCommandPool           resettable
         |      +-- VkCommandBuffer   empty submission / staging transfers
         |      +-- VkCommandBuffer   reusable Compute command
         +-- VkFence                 staging-transfer synchronization
         +-- VkFence                 reusable Compute synchronization
         +-- VkPipelineLayout        shared two-storage-buffer layout
         +-- VkDescriptorSetLayout   two Compute storage bindings
         +-- VkPipeline[]            exact-SPIR-V cached programs
         +-- persistent storage[]    direct or staged host mappings

backend_vulkan_session_close:
    vkDeviceWaitIdle
    vkDestroyFence
    vkFreeCommandBuffers
    vkDestroyCommandPool
    vkDestroyDevice
    free session allocation

backend_vulkan_catalog_close:
    requires vulkan_session == NULL
    free physical device handles
    vkDestroyInstance
```

The logical device enables **zero optional features and extensions**.
The physical device and Compute queue are checked against explicitly
declared public workload limits, not the GPU vendor/model name.

## Production lifecycle

With `ENABLE_VULKAN_DISCOVERY=1` the production `source/src/backend.c`
uses these owner transitions:

1. `backend_ctx_init` opens the Vulkan 1.0 physical-device catalogue
   alongside normal CUDA/HIP/Metal/OpenCL initialization.
2. `backend_ctx_devices_init` opens the isolated logical-device session
   **only in backend-information mode (`hashcat -I`)** using an explicit
   1×1×1 public Compute workload, one storage descriptor and a 4-byte
   storage-range requirement. It submits a single empty command buffer,
   waits on its fence and logs the real Vulkan result.
3. `backend_ctx_devices_destroy` closes the logical-device session
   before the ordinary device contexts are destroyed.
4. `backend_ctx_destroy` attempts session closure as a backstop and
   closes the Vulkan catalogue afterward. The catalogue refuses to
   destroy VkInstance while a VkDevice session is still active.

All Vulkan errors are reported with their real VkResult values.
Ordinary CUDA, HIP, Metal and OpenCL devices remain independent.
Vulkan devices are not inserted into HASHCAT's active hashing-device
list, and the `backend_session_begin` attack/kernel path is not used.

## Public C API

### `backend_vulkan_session_open`

```c
VkResult backend_vulkan_session_open (
  backend_ctx_t *backend_ctx,
  uint32_t device_index,
  const vulkan_auto_compute_requirements_t *workload,
  FILE *diagnostics);
```

Requires an opened parent catalogue, explicit in-range device index,
non-NULL workload and diagnostics stream, and an empty session owner.
Checks the driver's Vulkan API, queue availability and Compute limits.
Creates VkDevice, queue, pool, primary buffer and fence, then records,
submits and waits for one empty command buffer. On success the session
belongs to `backend_ctx->vulkan_session`.

Any failure releases all partially acquired Vulkan objects and leaves
the session pointer NULL. Malformed host calls return
`VK_ERROR_INITIALIZATION_FAILED`. A workload exceeding the actual
driver limits returns `VK_ERROR_FEATURE_NOT_PRESENT`.

### `backend_vulkan_session_close`

```c
VkResult backend_vulkan_session_close (
  backend_ctx_t *backend_ctx);
```

Waits for the device to idle, destroys its subordinate resources and
logical device, frees the allocation and clears the session pointer.
An empty session closes successfully. NULL context returns
`VK_ERROR_INITIALIZATION_FAILED`. If vkDeviceWaitIdle reports an error,
that error is returned after attempting resource teardown.

### `backend_vulkan_catalog_close`

```c
VkResult backend_vulkan_catalog_close (
  backend_ctx_t *backend_ctx);
```

Requires the logical-device session to be closed before VkInstance.
Returns `VK_ERROR_INITIALIZATION_FAILED` without releasing the
catalogue if an active session still owns VkDevice.

## Real Android hardware verification

Every Vulkan resource below was provided by a real mobile GPU driver.

| Android process | Vulkan 1.0 standalone | Vulkan 1.1 standalone | Full `hashcat -I` |
| --- | ---: | ---: | ---: |
| RedMagic / Adreno 730 / ARM64 | 2 | 2 | 1 |
| RedMagic / Adreno 730 / ARMv7 | 2 | 2 | 1 |
| Pixel 9 Pro XL / Mali-G715 / ARM64 | 2 | 2 | 1 |
| **Total** | **6** | **6** | **3** |

The standalone diagnostic completed **12 Vulkan sessions and 12
fence-verified empty queue submissions** across both requested API
versions. Those runs also observed:

- 48 rejected invalid host-session opens;
- 24 rejected active-session ownership operations, including attempts
  to destroy VkInstance before VkDevice;
- 12 real driver refusals for an explicitly unsupported local invocation
  count of 4,294,967,295;
- 9 rejected malformed native command lines.

Both complete Android API 24 HASHCAT executables were linked and
executed on the three device/process configurations. All three
`hashcat -I` processes returned exit **0** and completed one additional
Vulkan session, one empty fence submission and ordered cleanup each.
The ordinary OpenCL GPU was listed during the same native process.

Across the two test families this is **15 actual logical-device
lifecycles and 15 fence-completed empty submits**. No GPU shader was
dispatched and Vulkan hashing explicitly remained disabled.

## Android builds and reproducibility

Compile standalone API-24 Compute session diagnostics:

```powershell
.\tools\build-vulkan-backend-session.ps1 -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865 -AndroidApi 24
```

Run the diagnostic on an explicitly connected Android device:

```powershell
.\tools\run-vulkan-backend-session.ps1 -Serial $DeviceAdbSerial -Abi auto -ReportPrefix device-arm -TestCore11
```

For the RedMagic 32-bit process specify `-Abi armeabi-v7a`.

Build the complete Android HASHCAT frontends into an isolated directory:

```powershell
.\scripts\BUILD.ps1 -Target android-arm64-v8a -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865 -AndroidApi 24 -EnableVulkanDiscovery -VulkanOutputName vulkan-hashcat-session-20261101 -Package:$false

.\scripts\BUILD.ps1 -Target android-armeabi-v7a -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865 -AndroidApi 24 -EnableVulkanDiscovery -VulkanOutputName vulkan-hashcat-session-20261101 -Package:$false
```

The experimental build uses an explicit `COMPTIME=1793491200`
(2026-11-01 00:00 UTC) by default, instead of the Makefile's
time-of-build epoch, so repeated diagnostic builds have stable input.
The value can be supplied explicitly with `-VulkanComptime`.
Ordinary release builds keep their existing time-of-build policy.

Each full ABI build contains the HASHCAT v7.1.2 executable, 581
native modules, four bridges and 1,595 OpenCL asset files. Android
API 24 experimental builds exclude bundled UnRAR because it requires
the unavailable `lutimes` API. Runtime assets resolve relative to
the installed executable directory.

Static ELF validation checks Android API 24, exact ARM64/ARMv7 type,
`libvulkan.so`, Vulkan 1.0 `vkCreateDevice`, `vkQueueSubmit` and
`vkWaitForFences` imports, absence of a required Vulkan 1.1
`vkEnumerateInstanceVersion` symbol, and no unnecessary Vulkan loader
dependency in the standard module `module_00000.so`.

All native binaries are stored in ignored diagnostic directories.
No release ZIP, HASHCAT-APP repository or APK was modified.

## Archived evidence and independent verification

The 12 public transcripts and SHA-256 manifest are in:

```text
tests/fixtures/android-gpu/vulkan-backend-session-20261101/
  manifest.json
  redmagic-arm64-1.0.log
  redmagic-arm64-1.1.log
  redmagic-arm64-negative-controls.log
  redmagic-arm64-hashcat-info.log
  redmagic-armv7-1.0.log
  redmagic-armv7-1.1.log
  redmagic-armv7-negative-controls.log
  redmagic-armv7-hashcat-info.log
  pixel-arm64-1.0.log
  pixel-arm64-1.1.log
  pixel-arm64-negative-controls.log
  pixel-arm64-hashcat-info.log
```

The standalone diagnostic logs preserve the LF-only text emitted by
the Android test runner, which normalizes ADB output line endings.
The full `hashcat -I` transcripts likewise contain native
stdout/stderr plus the exit marker with LF separators. Neither
capture is **represented as byte-for-byte original ADB streams**.

The independent verifier audits 18 canonical source identities,
four diagnostic binaries/objects, two complete Android HASHCAT
executables, all 15 GPU session lifecycles, driver refusals,
ordinary OpenCL coexistence and deliberately corrupted evidence.

```powershell
python -B tools/test-vulkan-backend-session-evidence.py --artifacts build/diagnostic-multimode/vulkan-backend-session-20261101 --full build/diagnostic-multimode/vulkan-hashcat-session-20261101 --readelf D:\ANDROID\SDK\ndk\29.0.14206865\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe
```

Compatibility limitations: tested phones expose newer Vulkan loaders
even though only Vulkan 1.0 is requested. A physical Vulkan-1.0-only
GPU and actual Android 7.0/API-24 OS were unavailable. Concurrent
Vulkan device sessions, device-loss recovery, validation-layer
tracking, Vulkan hashing kernels and production hashing-scheduler
integration remain outside this milestone.

The follow-on [production-owned public SPIR-V Compute kernel](vulkan-backend-public-kernel-android.md)
executes an actual shader and verifies 64 GPU outputs from `hashcat -I`
on Android ARM64 and ARMv7. The earlier session log remains an exact
historical snapshot of the empty-submission-only milestone; its source
hashes are not reinterpreted as evidence of this later shader.

An independent public-integer Vulkan 1.0 buffer-transfer diagnostic is
available in `docs/vulkan-core10-transfer-buffer-portability.md`.
It verifies actual VkBuffer allocations, queue copying and noncoherent
memory synchronization without linking these resources to HASHCAT's
hashing-device list or production kernel scheduler.
