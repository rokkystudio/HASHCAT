# OpenCL baseline and regression gate (2026-11-04)

Date: 2026-11-04

This document records the OpenCL behaviour of the tracked hashcat source at
commit `f26fb3245a97d58c80e7cec599efe03f309b60a6` so that any later change to
the native tree can be checked with a single command. The gate itself compares
against the machine-readable copy in `docs/opencl-baseline.json`; this document
explains how that file was produced, what it covers and how to reproduce it.

## Recorded binaries

Both binaries were built from the same commit with the stock wrapper scripts,
without `-EnableVulkanDiscovery`:

```powershell
.\scripts\BUILD.ps1 -Target windows-x64
.\scripts\BUILD-ANDROID-ARM64.ps1
```

`hashcat -V` reports `v7.1.2` on both. The Android runtime contains 582
modules and 1595 OpenCL source files, the same counts as the released arm64
package. The Windows desktop build previously failed after
`Prepare-SourceRevision` because `Build-WindowsX64` ran `make` without creating
`obj`, `modules` and `bridges`; `scripts/BUILD-DESKTOP.ps1` now creates them,
matching the Android path.

## Why the gate runs with `--backend-ignore-cuda --backend-ignore-hip`

On the PC the NVIDIA driver exposes the same physical GPU twice: once through
CUDA and once through OpenCL. `hashcat -I` therefore lists device #01 (CUDA)
and device #02 (OpenCL, alias #01), and plain `hashcat -b` benchmarks the CUDA
copy while marking the OpenCL copy `skipped` as a duplicate. That would turn an
OpenCL baseline into a CUDA measurement without any visible sign.

Every recorded run therefore passes `--backend-ignore-cuda --backend-ignore-hip`.
With those flags the OpenCL device is `#01` on both hosts, the benchmark really
executes on OpenCL, and the device identifiers match what `hashcat -d` accepts
in that same session.

## PC: Windows 10 x64, NVIDIA GeForce RTX 3060 Ti

Recorded command:

```text
hashcat -I --backend-ignore-cuda --backend-ignore-hip
```

Full output:

```text
hashcat (v7.1.2) starting in backend information mode

OpenCL Info:
============

OpenCL Platform ID #1
  Vendor..: NVIDIA Corporation
  Name....: NVIDIA CUDA
  Version.: OpenCL 3.0 CUDA 13.3.80

  Backend Device ID #01
    Type...........: GPU
    Vendor.ID......: 32
    Vendor.........: NVIDIA Corporation
    Name...........: NVIDIA GeForce RTX 3060 Ti
    Version........: OpenCL 3.0 CUDA
    Processor(s)...: 38
    Preferred.Thrd.: 32
    Clock..........: 1845
    Memory.Total...: 8191 MB (limited to 2047 MB allocatable in one block)
    Memory.Free....: 8191 MB
    Memory.Unified.: 0
    Local.Memory...: 48 KB
    OpenCL.Version.: OpenCL C 1.2
    Driver.Version.: 610.88
    PCI.Addr.BDF...: 07:00.0
```

The same binary without the two flags additionally reports `CUDA Info:` with
`Backend Device ID #01 (Alias: #02)`, so the CUDA interface was open during the
recording. Its benchmark is intentionally not part of this baseline.

Benchmarks, `hashcat -b -m <mode> -w 2 -d 1 --backend-ignore-cuda --backend-ignore-hip`:

| mode | device | speed | raw line |
| --- | --- | --- | --- |
| 0 | #1 NVIDIA GeForce RTX 3060 Ti | 14042.2 MH/s | `Speed.#01........: 14042.2 MH/s (11.18ms) @ Accel:8 Loops:1024 Thr:512 Vec:1` |
| 1000 | #1 NVIDIA GeForce RTX 3060 Ti | 15367.9 MH/s | `Speed.#01........: 15367.9 MH/s (7.45ms) @ Accel:24 Loops:512 Thr:256 Vec:1` |
| 22000 | #1 NVIDIA GeForce RTX 3060 Ti | 459.2 kH/s | `Speed.#01........:   459.2 kH/s (12.76ms) @ Accel:5 Loops:512 Thr:256 Vec:1` |

## Android: REDMAGIC 7S Pro (NX709S), Android 13, ADB serial `91809037`

`hashcat -I --backend-ignore-cuda --backend-ignore-hip` executed from a
root-staged runtime copy in `/data/local/tmp/hc-baseline-91809037`:

