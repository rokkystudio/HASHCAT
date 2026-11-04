# Program-declared Vulkan 1.0 Compute execution inside Android HASHCAT

Date: 2026-11-01

## Unsubmitted recorded-command ownership (2026-11-04)

The public executor records when `vkEndCommandBuffer` has succeeded.
A successfully ended command with no queue submission has no pending
GPU work and can safely release its retained command and descriptor
leases without fence polling or device-idle recovery. Incomplete
recording or a genuine submission failure remains an error path.
The caller's original CPU error is retained independently of
successful resource cleanup.

An opt-in information diagnostic simulates a host abort immediately
before submission, confirms no submitted GPU work or poisoned session
state, then requires a normal four-stage arithmetic retry on the same
retained resources. It is isolated from other host-error diagnostics
and does not affect production scheduling or other GPU backends.

A real Mali ARM64 Vulkan 1.0 GPU completed the baseline, direct-memory
pre-submit abort and forced-staging pre-submit abort checks. The
diagnostic abort added zero managed submissions, and the healthy retry
added one; command and descriptor reuse remained valid without
recovery. The prior post-submit and post-fence CPU fault tests also
passed on the current native binary. Both ARM source variants compile;
APK packaging is still deferred.

## Submitted-command retirement during host-error cleanup (2026-11-04)

Cleanup of a session-borrowed public Compute command that has been
submitted but whose fence was not yet observed now calls the session's
real fence-completion API. Successful completion retires the managed
submission and permits the descriptor and command leases to return
without a separate vkDeviceWaitIdle. A real synchronization failure
still leaves the command pair unavailable for explicit recovery.
Standalone diagnostics continue their previous device-idle cleanup
behavior.

An explicit information-only host-abort test returns a simulated
CPU error immediately after a real four-stage GPU submission, before
normal polling or output validation. Cleanup retires that exact
submission, preserves the CPU error and requires a healthy retry
using the retained pipeline, command and storage resources. The test
is opt-in and independent of post-fence CPU-reference mismatch
testing. The production GPU backend remains unchanged.

The ARM64 Vulkan 1.0 device passed default information diagnostics,
the injected host abort in direct and staged-memory configurations,
and the existing post-fence CPU-reference test. Each host-abort run
verified five real managed Compute submissions and completions, an
unpolled submitted command retired by cleanup, a successful retry,
one clean session closure and zero unexpected recovery events.
The ARMv7 source also passed compile-time checks; no APK was rebuilt.

## Explicit CPU-only validation failure after GPU fence (2026-11-04)

The optional information-only post-fence test sets
`HASHCAT_VULKAN_CPU_REFERENCE_TEST=1`. After a real four-stage
arithmetic Compute submission has finished and its mapped output is
synchronized, one host CPU reference word is deliberately changed.
The normal reference comparison reports the resulting error, while
the completed GPU command and descriptor leases return to their
existing session caches. A second unmodified four-stage Compute job
must then succeed on those same retained GPU resources without
command poisoning or device-idle recovery.

The optional physical-device regression validates the injected
reference-error marker, detected CPU mismatch, successful second
submission, exact completed-command accounting and clean session
closure. A real ARM64 Vulkan 1.0 GPU passed the baseline, direct-memory
failure and forced-staging failure scenarios. The two injected
executions each verified five session-managed submissions and
completions, their original seven job fence completions and a
poison-free session close. The normal information mode remains free
of injected faults. The old production GPU path and Android APK
packaging are unchanged.

## Confirmed Compute completion independent of CPU result validation (2026-11-04)

Public diagnostic cleanup now uses the real submitted-command and
fence-completed flags to return a borrowed command lease as complete,
provided release itself has not failed. A later CPU result-validation
failure remains an error, without needlessly poisoning GPU command
ownership after the queue's fence was confirmed. Incomplete queue
submissions and genuine Vulkan synchronization failures still require
explicit recovery. Borrowed descriptor ownership and the independent
production GPU backend are not changed. Standalone fault diagnostics
continue their normal device-idle cleanup.

The ARM64 and ARMv7 source compile with warnings treated as errors.
Real GPU diagnostics verify the normal completed-command path and a
deliberately injected host-only reference mismatch after GPU completion,
followed by clean resource reuse and normal success.

## Nonblocking observation of arithmetic Compute fences (2026-11-04)

