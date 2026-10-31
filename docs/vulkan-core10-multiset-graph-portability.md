# Vulkan 1.0: explicit multi-stage graphs with multiple descriptor sets

Date: 2026-10-31

## Scope

This stage uses the existing `vulkan_core10_multiset_t` resource owner to
execute **one, two or three consecutive Vulkan 1.0 Compute pipelines**
through **one or two explicit descriptor sets**, each containing
`VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` and
`VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER` descriptors.

The new command recorder is independent of GPU manufacturer and driver version.
It accepts caller-declared pipeline indices, stage dependencies, storage
buffers, mapped transfer buffers, per-stage readbacks and dispatch sizes.
All commands run on one Vulkan 1.0 Compute queue with one reusable command
buffer and fence. It uses no extensions, optional device features, GPU-model
presets, or Vulkan 1.1+ API calls.

This is **synthetic public integer Compute**, not a working hashcat Vulkan
backend, password algorithm or bcrypt implementation. The existing OpenCL
mode-3200 failure is a separate issue.

## Source modules and documented contracts

| File | Responsibilities |
| --- | --- |
| `tools/vulkan-core10-multiset-graph.h` / `.c` | Validate and record a caller-declared sequential Compute graph into the existing multi-set runtime |
| `tools/vulkan-core10-multiset-graph-diagnostic.c` | Create one to three real pipelines, execute host-owned jobs through the synchronous port, independently verify per-stage readbacks with scalar CPU results and reject invalid job calls |
| `tools/vulkan-core10-job-port.h` / `.c` | Open one reusable Compute graph, validate caller-owned job buffers, synchronize mapped memory, submit once and return all stage outputs to host callers |
| `tools/vulkan-multiset-graph.comp` | Build-time-selected public integer shader using sparse storage bindings and one std140 `uvec4` uniform block |
| `tools/build-vulkan-core10-multiset-graph.ps1` | Build Android API 24 ARM64/ARMv7 executables and 12 Vulkan 1.0 SPIR-V variants |
| `tools/run-vulkan-core10-multiset-graph.ps1` | Execute real Android GPU runs with exact ABI selection, resource cleanup, optional noncoherent memory and opt-in fault injection |
| `tools/archive-vulkan-core10-multiset-graph-evidence.py` | Preserve original LF-only GPU logs and SHA-256 identities |
| `tools/test-vulkan-core10-multiset-graph-evidence.py` | Independently audit hardware records, descriptor layouts, barriers, memory plans, output hashes and deliberate record corruptions |

The public methods are:

- `vulkan_core10_multiset_graph_validate(runtime, buffer_specs, buffer_count, graph, diagnostics)`: Checks every index, declared usage, logical buffer size, mapped host buffer, pipeline handle, explicit stage-to-stage dependency, distinct readback, dispatch dimension and current physical GPU workgroup limit. Returns `VK_ERROR_INITIALIZATION_FAILED` for invalid graph declarations or `VK_ERROR_FEATURE_NOT_PRESENT` when a required dispatch exceeds the physical device's limit. It makes no Vulkan driver calls.
- `vulkan_core10_multiset_graph_record(runtime, buffer_specs, buffer_count, graph, diagnostics)`: Records one host→GPU transfer, the caller's ordered Compute stages, explicit shader-write→shader-read buffer barriers, all GPU→host readbacks and transfer→host visibility. Uses `vulkan_core10_multiset_begin` and `vulkan_core10_multiset_end`; the caller controls submission and cache synchronization.

The existing resource owner creates descriptor sets and pipelines, owns all
VkBuffer/VkDeviceMemory objects, and releases partially initialized resources
on errors. The graph recorder operates on the objects that owner creates; it
does not duplicate memory planning or create a vendor-specific backend.

The separate `vulkan_core10_job_port_t` is the **host-facing synchronous task
interface** used by this diagnostic. Its `job_port_open` records the graph
once. Each `job_port_submit` accepts independent caller-owned input and
uniform arrays, synchronizes GPU memory, reuses the command buffer and
copies every completed stage output into caller-owned destinations.
`job_port_close` releases the CPU-side readback metadata **before** the
Vulkan owner is destroyed. Full API contracts and lifecycle mapping to the
future hashcat device/session integration are documented in
`docs/vulkan-core10-job-port-integration.md`.

## Explicit graph geometry

The diagnostic intentionally exercises all combinations below:

| Stages | GPU buffers | Host-visible buffers | Total allocations | Storage descriptors | Uniform descriptors |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 2 | 3 | 5 | 2 | 1 |
| 2 | 3 | 4 | 7 | 3 | 1 |
| 3 | 4 | 5 | 9 | 4 | 1 |

