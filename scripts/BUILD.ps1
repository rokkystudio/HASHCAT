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

function Convert-AndroidAbiToTarget([string]$Abi) {
    switch ($Abi) {
        'arm64-v8a' { return 'android-arm64-v8a' }
        'armeabi-v7a' { return 'android-armeabi-v7a' }
        'x86' { return 'android-x86' }
        'x86_64' { return 'android-x86_64' }
        default { throw "Unsupported Android ABI: $Abi" }
    }
}

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
                foreach ($targetName in @('windows-x64', 'linux-x64', 'macos-x64', 'macos-arm64', 'macos-universal')) { Add-UniqueTarget $expanded $targetName }
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
    if ($files.Count -eq 0) { throw "Cannot package empty directory: $Directory" }

    if (Test-Path $archive) { Remove-Item -Force -Path $archive }
    Compress-Archive -Path (Join-Path $stagingDir '*') -DestinationPath $archive -Force
    Remove-Item -Recurse -Force -Path $stagingDir

    return $archive
}

function Reset-AndroidSourceOutputs([string]$SourceDirectory) {
    git -C $SourceDirectory clean -fdx -- obj modules bridges feeds *> $null
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
        "make hashcat modules bridges feeds UNAME=Android VERSION_TAG=$VersionTag CC=$cc CXX=$cxx AR=llvm-ar $($makeVars -join ' ')"
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
    & $ResolvedMsys2Bash -lc $filteredCommand
    if ($LASTEXITCODE -ne 0) { throw "hashcat Android build failed for $($Config.Abi) with exit code $LASTEXITCODE" }

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

$buildMutex = [System.Threading.Mutex]::new($false, 'Global\HASHCAT_WRAPPER_D_PROJECTS_HASHCAT')
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
                    git apply --check $PatchFile *> $null
                    if ($LASTEXITCODE -eq 0) {
                        Write-Output "Applying wrapper Android patch: $PatchFile"
                        git apply $PatchFile
                        if ($LASTEXITCODE -ne 0) { throw "git apply failed for: $PatchFile" }
                        $patchApplied = $true
                    }
                    else {
                        git apply --reverse --check $PatchFile *> $null
                        if ($LASTEXITCODE -eq 0) {
                            Write-Output 'Wrapper Android patch is already applied.'
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
            git -C $SourceDir checkout -- src/Makefile src/dynloader.c include/types.h src/cpu_features.c modules feeds bridges obj 2>$null
        }
        git -C $SourceDir clean -fdx -- obj modules bridges feeds 2>$null | Out-Null
        git -C $SourceDir checkout -- include/types.h src/cpu_features.c obj modules bridges feeds 2>$null
    }
    if ($lockTaken) { $buildMutex.ReleaseMutex() | Out-Null }
    if ($buildMutex) { $buildMutex.Dispose() }
}
