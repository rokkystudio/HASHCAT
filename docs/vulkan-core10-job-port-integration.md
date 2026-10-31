# Vulkan 1.0: synchronous public Compute job port

Date: 2026-10-31

## Purpose

`tools/vulkan-core10-job-port.h` and
`tools/vulkan-core10-job-port.c` define an actual, hardware-tested host
task interface above the existing Vulkan 1.0 multi-set resource owner and
multi-stage graph recorder.

This port separates **caller-owned job data** from **GPU resource ownership**.
It can execute any explicitly compiled compatible noncryptographic Compute
graph that the existing resource owner and graph validator accept. The port
does not select a hash algorithm or perform password/hash processing.

This is a prerequisite for a future hashcat-facing device/session integration,
not yet a production hashcat backend. The existing CUDA, HIP, Metal and
OpenCL paths remain unchanged.

## Public interface and ownership

`vulkan_core10_job_port_t` borrows `vulkan_core10_multiset_t`. It owns one
host allocation: a copy of each declared stage's readback buffer index.
The Vulkan instance, physical device, queue, allocations, descriptor sets,
pipelines, command pool, primary command buffer and fence belong to the
existing runtime/caller, **not** to the port.

The caller allocates and initializes its Vulkan runtime and stage graph,
then opens the port once:

`vulkan_core10_job_port_open(port, runtime, buffer_specs, buffer_count, graph, diagnostics)`

- Requires a zero-initialized, unused port, a fully created Vulkan owner,
  caller-provided buffer usage/size declarations and a non-NULL diagnostics
  stream.
- Calls `vulkan_core10_multiset_graph_record`, which validates stage
  indices, binding-independent declared buffer use, workgroup limits and
  dependencies, then records all Vulkan 1.0 commands into one reusable
  primary command buffer.
- Copies the readback indices from the supplied graph so the caller's
  stage/readback array no longer needs to remain allocated.
- Publishes exact input/uniform byte counts and stage count to the port.
- Returns a concrete `VkResult` on failure and frees the port allocation.
  The caller still owns and destroys the existing Vulkan runtime.

Each caller-owned job is submitted by:

`vulkan_core10_job_port_submit(port, input, input_bytes, uniform, uniform_bytes, outputs, output_count, output_bytes)`

- Validates a ready port, a recorded non-pending command buffer, non-NULL
  pointers, exact input/uniform/output sizes, one output pointer per stage
  and distinct output pointer values **before** modifying mapped memory.
  An invalid call returns `VK_ERROR_INITIALIZATION_FAILED` without
  dispatching any GPU work or advancing the job counter.
- Copies public input and std140 uniform bytes to the runtime's mapped
  host-visible allocations.
- Invokes `vulkan_core10_multiset_sync` for upload and uniform data.
  Noncoherent allocations perform actual Vulkan flush operations.
- Resubmits the same recorded command buffer, synchronously waits for its
  completion fence, invalidates every host-visible stage readback when
  noncoherent, and copies all stage outputs into caller-owned CPU buffers.
- Increments `jobs_completed` only after **every** stage readback succeeds.
  A Vulkan synchronization or submission failure sets `failed` and
  blocks subsequent submissions until the port is closed.

`vulkan_core10_job_port_close(port)` releases the copied readback indices,
zeroes all port state and leaves the Vulkan resources intact. The caller
then executes `vulkan_core10_multiset_destroy` and destroys its instance.
Closing an already empty port succeeds; passing NULL fails.

`VkResult` values are propagated unchanged. No retry, silent downgrade,
vendor-specific device selection, asynchronous queue, implicit allocation,
hashcat scheduler callback or shader reflection is implemented.

## Verified task lifecycle

```text
Caller-owned synthetic input and std140 uniforms
    |
    v
job_port_submit() -> validate exact input and output contract
    |
    +-- reject invalid arguments before Vulkan submission
    |
    +-- copy input/uniform to mapped HOST_VISIBLE allocations
    |
    +-- flush only when driver memory is noncoherent
    |
    +-- submit existing graph command buffer + wait for fence
    |
    +-- invalidate every stage's mapped readback when noncoherent
    |
    +-- copy each completed stage output to caller-owned buffers
    |
    v
Caller verifies all CPU-visible output words independently
```

The device-specific memory types and declared capacities are always read
from `vkGetPhysicalDeviceMemoryProperties` and the driver's
`VkMemoryRequirements`. The command sequence and the stage shaders are
independent of the selected GPU name.

The job port relies on the graph's existing Vulkan 1.0 barriers:

- CPU host writes to transfer/uniform reads.
- Transfer uploads to shader reads.
- Stage-to-stage shader writes to shader reads.
- Compute stage output to transfer downloads.
- Transfer downloads to host reads.

The last barrier and fence completion precede the corresponding CPU-visible
readback.

## Tests on physical Android GPUs

`tools/vulkan-core10-multiset-graph-diagnostic.c` now opens the job port
for every successful runtime instance. It supplies independent host input,
uniform and per-stage output arrays, invokes the port for four consecutive
jobs, and compares all 2,048 words per stage with the existing independent
scalar CPU oracle.

