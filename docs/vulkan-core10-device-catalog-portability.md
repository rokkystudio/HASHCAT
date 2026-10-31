# Vulkan 1.0 instance and device catalogue for Android Compute

Date: 2026-10-31

## Scope and ownership

`tools/vulkan-core10-device-catalog.h` and
`tools/vulkan-core10-device-catalog.c` implement a lifecycle interface
for Vulkan instance creation, physical-device enumeration and explicit
Compute-device selection. The component owns **VkInstance** and a
heap-allocated array of `VkPhysicalDevice` handles. Each physical-device
handle remains valid until that instance is destroyed.

The catalogue is not a `VkDevice` owner. The separately implemented
`vulkan_core10_multiset_t` creates and owns the logical device, queue,
descriptor sets, buffers, memory, command objects, pipelines and fence.
The host `vulkan_core10_job_port_t` borrows the runtime while executing
public, noncryptographic GPU jobs.

The dependency-safe destruction order is:

```text
vulkan_core10_device_catalog_open
    VkInstance + indexed VkPhysicalDevice list
      |
vulkan_core10_device_catalog_select
    selected physical device + compute queue family + API
      |
vulkan_core10_multiset_create
    VkDevice + queue + GPU buffers + pipelines + command objects
      |
vulkan_core10_job_port_open
    one recorded reusable Compute graph
      |
vulkan_core10_job_port_submit (synchronous; completed fence)
      |
vulkan_core10_job_port_close
    releases only CPU-owned metadata
      |
vulkan_core10_multiset_destroy
    vkDeviceWaitIdle + complete VkDevice resource teardown
      |
vulkan_core10_device_catalog_close
    frees enumeration array + vkDestroyInstance
```

No ownership is inferred from a phone model or GPU name. The Vulkan
instance requests no layers, extensions or optional physical-device
features, and the Compute device is checked against the exact declared
shader workload. Neither source file accesses hashcat password data or
connects to the production hashcat scheduler.

## API contracts

### `vulkan_core10_device_catalog_open`

```c
VkResult vulkan_core10_device_catalog_open (
  vulkan_core10_device_catalog_t *catalog,
  uint32_t minimum_api,
  const char *application_name,
  FILE *diagnostics);
```

The caller provides an empty, zero-initialized catalogue, a nonempty
application name and the exact minimum `VK_API_VERSION_1_0` or
`VK_API_VERSION_1_1`. The method:

1. Queries the loader through Vulkan 1.0 `vkGetInstanceProcAddr`.
   `vkEnumerateInstanceVersion` is optional and a missing entry point
   identifies a genuine Vulkan 1.0 loader.
2. Chooses exactly the requested API version via the existing
   `vulkan_core11_select_instance_version` policy. Vulkan 1.0 remains
   the default even when newer drivers are present.
3. Creates `VkInstance` with zero enabled extensions and zero layers.
4. Enumerates actual physical devices, allocates an indexed handle array
   and obtains its complete list. Zero devices or a driver failure returns
   the concrete error and releases any partially owned instance/array.
5. Marks the catalogue opened only when enumeration succeeds.

An already opened catalogue or invalid pointer/argument returns
`VK_ERROR_INITIALIZATION_FAILED` without creating a second instance.

### `vulkan_core10_device_catalog_select`

```c
VkResult vulkan_core10_device_catalog_select (
  vulkan_core10_device_catalog_t *catalog,
  uint32_t device_index,
  const vulkan_auto_compute_requirements_t *workload,
  vulkan_auto_device_t *selected,
  uint32_t *effective_api);
```

The caller supplies a physical-device index within the discovered array.
There is **no automatic device choice** or GPU-specific fallback.

The method validates the index, queries
`vkGetPhysicalDeviceProperties`, checks the exact requested
instance/device core API with
`vulkan_core11_select_device_version` and delegates the shader's
workgroup sizes/counts, storage-buffer descriptor count, largest storage
range and shared-memory requirement to `vulkan_auto_check_device`.
That checker obtains the actual compatible Compute queue family and
memory properties.

The output `vulkan_auto_device_t` and effective API are written only on
success. The catalogue counts successful selections, but owns no
`VkDevice`; callers retain the selected physical device only while
the catalogue is open.

### `vulkan_core10_device_catalog_close`

```c
VkResult vulkan_core10_device_catalog_close (
  vulkan_core10_device_catalog_t *catalog);
```