The three public arithmetic diagnostics perform an exact zero-timeout
fence poll through the retained Vulkan session before deciding whether
an unlimited wait is needed. VK_SUCCESS retires the submission without
another wait; VK_TIMEOUT keeps command and descriptor ownership until
the same submission completes. A Vulkan error is returned rather than
marking incomplete output readable.

Hardware evidence requires exactly one poll per one-, two- and
four-stage invocation, a matching pending-state log for each real
timeout, successful readback of every stage and no unexpected command
recovery. Both direct and forced-staging ARM64 runs returned three
real VK_TIMEOUT results from three zero-timeout polls and completed
all three commands normally after waiting on their fences. Both
full diagnostic sets passed, without command poison or recovery.
The standalone diagnostic executor and its separate fence probe
continue using their original direct queue operations. This does not
select or modify other GPU backends.

## Public arithmetic execution using the retained command pair (2026-11-04)

When the public one-, two- or four-stage arithmetic executor borrows
a session command buffer, it uses the session-owned queue submission
and unlimited fence-completion APIs rather than issuing its own
`vkResetFences`, `vkQueueSubmit` and `vkWaitForFences`. The owner
tracks three normally completed submissions during one complete
information diagnostic. Borrowed descriptors, stage pipelines and
storage allocations remain cached for later program invocations.

Independent diagnostics and the explicit unsignaled-fence probe
continue using their existing direct Vulkan calls. The fixed-vector
and byte-message diagnostic paths remain separate, as do the
published production backend and all its workload scheduling.
The physical-device test requires exact managed submission,
completion and clean-close records in addition to normal GPU
readback and command reuse. An ARM64 Vulkan 1.0 device test passed:
three session submissions, three completions and all five diagnostic
job fences, followed by normal session closure without recovery.
The compiled legacy runtime and APK packaging were not modified.

## Progressive cancellation-aware Vulkan shutdown (2026-11-04)

A new session shutdown-poll API offers explicit cancellation and
fence-verified retirement for one pending managed Compute command
or staging transfer. An unfinished GPU submission leaves all
resources leased and returns `VK_NOT_READY`; a subsequent poll
finishes the same submission rather than submitting replacement work.
Completed canceled Compute commands release retained descriptors and
their command lease, while completed staging transfers release the
transfer lease. Already confirmed session-managed Compute commands
also release any retained command and descriptor leases without
polling their completed fence a second time. A previously completed
command remains accounted as completed, not canceled. The final
quiescent session uses the original device-idle-verified close. A borrowed but unsubmitted command and
a poisoned command/transfer pair are left for their owner to resolve.
The timeout applies to the individual GPU fence wait; final device
idle can still block. No other production GPU backend is switched.

Three additional Vulkan 1.0 ARM64 sessions validated graceful
shutdown with an in-flight Compute command, an in-flight staging
copy and a previously completed Compute command whose resources
were still borrowed. The first two zero-timeout attempts returned
pending status and later closed; the third confirmed that the
descriptors and command lease can be retired after an earlier fence
wait. All sessions closed cleanly, including idempotent repeat calls.
The separate, existing full session regression also passed. ARM64 and
ARMv7 native binaries linked; APK packaging remains deferred.

## Guarded closure of active Vulkan Compute sessions (2026-11-04)

The Vulkan session now exposes an optional close-if-ready operation
that returns `VK_NOT_READY` when any GPU submission, cancellation,
Compute/descriptor lease or poisoned command/transfer pair remains
active. Unlike the existing unconditional teardown path, a deferred
close neither waits for GPU idle nor destroys any resource.
When all leases have been returned and pending work completed, it
delegates to the existing device-idle-verified session close.
A mismatched device index is rejected and an already empty owner
remains idempotent. The original production GPU backend is not
selected, closed or modified.

Two real Vulkan 1.0 Compute-session lifetimes completed on ARM64,
with ten deferred-close rejections across pending Compute work,
managed cancellation, held descriptors, command recovery and staging
transfers. Both sessions subsequently closed cleanly through the
guarded entry point. The ARM64/ARMv7 native standalone binaries
compiled and linked without full Android package rebuilding.

## Read-only session status for deterministic lifecycle handling (2026-11-04)

A native session status API reports exact host-maintained ownership
and submission state, including outstanding command/descriptor leases,
staging transfers, cancellation requests, poisoned command pairs,
completed work and discarded results. It does not query or guess fence
state and cannot mark a GPU operation complete. The owning caller
must use the existing timed or blocking fence-completion entry points
and serialize session access before considering resources reusable.

