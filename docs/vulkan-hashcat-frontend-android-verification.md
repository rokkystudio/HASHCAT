# Full Android HASHCAT frontend: Vulkan 1.0 discovery verification

Date: 2026-11-01

## Result

A complete native HASHCAT v7.1.2 frontend, built from the current source
tree with **ENABLE_VULKAN_DISCOVERY=1**, was linked for Android API 24
ARM64 and ARMv7. Both binaries ran as actual Android processes, not as
a substitute for the full frontend.

The ordinary `hashcat -I` command discovered Vulkan 1.0 physical devices
inside the production `backend_ctx_init` / `backend_ctx_destroy` lifetime,
reported the existing OpenCL GPU in the same run, then destroyed the Vulkan
instance. Every recorded native process returned exit code **0**.

This is **device discovery**, not a functioning Vulkan password-processing
backend. HASHCAT continues to list the actual OpenCL devices for kernel
execution; Vulkan Compute scheduling explicitly reports disabled.

## Real Android matrix

| Device | ABI | Complete `hashcat -I` | Vulkan catalogues opened / closed | Ordinary OpenCL GPU |
| --- | --- | --- | ---: | --- |
| RedMagic, Adreno 730 | `arm64-v8a` | PASS, exit 0 | 1 / 1 | QUALCOMM Adreno(TM) |
| RedMagic, Adreno 730 | `armeabi-v7a` | PASS, exit 0 | 1 / 1 | QUALCOMM Adreno(TM) |
| Pixel 9 Pro XL, Mali-G715 | `arm64-v8a` | PASS, exit 0 | 1 / 1 | ARM Mali-G715 r0p0 |
| **Total** | 2 ABIs | **3/3 PASS** | **3 / 3** | **3 original OpenCL listings** |

The native logs require these events in dependency-safe order:

```text
VULKAN_LOADER_VERSION=1.x.y source=vkEnumerateInstanceVersion
SELECT_INSTANCE_API=1.0 loader_minor=x cap=1.1
DEVICE_CATALOG_OPEN requested_api=1.0 instance_api=1.0 devices=1 extensions=0 layers=0
BACKEND_VULKAN_CATALOG_READY devices=1 requested_api=1.0 scheduled=0
Vulkan 1.0 discovery found 1 physical device(s); Vulkan Compute scheduling is not enabled.
OpenCL Info:
OpenCL Platform ID #1
  Backend Device ID #01
    Type...........: GPU
DEVICE_CATALOG_CLOSE devices=1 selections=0
HC_NATIVE_EXIT=0
```

The loader was newer than Vulkan 1.0 on all three processes. The
requested API and created instance remained **1.0**. No physical
Vulkan-1.0-only GPU or actual Android 7.0/API-24 OS device was tested.

The previous independently archived **120 public synthetic Compute graph
GPU runs** and **330 device-catalogue lifecycle sessions** remain
separate validations. The full HASHCAT `-I` test does **not** execute
a Vulkan Compute kernel.

## Production source integration

`source/src/backend.c` opens the opt-in Vulkan catalogue at the end
of a successful ordinary `backend_ctx_init`, logs the discovered
physical-device count and closes it in `backend_ctx_destroy`.
The independent `source/src/backend_vulkan.c` owns the catalogue
allocation within `backend_ctx_t`. CUDA, HIP, Metal and OpenCL
discovery and scheduling continue in their original code paths.

A Vulkan-only Android system without one of the previously supported
compute runtimes does not automatically gain a working hashing backend.
No password/hash input, kernel scheduling or GPU workload is supplied
through this Vulkan discovery path.

Full method and type contracts:
`docs/vulkan-backend-ctx-discovery.md`.

## Isolated Android API 24 build

The existing `scripts/BUILD.ps1` accepts the explicit switch
`-EnableVulkanDiscovery` for Android-only builds. It passes
`ENABLE_VULKAN_DISCOVERY=1` to the native Makefile and requires
`-Package:$false`.

For Android API levels below 26, the experimental build also uses
`ENABLE_UNRAR=0`. The bundled upstream UnRAR code invokes
`lutimes`, which is unavailable under the Android 24 NDK headers.
This opt-in build does not contain the optional UnRAR support.
The normal HASHCAT build flags and released Android archives are
unchanged.

