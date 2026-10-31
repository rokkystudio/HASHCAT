# Experimental Vulkan 1.0 catalogue inside hashcat backend_ctx

Date: 2026-10-31

## Scope

HASHCAT now has an **opt-in Android Vulkan device-discovery path** in the
actual production backend context. It uses the generic Vulkan 1.0 device
catalogue already exercised on real Adreno/Mali GPUs and stores its owner
in hashcat's existing backend_ctx_t.

The integration is **discovery only**. Vulkan physical devices are not
registered as hashcat hashing devices. This change does not implement
Vulkan password kernels, change hashcat's CUDA/HIP/Metal/OpenCL scheduler
or make a Vulkan-only phone a working hashcat execution target.

Existing normal builds do not enable the experimental Vulkan discovery.
The Android build must explicitly specify ENABLE_VULKAN_DISCOVERY=1.

## Production source contracts

| File | Responsibility |
| --- | --- |
| source/include/types.h | Appends a typed Vulkan catalogue pointer, physical device count and VkResult status to backend_ctx_t, preserving offsets of existing backend fields |
| source/include/backend_vulkan.h | Declares three backend-facing Vulkan catalogue lifecycle and device-selection methods |
| source/src/backend_vulkan.c | Owns the Vulkan catalogue heap allocation, records Vulkan discovery state in backend_ctx_t and exposes exact Compute-device selection |
| source/src/backend.c | During a successful ordinary backend_ctx_init, opens the optional Vulkan 1.0 catalogue and reports its result. During backend_ctx_destroy, closes the catalogue before clearing backend state |
| source/src/Makefile | Enables the Android-only build flag, compiles the Vulkan 1.0 device discovery sources, and links the Android Vulkan loader |
| tools/vulkan-core10-device-catalog.c/.h | Own VkInstance, enumerate physical devices, select requested device indices and close the instance |
| tools/vulkan-backend-catalog-diagnostic.c | Exercises two complete Vulkan backend_ctx discovery lifecycles with exact public Compute requirements and invalid-call controls |
| tools/build-vulkan-backend-catalog.ps1 | Compiles actual source/src/backend.c and links an independent backend_ctx diagnostic for Android API 24 ARM64 and ARMv7 |
| tools/run-vulkan-backend-catalog.ps1 | Runs the native backend diagnostic on connected Android devices and archives LF-only logs |
| tools/archive-vulkan-backend-catalog-evidence.py | Archives original public Android evidence and exact source/native binary hashes |
| tools/test-vulkan-backend-catalog-evidence.py | Independently verifies discovery event order, API/device capability contracts, source/binary digests and deliberate log corruptions |

The fields appended to backend_ctx_t are:

- vulkan_catalog: the owned Vulkan physical-device catalogue; NULL when not
  available. Its VkInstance remains valid for the complete owning backend
  context lifetime.
- vulkan_devices_cnt: the number of physically enumerated Vulkan devices,
  not the number of hashcat password-processing GPU devices.
- rc_vulkan_init: the actual VkResult of the optional discovery operation.

The CUDA/HIP/Metal/OpenCL backend fields and indices retain their existing
layout and responsibilities.

### backend_vulkan_catalog_open

VkResult backend_vulkan_catalog_open(backend_ctx_t *backend_ctx,
uint32_t minimum_api, FILE *diagnostics)

Requires an empty Vulkan catalogue pointer and zero physical-device count.
Validates Vulkan 1.0 or explicitly requested 1.1, allocates a physical-device
catalogue, creates VkInstance through the existing API policy, enumerates
physical devices and stores the successful owner in backend_ctx_t.

Driver/loader failures return their VkResult, retain no catalogue allocation
and are recorded in rc_vulkan_init. The method does not register new
hashcat kernel devices or alter existing CPU/OpenCL/CUDA/HIP/Metal state.

### backend_vulkan_catalog_select