Storage reservation now rejects a poisoned Compute command pair in
addition to pending transfers, outstanding commands and descriptor
leases. This prevents memory replacement while the result of an
incomplete GPU submission has not been verified by device-idle
recovery. The published production GPU mechanism remains independent.

The standalone ARM64 Vulkan 1.0 hardware check confirmed the
read-only status at each major transition in two sessions, with
normal/canceled Compute submission, staging cancellation, resource
retention and recovery. The final diagnostic counts are based on
actual driver-confirmed transfers: seventeen completed and two
canceled per session. Native ARM64/ARMv7 builds succeeded; application
APK packaging remains deferred to the next integration checkpoint.

## Descriptor ownership across pending Compute work (2026-11-04)

Cached descriptor sets retain exclusive ownership for the complete
lifetime of a session-managed Compute queue submission. Neither a
pending cancellation nor an incomplete command release permits a
descriptor pool to be rebound or returned while a real GPU operation
may reference those descriptors. A pending submission or poisoned
Compute command pair explicitly rejects descriptor lease release.

Recovery of an incomplete Compute command now permits an outstanding
descriptor lease. Vulkan device idle must be confirmed before the
poisoned command/fence pair is replaced, after which the descriptor
lease can safely return to its stage-count cache. On ordinary or
canceled successful fence completion, the descriptor lease can be
returned before the command buffer lease without reallocating GPU
resources. Legacy GPU execution and its existing per-job cleanup
order remain unchanged.

The ARM64 Vulkan 1.0 device test completed two session lifetimes with
six descriptor acquisitions each, including early-release rejection,
command/fence reuse, a deliberate incomplete submission and recovery,
and readback through retained staging buffers. Both ARM ABIs passed
native compilation and linking; full Android APK packaging is deferred
until a later integration checkpoint.

## Explicit Compute submission ownership and cancellation (2026-11-04)

The reusable Vulkan Compute command/fence pair now supports one
session-managed queue submission per command lease. A cancellation
request records disinterest in the result but cannot cancel a
previously submitted Vulkan command. Only the real completion fence
retires that pending work; a VK_TIMEOUT poll retains the exclusive
command lease and does not permit premature descriptor or storage
reuse. A successful wait records either a canceled or an ordinary
completion and permits explicit normal lease release and subsequent
command buffer reuse. Driver errors preserve the existing poisoned
resource/recovery contract.

The original command-acquire/release path remains valid for existing
jobs using their own vkQueueSubmit and fence synchronization. The
production GPU backend stays independent. Physical Vulkan 1.0 tests
on ARM64 completed two separate session lifetimes, two cooperative
Compute cancellations, two normal Compute completions and readback
after command/fence reuse. No APK rebuilding is required for this
native lifecycle-only change.

## Cancellation-aware staging lifetime (2026-11-04)

The Vulkan session can now mark a submitted staging upload or download
as canceled without blocking the caller or altering the other GPU
backend. Vulkan queue work already submitted cannot be revoked. An
actual fence completion is therefore still required before the
transfer buffer and command pair can be reused. Pending cancellation
never clears ownership on VK_TIMEOUT, and canceled downloads are not
published or invalidated for CPU consumers.

The session tracks canceled submissions separately from completed
uploads/downloads. A completed canceled transfer permits subsequent
normal transfers on the same Vulkan logical device; the independent
pipeline and descriptor caches remain retained. The existing timed and
unlimited-wait APIs remain available, as does driver-verified recovery
for real failures. A Mali ARM64 hardware diagnostic verified both
cancellation directions and exact readback across two sessions. The
initial validation uses a standalone native build rather than
rebuilding every Android package.

## Nonblocking staging fence completion (2026-11-04)

An explicit staging completion API accepts a Vulkan nanosecond timeout.
A zero-timeout poll can return VK_TIMEOUT while retaining the exact
pending transfer, descriptor/storage ownership and same Compute
session. An already signaled fence completes immediately. Subsequent
unlimited-wait completion and explicit interrupted-submission recovery
remain available. No poll result triggers automatic fallback to the
other native backend.

Physical device validation included eight runs and sixteen independent
sessions with thirty-two zero-timeout Vulkan fence polls: eleven real
VK_TIMEOUT results and twenty-one immediate successes. Both paths
retained the correct buffer lifetime and validated GPU readback.

