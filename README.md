# HASHCAT

This repository contains both the HASHCAT build wrapper and the hashcat source tree used to produce Android runtime binaries for `HASHCAT-APP`.

There is no separate Android source repository and no production build-time patch layer. Work on hashcat itself happens directly inside this repository under:

```text
source/
```

The wrapper scripts under `scripts/` build and package the runtime from that tracked source tree.

## Layout

```text
HASHCAT/
  source/      tracked hashcat source used by our Android runtime
  scripts/     build, update and clean scripts
  build/       generated build outputs, ignored
  release/     generated ZIP artifacts, ignored
```

## Common commands

Verify the source tree and show build metadata:

```powershell
.\scripts\UPDATE.ps1
```

Build Android arm64 runtime:

```powershell
.\scripts\BUILD-ANDROID-ARM64.ps1
```

Build all Android ABIs:

```powershell
.\scripts\BUILD-ANDROID.ps1 -Abi all
```

Build the default target set:

```powershell
.\scripts\BUILD.ps1
```

Clean generated outputs:

```powershell
.\scripts\CLEAN.ps1
```

## Experimental Vulkan 1.0 device and Compute-session verification

The native hashcat backend includes opt-in Vulkan 1.0 physical-device
discovery and an isolated GPU session in `hashcat -I`. The session
validates its queue with an empty fence-completed submission. It never
schedules hashcat kernels on Vulkan or replaces CUDA, HIP, Metal or OpenCL.

Build isolated Android API 24 ARM64/ARMv7 executables without publishing release packages:

```powershell
.\scripts\BUILD.ps1 -Target android-arm64-v8a -AndroidApi 24 -EnableVulkanDiscovery -VulkanOutputName vulkan-hashcat-session-20261101 -Package:$false
.\scripts\BUILD.ps1 -Target android-armeabi-v7a -AndroidApi 24 -EnableVulkanDiscovery -VulkanOutputName vulkan-hashcat-session-20261101 -Package:$false
```

The full native HASHCAT frontend and its empty Vulkan Compute session
passed `hashcat -I` on RedMagic ARM64, RedMagic ARMv7 and Pixel ARM64.
Ownership, Android API24 build instructions, independent logs and
limitations are in [the Vulkan Compute session report](docs/vulkan-backend-compute-session-android.md).
The [initial physical-device discovery milestone](docs/vulkan-hashcat-frontend-android-verification.md)
remains archived separately.

A [standalone Vulkan 1.0 public GPU-buffer transfer diagnostic](docs/vulkan-core10-transfer-buffer-portability.md)
validates real host-visible memory, buffer copies, fences and noncoherent
flush/invalidate on Adreno and Mali. This diagnostic is deliberately
independent of the HASHCAT hashing backend. The
[resource-acquisition fault verification](docs/vulkan-core10-transfer-fault-portability.md)
also tests ten real Vulkan creation/cleanup interruption points on
Android GPU hardware. The independent
[fault-to-live-session recovery matrix](docs/vulkan-core10-transfer-recovery-portability.md)
verifies two working VkDevices and fence-completed public data copies
under the same VkInstance after each controlled acquisition failure.
A [real Vulkan fence-status test](docs/vulkan-core10-transfer-fence-status-portability.md)
verifies driver-returned `VK_NOT_READY` and `VK_TIMEOUT`, subsequent
GPU copy completion, fence resets and noncoherent memory synchronization
without causing or simulating GPU device loss. The independent
[fail-closed Vulkan result policy](docs/vulkan-core10-result-gate-android.md)
accepts real nonfatal fence statuses and verifies that an explicitly
labeled, application-generated terminal result prevents further GPU
submissions by callers consulting the policy. This is not an actual
device-loss or HASHCAT production-backend recovery test. A separate
[test-linked Vulkan API boundary matrix](docs/vulkan-core10-api-fault-boundary-android.md)
returns controlled errors directly from intercepted `vkQueueSubmit` or
`vkWaitForFences` calls to the existing transfer method; it verifies
strict rejection of further GPU submissions and a fresh-device copy
on real Adreno and Mali hardware, without inducing physical device loss.

## Working on hashcat source

Edit files directly under `source/`, then commit them in this repository:

```powershell
cd D:\PROJECTS\HASHCAT
# edit source\OpenCL\..., source\src\..., etc.
git status
git add source scripts README.md
git commit -m "..."
git push origin master
```

## Runtime artifacts

The Android arm64 package is written to:

```text
release\hashcat-v7.1.2-android-arm64-v8a.zip
```

`VersionTag` still controls archive naming and the version passed to the hashcat makefile. Change it intentionally when the bundled runtime version changes.

After producing a runtime ZIP, publish/copy it into `D:\PROJECTS\HASHCAT-APP`, rebuild the app, and bump the app version for app-side changes.