VkResult backend_vulkan_catalog_select(backend_ctx_t *backend_ctx,
uint32_t device_index, const vulkan_auto_compute_requirements_t *workload,
vulkan_auto_device_t *selected, uint32_t *effective_api)

Checks an explicitly indexed physical device for exact declared Compute
requirements. The underlying catalogue checks Vulkan instance/device API,
actual workgroup/dispatch limits, storage descriptor count/range, shared
memory and a real Compute queue family. It writes the physical device,
memory properties and effective core API on success. No logical VkDevice
is created here, and the production HASHCAT scheduler remains unchanged.

### backend_vulkan_catalog_close

VkResult backend_vulkan_catalog_close(backend_ctx_t *backend_ctx)

Closes the owned catalogue and VkInstance, frees the catalogue allocation
and clears the pointer/count/status fields. It is safe for an empty
catalogue. Existing GPU backend allocations are unaffected. All future
logical Vulkan devices must be destroyed before this operation.

## Actual production hook and ownership

When ENABLE_VULKAN_DISCOVERY=1 is compiled on Android, the end of a
successful backend_ctx_init performs:

1. Initializes the normal CUDA/HIP/Metal/OpenCL state.
2. Marks the ordinary backend context enabled.
3. Opens the Vulkan 1.0 physical-device catalogue with the exact API
   minimum; logs either the discovered physical-device count or a
   clear warning with the VkResult.
4. Leaves the backend's password-processing device list and active
   scheduling counts unchanged.

The beginning of backend_ctx_destroy closes the Vulkan catalogue before
freeing ordinary backend state. A failed optional Vulkan discovery does
not claim to provide a working Vulkan kernel backend.

The ordinary hashcat initialization still requires at least one existing
supported runtime to activate the production backend. On a Vulkan-only
device without any working CUDA/HIP/Metal/OpenCL backend, the current
production startup does not yet provide a usable compute device; this
experimental path does not bypass that condition.

Source and ownership are intentionally unambiguous: backend_ctx_t owns
the catalogue; vulkan_core10_device_catalog_t owns VkInstance; the
separate vulkan_core10_multiset_t owns VkDevice and buffers when used
by the standalone synthetic Compute diagnostic. No logic infers
supported hashes or capabilities from a GPU vendor name.

## Android build

The source/src/Makefile enables the integration **only** when running an
Android native build with the opt-in flag:

    ENABLE_VULKAN_DISCOVERY=1

This adds WITH_VULKAN_DISCOVERY, ../tools as an include directory, the
native backend_vulkan.c object, the generic Vulkan discovery source
objects and the Android loader link option -lvulkan **only for the
HASHCAT frontend or owning shared library**. The kernel modules and
bridges have no extra Vulkan loader dependency.

An ENABLE_VULKAN_DISCOVERY=1 attempt with a non-Android native build
is explicitly rejected by the Makefile. All other builds retain the
previous Vulkan-free linking configuration.

Compile the real hashcat backend.c integration and the public diagnostic
for both Android API 24 ABIs:

~~~powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/build-vulkan-backend-catalog.ps1 -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865
~~~

The build produces four SHA-256-identified artifacts: two actual hashcat
backend.c object files and two linked backend-catalogue Android
executables. The binary checks require libvulkan.so and reject mandatory
Vulkan-1.1-only entry points.

The independent catalogue diagnostic itself does not link the full
hashcat executable. The explicit -EnableVulkanDiscovery option in
scripts/BUILD.ps1 also builds **the complete Android HASHCAT frontend**
for ARM64 and ARMv7, without publishing new release packages. Both
full binaries were executed on real devices using hashcat -I. The full
build uses relative runtime-asset discovery and excludes bundled UnRAR
on Android API 24 because that dependency requires lutimes. The exact
hardware evidence is in docs/vulkan-hashcat-frontend-android-verification.md.
HASHCAT-APP continues to package its previously published archives;
no APK or app repository was updated.

## Real Android verification

