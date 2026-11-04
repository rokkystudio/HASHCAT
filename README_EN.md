# HASHCAT

A hashcat fork for Android. This repository builds the runtime that is packaged
into [HASHCAT-APP](https://github.com/rokkystudio/HASHCAT-APP): the executable,
hash-mode modules, OpenCL kernels, rules and tunings.

There is no separate Android source repository: hashcat work happens directly
here, in `source/`. The scripts in `scripts/` build and package the runtime from
this tree.

**This is a fork, not a mirror.** Upstream synchronisation is disabled; local
changes are listed in [docs/FORK.md](docs/FORK.md).

## What gets built

Four Android archives, one per ABI: `arm64-v8a`, `armeabi-v7a`, `x86`,
`x86_64`. The version comes from `-VersionTag` (currently `v7.1.2`), the archive
name is `hashcat-v7.1.2-android-<abi>.zip`, about 95–100 MB per ABI.

Each archive contains `hashcat`, `modules/`, `bridges/`, `OpenCL/`, `rules/`,
`tunings/`, `hashcat.hcstat2` and `libc++_shared.so`. Inside the app the
executable is renamed to `libhashcat_exec.so` and started as a regular CLI
process.

## Requirements

* Windows (builds verified on Windows 10/11);
* MSYS2 with `bash`, `make`, `gcc`/`clang` — used as the build environment;
* Android NDK (by default taken from `D:\ANDROID\SDK\ndk\<version>`, or pass
  `-AndroidNdk`);
* Git and PowerShell 5.1+.

## Building

Android arm64:

```powershell
.\scripts\BUILD-ANDROID-ARM64.ps1
```

A single ABI:

```powershell
.\scripts\BUILD-ANDROID.ps1 -Abi armeabi-v7a
```

All four ABIs:

```powershell
.\scripts\BUILD-ANDROID.ps1 -Abi all
```

Every target, including desktop:

```powershell
.\scripts\BUILD-ALL.ps1
```

Useful parameters: `-AndroidApi` (default `26`), `-VersionTag`,
`-Package:$false` (build without packaging a ZIP), `-Clean`, `-MakeJobs`.

Clean up:

```powershell
.\scripts\CLEAN.ps1
```

Output: the build directory `build\<abi>\` and the archive
`release\hashcat-<version>-android-<abi>.zip`.

## CLI contract for the app

Besides the regular hashcat options, the fork adds a machine-readable backend
description and explicit backend selection:

```powershell
hashcat --backend-info-json        # JSON on stdout, exit code 0
hashcat --backend-ignore-vulkan    # do not initialise Vulkan
hashcat --backend-ignore-opencl    # do not initialise OpenCL
```

Example output:

```json
{
  "schema": 1,
  "hashcat_version": "v7.1.2",
  "backends": [
    { "id": "opencl", "detected": true, "schedulable": true,
      "devices": [ { "id": 1, "name": "QUALCOMM Adreno(TM)", "type": "GPU",
                     "vendor_id": 2147483648, "backend": "opencl" } ] },
    { "id": "vulkan", "detected": false, "schedulable": false, "devices": [] }
  ],
  "modes": {}
}
```

`detected` means the interface opened and found at least one device;
`schedulable` means workloads can run on that backend; `devices[].id` is what
`-d` accepts. Field-by-field description and exit codes:
[docs/backend-cli-contract.md](docs/backend-cli-contract.md).

## Android / Adreno specifics

The Adreno OpenCL compiler fails to link pure kernels of several modes and
returns `CL_OUT_OF_HOST_MEMORY`, while the optimized variant of the same mode
builds and runs. Patch P3 prefers the optimized kernel on Qualcomm devices:
modes without an optimized variant keep their pure kernel, and hashcat reports
the resulting password-length limit itself.

The behaviour can be disabled with an environment variable:

```powershell
$env:HASHCAT_DISABLE_ADRENO_OPTIMIZED = '1'
```

Details and mandatory checks: [docs/FORK.md](docs/FORK.md).

## Vulkan

`source/` already contains a Vulkan 1.0 layer: physical-device catalogue, memory,
descriptors, pipelines, queues, fences and 17 hash primitives in SPIR-V.
Vulkan hashing is **not enabled**: devices are not registered as compute devices
and no workload is scheduled on them.

For diagnostics there is a separate build with `-EnableVulkanDiscovery`
(Android only, requires `-Package:$false`): in `hashcat -I` mode it opens a
Vulkan session and runs public and cryptographic kernel chains. Plans and current
state: `docs/vulkan-backend-*.md`.

## OpenCL regression

OpenCL behaviour is compared against a baseline so that any change in the native
part can be verified:

```powershell
.\tools\check-opencl-regression.ps1 -HashcatExecutable build\windows-x64\hashcat.exe -Baseline docs\opencl-baseline.json
```

The baseline (`docs/opencl-baseline.json`) stores the device list and the speed
of three modes for PC and Android. To recapture:

```powershell
.\tools\capture-opencl-baseline.ps1 -HashcatExecutable build\arm64-v8a -Transport adb -AdbSerial <serial> -HostName android -Label 'REDMAGIC 7S Pro, Android 13'
```

Exit codes: `0` — matches, `1` — device set differs, `2` — speed dropped past the
threshold, `3` — invocation error.

## Layout

```text
HASHCAT/
  source/      hashcat sources we build
  scripts/     build and clean
  tools/       diagnostics, baselines, evidence archiving
  tests/       per-mode run artifacts
  patches/     patches applied during the build
  third_party/ bundled third-party archives
  docs/        reports, contracts, baselines
  build/       build output (ignored)
  release/     packaged archives (ignored)
```

## Workflow

1. Change `source/` (or `scripts/`, `tools/`).
2. Build the required ABIs and verify on a device.
3. The OpenCL regression gate must stay green.
4. Commit to this repository.
5. Copy the archives into `HASHCAT-APP/hashcat/binaries`, refresh
   `SHA256SUMS.txt`, rebuild the app and bump its version.

```powershell
cd D:\PROJECTS\HASHCAT
git status
git add source scripts docs README.md
git commit -m "..."
git push origin master
```

## Versions

`-VersionTag` controls the archive names and the version hashcat's makefile
receives. Change it deliberately: the archive name is tied to the
`hashcatVersion` constant in `HASHCAT-APP/hashcat/build.gradle.kts`.

## License

The fork inherits hashcat's license (MIT). Authors and terms:
`source/docs/credits.txt`.