Full ARM64/ARMv7 native builds subsequently completed twelve real
frontend GPU information-mode sessions, including six forced-staging
runs and two mixed-runtime coexistence runs. Sixty public Compute
programs completed with independent queue fences, and every session
closed with zero unexpected recoveries. The optional dual Android
package retains all four original production ABI executables and
packages the new diagnostic ARM binaries independently, confirmed
through exact SHA-256 checks. A reusable physical-GPU test runner
validates timed fence results, resource ownership and complete
native session closure.

## Expanded Android GPU and in-app diagnostics (2026-11-04)

A third Android GPU completed four additional Vulkan 1.0 information
runs: ARM64 and ARMv7, each with direct and forced-staging storage.
Each run verified its public four-stage Compute chain, digest known
answers, descriptor/pipeline reuse, fence completion, and a healthy
session closing with zero recoveries. These are real device executions,
not replayed fixture evidence.

The installed Android 16 app was also exercised through its actual
backend-information dialog. The Vulkan diagnostic choice completed
GPU verification with native exit status 0 and the independent
executable. The legacy choice launched the original executable but
failed native platform discovery with status 255 on that device:
its public OpenCL entrypoint attempted to load a vendor graphics
dependency inaccessible to the app linker namespace. Direct ADB
shell tests use a different linker namespace; they do not prove that
vendor OpenCL is available within the app. The UI explicitly reports
the nonzero exit and retains the production backend unchanged.

## Dual native APK and runtime device-information selection (2026-11-04)

Android packaging now supports an opt-in dual native runtime. Four
published legacy ABI executables and their normal assets remain intact,
while ARM64 and ARMv7 additionally package a standalone Vulkan
information executable and matching Vulkan diagnostic asset tree.
The two executables and their writable shared-asset directories remain
independent. An information-only control selects the published GPU
information path or isolated Vulkan 1.0 Compute diagnostics at runtime.

The production workload launcher always selects the published
executable. Vulkan information excludes traditional GPU backends
and never registers production workload devices or target comparison.
Default APK builds contain only published binaries; diagnostic-only
APK selection remains available for earlier development workflows.

The dual APK passed exact SHA-256 checks for all four legacy executables
and two additional diagnostic ARM binaries, as well as ABI-specific
asset presence. Both GPU-information modes executed successfully from
the same APK on Mali ARM64 and Adreno ARMv7 physical devices, each
with a zero native exit status. Android Kotlin and resource compilation
also passed for the dual runtime.

## Independent staging lifetime and backend selection (2026-11-04)

The transfer queue pair has a separate pending/recovery lifetime from
the Compute command/fence pair. The synchronous transfer API remains
supported, while explicit submit/complete calls provide a path for
overlapping future CPU staging work with Vulkan submissions. An
incomplete or failed transfer rejects new Compute work and descriptor
or buffer changes until driver-confirmed recovery completes.

The native runtime retains independent loader/scheduler support for
the existing production GPU backends. The Android package defaults to
published legacy binaries unless diagnostic Vulkan inputs are selected
explicitly. Vulkan diagnostic selection does not claim production
Compute scheduling or displace the existing backend's execution path.

Six physical Vulkan 1.0/1.1 staging diagnostic runs completed twelve
full session lifetimes with real abandoned transfers and one recovery
per lifetime. Eight full frontend information-mode GPU sessions also
passed, including mixed-runtime sessions that retain the original GPU
loader. No successful frontend Compute job entered a recovery path.

## Failed Compute submission ownership and recovery (2026-11-03)

The executor passes its original Compute execution VkResult into job
cleanup. A successful device-idle cleanup does not erase a failed
dispatch result or return a failed command pair for reuse. Instead,
`backend_vulkan_session_command_release()` marks the command pair
unavailable. The explicit
`backend_vulkan_session_command_recover()` operation can restore it
only after a successful real device-idle wait and allocation of a new
primary command buffer and fence.

This preserves cached shader pipelines, descriptor sets, Compute
storage and the existing logical device across a recoverable
submission-interruption boundary. If the Vulkan driver fails the idle
wait or loses the device, the session remains unavailable until its
device ownership is closed. No driver failures are synthesized.

The real-device recovery matrix passed on Mali-G715 ARM64 and
Adreno 730 ARM64/ARMv7 for both Vulkan 1.0 and 1.1. Twelve
session lifetimes verified two genuine queue submissions apiece,
a fence-unobserved host abort, explicit poisoned-state recovery
and a post-recovery staging readback on the same logical device.
The native result from a failed job remains available to its caller.