Each successful runtime additionally submits five invalid CPU-side job
requests, and six requests when two or three stages are active:

1. Input shorter than the declared upload length.
2. Uniform buffer shorter than the declared std140 length.
3. Too few readback destinations.
4. Missing output pointer array.
5. Attempting to reopen an already active job port.
6. Reusing the same destination pointer for two stages (two/three-stage
   graphs only).

Every such invalid job is rejected before any GPU submission and does not
advance the job completion count. After the valid jobs, the port closes
before the Vulkan runtime releases its resources.

| Actual hardware/ABI | Successful task-graph runs | Completed GPU jobs | Host-side invalid jobs |
| --- | ---: | ---: | ---: |
| RedMagic / Adreno 730 / ARM64 | 36 | 144 | 204 |
| RedMagic / Adreno 730 / ARMv7 | 36 | 144 | 204 |
| Pixel / Mali / ARM64 | 48 | 192 | 272 |
| **Total** | **120** | **480** | **680** |

All successful tasks independently verify every active stage output,
totalling **1,966,080 public 32-bit values**. Pixel runs 12 exact
noncoherent Vulkan 1.0 scenarios with real driver
`vkFlushMappedMemoryRanges` and `vkInvalidateMappedMemoryRanges` calls.

Additional unchanged controls in the real Vulkan runtime and runner:

- **186** injected post-acquisition partial-resource cleanup trials.
- **24** explicit, unsupported forced noncoherent-memory cases on RedMagic.
- **30** invalid Android command-line input cases.
- **10** independently rejected GPU/log mutation cases, including corrupt
  task contract reports, job completion flags, noncoherent cache calls and
  Vulkan resource cleanup counts.

The actual original Android logs are stored in
`tests/fixtures/android-gpu/vulkan-multiset-graph-20261031/`, containing
**150 LF-only logs**, with complete source SHA-256 and Android
ARM64/ARMv7/SPIR-V build identities.

## Build and evidence reproduction

Compile using the installed NDK targeting Android API 24:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/build-vulkan-core10-multiset-graph.ps1 -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865
```

Run the GPU matrix on a connected device using its actual ADB serial:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/run-vulkan-core10-multiset-graph.ps1 -Serial $DeviceAdbSerial -Abi auto -ReportPrefix device-arm -TestCore11 -TestNoncoherent -TestFaults
```

Choose `-Abi armeabi-v7a` explicitly for a device's 32-bit process.
The runner verifies each real GPU run and stores the original log.

Verify the archived GPU outputs, cache synchronization, task contract and
native binaries without accessing Android:

```powershell
python -B tools/test-vulkan-core10-multiset-graph-evidence.py --artifacts build/diagnostic-multimode/vulkan-multiset-graph-20261031
```

The offline verifier recalculates every public per-stage FNV-1a64 checksum
from a Python integer oracle. It checks the new `JOB_PORT_READY`,
`JOB_PORT_INVALID_CONTROLS`, `JOB_PORT_SUBMIT` and
`JOB_PORT_CLOSE` events as well as the existing descriptor, memory
heap, cache atom, stage barrier, acquisition-fault and fence events.

## Integration boundary within hashcat

Relevant existing host-side lifecycle functions in
`source/src/backend.c` and `source/include/backend.h` are:

| Production owner | Conceptual future Vulkan responsibility |
| --- | --- |
| `backend_ctx_init` | Load and validate Vulkan backend availability |
| `backend_ctx_devices_init` | Enumerate suitable Vulkan physical devices and identify compute queues |
| `backend_session_begin` | Establish device-owned Vulkan resources and a validated Compute session |
| `backend_session_destroy` | Close the active job port before releasing the device and instance |
| `backend_ctx_devices_destroy` | Release per-device Vulkan data and cross-backend discovery state |

These are **integration targets, not active call sites**. No production
hashcat source file links to the new port yet. The port does not accept
`hashcat_ctx_t`, `hc_device_param_t`, password candidates or hashes.

A production backend still needs explicit device discovery and selection
alongside CUDA/HIP/Metal/OpenCL, per-device ownership in hashcat's
`backend_ctx_t`, a dedicated kernel interface, scheduler integration,
error reporting, event accounting, autotuning, Android packaging and
runtime selection. Those steps must be implemented and tested separately.

## Limits

- Vulkan 1.0 core commands; no Vulkan 1.1+ requirement. No physically
  Vulkan-1.0-only GPU or Android OS API-24 device was available to test.
- Synchronous jobs on a single queue and one reusable primary command
  buffer. No concurrent submit queue, timeline semaphore, device loss
  recovery or queued-task cancellation.
- No dynamic descriptor offset, per-stage pipeline-layout switch or SPIR-V
  reflection. The caller provides the exact compatible shader/descriptor
  graph and immutable resource specifications.
- The task port does not own GPU resources or validate shader semantics.
  It validates host-facing byte sizes and output buffer pointer identity.
- No production cryptographic kernel, OpenCL bcrypt workaround, password
  candidate generation or offline attack workflow is implemented.