```text
hashcat (v7.1.2) starting in backend information mode

OpenCL Info:
============

OpenCL Platform ID #1
  Vendor..: QUALCOMM
  Name....: QUALCOMM Snapdragon(TM)
  Version.: OpenCL 3.0 QUALCOMM build: commit #21eaafda81 changeid #I578ccc16f0 Date: 07/19/22 Tue Local Branch:  Remote Branch:

  Backend Device ID #01
    Type...........: GPU
    Vendor.ID......: 2147483648
    Vendor.........: QUALCOMM
    Name...........: QUALCOMM Adreno(TM)
    Version........: OpenCL 3.0 Adreno(TM) 730
    Processor(s)...: 4
    Preferred.Thrd.: 32
    Clock..........: 1
    Memory.Total...: 5523 MB (limited to 512 MB allocatable in one block)
    Memory.Free....: 2761 MB
    Memory.Unified.: 1
    Local.Memory...: 32 KB
    OpenCL.Version.: OpenCL C 3.0 Adreno(TM) 730
    Driver.Version.: OpenCL 3.0 QUALCOMM build: commit #21eaafda81 changeid #I578ccc16f0 Date: 07/19/22 Tue Local Branch:  Remote Branch:  Compiler E031.38.11.04
```

Benchmarks with the requested `-w 2` profile are **not reproducible on this
Adreno build**. With `-w 2` the Qualcomm compiler fails to link the pure
benchmark kernel and hashcat exits 251:

```text
clLinkProgram(): CL_OUT_OF_HOST_MEMORY
* Device #1: Kernel .../OpenCL/m00000_a3-pure.cl build failed.
```