The experimental link sets `SHARED_FOLDER=` so Android's existing
`folder_config_init` discovers runtime assets relative to the **actual
executable directory**, rather than using the absolute Windows/MSYS2
path from the host build environment.

The native Makefile links the Android `libvulkan.so` dependency only
into the main HASHCAT frontend or, when built as a shared library,
the owning HASHCAT library. Modules and bridges do not gain an
unneeded Vulkan link dependency.

Build the two independently tested Android ABIs:

```powershell
.\scripts\BUILD.ps1 -Target android-arm64-v8a -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865 -AndroidApi 24 -EnableVulkanDiscovery -Package:$false
.\scripts\BUILD.ps1 -Target android-armeabi-v7a -AndroidNdk D:\ANDROID\SDK\ndk\29.0.14206865 -AndroidApi 24 -EnableVulkanDiscovery -Package:$false
```

The experimental outputs stay under the ignored directories:

```text
build/diagnostic-multimode/vulkan-hashcat-frontend-20261101/arm64-v8a/
build/diagnostic-multimode/vulkan-hashcat-frontend-20261101/armeabi-v7a/
```

Each full ABI build includes:

| Artifact | ARM64 | ARMv7 |
| --- | ---: | ---: |
| Complete executable `hashcat` | 1 | 1 |
| Native hash modules | 581 | 581 |
| Native bridges | 4 | 4 |
| OpenCL shared resource files | 1,595 | 1,595 |

`llvm-readelf` independently confirms for **both** executables:
an ELF Android **API 24** note, correct ELF32/ELF64 machine type,
required `libvulkan.so`, imported Vulkan 1.0
`vkGetInstanceProcAddr` and **no mandatory**
`vkEnumerateInstanceVersion` import. A native
`modules/module_00000.so` does **not** depend on `libvulkan.so`.

These binaries were produced and launched as independent diagnostics;
the production release archives and `HASHCAT-APP` were not modified.

## Evidence and offline verification

The three archived UTF-8 LF-only files were extracted from actual
stored Android `hashcat -I` process output. The enclosing host-console
report headings were omitted and line endings normalized to LF.
They retain the complete captured native diagnostic lines and Android
process exit status; **they are not presented as unmodified raw ADB
byte streams**.

```text
tests/fixtures/android-gpu/vulkan-hashcat-frontend-20261101/
  redmagic-arm64-backend-info.log
  redmagic-armv7-backend-info.log
  pixel-arm64-backend-info.log
  manifest.json
```

The manifest contains SHA-256 identities of **14 source files**,
both complete Android frontend executables and the three captured GPU
logs, with explicit capture provenance and unsupported-environment
limitations.

The independent offline verifier checks source/binary hashes, the
order and exact cardinality of catalogue events, Vulkan 1.0 API
selection, presence of the normal OpenCL GPU and absence of
Vulkan task scheduling. It rejects **nine deliberately modified
records**, including false success, incorrect Vulkan device count,
out-of-order instance destruction and a false claim that Vulkan
scheduling was enabled.

Re-verify the archived logs without Android:

```powershell
python -B tools/test-vulkan-hashcat-frontend-evidence.py
```

Check the native binaries, build contents and actual ELF headers
as well:

```powershell
python -B tools/test-vulkan-hashcat-frontend-evidence.py --artifacts build/diagnostic-multimode/vulkan-hashcat-frontend-20261101 --readelf D:\ANDROID\SDK\ndk\29.0.14206865\toolchains\llvm\prebuilt\windows-x86_64\bin\llvm-readelf.exe
```

The offline manifest recorder reads the existing captures and
build outputs but does not run HASHCAT:

```powershell
python -B tools/archive-vulkan-hashcat-frontend-evidence.py
```

## What remains

- A Vulkan-only target still requires a real per-device session owner
  and integration with hashcat's task lifecycle.
- Vulkan physical devices are not inserted into the production
  hashing-device list and do not execute cryptographic kernels.
- No APK or native release package has been updated; these remain
  experimental native outputs.
- No physically Vulkan 1.0-only hardware, actual Android API-24 OS,
  Vulkan validation-layer leak checks or device-loss recovery was
  available for this milestone.
- Existing OpenCL bcrypt mode-3200 and compiler-failure issues are
  independent of this Vulkan discovery verification.