Eight full information-mode GPU sessions also passed the shared
Compute regression suite with forty fence-completed jobs, zero
recovery calls and the expected pipeline/descriptor/command reuse.
This covers direct and forced-staged storage across the tested
ARM64/ARMv7 device combinations and mixed-runtime execution.

## Per-job fence synchronization (2026-11-03)

A synchronous session-backed program records its real queue submission
and signals completion only after vkWaitForFences succeeds for the leased
Compute fence. A job with a completed fence releases its temporary
resources and returns descriptor and command leases without waiting for
the entire VkDevice to idle. Jobs with an outstanding submission use a
device-idle recovery wait during cleanup; a failed idle wait prevents
unsafe command lease reuse.

Standalone diagnostics retain their independent VkDeviceWaitIdle
cleanup for fault coverage. The common session still waits for device
idle at final closure. Hardware verification requires five successful
Compute completion records per program suite with wait_idle=0, preserving
the exact output and resource ownership checks already performed.

## Reusable descriptor bindings (2026-11-03)

For a session-backed program, the executor obtains descriptor sets from
a pool sized for the program's exact stage count. A one-, two- or
four-stage program receives its corresponding retained set array.
The session validates that every source/destination buffer exists in
its storage slots and the logical byte ranges fit those allocations.
Both storage bindings are refreshed on every acquisition, including
when buffers are replaced by later capacity growth.

An exclusive descriptor lease prevents stale bindings from being
reallocated or modified during GPU execution. The job returns the lease
after completed GPU fence synchronization, keeping the pool and set handles available for
the next program. Standalone fault diagnostics retain the original
one-job descriptor creation, ownership and cleanup behaviour.

The current physical-device descriptor-cache validation completed eight
information-mode runs, covering Vulkan-only and mixed-runtime execution,
direct and staged memory and both ARM ABIs. Each verified five programs
using three stage-count descriptor pools, two pool reuses and 18
descriptor writes. Descriptor pools/sets and their session-owned layouts
were released after GPU completion. Separate Vulkan 1.0/1.1 diagnostics
completed 12 resource lifetimes with exact binding reuses and invalid
lease rejection.

## Session-backed reusable Compute resources (2026-11-03)

The information-mode executor borrows a persistent VkDevice, Compute queue,
retained storage slots, exact SPIR-V-keyed pipelines, a shared descriptor
and pipeline layout, and an exclusively leased command buffer/fence.
It borrows cached descriptor sets matching the declared stage count
and updates each set to the current source and destination storage
buffers and exact byte ranges. After GPU completion, it returns both
descriptor and command leases.

Pipelines are cached by comparing complete immutable SPIR-V words, allowing
stages shared across distinct public graphs to reuse one compiled VkPipeline.
A separate command pair remains available for staging transfers.
Standalone public fault diagnostics retain their independent Vulkan
resources, lifecycle and deterministic fault checkpoints.

The November 3 real-GPU matrix completed eight integrated information-mode
sessions, including direct and forced-staging storage on ARM64/ARMv7 and
mixed-runtime execution on Mali ARM64. Every session processed nine
declared Compute stages using four cached pipelines and five cache hits;
five command leases reused one buffer/fence pair four times. All program
outputs and final Vulkan resource-close counters passed verification.

## Scope

The Vulkan Compute executor consumes explicit `backend_vulkan_program_t`
declarations for public one-stage, two-stage and four-stage graphs and for
the current cryptographic batch programs. Shader modules, buffer ranges and
stage dependencies come from the declaration rather than hard-coded stage
numbers.

The public verification graphs are:

```text
public_u32_transform:
  buffer 0 -> buffer 1

public_u32_transform -> public_u32_rotate:
  buffer 0 -> buffer 1 -> buffer 2

public_u32_transform -> public_u32_rotate -> public_u32_rotate -> public_u32_rotate:
  buffer 0 -> buffer 1 -> buffer 2 -> buffer 3 -> buffer 4
```

Each declaration supplies the immutable SPIR-V array, exact word count,
source/destination storage buffer indices, local workgroup dimensions
and dispatch group counts.

