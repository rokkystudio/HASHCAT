# HASHCAT

This repository contains both the HASHCAT build wrapper and the hashcat source tree used to produce Android runtime binaries for `HASHCAT-APP`.

> **Это форк hashcat.** Синхронизация с upstream отключена, `source/` — наш код. Локальные патчи перечислены в [docs/FORK.md](docs/FORK.md). Скриптов обновления из upstream в репозитории нет.

There is no separate Android source repository and no production build-time patch layer. Work on hashcat itself happens directly inside this repository under:

```text
source/
```

The wrapper scripts under `scripts/` build and package the runtime from that tracked source tree.

## Layout

```text
HASHCAT/
  source/      tracked hashcat source used by our Android runtime
  scripts/     build and clean scripts
  build/       generated build outputs, ignored
  release/     generated ZIP artifacts, ignored
```

## Common commands

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
The [first production-owned Vulkan SPIR-V Compute kernel](docs/vulkan-backend-public-kernel-android.md)
now runs inside the real Android `hashcat -I` process: it loads one
noncryptographic shader, binds two storage buffers, dispatches 64 GPU
threads and checks every public integer result against a CPU reference.
Hashcat's password-processing kernels and scheduler remain separate.
The [two-stage production Vulkan SPIR-V Compute chain](docs/vulkan-backend-public-chain-android.md)
executes a second public kernel directly on the first kernel's GPU
output after a shader-write-to-shader-read barrier, checks both
intermediate and final buffers against CPU arithmetic, and verifies
three complete Android `hashcat -I` processes on Adreno ARM64/ARMv7
and Mali ARM64. Password-processing Vulkan kernels remain disabled. A
[declarative Vulkan program graph contract](docs/vulkan-backend-program-graph.md)
validates ordered SPIR-V stages, storage-buffer dependencies and exact
Compute limits without a Vulkan driver call. The follow-on
[program-declared executor](docs/vulkan-backend-program-executor-android.md)
uses the same declarations for descriptor bindings, SPIR-V modules,
dispatch counts and GPU barriers in a real Pixel Android `hashcat -I`
process while HASHCAT hashing remains disconnected. The current
[dynamic four-stage Vulkan Compute executor](docs/vulkan-backend-dynamic-executor-android.md)
allocates buffers, descriptors, shader modules and pipelines from
validated program counts, executes four ordered public SPIR-V stages
on Adreno ARM64/ARMv7 and Mali ARM64, and verifies both standalone and
full Android `hashcat -I` results without enabling Vulkan hashing.
The [controlled Vulkan cleanup and recovery tests](docs/vulkan-backend-public-fault-android.md)
exercise 37 explicit resource checkpoints on Adreno ARM64/ARMv7 and
Mali ARM64. Every simulated failure is followed by a real working
four-stage Vulkan device under the same instance. The follow-on
[production API-boundary fault diagnostic](docs/vulkan-backend-public-api-fault-android.md)
delivers test-only negative results at the executor's linked buffer,
shader, pipeline, queue-submit and fence-wait calls, confirms real
cleanup waits and recreates healthy four-stage devices on all three
GPU/process configurations. A separate
[real fence-timeout diagnostic](docs/vulkan-backend-public-fence-timeout-android.md)
observes an actual driver VK_TIMEOUT after real four-stage submission,
waits for device idle during production cleanup and then recreates a
working device on Mali ARM64 and Adreno ARM64/ARMv7.

The [first cryptographic Vulkan stage](docs/vulkan-backend-md5-android.md)
executes the complete 64-round MD5 compression primitive through the
same production dynamic executor. Four fixed public one-block messages
match their standard 128-bit digests on Mali ARM64 and Adreno
ARM64/ARMv7 for Vulkan 1.0 and 1.1.

The [variable-length SHA-256 batch](docs/vulkan-backend-sha256-android.md)
uses the same resource owner with exact per-buffer storage ranges. One
caller-provided byte batch covers one- and two-block messages, performs
complete SHA-256 padding/compression and returns canonical 32-byte
digests. Mali ARM64 and Adreno ARMv7 hardware checks pass, and full
Pixel ARM64 plus RedMagic ARMv7 `hashcat -I` processes execute MD5,
SHA-256 and ordinary OpenCL discovery in sequence. Vulkan still does
not consume HASHCAT scheduler candidates or register as a hashing
device.

The [common Vulkan hash batch API](docs/vulkan-backend-hash-primitives-android.md)
includes MD4, variable-length MD5, RIPEMD-160, SHA-1, SHA-224,
SHA-256, SHA-384, SHA-512, unkeyed BLAKE2s-256,
BLAKE2b-256/384/512, SHA3-224/256/384/512 and Keccak-256. All seventeen
algorithms share dynamic Vulkan resource ownership, exact message lengths
and canonical digest serialization. SHA-384/512, BLAKE2b and Keccak-f[1600]
use pairs of uint32 words for 64-bit operations without requiring the
optional shaderInt64 feature. ARM64 and ARMv7 native compilation succeeds.
Hardware execution and digest equality for the additional non-SHA-256
algorithms have not been confirmed; the fixed-vector MD5 information-mode
stage remains separate from arbitrary-length batch MD5.

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
