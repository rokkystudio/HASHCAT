# Backend CLI contract

Date: 2026-11-04

The Android application runs the bundled hashcat as a plain command line tool.
This document records the surface it depends on: the backend selection flags,
the machine-readable backend description, the exit codes and the checks that
keep them honest. Everything below was verified on binaries built from this
tree (`hashcat -V` = `v7.1.2`) by `scripts/BUILD.ps1 -Target windows-x64` and
`scripts/BUILD-ANDROID-ARM64.ps1`.

## Backend selection flags

hashcat exposes one `--backend-ignore-*` flag per backend interface. Setting
one prevents that interface from being opened at startup, so its devices
disappear from `-I`, `--backend-info-json`, and from device selection:

| flag | interface | availability |
| --- | --- | --- |
| `--backend-ignore-cuda` | CUDA | every build |
| `--backend-ignore-hip` | HIP | every build |
| `--backend-ignore-metal` | Metal | macOS builds only |
| `--backend-ignore-opencl` | OpenCL | every build |
| `--backend-ignore-vulkan` | Vulkan 1.0 discovery | every build; builds with `ENABLE_VULKAN` or `ENABLE_VULKAN_DISCOVERY` open a Vulkan catalogue, elsewhere the flag has no observable effect |

`--backend-ignore-vulkan` only stops the Vulkan catalogue from being opened.
It does not change the initialization order of CUDA, HIP, Metal and OpenCL and
it does not change OpenCL device identifiers.

The flag was verified on the REDMAGIC NX709S with an Android arm64 build that
has `ENABLE_VULKAN_DISCOVERY` enabled (API 24). Without the flag the binary
enumerated one Vulkan physical device and printed 110 Vulkan-related lines:

```text
DEVICE_CATALOG_OPEN requested_api=1.0 instance_api=1.0 devices=1 extensions=0 layers=0
Vulkan 1.0 discovery found 1 physical device(s); Vulkan Compute scheduling is not enabled.
VULKAN_CANDIDATE index=0 name=Adreno (TM) 730 vendor=0x5143 device=0x7030002 api=1.1.128 driver=0x80267000
```

With `--backend-ignore-vulkan` the same binary printed none of them: exactly
the 26-line OpenCL report, still device #01, still exit code 0. The catalogue
is therefore closed before any Vulkan call is made.

## Machine-readable backend description

`hashcat --backend-info-json` prints the backend interfaces and the devices
hashcat registered for them as JSON on stdout, then exits with code 0 without
starting a cracking session. `-I` keeps printing the human readable report and
is byte-for-byte unchanged by the new option.

Example, Windows host with one NVIDIA GPU (the same GPU is exposed through the
CUDA and the OpenCL interface, so it appears twice with different ids):

```json
{
  "schema": 1,
  "hashcat_version": "v7.1.2",
  "backends": [
    {
      "id": "cuda",
      "detected": true,
      "schedulable": true,
      "devices": [
        { "id": 1, "name": "NVIDIA GeForce RTX 3060 Ti", "type": "GPU", "vendor_id": 0, "backend": "cuda" }
      ]
    },
    {
      "id": "hip",
      "detected": false,
      "schedulable": false,
      "devices": []
    },
    {
      "id": "metal",
      "detected": false,
      "schedulable": false,
      "devices": []
    },
    {
      "id": "opencl",
      "detected": true,
      "schedulable": true,
      "devices": [
        { "id": 2, "name": "NVIDIA GeForce RTX 3060 Ti", "type": "GPU", "vendor_id": 32, "backend": "opencl" }
      ]
    },
    {
      "id": "vulkan",
      "detected": false,
      "schedulable": false,
      "devices": []
    }
  ],
  "modes": {}
}
```

Field rules:

- `backends` always contains five entries in this order: `cuda`, `hip`,
  `metal`, `opencl`, `vulkan`, whether or not the interface is compiled in,
  opened, or owns devices.
- `detected` is `true` when the interface opened and reported at least one
  device. For Vulkan this means the catalogue opened and enumerated at least
  one physical device.
- `schedulable` is `true` when hashcat can schedule its kernels on that
  backend. For Vulkan it is `true` only when at least one mode program is
  registered and the fixed-vector mode 0 self-check passed on a device in this
  process. A detected Vulkan device whose self-check failed keeps
  `schedulable: false`.
- `devices` lists the devices hashcat registered for that interface. `id`
  equals the backend device id printed by `-I` and accepted by `-d` in the same
  configuration. Vulkan devices are always listed when the catalogue opened;
  `schedulable` is the flag that says whether they may receive work.
- Consumers must gate execution on `schedulable`, not on a non-empty `devices`
  array: `devices` lists the devices registered for an interface, while
  `schedulable` says whether hashcat can run kernels on that interface.
  Enabling a backend merely because `devices` is non-empty is a consumer
  defect, not a contract change.
- `type` is one of `GPU`, `CPU`, `FPGA`, `ACCEL`. hashcat has no FPGA device
  class today, so `FPGA` belongs to the enum but is never emitted.