Before resources are acquired, the program is validated against the actual
selected GPU's `VkPhysicalDeviceLimits` using
`backend_vulkan_program_validate()`. An active backend session supplies
the existing logical device, queue and persistent direct-or-staged storage.
Standalone diagnostics create their own temporary logical device and
direct-mapped storage. The same
validated declaration is consumed by descriptor writes, shader-module
creation, dispatch recording and inter-stage barriers.

No HASHCAT cryptographic shader, password candidate, target hash, salt,
attack mode or normal hashing scheduler is connected to this executor.
The integration remains information-mode verification only.

## Executor contract

### Static program declarations

`source/src/backend_vulkan_kernel.c` owns two immutable declarations.

The one-stage declaration contains one stage, two buffers and a
256-byte logical descriptor range.

The two-stage declaration contains two stages, three buffers and the
same 256-byte range. Stage two explicitly declares buffer 1 as its
source, matching stage one's destination.

Both stages declare:

```text
local_size = (32, 1, 1)
group_count = (2, 1, 1)
```

The declarations reference the complete generated SPIR-V 1.0 arrays:

```text
backend_vulkan_public_spirv
backend_vulkan_public_stage2_spirv
```

### Physical-device validation

`backend_vulkan_kernel_verify_program()` first uses the backend Vulkan
catalogue to select a physical device and retrieve its real properties.

The selected program then passes through:

```c
backend_vulkan_program_validate (
  program,
  &selected.properties.limits,
  diagnostics
);
```

A structural or unsupported-limit result is returned before
`vkCreateDevice`.

On the verified Pixel Mali-G715 process, the resulting event is:

```text
BACKEND_VULKAN_PROGRAM_ACCEPT stages=1 buffers=2 bytes=256 descriptors_per_stage=2
```

or:

```text
BACKEND_VULKAN_PROGRAM_ACCEPT stages=2 buffers=3 bytes=256 descriptors_per_stage=2
```

### Resource ownership

`backend_vulkan_public_job_t` stores a pointer to the exact validated
program. A session-backed job borrows `VkDevice`, Compute queue and the
required persistent storage slots. A slot can be direct mapped or can pair
descriptor-bound Compute storage with a host-visible transfer buffer. The job
owns temporary host arrays while borrowing stage-count descriptor
pools/sets, cached Compute pipelines, compatible layouts and a reusable
command buffer/fence from the session. Exact per-stage descriptor
bindings are updated before GPU dispatch, and leases are returned after
completion. Standalone diagnostics own their temporary device,
descriptors, shaders, pipelines, commands and direct-mapped storage.

`backend_vulkan_public_release()` reads the program stage/buffer counts
when releasing job-owned descriptor, shader and command resources. Cached
pipelines, command leases and storage remain owned by the session.
The session releases cached pipelines, command handles and retained
storage before destroying `VkDevice`, while the catalogue owns
`VkInstance`.

The executor accepts the complete validated linear declaration used by the
one-stage, two-stage and four-stage public graphs as well as the one-stage
cryptographic batch programs.

### Descriptor and pipeline construction

`backend_vulkan_public_pipeline_open()` consumes each declared stage.

For every stage it uses:

```text
source_buffer_index      -> descriptor binding 0
destination_buffer_index -> descriptor binding 1
spirv + spirv_words      -> VkShaderModule
```

It no longer chooses the module or descriptor buffers from
`stage == 0` / `stage == 1` conditions.

Every pipeline still uses entry point `main`, two
`VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` bindings, no optional features,
no extensions and no push constants.

### Dispatch recording

`backend_vulkan_public_dispatch()` writes the public input to the
source buffer declared by stage zero.

For each later stage, the method inserts a buffer barrier on that
stage's declared source:

```text
srcAccess = VK_ACCESS_SHADER_WRITE_BIT
dstAccess = VK_ACCESS_SHADER_READ_BIT
srcStage  = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
dstStage  = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
```

The pipeline and descriptor set for that exact program stage are bound,
then `vkCmdDispatch` uses its declared three-dimensional `group_count`.

Final shader-write-to-host-read barriers target each stage's declared
destination buffer. A real Vulkan fence completes before noncoherent
memory invalidation and CPU comparison.

The arithmetic verifier still independently checks all 64 values
produced by each public shader. Command recording follows the program
declaration; the CPU arithmetic is intentionally specific to these
two diagnostic modules.

## Build integration

When `ENABLE_VULKAN_DISCOVERY=1` is selected for Android,
`source/src/Makefile` now links:

