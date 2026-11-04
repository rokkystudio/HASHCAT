# Backend CLI contract

Date: 2026-11-04

This document records the command-line surface of the tracked hashcat build
that the Android application relies on.

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
| `--backend-ignore-vulkan` | Vulkan 1.0 discovery | every build; only builds with `ENABLE_VULKAN_DISCOVERY` open a Vulkan catalogue, elsewhere the flag has no observable effect |

`--backend-ignore-vulkan` only stops the Vulkan catalogue from being opened.
It does not change the initialization order of CUDA, HIP, Metal and OpenCL and
it does not change OpenCL device identifiers.
