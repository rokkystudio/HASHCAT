param(
    [ValidateSet('android', 'android-all', 'android-arm64-v8a', 'android-armeabi-v7a', 'android-x86', 'android-x86_64', 'windows-x64', 'linux-x64', 'macos-x64', 'macos-arm64', 'macos-universal', 'all')]
    [string[]]$Target = @('android-all', 'windows-x64'),
    [ValidateSet('arm64-v8a', 'armeabi-v7a', 'x86', 'x86_64')]
    [string]$AndroidAbi = 'arm64-v8a',
    [string]$Msys2Bash = '',
    [string]$Msys2Root = '',
    [string]$WslDistro = '',
    [string]$MakeJobs = '8',
    [string]$AndroidNdk = '',
    [string]$AndroidApi = '26',
    [string]$VersionTag = 'v7.1.2',
    [bool]$Package = $true,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
[Console]::InputEncoding = $utf8NoBom
[Console]::OutputEncoding = $utf8NoBom
$OutputEncoding = $utf8NoBom

$ScriptDir = $PSScriptRoot
$RootDir = (Resolve-Path (Join-Path $ScriptDir '..')).Path
$SourceDir = Join-Path $RootDir 'source'
$BuildRootDir = Join-Path $RootDir 'build'
$ReleaseDir = Join-Path $RootDir 'release'
$TempDir = Join-Path $SourceDir '.tmp'
$PatchFile = Join-Path $RootDir 'patches\hashcat-android-ndk.patch'

$AndroidTargetSpecs = [ordered]@{
    'android-arm64-v8a' = [pscustomobject][ordered]@{
        Target = 'android-arm64-v8a'
        Abi = 'arm64-v8a'
        ClangPrefix = 'aarch64-linux-android'
        MakeVars = @('IS_AARCH64=1', 'IS_ARM=1')
    }
    'android-armeabi-v7a' = [pscustomobject][ordered]@{
        Target = 'android-armeabi-v7a'
        Abi = 'armeabi-v7a'
        ClangPrefix = 'armv7a-linux-androideabi'
        MakeVars = @('IS_AARCH64=0', 'IS_ARM=1')
    }
    'android-x86_64' = [pscustomobject][ordered]@{
        Target = 'android-x86_64'
        Abi = 'x86_64'
        ClangPrefix = 'x86_64-linux-android'
        MakeVars = @('IS_AARCH64=0', 'IS_ARM=0')
    }
    'android-x86' = [pscustomobject][ordered]@{
        Target = 'android-x86'
        Abi = 'x86'
        ClangPrefix = 'i686-linux-android'
        MakeVars = @('IS_AARCH64=0', 'IS_ARM=0')
    }
}

function Convert-ToMsysPath([string]$Path) {
    return (($Path -replace '^([A-Za-z]):', '/$1') -replace '\\', '/')
}

function Add-UniqueTarget([System.Collections.Generic.List[string]]$List, [string]$Item) {
    if (-not $List.Contains($Item)) { [void]$List.Add($Item) }
}

function Invoke-GitApplyCheck([string]$PatchPath, [switch]$Reverse) {
    $gitApplyErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        $arguments = @('apply')
        if ($Reverse) { $arguments += '--reverse' }
        $arguments += @('--check', '--ignore-space-change', '--ignore-whitespace', $PatchPath)

        & git @arguments *> $null

        return $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $gitApplyErrorActionPreference
    }
}

function Convert-AndroidAbiToTarget([string]$Abi) {
    switch ($Abi) {
        'arm64-v8a' { return 'android-arm64-v8a' }
        'armeabi-v7a' { return 'android-armeabi-v7a' }
        'x86' { return 'android-x86' }
        'x86_64' { return 'android-x86_64' }
        default { throw "Unsupported Android ABI: $Abi" }
    }
}

<#
.SYNOPSIS
Checks whether the current Windows host has a runnable default WSL distribution.

.DESCRIPTION
Runs a no-op bash command through wsl.exe and returns true only when the default distribution can execute Linux build commands.
#>
function Test-WslBuildHost {
    if (-not [System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform([System.Runtime.InteropServices.OSPlatform]::Windows)) { return $false }
    if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue)) { return $false }

    $wslErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & wsl.exe -- bash -lc true *> $null
        return $LASTEXITCODE -eq 0
    }
    catch {
        return $false
    }
    finally {
        $ErrorActionPreference = $wslErrorActionPreference
    }
}

<#
.SYNOPSIS
Returns desktop build targets supported by the current host.