The independent diagnostic includes actual hashcat types.h and uses the
same source/src/backend_vulkan.c interface as the opt-in production hook.
It tests explicit public Compute requirements: 32 local invocations,
64 workgroups, four storage descriptors, 8,192-byte storage range and
zero shared memory.

Every test opens, enumerates, explicitly selects and closes a Vulkan
catalogue **twice** on a real Android GPU. Six invalid backend API calls
per lifecycle (including an unopened selection and five malformed
requests after opening) return initialization errors before selection.
Each trial also submits an explicit *unsupported Compute requirement*
with 4,294,967,295 local invocations. The Vulkan driver reports its
actual local invocation maximum, the capability checker returns
`VK_ERROR_FEATURE_NOT_PRESENT` (-8), and the catalogue preserves zero
successful selections until a valid Compute request is supplied.
This produced **12 real capability refusals**, with no GPU dispatch.
It also verifies that hashcat's ordinary enabled flag and
backend_devices_cnt remain untouched.

| Android process | Vulkan 1.0 | Vulkan 1.1 | Actual catalogue lifecycles |
| --- | --- | --- | ---: |
| RedMagic / Adreno 730 / ARM64 | PASS | PASS | 4 |
| RedMagic / Adreno 730 / ARMv7 | PASS | PASS | 4 |
| Pixel / Mali-G715 / ARM64 | PASS | PASS | 4 |
| **Total** | **3/3 PASS** | **3/3 PASS** | **12** |

Across these six executions: **72 invalid backend API calls** were
rejected before device selection and **nine malformed CLI requests**
were rejected with exit code 2.

Reproduce each connected Android run with its actual ADB serial:

~~~powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File tools/run-vulkan-backend-catalog.ps1 -Serial $DeviceAdbSerial -Abi auto -ReportPrefix device-arm -TestCore11
~~~

RedMagic ARMv7 requires -Abi armeabi-v7a. The runner checks the device
ABI list rather than guessing its ABI from its name.

Nine LF-only logs and a source/native artifact SHA-256 manifest are at:

    tests/fixtures/android-gpu/vulkan-backend-catalog-20261031/

Verify the archive offline:

~~~powershell
python -B tools/test-vulkan-backend-catalog-evidence.py --artifacts build/diagnostic-multimode/vulkan-backend-catalog-20261031
~~~

The verifier checks all source and binary digests, enumerated physical
device counts, API levels, real Compute limits, exact event order, the
no-kernel-scheduling contract, every invalid host call and rejected
malformed CLI. Twelve deliberately corrupted records include incorrect
device counts, premature instance closure, modified API/capability
limits, invalid-call counts and a false scheduling claim.

## Further work and explicit boundaries

The generic synthetic Vulkan 1.0 Compute graph is independently tested
on these GPUs with 120 successful runs and 480 fence-separated jobs.
The current backend_ctx bridge **does not execute that graph inside the
production hashcat frontend**.

Full Android API-24 HASHCAT frontends for ARM64 and ARMv7 now link the
optional Vulkan discovery. Real hashcat -I sessions on RedMagic ARM64,
RedMagic ARMv7 and Pixel ARM64 confirmed Vulkan catalogue creation,
normal OpenCL GPU discovery and catalogue destruction with process exit 0.
The next work is explicit Vulkan device/session ownership beyond discovery
and eventually separate integration with the production scheduling lifecycle.
See docs/vulkan-hashcat-frontend-android-verification.md for hardware logs,
source/binary SHA-256 and independent ELF inspection.

There is no full Vulkan-only hashcat backend, no SHA/bcrypt password
kernel port, no candidate processing or GPU-cracking scheduler. The
existing OpenCL mode-3200 failure and compiler SIGSEGV remain separate.

The compiled minimum is Android API 24 and core Vulkan 1.0, but real
Android 7/API-24 OS hardware and physically Vulkan-1.0-only GPUs were
not available for this verification.