- `vendor_id` is the OpenCL vendor id, printed as an unsigned number
  (`2147483648` is Qualcomm's generic id seen as `0x80000000`). Backends that
  expose no numeric vendor id report `0`.
- `modes` is reserved: in `schema` 1 it is always `{}`. Consumers must accept a
  missing field and an empty object.
- Numbers are JSON numbers, strings are UTF-8, the key order inside every
  object is fixed as shown, and the output ends with a newline. In a successful
  run only JSON is written to stdout; warnings go to stderr.

Compatibility rules: `schema` is raised only for an incompatible change. New
optional fields may be added without raising it, and consumers must ignore
fields they do not know.

## Exit codes

| code | meaning |
| --- | --- |
| 0 | `--backend-info-json`, `-I` or `--version` completed |
| 255 | hashcat returned -1, i.e. it rejected the invocation (unknown option) or session initialization failed (no usable compute device) |

`--backend-info-json` exits 0 only after the JSON was written. If
initialization fails — every interface disabled, or no usable device — hashcat
exits 255 and prints no JSON at all; it writes its own diagnostics instead,
partly on stderr (`ATTENTION! No OpenCL, HIP or CUDA compatible platform
found.`) and partly on stdout (the driver advice block). A consumer must
therefore parse stdout as JSON, not scan it for keywords.

hashcat also uses exit code 1 for a completed cracking session that did not
recover the hash. That path is out of scope here because `--backend-info-json`
never starts a session.

## Reproducing the checks

```powershell
# Build the two runtimes
.\scripts\BUILD.ps1 -Target windows-x64
.\scripts\BUILD-ANDROID-ARM64.ps1

# JSON contract
python tools/test-backend-info-json.py --hashcat build\windows-x64\hashcat.exe

# OpenCL regression gate, see docs/opencl-baseline-2026-11-04.md
powershell -File tools/check-opencl-regression.ps1 `
    -HashcatExecutable build\windows-x64\hashcat.exe `
    -Baseline docs/opencl-baseline.json
```

On Android the same option is checked against a staged runtime:

```text
hashcat --backend-info-json | python -c "import json,sys; json.load(sys.stdin)"
```

The device reported one OpenCL device (`QUALCOMM Adreno(TM)`, id 1, vendor
`2147483648`) and `vulkan.detected = false`, because that runtime was built
without `ENABLE_VULKAN_DISCOVERY`.

## Vulkan workload contract (M3)

Vulkan is schedulable for exactly one workload: `-m 0 -a 0`, no rules. The
hashcat invocation is accepted when the Vulkan device was selected with `-d`
(or is the only active backend, e.g. with `--backend-ignore-opencl`) and the
self-check passed. Everything else is refused before any GPU work with a
non-zero exit code (255) and one of the messages below.

| command fragment | exit code | message |
| --- | --- | --- |
| `-m 1000` (Vulkan selected) | 255 | `Vulkan backend: hash mode 1000 is not implemented; only -m 0 (MD5) is available.` |
| `-m 22000` (Vulkan selected) | 255 | `Vulkan backend: hash mode 22000 is not implemented; only -m 0 (MD5) is available.` |
| `-a 3` | 255 | `Vulkan backend: attack mode -a 3 is not implemented; only -a 0 (straight wordlist) is available.` |
| `-a 6` / `-a 7` | 255 | `Vulkan backend: attack mode -a 6 is not implemented; only -a 0 (straight wordlist) is available.` |
| `-r rules.txt` | 255 | `Vulkan backend: rules (-r / --generate-rules) are not implemented.` |
| `-O` | 255 | `Vulkan backend: -O / --optimized-kernel is not implemented.` |
| `--increment` | 255 | `Vulkan backend: --increment is not implemented.` |
| `-S` | 255 | `Vulkan backend: -S / --slow-candidates is not implemented.` |
| `-b` | 255 | `Vulkan backend: benchmark mode (-b) is not implemented.` |
| `-z` / `--brain` | 255 | `Vulkan backend: brain is not implemented.` |
| candidate longer than 55 bytes | 255 | `Vulkan mode 0 straight supports candidates of 1..55 bytes; candidate N is L bytes.` |

The long-candidate refusal is per batch: hashcat streams the wordlist, so the
first batch that contains an over-long candidate stops the session with this
message instead of silently skipping it.

`vulkan.devices` lists registered devices when the build has `ENABLE_VULKAN`
or `ENABLE_VULKAN_DISCOVERY`; `schedulable` now follows the self-check rule
above, so a consumer that gates on `schedulable` will offer Vulkan only when it
can actually run mode 0.

## Limitations of this version

- `vulkan.schedulable` is `true` only for mode 0 / `-a 0` without rules; every
  other mode or attack is refused (table above).
- An unselected Vulkan device next to an active OpenCL device is reported as
  detected and schedulable (after the self-check) but does not receive work in
  that run, because Vulkan participates only when selected with `-d` or when it
  is the only active backend.
- `modes` is reserved and always `{}`.
- `--backend-ignore-metal` exists only in macOS builds; the other four
  `--backend-ignore-*` flags exist everywhere.
- Legacy `-I --machine-readable` output keeps its pre-existing layout and does
  not gain a Vulkan section; `--backend-info-json` is the machine-readable
  backend contract.
- `--backend-ignore-vulkan` is accepted everywhere but observable only in
  builds with `ENABLE_VULKAN` or `ENABLE_VULKAN_DISCOVERY`.
- `vendor_id` is `0` for CUDA, HIP and Metal devices.
- If no compute device is available, `--backend-info-json` prints no JSON and
  hashcat exits 255.