There is one GPU input, one GPU output per stage, one CPU upload, one CPU
download per stage and one CPU-mapped uniform buffer. Each GPU buffer has
2,048 `uint32` elements (8,192 logical bytes); the uniform buffer has 16
logical bytes and uses std140 `uvec4` alignment.

Binding numbers within the descriptor sets are deliberately sparse:

| Resource | Set when one set | Set when two sets | Binding |
| --- | ---: | ---: | ---: |
| GPU input | 0 | 0 | 2 |
| Output A | 0 | 1 | 4 |
| Output B | 0 | 1 | 6 |
| Output C | 0 | 1 | 8 |
| Uniform parameters | 0 | 1 | 10 |

Only output bindings for the requested number of stages are present.
Each stage consumes the preceding GPU buffer and produces a distinct output.
All pipelines use one compatible `VkPipelineLayout` and the same bound sets.

The command sequence is:

1. HOST_WRITE → TRANSFER_READ and UNIFORM_READ via a Vulkan 1.0 memory barrier.
2. Upload staging → GPU input via `vkCmdCopyBuffer`.
3. TRANSFER_WRITE → SHADER_READ via `VkBufferMemoryBarrier`.
4. Bind the explicit descriptor sets. For each stage, bind its distinct
   Compute pipeline and dispatch 64 × 1 × 1 workgroups of 32 invocations.
   Between adjacent stages, issue a shader-write→shader-read
   `VkBufferMemoryBarrier` on the predecessor's output.
5. COMPUTE_SHADER_WRITE → TRANSFER_READ via a Vulkan 1.0 memory barrier.
6. Copy **each** stage output into a separate CPU-visible download buffer.
7. TRANSFER_WRITE → HOST_READ via a Vulkan 1.0 memory barrier.
8. For each caller-owned job, validate the exact upload, uniform and output
   lengths before any GPU submission. The task port copies input and uniform
   bytes, flushes noncoherent allocations when applicable, submits the *same*
   recorded command buffer, waits for the fence and invalidates all readbacks.
   The diagnostic independently verifies every output word of every stage.