The same happens for `-m 1000` (`m01000_a3-pure.cl`) and regardless of whether
hashcat runs as `root` from `/data/local/tmp` or as the application user from
the app cache; it is the workload profile, not the staging, that triggers it.
Without `-w` (hashcat's own default profile) the same binary benchmarks both
modes and reproduces the numbers already recorded in
`docs/android-gpu-multimode-benchmarks.md`. The Android section therefore
records `workload_profile: 0`, which means "no `-w` was passed", and the gate
replays exactly that. `-m 22000` completes in both cases, because it only has
one kernel.

Benchmarks, `hashcat -b -m <mode> -d 1 --backend-ignore-cuda --backend-ignore-hip`
(default workload):

| mode | device | speed | raw line |
| --- | --- | --- | --- |
| 0 | #1 QUALCOMM Adreno(TM) | 2121.3 MH/s | `Speed.#01........:  2121.3 MH/s (93.04ms) @ Accel:98 Loops:1024 Thr:512 Vec:1` |
| 1000 | #1 QUALCOMM Adreno(TM) | 3029.8 MH/s | `Speed.#01........:  3029.8 MH/s (79.25ms) @ Accel:64 Loops:1024 Thr:1024 Vec:1` |
| 22000 | #1 QUALCOMM Adreno(TM) | 757 H/s | `Speed.#01........:      757 H/s (82.51ms) @ Accel:4 Loops:64 Thr:256 Vec:1` |

The arm64 runtime is pushed to `/data/local/tmp/hc-baseline-91809037` and
executed through `su -c`. Running the same staged tree as the application user
is not possible: SELinux denies executing a file from `/data/local/tmp` in the
app domain, so the capture script runs the device side as root.

## Machine-readable baseline

`docs/opencl-baseline.json` is the file the gate reads. Its layout is:

```json
{
  "schema": 1,
  "generated_at": "2026-11-04T23:05:05+03:00",
  "git_head": "f26fb3245a97d58c80e7cec599efe03f309b60a6",
  "hashcat_version": "v7.1.2",
  "hosts": {
    "pc": {
      "label": "Windows 10 x64 / NVIDIA GeForce RTX 3060 Ti",
      "transport": "local",
      "serial": null,
      "capture_arguments": "--backend-ignore-cuda --backend-ignore-hip",
      "devices": [
        { "id": 1, "name": "NVIDIA GeForce RTX 3060 Ti", "type": "GPU", "vendor_id": 32, "backend": "opencl" }
      ],
      "benchmarks": [
        { "mode": 0, "workload_profile": 2, "device_id": 1, "device_name": "NVIDIA GeForce RTX 3060 Ti", "speed_khs": 14042200 }
      ]
    }
  }
}
```

- `devices[].id` is the backend device identifier printed by `hashcat -I` and
  accepted by `hashcat -d` in the same configuration.
- `devices[].type` is normalized to `GPU`, `CPU`, `FPGA` or `ACCEL`. hashcat
  itself prints `Accelerator`; `FPGA` exists in the contract but hashcat has no
  FPGA device class today.
- `devices[].vendor_id` is the OpenCL vendor id, printed unsigned
  (`2147483648` is Qualcomm's generic id seen as `0x80000000`).
- `benchmarks[].speed_khs` is always kH/s, converted from the unit hashcat
  printed (`H/s`, `kH/s`, `MH/s`, `GH/s`, `TH/s`).
- `hosts` holds one section per host, so the PC and the device do not overwrite
  each other. `tools/capture-opencl-baseline.ps1` merges one section and keeps
  the others.

## Reproducing the baseline

Re-recording a host replaces only that host section:

```powershell
# PC
.\tools\capture-opencl-baseline.ps1 -HashcatExecutable build\windows-x64\hashcat.exe `
    -HostName pc -Label 'Windows 10 x64 / NVIDIA GeForce RTX 3060 Ti'

# REDMAGIC 7S Pro over ADB (stages build\arm64-v8a on the device)
.\tools\capture-opencl-baseline.ps1 -HashcatExecutable build\arm64-v8a `
    -Transport adb -AdbSerial 91809037 -HostName android `
    -Label 'REDMAGIC 7S Pro (NX709S), Android 13'
```

Raw `hashcat` output for every recording is written to
`build/opencl-baseline/<host>-*.txt` (ignored by git) so the numbers in the
JSON can be checked against the original log.

## Regression gate

```powershell
powershell -File tools/check-opencl-regression.ps1 `
    -HashcatExecutable build\windows-x64\hashcat.exe `
    -Baseline docs/opencl-baseline.json
```

The gate runs `hashcat -I` and the recorded benchmarks, then:

1. compares the device list by identifier, name and type; any missing, extra or
   changed device fails the gate;
2. compares every recorded benchmark speed. A drop above `-WarnPercent`
   (default 10) is reported as a warning, a drop above `-FailPercent`
   (default 30) fails the gate.

`-Device <id>` limits the benchmark comparison to one device, and passing the
`.md` report as `-Baseline` resolves the JSON sibling automatically.

Exit codes:

| code | meaning |
| --- | --- |
| 0 | device list and benchmark speeds match the baseline |
| 1 | backend device list differs (missing, extra, renamed or retyped device) |
| 2 | a benchmark speed dropped beyond `-FailPercent`, or a benchmark did not complete |
| 3 | usage or input error: missing executable, unreadable baseline, wrong host section |

Exit code 3 exists so that a broken invocation can never be mistaken for a real
regression.

Benchmark numbers are single samples, not statistics. Two gate runs against the
same binary produced +0.4 % / +2.9 % / +17.2 % and +0.1 % / +6.3 % / +15.6 %
(mode 0 / 1000 / 22000) relative to the recorded baseline, so WPA is by far the
noisiest mode; the default thresholds leave room for that spread, but a change
close to a threshold should be confirmed with several runs.

## What this baseline does not cover

- CUDA, HIP and Metal are not measured; the gate deliberately records the
  OpenCL view of the same hardware.
- Only modes 0, 1000 and 22000 are timed, one workload profile (`-w 2`), one
  device per host. Kernel-specific regressions outside these modes are not
  detected by the gate.
- The Windows and Android binaries come from the same commit, but Android runs
  are recorded separately and are not compared to the PC automatically: the
  gate checks the host section selected by `-HostName`.

## Повторный снимок 2026-11-05 (форк, патч P3)

Снимок переснят на `0d29eec` после патча P3 (предпочтение optimized-ядра на
Adreno). Числа Android практически не изменились, потому что `hashcat -b`
и до патча использовал optimized-ядра: режим бенчмарка сам включает их, а
отключает только явный `-w`.

| Режим | 2026-11-04 | 2026-11-05 | Δ |
|---|---|---|---|
| 0 (MD5) | 2121.3 MH/s | 2120.0 MH/s | −0.06 % |
| 1000 (NTLM) | 3029.8 MH/s | 3021.8 MH/s | −0.25 % |
| 22000 (WPA) | 757 H/s | 692 H/s | −8.6 % |

Расхождение по 22000 укладывается в разброс между замерами этого режима
(наблюдался до 17 %), поэтому пороги гейта остаются параметрами.

Отдельно: ранняя попытка снять `-b -m 0 -w 1` на этом Adreno падала с
`CL_OUT_OF_HOST_MEMORY`, потому что `-w` отключает optimized-ядро в режиме
бенчмарка. Обычная атака (`-a 0`) без патча P3 падала так же, то есть до
патча режим 0 на этом устройстве не работал вообще.