```text
obj/backend_vulkan_program.NATIVE.o
```

beside the existing catalogue, session and public-kernel executor
objects.

The standalone public-kernel and public-chain build scripts also link
`source/src/backend_vulkan_program.c` so they exercise the same
validator used by the full HASHCAT process.

Both complete Android frontends were built for API24 targets:

```text
arm64-v8a
armeabi-v7a
```

The ARMv7 executable was compiled and its exact SHA-256 archived, but
this current revision was not run on physical ARMv7 hardware because
the RedMagic device was disconnected.

## Physical Pixel validation

The current source revision was executed on Pixel 9 Pro XL / Mali-G715.

### Standalone production-linked diagnostic

Two real Android processes were saved:

```text
requested Vulkan 1.0
requested Vulkan 1.1
```

Each process opens two independent Vulkan catalogue/device lifecycles.
Each lifecycle performs:

1. real physical-device selection;
2. exact program validation against driver limits;
3. creation of the two declared pipelines and descriptor sets;
4. two real `vkCmdDispatch` calls;
5. shader-write-to-shader-read synchronization;
6. real fence completion;
7. CPU verification of 64 intermediate plus 64 final words;
8. complete temporary VkDevice teardown before VkInstance close.

Across both native processes:

```text
program validations: 4
vkCmdDispatch calls: 8
public uint32 words verified: 512
```

### Full HASHCAT information mode

A newly built ARM64 `hashcat -I` process completed with native exit zero.

It performed:

```text
1 empty Vulkan session verification
1 one-stage program validation
1 one-stage vkCmdDispatch
1 two-stage program validation
2 two-stage vkCmdDispatch calls
192 total public uint32 words verified
normal OpenCL Mali-G715 enumeration
```

The process emitted both program-accept events before their respective
temporary device executions and later printed the ordinary OpenCL
backend information.

Across the standalone and full-HASHCAT current-revision evidence:

```text
real Android processes: 3
program validations against real GPU limits: 6
real vkCmdDispatch calls: 11
public uint32 words verified: 704
```

## Evidence and independent verification

Permanent evidence is stored in:

```text
tests/fixtures/android-gpu/vulkan-backend-program-executor-20261101/
  manifest.json
  pixel-arm64-1.0.log
  pixel-arm64-1.1.log
  pixel-arm64-hashcat-program-info.log
  pixel-arm64-negative-controls.log
```

Archive the current build products and saved process logs:

```powershell
python -B tools/archive-vulkan-backend-program-executor-evidence.py
```

Verify source identities, Android ARM64/ARMv7 native diagnostics,
production backend objects, complete HASHCAT executables, module
artifacts and saved Pixel traces:

```powershell
python -B tools/test-vulkan-backend-program-executor-evidence.py --native build/diagnostic-multimode/vulkan-backend-program-executor-20261101 --full build/diagnostic-multimode/vulkan-hashcat-program-executor-20261101 --readelf D:\ANDROID\SDK\ndk\29.0.14206865\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe
```

The verifier requires Vulkan shader/pipeline/dispatch symbols in the
native GPU executables and complete HASHCAT frontends. It also rejects
deliberately falsified validation, dispatch, barrier, scheduling and
OpenCL-device records.

## Historical hardware evidence

The earlier two-stage evidence contains physical runs on:

```text
RedMagic Adreno 730 ARM64
RedMagic Adreno 730 ARMv7
Pixel Mali-G715 ARM64
```

Those records remain tied to commit `5a62e5d`. Their verifier resolves
source hashes from the commit containing that exact saved manifest, so
current executor refactoring does not relabel historical hardware output
as current-revision execution.

The current program-driven executor evidence intentionally claims only
the Pixel ARM64 runtime tested after this integration.

## Verification boundary

The declaration object is now connected to public Compute execution,
but the ordinary HASHCAT hashing scheduler remains separate.

This milestone does not port a password-processing OpenCL kernel to
SPIR-V, load arbitrary external shaders, accept attack-mode buffers,
or register Vulkan as a hashing backend.

The follow-on
[dynamic four-stage Vulkan executor](vulkan-backend-dynamic-executor-android.md)
allocates descriptor/pipeline/buffer ownership arrays directly from
validated program counts and verifies a four-stage synthetic uint32
chain on Adreno ARM64/ARMv7 and Mali ARM64. The evidence in this
document remains tied to its original Pixel-only source revision.
