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
    [string]$SourceRef = 'v7.1.2',
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

<#
.SYNOPSIS
Prepares the exact upstream source revision used by the build.

.DESCRIPTION
Requires source HEAD to resolve to SourceRef, refuses tracked source changes and removes untracked or ignored source outputs before release artifacts are produced.
#>
function Prepare-SourceRevision([string]$ExpectedRef) {
    if (-not (Test-Path (Join-Path $SourceDir '.git'))) {
        throw "hashcat source checkout is missing: $SourceDir. Run scripts\UPDATE.ps1 first."
    }

    $head = (& git -C $SourceDir rev-parse HEAD).Trim()
    $expected = (& git -C $SourceDir rev-parse "$ExpectedRef^{commit}" 2>$null)
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($expected)) {
        throw "Required upstream source ref is not available: $ExpectedRef. Run scripts\UPDATE.ps1."
    }

    if ($head -ne $expected.Trim()) {
        throw "source HEAD $head does not match required SourceRef $($expected.Trim()). Run scripts\UPDATE.ps1 before building."
    }

    $trackedChanges = (& git -C $SourceDir status --porcelain --untracked-files=no)
    if ($trackedChanges) {
        throw 'source contains tracked local changes. Release builds require the pinned upstream checkout.'
    }

    & git -C $SourceDir clean -fdx | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw 'Unable to remove generated, untracked or ignored files from source.'
    }
}

$AndroidTargetSpecs = [ordered]@{
    'android-arm64-v8a' = [pscustomobject][ordered]@{
        Target = 'android-arm64-v8a'
        Abi = 'arm64-v8a'
        ClangPrefix = 'aarch64-linux-android'
        RuntimeTriple = 'aarch64-linux-android'
        MakeVars = @('IS_AARCH64=1', 'IS_ARM=1')
    }
    'android-armeabi-v7a' = [pscustomobject][ordered]@{
        Target = 'android-armeabi-v7a'
        Abi = 'armeabi-v7a'
        ClangPrefix = 'armv7a-linux-androideabi'
        RuntimeTriple = 'arm-linux-androideabi'
        MakeVars = @('IS_AARCH64=0', 'IS_ARM=1')
    }
    'android-x86_64' = [pscustomobject][ordered]@{
        Target = 'android-x86_64'
        Abi = 'x86_64'
        ClangPrefix = 'x86_64-linux-android'
        RuntimeTriple = 'x86_64-linux-android'
        MakeVars = @('IS_AARCH64=0', 'IS_ARM=0')
    }
    'android-x86' = [pscustomobject][ordered]@{
        Target = 'android-x86'
        Abi = 'x86'
        ClangPrefix = 'i686-linux-android'
        RuntimeTriple = 'i686-linux-android'
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
    if (-not [string]::IsNullOrWhiteSpace($SourceRef)) { $desktopParams.SourceRef = $SourceRef }
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

<#
.SYNOPSIS
Removes generated Android outputs from the disposable upstream checkout.

.DESCRIPTION
Deletes every untracked and ignored file produced by prior builds while preserving tracked source files and temporary tracked Android patch changes.
#>
function Reset-AndroidSourceOutputs([string]$SourceDirectory) {
    & git -C $SourceDirectory clean -fdx | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to clean generated Android source outputs: $SourceDirectory"
    }
}

<#
.SYNOPSIS
Builds one Android ABI from the pinned upstream source plus the repository Android patch.

.DESCRIPTION
Uses Android NDK Clang through MSYS2, builds the hashcat frontend and native modules/bridges, then copies ABI-specific binaries and shared runtime assets into build and release directories.
#>
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

    # hashcat's Rust bridge rules can build host .dll/.so plugins, but they do not currently cross-build
    # Rust sub-plugins for Android ABIs. Keep Android artifacts native/NDK-only instead of mixing host Rust output.
    $makeVars += @(
        'MAINTAINER_MODE=1',
        'PYTHON_CONFIG=',
        'REPORT_MISSING_SO=true',
        'RUST_CARGO=__hashcat_android_cross_cargo_disabled__',
        'RUST_RUSTUP=__hashcat_android_cross_rustup_disabled__'
    )

    $makeCommand = @(
        'set -e',
        "cd $sourceForMsys",
        'mkdir -p obj modules bridges bridges/subs',
        "export PATH=${ndkBinForMsys}:`$PATH",
        "export TMPDIR=$tempForMsys",
        "export TMP=$tempForMsys",
        "export TEMP=$tempForMsys",
        "make -j $MakeJobs hashcat modules bridges UNAME=Android PRODUCTION=1 VERSION_TAG=$VersionTag CC=$cc CXX=$cxx AR=llvm-ar $($makeVars -join ' ')"
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

    $libcxx = Join-Path $ResolvedAndroidNdk "toolchains\llvm\prebuilt\windows-x86_64\sysroot\usr\lib\$($Config.RuntimeTriple)\libc++_shared.so"
    if (-not (Test-Path -LiteralPath $libcxx)) { throw "Android libc++ runtime is missing: $libcxx" }
    Copy-Item -Force -LiteralPath $libcxx -Destination (Join-Path $buildAbiDir 'libc++_shared.so')

    foreach ($name in @('modules', 'bridges')) {
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

    foreach ($name in @('OpenCL', 'rules', 'tunings')) {
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

    Prepare-SourceRevision $SourceRef

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
        if ($AndroidTargets.Count -gt 0) {
            Reset-AndroidSourceOutputs $SourceDir

            if ($patchApplied) {
                Write-Output 'Restoring upstream source before desktop builds.'
                git -C $SourceDir checkout -- src/Makefile src/affinity.c src/main.c src/folder.c src/monitor.c include/types.h src/cpu_features.c src/bridges/bridge_argon2id_reference.c deps/scrypt-jane-master/code/scrypt-jane-portable.h
                if ($LASTEXITCODE -ne 0) { throw 'Unable to restore upstream source before desktop builds.' }
                $patchApplied = $false
            }
        }

        Invoke-DesktopTargets $DesktopTargets
    }
}
finally {
    if (Test-Path $SourceDir) {
        if ($patchApplied) {
            Write-Output 'Reverting temporary wrapper Android patch.'
            git -C $SourceDir checkout -- src/Makefile src/affinity.c src/main.c src/folder.c src/monitor.c include/types.h src/cpu_features.c src/bridges/bridge_argon2id_reference.c deps/scrypt-jane-master/code/scrypt-jane-portable.h 2>$null
        }
        $restoreErrorActionPreference = $ErrorActionPreference
        try {
            $ErrorActionPreference = 'Continue'
            git -C $SourceDir clean -fdx 2>$null | Out-Null
            if ($LASTEXITCODE -ne 0) {
                Start-Sleep -Milliseconds 500
                git -C $SourceDir clean -fdx 2>$null | Out-Null
                if ($LASTEXITCODE -ne 0) {
                    Write-Warning 'Generated Android build artifacts could not be completely removed.'
                }
            }
            git -C $SourceDir checkout -- src/Makefile src/affinity.c src/main.c src/folder.c src/monitor.c include/types.h src/cpu_features.c src/bridges/bridge_argon2id_reference.c deps/scrypt-jane-master/code/scrypt-jane-portable.h 2>$null
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