The caller first destroys every dependent `VkDevice`.
This method frees the indexed physical-device array, destroys the owned
Vulkan instance and zeroes all catalogue fields. Closing an empty
catalogue succeeds; a NULL catalogue returns
`VK_ERROR_INITIALIZATION_FAILED`.

## Actual hardware verification

The catalogue is used by the complete GPU diagnostic
`tools/vulkan-core10-multiset-graph-diagnostic.c`. It opens once per
invocation, validates five intentionally invalid catalogue requests,
selects physical device index zero after real API/workload checks,
creates and executes the GPU task runtime, destroys that runtime and
finally closes the Vulkan instance.

| Device process | Successful GPU graphs | Explicit memory refusal | Partial Vulkan device faults |
| --- | ---: | ---: | ---: |
| RedMagic / Adreno 730 / ARM64 | 36 | 12 | 62 |
| RedMagic / Adreno 730 / ARMv7 | 36 | 12 | 62 |
| Pixel / Mali / ARM64 | 48 | 0 | 62 |
| **Total** | **120** | **24** | **186** |

These scenarios exercise **330 actual instance open/selection/close
lifecycles**: 120 successful runs, 24 expected incompatible-memory
rejections and 186 controlled Vulkan resource-acquisition faults. Every
instance is destroyed after `MULTI_DESTROY`; no test uses a synthetic
physical device or fake Vulkan instance.

Each of the 330 runs tests five invalid catalogue calls before any
successful selection: use of an unopened catalogue, reopening an active
instance, an out-of-bounds device index, missing workload requirements
and a missing selected-device output. All **1,650 invalid calls** return
`VK_ERROR_INITIALIZATION_FAILED` without advancing selection state.

The existing GPU job port remains fully exercised by four actual
fence-separated submissions per successful run, totalling **480 GPU
jobs**, with **1,966,080 32-bit per-stage results** independently checked
on the CPU. The Pixel scenarios include **12 real HOST_VISIBLE
noncoherent** GPU runs with driver flush/invalidate calls.

The independent offline verifier checks each catalogue's exact
`DEVICE_CATALOG_OPEN`, `DEVICE_CATALOG_INVALID_CONTROLS`,
`DEVICE_CATALOG_SELECT` and `DEVICE_CATALOG_CLOSE` events and their
order relative to `MULTI_DESTROY`. Four deliberately modified
catalogue fields and a reordered instance/device teardown must be
rejected. In total, **15 deliberately corrupted logs** are rejected.

The original **150 LF-only logs** and all source and binary SHA-256
digests remain at
`tests/fixtures/android-gpu/vulkan-multiset-graph-20261031/`.

## Reproduce

Build Android API 24 ARM64/ARMv7 native executables and all twelve
validated Vulkan SPIR-V 1.0 variants:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/build-vulkan-core10-multiset-graph.ps1 -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865
```

Run on a connected Android device with its real ADB serial, using
`-Abi armeabi-v7a` for the 32-bit process when supported:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/run-vulkan-core10-multiset-graph.ps1 -Serial $DeviceAdbSerial -Abi auto -ReportPrefix device-arm -TestCore11 -TestNoncoherent -TestFaults
```

Verify real hardware evidence and exact build artefacts without Android
or a Vulkan driver:

```powershell
python -B tools/test-vulkan-core10-multiset-graph-evidence.py --artifacts build/diagnostic-multimode/vulkan-multiset-graph-20261031
```

## Compatibility boundaries

- Vulkan **1.0** is the baseline. Vulkan 1.1 is an explicitly optional
  compatibility request; no Vulkan 1.2 API, extension or optional
  feature is required.
- Native binaries target Android **API 24**, and work on tested newer
  Android OS versions. Neither a physical Vulkan 1.0-only GPU nor an
  Android 7.0/API-24 OS device was available for these tests.
- The catalogue currently owns instance/device discovery only. It does
  not hold production hashcat `backend_ctx_t`/`hc_device_param_t`
  records, attach to hashcat's device list, manage a persistent hashcat
  session or select/execute cryptographic kernels.
- The GPU runtime synchronously submits one reusable command buffer;
  multi-queue scheduling, interrupted GPU/device loss, validation-layer
  tracking and concurrent sessions are not implemented or verified.
- This remains synthetic public integer Compute. No bcrypt kernel,
  hashcat module, candidate generator or password-cracking workflow is
  implemented by the catalogue.
