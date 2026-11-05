# Vulkan backend device registration (M1)

Date: 2026-11-05

Milestone M1 makes the already verified Vulkan 1.0 layer visible as a hashcat
device. Vulkan hashing is still disabled: a registered Vulkan device can be
listed, selected and inspected, but it never receives a kernel.

The feature is compiled only into Android builds with
`ENABLE_VULKAN_DISCOVERY`. Desktop and normal Android release builds are
unchanged.

## What changed

* `hc_device_param_t` carries `is_vulkan` and an opaque
  `struct hc_vulkan_device *vulkan` (NULL in M1; the payload arrives in M2).
* `backend_ctx_t` carries `vulkan_devices_active` and
  `backend_device_from_vulkan[DEVICES_MAX]`.
* `backend_vulkan_devices_init()` appends one `hc_device_param_t` entry per
  physical device from the already opened catalogue. It fills the device
  name, the physical-device class (`CPU`/`GPU`), the Vulkan vendor id and the
  reported `Vulkan MAJOR.MINOR` version.
* The entries are appended after CUDA, HIP, Metal and OpenCL, so existing
  `-d` identifiers do not move and OpenCL output is unchanged.
* Registered Vulkan entries are marked `skipped`: they are visible in `-I`
  and `--backend-info-json`, and `-d` accepts their id, but no scheduling
  loop touches them.
* `is_same_device()` never aliases a Vulkan entry, so a Vulkan device with no
  PCI address can neither disappear from the table nor swallow the OpenCL
  device of the same GPU.
* `backend_vulkan_devices_term()` releases the strings owned by the Vulkan
  entries. It does not close the catalogue; `backend_vulkan_catalog_close()`
  still owns that.

## Device table semantics

| property | value in M1 |
| --- | --- |
| visible in `hashcat -I` | yes, `Vulkan Info:` section |
| listed in `--backend-info-json` | yes, `"id": "vulkan"`, `"detected": true` |
| `"schedulable"` | always `false` |
| accepted by `-d` | yes, but a Vulkan-only selection is refused |
| counted in `backend_devices_active` | no |
| receives kernels | no |

The `devices` array in the JSON manifest lists registered Vulkan devices with
the same ids that `-I` prints and `-d` accepts. Schema and field names are
unchanged (`schema` stays 1). `devices` means "registered", `schedulable`
means "can execute hashcat kernels"; consumers must gate on `schedulable`.
Legacy `-I --machine-readable` keeps its layout without a Vulkan section.

## Fields that are intentionally zero

The generic device table has no backend-neutral processor, clock or memory
metadata, and M1 does not query them. `-I` therefore prints:

```text
Processor(s)...: 0
Clock..........: 0
Memory.Total...: 0 MB
Memory.Free....: 0 MB
```

These zeros mean "not collected for Vulkan yet", not "the GPU has no memory".
They are filled in a later milestone together with scheduling.

## Refusal paths

Three invocations are refused with an explanatory message and exit code 255
(`hashcat` returns `-1`):

1. No schedulable interface is open at all, but the Vulkan catalogue found a
   device (for example `--backend-ignore-opencl`, or a system without an
   OpenCL runtime):

   ```text
   Vulkan devices were found but cannot execute hashcat workloads yet.
   OpenCL, CUDA or HIP is required for hashing; use "hashcat -I" or
   "hashcat --backend-info-json" to inspect the Vulkan devices.
   ```

2. `-d <vulkan-id>` is the only selected device:

   ```text
   Device #02 is a Vulkan device and cannot execute hashcat workloads yet.
   Select an OpenCL, CUDA or HIP device; use "hashcat -I" or
   "hashcat --backend-info-json" to inspect the Vulkan devices.
   ```

3. `-d` selects a Vulkan device together with a schedulable device. The
   session runs on the schedulable device and prints one warning:

   ```text
   Device #02 is a Vulkan device and cannot execute hashcat workloads yet; it is ignored.
   ```

No path dereferences `device_param->vulkan` (it is NULL in M1) and no path
reaches kernel generation for a Vulkan device.

## How to build and check

```powershell
# Android arm64 diagnostic build with Vulkan discovery
.\scripts\BUILD.ps1 -Target android-arm64-v8a -AndroidApi 26 `
    -EnableVulkanDiscovery -VulkanOutputName vulkan-device-registration -Package:$false

# Push/stage the result and run on the device
adb -s <serial> shell "cd <shared-folder> && ./hashcat -I"
adb -s <serial> shell "cd <shared-folder> && ./hashcat --backend-info-json"
adb -s <serial> shell "cd <shared-folder> && ./hashcat -m 0 -a 0 h.txt w.txt --backend-ignore-opencl"
adb -s <serial> shell "cd <shared-folder> && ./hashcat -m 0 -a 0 h.txt w.txt -d <vulkan-id>"
```

Expected:

* `-I` gains a `Vulkan Info:` section with a `Backend Device ID #NN` matching
  the id used by `-d`, and with the Vulkan device name.
* `--backend-info-json` reports `"detected": true`, `"schedulable": false`
  and the registered device in `"devices"`.
* A normal `hashcat -m 0 -a 0` run is unchanged and uses OpenCL.
* `-I --backend-ignore-vulkan` prints no Vulkan section.

`tools/test-backend-info-json.py` accepts registered Vulkan devices while
still requiring `"schedulable": false`. The OpenCL regression gate
(`tools/check-opencl-regression.ps1`) is defined against a build without
Vulkan discovery; a Vulkan-enabled binary intentionally exposes one extra
device, so compare OpenCL behavior on the non-Vulkan build.

## M2 debt (resolved)

M1 stored the Vulkan metadata (type, vendor id, vendor name, version) in the
reused `opencl_device_*` slots of `hc_device_param_t`. M2 moved it into
`hc_vulkan_device`; see `docs/vulkan-backend-runtime.md`. No reader touches
`opencl_device_*` for a Vulkan device any more, and `device_param->vulkan` is
populated for every registered entry.

## Still not implemented

* Vulkan kernel scheduling, memory planning and result read-back through the
  hashcat session.
* Processor, clock and memory reporting.
* `hc_vulkan_device` payload and any non-NULL `device_param->vulkan`.