This uses core Vulkan 1.0 pipeline layout/descriptor binding semantics and
`vkCmdPipelineBarrier` synchronization, consistent with the
[Khronos descriptor sets specification](https://docs.vulkan.org/spec/latest/chapters/descriptorsets.html)
and [Khronos pipeline barrier reference](https://docs.vulkan.org/refpages/latest/refpages/source/vkCmdPipelineBarrier.html).

## Reproducible build

Build using the Android NDK already installed on the PC:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/build-vulkan-core10-multiset-graph.ps1 -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865
```

The build compiles both `aarch64-linux-android24` and
`armv7a-linux-androideabi24`, validates all 12 public shader modules
under Vulkan 1.0 and 1.1, and checks that neither Android executable
depends on the optional `vkEnumerateInstanceVersion` symbol.

Compile-time options `GRAPH_STAGE=0..2` and `GRAPH_SETS=1..2` select an
explicit matching shader interface. Each of six layouts has optimized and
unoptimized SPIR-V binaries. The CPU-supplied parameters vary for each
sequential command-buffer submission.

## Confirmed on actual hardware

The exact one-, two- and three-stage graph runs on three device/ABI
configurations. The default Vulkan version is **1.0**, while 1.1 is
requested only during optional compatibility checks.

| Device / process ABI | Real successful GPU runs | Forced noncoherent memory |
| --- | ---: | --- |
| RedMagic / Adreno 730 / ARM64 | **36 PASS** | 12 exact incompatible-memory refusals |
| RedMagic / Adreno 730 / ARMv7 | **36 PASS** | 12 exact incompatible-memory refusals |
| Pixel / Mali-G715 / ARM64 | **48 PASS** | 12 real GPU runs using noncoherent mappings and flush/invalidate |

Across these three configurations, the verified evidence contains **120
successful GPU runs**, **480 fence-separated submissions**, **1,966,080
independently CPU-verified 32-bit stage outputs**, **24 explicit
unsupported-memory refusals**, **186 real post-acquisition cleanup
tests** and **30 rejected malformed CLI controls**.

Each successful GPU run checks all 2,048 words in every active stage
against the CPU reference, not just the final stage. In the Pixel forced
noncoherent mode, Vulkan reports `nonCoherentAtomSize = 64`, and each
submission invokes actual driver flush calls on the upload and uniform
allocations and actual invalidate calls on each readback.

RedMagic's main Vulkan heap is reported as **11,584,221,184 bytes** by
its ARM64 process and **4,294,967,296 bytes** by ARMv7. The independent
validator requires these distinct archived ABI observations and verifies
each run's heap reservations against the buffer allocations reported by
that process, without using the model of the phone to select allocations.

The 186 post-acquisition fault tests cover **23 acquisition points** of
the one-stage, one-set graph and **39 acquisition points** of the
three-stage, two-set graph on each of the three process configurations.
Every injected failure returns before dispatch and frees the precisely
acquired Vulkan resources. These are controlled application-level
failure points, not actual GPU device loss.

The GPU job port additionally rejected **680** invalid host-facing calls
before submission: five for every single-stage configuration and six for
every two-/three-stage configuration. These include incorrect byte counts,
missing outputs, duplicate stage destination pointers and opening an active
port twice. The independent validator checks the `JOB_PORT_READY`,
`JOB_PORT_INVALID_CONTROLS`, `JOB_PORT_SUBMIT` and `JOB_PORT_CLOSE` event
sequence and rejects **ten** deliberately corrupted GPU/task/cleanup records,
including a missing Pixel noncoherent flush and an inaccurate ARMv7
resource cleanup ledger. A recorded
three-stage, two-set graph acquired and released exactly 39 Vulkan objects:
nine GPU/host buffers, nine allocations, five mappings, two set layouts,
two descriptor sets, three shader modules, three Compute pipelines and
the remaining command/pipeline objects.

Selected independent FNV-1a64 checksums:

| Submission | Stage A | Stage B | Stage C |
| --- | --- | --- | --- |
| 0 | `da420145579e3ab0` | `2e11a9b4e2906b5d` | `78fd6584d78a2c98` |
| 1 | `3dc96f338cbbffac` | `55af4d53d195f1b5` | `e35d96f4d5d34829` |
| 2 | `fc6cfc42aeaff265` | `1d55deed24fd7c1f` | `7c575a1d46ede826` |
| 3 | `00ba58c7ff9a53e2` | `ebe62698c3a64353` | `007be5e7346adf5a` |

The same checksums appear for optimized and nonoptimized variants and both
descriptor layouts. The independent Python verifier calculates all 12 values
from the public formula rather than trusting C diagnostic output.

The original 150 LF-only GPU, fault and CLI logs and a source/build SHA-256 manifest are saved at:

`tests/fixtures/android-gpu/vulkan-multiset-graph-20261031/`

Run the complete hardware matrix on a connected Android GPU, using its actual
ADB serial and one explicitly supported ABI:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/run-vulkan-core10-multiset-graph.ps1 -Serial $DeviceAdbSerial -Abi auto -ReportPrefix device-arm -TestCore11 -TestNoncoherent -TestFaults
```

The runner selects ARM64 or ARMv7 from `ro.product.cpu.abilist`.
For RedMagic ARMv7 specify `-Abi armeabi-v7a`. For both RedMagic and
Pixel, Vulkan 1.0 is the baseline; a device unable to allocate
`HOST_VISIBLE` noncoherent staging and uniform memory receives an
explicit `VK_ERROR_FEATURE_NOT_PRESENT` without Compute dispatch.

To verify archived hardware observations and exact compiled artifacts:

```powershell
python -B tools/test-vulkan-core10-multiset-graph-evidence.py --artifacts build/diagnostic-multimode/vulkan-multiset-graph-20261031
```

Without `--artifacts`, this checker works offline using the source and
archived logs; it never invokes Android or Vulkan.

## Known boundaries

- The three-stage graph runs on RedMagic ARM64, RedMagic ARMv7 and Pixel
  ARM64. A physically Vulkan 1.0-only GPU and an Android 7.0/API 24
  system were not available; these tests request Vulkan 1.0 on newer drivers.
- Pixel's noncoherent multi-stage path completes actual GPU work with
  driver `flush`/`invalidate`. RedMagic rejects unsupported forced memory
  requirements, and its normal coherent pathway completes the same computation.
- The 186 fault cases stop after successful native resource acquisitions,
  rather than inducing actual device loss or proving driver-internal leak
  absence. Vulkan validation-layer execution was not tested.
- Descriptor types remain `STORAGE_BUFFER` and `UNIFORM_BUFFER`; multiple
  queues, concurrent command submissions, push constants, images, dynamic
  descriptors, pipeline-specific layouts, SPIR-V reflection and device-loss
  recovery are not implemented.
- This is resource infrastructure and a noncryptographic public integer
  test. It is not connected to hashcat's device scheduling, module dispatch,
  candidate preparation or cryptographic kernels. Neither bcrypt/OpenCL
  error 245 nor the separate compiler failure 139 is resolved.
