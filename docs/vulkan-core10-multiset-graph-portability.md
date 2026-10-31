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
| `tools/vulkan-core10-multiset-graph-diagnostic.c` | Create one to three real pipelines, prepare host/GPU transfers, synchronize mapped memory and independently compare all per-stage readbacks with scalar CPU results |
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
on errors. The new recorder operates on the objects that owner creates; it
does not duplicate memory planning or create a vendor-specific backend.

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
8. For each submission, update upload and uniform bytes, flush noncoherent
   mapped allocations when applicable, submit the *same* recorded command
   buffer, wait for the fence, invalidate noncoherent readbacks when applicable,
   and independently verify every output word of every stage.

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

RedMagic / Adreno 730 / **ARM64** was rerun with the new multi-stage
and multi-set recorder. The requested Vulkan version was 1.0 by default;
Vulkan 1.1 was exercised only by explicit opt-in.

| Real RedMagic ARM64 trial | Result |
| --- | --- |
| 1, 2, 3 stages × 1, 2 descriptor sets × default 1.0, explicit 1.0, explicit 1.1 × O0 / O | **36 GPU runs PASS** |
| Four completed, fence-separated command submissions per successful run | **144 PASS** |
| CPU-verified output values across all stage readbacks and submissions | **589,824 32-bit words** |
| Forced HOST_VISIBLE without HOST_COHERENT | **12 exact unsupported-memory refusals**, no Compute dispatch |
| Malformed CLI controls | **10 exit-code-2 rejections** |
| Independently rejected altered GPU logs | **5 mutation checks** |

Every one of the 36 successful GPU runs verified all 2,048 words in every
active stage against the CPU reference, not just the final stage. A recorded
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

The original 49 LF-only logs and a source/build SHA-256 manifest are saved at:

`tests/fixtures/android-gpu/vulkan-multiset-graph-20261031/`

To verify the archived real GPU observations and exact compiled artifacts:

```powershell
python -B tools/test-vulkan-core10-multiset-graph-evidence.py --artifacts build/diagnostic-multimode/vulkan-multiset-graph-20261031
```

Without `--artifacts`, this checker works offline using the source and
archived logs; it never invokes Android or Vulkan.

## Known boundaries

- The new three-stage graph was actually executed on **RedMagic ARM64 only**.
  RedMagic ARMv7 and Pixel ARM64 binaries compile, but this stage does not
  claim fresh executions on those configurations. A Pixel attempt was not
  completed in this pass.
- No GPU physically limited to Vulkan 1.0 and no device running Android
  API 24 were available for hardware testing. The tested phones are newer
  GPUs with a **requested** Vulkan 1.0 execution path.
- Pixel's noncoherent memory paths have earlier single-stage hardware
  evidence, but the new three-stage noncoherent path is not yet validated
  on Pixel. RedMagic correctly rejected incompatible forced requests.
- The original Vulkan multi-set runtime and its recorded fault test results
  remain unchanged. The new multi-stage runner provides opt-in acquisition
  fault testing, but **no new fault matrix is included in this archive**.
- Descriptor types remain `STORAGE_BUFFER` and `UNIFORM_BUFFER`; multiple
  queues, concurrent command submissions, push constants, images, dynamic
  descriptors, pipeline-specific layouts, SPIR-V reflection and device-loss
  recovery are not implemented.
- This is resource infrastructure and a noncryptographic public integer
  test. It is not connected to hashcat's device scheduling, module dispatch,
  candidate preparation or cryptographic kernels. Neither bcrypt/OpenCL
  error 245 nor the separate compiler failure 139 is resolved.
