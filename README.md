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