.DESCRIPTION
Windows builds windows-x64 and adds linux-x64 when a default WSL distribution can run bash. Linux builds linux-x64. macOS builds all macOS variants.
#>
function Get-SupportedDesktopTargets {
    if ([System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform([System.Runtime.InteropServices.OSPlatform]::Windows)) {
        $targets = @('windows-x64')
        if (Test-WslBuildHost) {
            $targets += 'linux-x64'
        }
        else {
            Write-Warning 'Skipping linux-x64 for -Target all because no runnable default WSL distribution is available.'
        }
        Write-Warning 'Skipping macOS targets for -Target all because the current host is Windows.'
        return $targets
    }

    if ([System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform([System.Runtime.InteropServices.OSPlatform]::Linux)) {
        return @('linux-x64')
    }

    if ([System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform([System.Runtime.InteropServices.OSPlatform]::OSX)) {
        return @('macos-x64', 'macos-arm64', 'macos-universal')
    }

    return @()
}

<#
.SYNOPSIS
Expands aggregate build targets into concrete targets.

.DESCRIPTION
Expands android and android-all into Android ABI targets. The all target includes every Android ABI and only desktop targets supported by the current host.
#>
function Expand-BuildTargets([string[]]$RequestedTargets, [string]$DefaultAndroidAbi) {
    $expanded = [System.Collections.Generic.List[string]]::new()

    foreach ($item in $RequestedTargets) {
        switch ($item) {
            'android' { Add-UniqueTarget $expanded (Convert-AndroidAbiToTarget $DefaultAndroidAbi) }
            'android-all' {
                foreach ($targetName in $AndroidTargetSpecs.Keys) { Add-UniqueTarget $expanded $targetName }
            }
            'all' {
                foreach ($targetName in $AndroidTargetSpecs.Keys) { Add-UniqueTarget $expanded $targetName }
                foreach ($targetName in (Get-SupportedDesktopTargets)) { Add-UniqueTarget $expanded $targetName }
            }
            default { Add-UniqueTarget $expanded $item }
        }
    }

    return @($expanded)
}

function Invoke-DesktopTargets([string[]]$RequestedTargets) {
    $RequestedTargets = @($RequestedTargets | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    if ($RequestedTargets.Count -eq 0) { return }

    $desktopScript = Join-Path $ScriptDir 'BUILD-DESKTOP.ps1'
    if (-not (Test-Path -LiteralPath $desktopScript)) {
        throw "Desktop build script is missing: $desktopScript"
    }

    $desktopParams = @{ Target = $RequestedTargets }
    if (-not [string]::IsNullOrWhiteSpace($Msys2Bash)) { $desktopParams.Msys2Bash = $Msys2Bash }
    if (-not [string]::IsNullOrWhiteSpace($Msys2Root)) { $desktopParams.Msys2Root = $Msys2Root }
    if (-not [string]::IsNullOrWhiteSpace($WslDistro)) { $desktopParams.WslDistro = $WslDistro }
    if (-not [string]::IsNullOrWhiteSpace($MakeJobs)) { $desktopParams.MakeJobs = $MakeJobs }
    if (-not [string]::IsNullOrWhiteSpace($VersionTag)) { $desktopParams.VersionTag = $VersionTag }
    if ($Clean) { $desktopParams.Clean = $true }
    if ($Package) { $desktopParams.Package = $true }

    Write-Output "Starting desktop build target(s): $($RequestedTargets -join ', ')"
    & $desktopScript @desktopParams
}

function Resolve-Msys2Bash([string]$ExplicitPath) {
    if (-not [string]::IsNullOrWhiteSpace($ExplicitPath)) { return (Resolve-Path $ExplicitPath).Path }
    if (-not [string]::IsNullOrWhiteSpace($env:MSYS2_ROOT)) {
        $candidate = Join-Path $env:MSYS2_ROOT 'usr\bin\bash.exe'
        if (Test-Path $candidate) { return $candidate }
    }
    $fromPath = Get-Command bash.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($fromPath) { return $fromPath.Source }
    foreach ($candidate in @('C:\msys64\usr\bin\bash.exe', 'C:\msys32\usr\bin\bash.exe')) {
        if (Test-Path $candidate) { return $candidate }
    }
    throw 'MSYS2 bash was not found. Pass -Msys2Bash or set MSYS2_ROOT.'
}

function Resolve-AndroidNdk([string]$ExplicitPath) {
    if (-not [string]::IsNullOrWhiteSpace($ExplicitPath)) { return (Resolve-Path $ExplicitPath).Path }

    $localProperties = Join-Path $RootDir 'local.properties'
    if (Test-Path -LiteralPath $localProperties) {
        foreach ($line in Get-Content -LiteralPath $localProperties) {
            $trimmed = $line.Trim()
            if ($trimmed.Length -eq 0 -or $trimmed.StartsWith('#')) { continue }
            $match = [regex]::Match($trimmed, '^(android\.ndk|android\.ndk\.home|ndk\.dir)\s*=\s*(.+)$')
            if (-not $match.Success) { continue }
            $value = $match.Groups[2].Value.Trim().Trim('"').Replace('/', [IO.Path]::DirectorySeparatorChar)
            if (-not [string]::IsNullOrWhiteSpace($value) -and (Test-Path -LiteralPath $value)) {
                return (Resolve-Path $value).Path
            }
        }
    }

    foreach ($name in @('ANDROID_NDK_HOME', 'ANDROID_NDK_ROOT')) {
        $value = [Environment]::GetEnvironmentVariable($name)
        if (-not [string]::IsNullOrWhiteSpace($value) -and (Test-Path $value)) { return (Resolve-Path $value).Path }
    }
    foreach ($name in @('ANDROID_HOME', 'ANDROID_SDK_ROOT')) {
        $sdk = [Environment]::GetEnvironmentVariable($name)
        if ([string]::IsNullOrWhiteSpace($sdk)) { continue }
        $ndkRoot = Join-Path $sdk 'ndk'
        if (Test-Path $ndkRoot) {
            $latest = Get-ChildItem $ndkRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
            if ($latest) { return $latest.FullName }
        }
    }

    if (-not [string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
        $ndkRoot = Join-Path $env:LOCALAPPDATA 'Android\Sdk\ndk'
        if (Test-Path $ndkRoot) {
            $latest = Get-ChildItem $ndkRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1
            if ($latest) { return $latest.FullName }
        }
    }

    return ''
}

function New-DirectoryPackage([string]$Directory, [string]$Platform) {
    New-Item -ItemType Directory -Force -Path $ReleaseDir | Out-Null
    $archive = Join-Path $ReleaseDir "hashcat-$VersionTag-$Platform.zip"
    $stagingRoot = Join-Path $BuildRootDir '.package-staging'
    $stagingDir = Join-Path $stagingRoot $Platform

    if (Test-Path $stagingDir) { Remove-Item -Recurse -Force -Path $stagingDir }
    New-Item -ItemType Directory -Force -Path $stagingDir | Out-Null

    Copy-Item -Recurse -Force -Path (Join-Path $Directory '*') -Destination $stagingDir

    $files = @(Get-ChildItem -LiteralPath $stagingDir -Recurse -File)
    $stagingFullPath = [System.IO.Path]::GetFullPath($stagingDir).TrimEnd([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar)
    if ($files.Count -eq 0) { throw "Cannot package empty directory: $Directory" }

    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem

    if (Test-Path $archive) { Remove-Item -Force -Path $archive }

    $archiveStream = [System.IO.File]::Open($archive, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
    try {
        $zip = [System.IO.Compression.ZipArchive]::new($archiveStream, [System.IO.Compression.ZipArchiveMode]::Create, $false)
        try {
            foreach ($file in $files) {
                $fileFullPath = [System.IO.Path]::GetFullPath($file.FullName)
                $relativePath = $fileFullPath.Substring($stagingFullPath.Length).TrimStart([System.IO.Path]::DirectorySeparatorChar, [System.IO.Path]::AltDirectorySeparatorChar).Replace('\', '/')
                $entry = $zip.CreateEntry($relativePath, [System.IO.Compression.CompressionLevel]::Optimal)

                $sourceStream = [System.IO.File]::Open($file.FullName, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite -bor [System.IO.FileShare]::Delete)
                try {
                    $entryStream = $entry.Open()
                    try {
                        $sourceStream.CopyTo($entryStream)
                    }
                    finally {
                        $entryStream.Dispose()
                    }
                }
                finally {
                    $sourceStream.Dispose()
                }
            }
        }
        finally {
            $zip.Dispose()
        }
    }
    finally {
        $archiveStream.Dispose()
    }

    Remove-Item -Recurse -Force -Path $stagingDir

    return $archive
}

function Reset-AndroidSourceOutputs([string]$SourceDirectory) {
    git -C $SourceDirectory clean -fdx -- .tmp obj modules bridges feeds *> $null
    git -C $SourceDirectory checkout -- obj modules bridges feeds *> $null
    foreach ($fileName in @('hashcat', 'hashcat.exe', 'hashcat.bin')) {
        $path = Join-Path $SourceDirectory $fileName
        if (Test-Path -LiteralPath $path) { Remove-Item -Force -Path $path }
    }
}

function Invoke-AndroidTarget([pscustomobject]$Config, [string]$ResolvedAndroidNdk, [string]$ResolvedMsys2Bash) {
    $ndkToolchainBin = Join-Path $ResolvedAndroidNdk 'toolchains\llvm\prebuilt\windows-x86_64\bin'
    $clang = Join-Path $ndkToolchainBin "$($Config.ClangPrefix)$AndroidApi-clang.cmd"
    $clangxx = Join-Path $ndkToolchainBin "$($Config.ClangPrefix)$AndroidApi-clang++.cmd"

    if (-not (Test-Path -LiteralPath $clang)) { throw "Android NDK clang is missing: $clang" }
    if (-not (Test-Path -LiteralPath $clangxx)) { throw "Android NDK clang++ is missing: $clangxx" }

    Reset-AndroidSourceOutputs $SourceDir
    New-Item -ItemType Directory -Force -Path $TempDir | Out-Null

    $sourceForMsys = Convert-ToMsysPath $SourceDir
    $ndkBinForMsys = Convert-ToMsysPath $ndkToolchainBin
    $tempForMsys = Convert-ToMsysPath $TempDir
    $buildAbiDir = Join-Path $BuildRootDir $Config.Abi
    $cc = "$($Config.ClangPrefix)$AndroidApi-clang"
    $cxx = "$($Config.ClangPrefix)$AndroidApi-clang++"
    $makeVars = @($Config.MakeVars)

    # Avoid Android NDK 32-bit sys/user.h conflict with hashcat's own struct tag name.
    $typesHeader = Join-Path $SourceDir 'include\types.h'
    $typesText = Get-Content -LiteralPath $typesHeader -Raw
    $patchedTypesText = $typesText -replace 'typedef\s+struct\s+user\s*\{', "typedef struct hashcat_hash_user`r`n{"
    if ($patchedTypesText -ne $typesText) {
        [IO.File]::WriteAllText($typesHeader, $patchedTypesText, $utf8NoBom)
    }
    if ((Get-Content -LiteralPath $typesHeader -Raw) -match 'typedef\s+struct\s+user\s*\{') {
        throw 'Android struct user collision remains in include/types.h'
    }

    # Android i686 NDK exposes CPUID helpers in cpuid.h, but upstream cpu_features.c does not include it explicitly.
    $cpuFeaturesSource = Join-Path $SourceDir 'src\cpu_features.c'
    $cpuFeaturesText = Get-Content -LiteralPath $cpuFeaturesSource -Raw
    if ($cpuFeaturesText -notmatch '#include\s+<cpuid\.h>') {
        $cpuFeaturesInclude = @'
#include "cpu_features.h"

#if defined (__x86_64__) || defined (_M_X64) || defined (__i386__) || defined (_M_IX86)
#include <cpuid.h>
#endif
'@
        $cpuFeaturesText = $cpuFeaturesText.Replace('#include "cpu_features.h"', $cpuFeaturesInclude)
        [IO.File]::WriteAllText($cpuFeaturesSource, $cpuFeaturesText, $utf8NoBom)
    }

    # Android cannot execute a copy from writable app data. The app runs the packaged
    # executable from nativeLibraryDir and points Hashcat at its extracted runtime tree.
    $mainSource = Join-Path $SourceDir 'src\main.c'
    $mainText = Get-Content -LiteralPath $mainSource -Raw
    if ($mainText -notmatch 'HASHCAT_SHARED_FOLDER') {
        $sharedFolderPattern = '(?ms)^  #if defined \(SHARED_FOLDER\)\r?\n  shared_folder = SHARED_FOLDER;\r?\n  #endif'
        $androidSharedFolderBlock = @'
  #if defined (SHARED_FOLDER)
  shared_folder = SHARED_FOLDER;
  #endif

  #if defined (__ANDROID__)
  const char *android_shared_folder = getenv ("HASHCAT_SHARED_FOLDER");

  if (android_shared_folder != NULL)
  {
    if (android_shared_folder[0] != 0) shared_folder = android_shared_folder;
  }
  #endif
'@
        $patchedMainText = [regex]::Replace($mainText, $sharedFolderPattern, $androidSharedFolderBlock, 1)
        if ($patchedMainText -eq $mainText) {
            throw 'Unable to locate shared_folder initialization in src/main.c'
        }
        [IO.File]::WriteAllText($mainSource, $patchedMainText, $utf8NoBom)
    }

    # Android runs the frontend from read-only nativeLibraryDir while shared runtime and mutable
    # state live under the application's files directory supplied through HASHCAT_SHARED_FOLDER.
    $folderSource = Join-Path $SourceDir 'src\folder.c'
    $folderText = Get-Content -LiteralPath $folderSource -Raw
    if ($folderText -notmatch 'HASHCAT_ANDROID_SHARED_ROOT') {
        $folderPattern = '(?m)^  if \(strcmp \(install_dir, resolved_install_folder\) == 0\)$'
        $androidFolderBlock = @'
  #if defined (__ANDROID__)
  /* HASHCAT_ANDROID_SHARED_ROOT */
  if ((shared_folder != NULL) && (shared_folder[0] != 0))
  {
    profile_dir = hcstrdup (shared_folder);
    cache_dir   = hcstrdup (shared_folder);
    session_dir = hcstrdup (shared_folder);
    shared_dir  = hcstrdup (shared_folder);
  }
  else
  #endif
  if (strcmp (install_dir, resolved_install_folder) == 0)
'@
        $patchedFolderText = [regex]::Replace($folderText, $folderPattern, $androidFolderBlock, 1)
        if ($patchedFolderText -eq $folderText) {
            throw 'Unable to locate Android folder configuration insertion point in src/folder.c'
        }
        [IO.File]::WriteAllText($folderSource, $patchedFolderText, $utf8NoBom)
    }

    # Android does not use the desktop versioned-library directory scan.
    $dynloaderSource = Join-Path $SourceDir 'src\dynloader.c'
    $dynloaderText = Get-Content -LiteralPath $dynloaderSource -Raw
    if ($dynloaderText -notmatch '#if !defined \(__ANDROID__\)\r?\nstatic bool hc_dynlib_ver_parse') {
        $dynloaderText = $dynloaderText.Replace(
            'static bool hc_dynlib_ver_parse (const char *name, const char *stem, int *ver)',
            ('#if !defined (__ANDROID__)' + [Environment]::NewLine + 'static bool hc_dynlib_ver_parse (const char *name, const char *stem, int *ver)')
        )
        $dynloaderText = [regex]::Replace(
            $dynloaderText,
            '(?m)^#if !defined \(__ANDROID__\)\r?\n(?=static void hc_dynlib_best)',
            '',
            1
        )
        if ($dynloaderText -notmatch '#if !defined \(__ANDROID__\)\r?\nstatic bool hc_dynlib_ver_parse') {
            throw 'Unable to isolate desktop dynamic-loader helpers in src/dynloader.c'
        }
        [IO.File]::WriteAllText($dynloaderSource, $dynloaderText, $utf8NoBom)
    }

    # -fno-plt is not used by Android NDK targets and Clang reports it for ARMv7 compilation units.
    $makefileSource = Join-Path $SourceDir 'src\Makefile'
    $makefileText = Get-Content -LiteralPath $makefileSource -Raw
    if ($makefileText -notmatch 'ifneq \(\$\(UNAME\),Android\)\r?\nCFLAGS\s+\+= -fno-plt') {
        $fnoPltPattern = '(?m)^ifeq \(\$\(and \$\(filter MSYS2,\$\(UNAME\)\),\$\(filter 1,\$\(CC_NATIVE_CLANG\)\)\),\)\r?\nCFLAGS\s+\+= -fno-plt\r?\nendif$'
        $androidFnoPltBlock = @'
ifeq ($(and $(filter MSYS2,$(UNAME)),$(filter 1,$(CC_NATIVE_CLANG))),)
ifneq ($(UNAME),Android)
CFLAGS                  += -fno-plt
endif
endif
'@
        $patchedMakefileText = [regex]::Replace($makefileText, $fnoPltPattern, $androidFnoPltBlock, 1)
        if ($patchedMakefileText -eq $makefileText) {
            throw 'Unable to locate -fno-plt configuration in src/Makefile'
        }
        [IO.File]::WriteAllText($makefileSource, $patchedMakefileText, $utf8NoBom)
    }

    # Host CPU tuning describes the build machine and is not part of Android cross-target ABI selection.
    $makefileText = Get-Content -LiteralPath $makefileSource -Raw
    if ($makefileText -notmatch 'ifneq \(\$\(UNAME\),Android\)\r?\nCFLAGS\s+\+= \$\(CFLAGS_HOST_ONLY\)') {
        $hostFlagsPattern = '(?m)^CFLAGS\s+\+= \$\(CFLAGS_HOST_ONLY\)$'
        $androidHostFlagsBlock = @'
ifneq ($(UNAME),Android)
CFLAGS                  += $(CFLAGS_HOST_ONLY)
endif
'@
        $patchedMakefileText = [regex]::Replace($makefileText, $hostFlagsPattern, $androidHostFlagsBlock, 1)
        if ($patchedMakefileText -eq $makefileText) {
            throw 'Unable to isolate host-only CPU flags from Android targets in src/Makefile'
        }
        [IO.File]::WriteAllText($makefileSource, $patchedMakefileText, $utf8NoBom)
    }

    # GCC-compatible Android ARM targets do not implement the x86 fastcall calling convention.
    $scryptPortableSource = Join-Path $SourceDir 'deps\scrypt-jane-master\code\scrypt-jane-portable.h'
    $scryptPortableText = Get-Content -LiteralPath $scryptPortableSource -Raw
    if ($scryptPortableText -notmatch 'defined\(__ANDROID__\).*defined\(__i386__\)') {
        $fastcallPattern = '(?m)^\t#undef FASTCALL\r?\n\t#if \(COMPILER_GCC >= 30400\)\r?\n\t\t#define FASTCALL __attribute__\(\(fastcall\)\)\r?\n\t#else\r?\n\t\t#define FASTCALL\r?\n\t#endif$'
        $androidFastcallBlock = @'
	#undef FASTCALL
	#if defined(__ANDROID__) && !defined(__i386__) && !defined(__x86_64__)
		#define FASTCALL
	#elif (COMPILER_GCC >= 30400)
		#define FASTCALL __attribute__((fastcall))
	#else
		#define FASTCALL
	#endif
'@
        $patchedScryptPortableText = [regex]::Replace($scryptPortableText, $fastcallPattern, $androidFastcallBlock, 1)
        if ($patchedScryptPortableText -eq $scryptPortableText) {
            throw 'Unable to locate FASTCALL configuration in scrypt-jane-portable.h'
        }
        [IO.File]::WriteAllText($scryptPortableSource, $patchedScryptPortableText, $utf8NoBom)
    }

    # bypass_delay is stored as u32 while the elapsed timer uses time_t.
    $monitorSource = Join-Path $SourceDir 'src\monitor.c'
    $monitorText = Get-Content -LiteralPath $monitorSource -Raw
    $monitorText = $monitorText.Replace(
        'if ((status_ctx->timer_bypass_cur - status_ctx->timer_bypass_start) >= user_options->bypass_delay)',
        'if ((status_ctx->timer_bypass_cur - status_ctx->timer_bypass_start) >= (time_t) user_options->bypass_delay)'
    )
    [IO.File]::WriteAllText($monitorSource, $monitorText, $utf8NoBom)

    # hashcat's Rust bridge rules can build host .dll/.so plugins, but they do not currently cross-build
    # Rust sub-plugins for Android ABIs. Keep Android artifacts native/NDK-only instead of mixing host Rust output.
    $makeVars += @(
        'PYTHON_MP_SKIP_SO=true',
        'PYTHON_SP_SKIP_SO=true',
        'RUST_CARGO=__hashcat_android_cross_cargo_disabled__',
        'RUST_RUSTUP=__hashcat_android_cross_rustup_disabled__'
    )

    $makeCommand = @(
        'set -e',
        "cd $sourceForMsys",
        'mkdir -p obj modules bridges feeds bridges/subs',
        "export PATH=${ndkBinForMsys}:`$PATH",
        "export TMPDIR=$tempForMsys",
        "export TMP=$tempForMsys",
        "export TEMP=$tempForMsys",
        "make -j $MakeJobs hashcat UNAME=Android PRODUCTION=1 VERSION_TAG=$VersionTag CC=$cc CXX=$cxx AR=llvm-ar $($makeVars -join ' ')",
        "make -j $MakeJobs modules bridges feeds UNAME=Android PRODUCTION=1 VERSION_TAG=$VersionTag CC=$cc CXX=$cxx AR=llvm-ar $($makeVars -join ' ')"
    ) -join '; '

    $skipNoticeRegex = 'Skipping (freethreaded|regular) plugin (72000|73000|74000)'
    $skipNoticeRegex += '|To use -m 7[234]000, you must install'
    $skipNoticeRegex += '|To use it, you must install Rust'
    $skipNoticeRegex += '|Otherwise, you can safely ignore this warning'
    $skipNoticeRegex += '|For more information, see .docs/hashcat-(python|rust)-plugin-requirements'
    $skipNoticeRegex += '|Skipping generic attack-mode 8 plugin'
    $skipNoticeRegex += '|cargo not found'
    $skipNoticeRegex += '|rustup not found'
    $filteredCommand = "$makeCommand 2>&1 | grep -v -E '$skipNoticeRegex'; exit `${PIPESTATUS[0]}"

    Write-Output "Starting Android build: $($Config.Abi)"
    $buildErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $ResolvedMsys2Bash -lc $filteredCommand
        $buildExitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $buildErrorActionPreference
    }
    if ($buildExitCode -ne 0) { throw "hashcat Android build failed for $($Config.Abi) with exit code $buildExitCode" }

    if (Test-Path $buildAbiDir) { Remove-Item -Recurse -Force -Path $buildAbiDir }
    New-Item -ItemType Directory -Force -Path $buildAbiDir | Out-Null

    $frontend = Join-Path $SourceDir 'hashcat'
    if (-not (Test-Path $frontend)) { throw "hashcat frontend was not produced: $frontend" }
    Copy-Item -Force -Path $frontend -Destination (Join-Path $buildAbiDir 'hashcat')

    foreach ($name in @('modules', 'bridges', 'feeds')) {
        $src = Join-Path $SourceDir $name
        $dst = Join-Path $buildAbiDir $name
        if (-not (Test-Path $src)) { throw "Required native output directory is missing: $src" }
        New-Item -ItemType Directory -Force -Path $dst | Out-Null
        Get-ChildItem -Path $src -Filter '*.so' -File | ForEach-Object {
            Copy-Item -Force -Path $_.FullName -Destination (Join-Path $dst $_.Name)
        }
        if ($name -eq 'bridges') {
            $subs = Join-Path $src 'subs'
            if (Test-Path $subs) {
                $dstSubs = Join-Path $dst 'subs'
                New-Item -ItemType Directory -Force -Path $dstSubs | Out-Null
                Get-ChildItem -Path $subs -Filter '*.so' -File -ErrorAction SilentlyContinue | ForEach-Object {
                    Copy-Item -Force -Path $_.FullName -Destination (Join-Path $dstSubs $_.Name)
                }
            }
        }
    }

    foreach ($name in @('OpenCL', 'rules', 'tunings', 'pcfg')) {
        $src = Join-Path $SourceDir $name
        $dst = Join-Path $buildAbiDir $name
        if (-not (Test-Path $src)) { throw "Required runtime asset directory is missing: $src" }
        if (Test-Path $dst) { Remove-Item -Recurse -Force -Path $dst }
        Copy-Item -Recurse -Force -Path $src -Destination $dst
    }

    $hcstat = Join-Path $SourceDir 'hashcat.hcstat2'
    if (Test-Path $hcstat) {
        Copy-Item -Force -Path $hcstat -Destination (Join-Path $buildAbiDir 'hashcat.hcstat2')
    }

    $archive = $null
    if ($Package) {
        $archive = New-DirectoryPackage $buildAbiDir "android-$($Config.Abi)"
    }

    [pscustomobject][ordered]@{
        Target = $Config.Target
        AndroidAbi = $Config.Abi
        Build = $buildAbiDir
        Package = $archive
        FrontendBytes = (Get-Item (Join-Path $buildAbiDir 'hashcat')).Length
        Modules = (Get-ChildItem (Join-Path $buildAbiDir 'modules') -Filter '*.so' -File | Measure-Object).Count
        Bridges = (Get-ChildItem (Join-Path $buildAbiDir 'bridges') -Filter '*.so' -File | Measure-Object).Count
        BridgeSubs = (Get-ChildItem (Join-Path $buildAbiDir 'bridges\subs') -Filter '*.so' -File -ErrorAction SilentlyContinue | Measure-Object).Count
        Feeds = (Get-ChildItem (Join-Path $buildAbiDir 'feeds') -Filter '*.so' -File | Measure-Object).Count
        OpenCL = (Get-ChildItem (Join-Path $buildAbiDir 'OpenCL') -Recurse -File | Measure-Object).Count
    } | Format-List
}

$ExpandedTargets = Expand-BuildTargets $Target $AndroidAbi
$AndroidTargets = @($ExpandedTargets | Where-Object { $AndroidTargetSpecs.Contains($_) })
$DesktopTargets = @($ExpandedTargets | Where-Object { -not $AndroidTargetSpecs.Contains($_) })

$buildMutex = [System.Threading.Mutex]::new($false, 'Global\HASHCAT_WRAPPER_D_PROJECTS_HASHCAT_ANDROID_RUNTIME_V2')
$lockTaken = $false
$patchApplied = $false

try {
    $lockTaken = $buildMutex.WaitOne(0)
    if (-not $lockTaken) {
        throw 'Another HASHCAT build/update/clean process is already running. Close/stop the other run and try again.'
    }

    if ($AndroidTargets.Count -gt 0) {
        $ResolvedAndroidNdk = Resolve-AndroidNdk $AndroidNdk
        if ([string]::IsNullOrWhiteSpace($ResolvedAndroidNdk)) {
            if ($DesktopTargets.Count -gt 0) {
                Write-Warning "Android NDK was not found; skipping Android target(s): $($AndroidTargets -join ', ')"
            }
            else {
                throw 'Android NDK was not found. Pass -AndroidNdk or set ANDROID_NDK_HOME / ANDROID_NDK_ROOT / ANDROID_HOME.'
            }
        }
        else {
            $ResolvedMsys2Bash = Resolve-Msys2Bash $Msys2Bash

            if (-not (Test-Path $SourceDir)) { throw "hashcat source directory is missing: $SourceDir" }
            if (-not (Test-Path (Join-Path $SourceDir 'src\Makefile'))) { throw "hashcat Makefile is missing in source: $SourceDir" }

            if (Test-Path $PatchFile) {
                Push-Location $SourceDir
                try {
                    $applyCheckExitCode = Invoke-GitApplyCheck $PatchFile
                    if ($applyCheckExitCode -eq 0) {
                        Write-Output "Applying wrapper Android patch: $PatchFile"
                        git apply $PatchFile
                        if ($LASTEXITCODE -ne 0) { throw "git apply failed for: $PatchFile" }
                        $patchApplied = $true
                    }
                    else {
                        $reverseCheckExitCode = Invoke-GitApplyCheck $PatchFile -Reverse
                        if ($reverseCheckExitCode -eq 0) {
                            Write-Output 'Wrapper Android patch is already applied.'
                            $patchApplied = $true
                        }
                        else {
                            throw "Wrapper Android patch cannot be applied cleanly: $PatchFile"
                        }
                    }
                }
                finally { Pop-Location }
            }

            foreach ($androidTarget in $AndroidTargets) {
                Invoke-AndroidTarget $AndroidTargetSpecs[$androidTarget] $ResolvedAndroidNdk $ResolvedMsys2Bash
            }
        }
    }

    if ($DesktopTargets.Count -gt 0) {
        Invoke-DesktopTargets $DesktopTargets
    }
}
finally {
    if (Test-Path $SourceDir) {
        if ($patchApplied) {
            Write-Output 'Reverting temporary wrapper Android patch.'
            git -C $SourceDir checkout -- src/Makefile src/dynloader.c src/main.c src/folder.c src/monitor.c include/types.h src/cpu_features.c deps/scrypt-jane-master/code/scrypt-jane-portable.h modules feeds bridges obj 2>$null
        }
        $restoreErrorActionPreference = $ErrorActionPreference
        try {
            $ErrorActionPreference = 'Continue'
            git -C $SourceDir clean -fdx -- .tmp obj modules bridges feeds 2>$null | Out-Null
            if ($LASTEXITCODE -ne 0) {
                Start-Sleep -Milliseconds 500
                git -C $SourceDir clean -fdx -- .tmp obj modules bridges feeds 2>$null | Out-Null
                if ($LASTEXITCODE -ne 0) {
                    Write-Warning 'Generated Android build artifacts could not be completely removed.'
                }
            }
            git -C $SourceDir checkout -- src/main.c src/folder.c src/monitor.c include/types.h src/cpu_features.c deps/scrypt-jane-master/code/scrypt-jane-portable.h obj modules bridges feeds 2>$null
            if ($LASTEXITCODE -ne 0) {
                Write-Warning 'Generated Android source changes could not be completely restored.'
            }
        }
        finally {
            $ErrorActionPreference = $restoreErrorActionPreference
        }
    }
    if ($lockTaken) { $buildMutex.ReleaseMutex() | Out-Null }
    if ($buildMutex) { $buildMutex.Dispose() }
}
