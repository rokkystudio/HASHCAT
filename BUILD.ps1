param(
    [string]$AndroidAbi = 'arm64-v8a',
    [string]$Msys2Bash = '',
    [string]$AndroidNdk = '',
    [string]$AndroidApi = '26',
    [string]$VersionTag = 'v7.1.2'
)

$ErrorActionPreference = 'Stop'
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
[Console]::InputEncoding = $utf8NoBom
[Console]::OutputEncoding = $utf8NoBom
$OutputEncoding = $utf8NoBom

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
    throw 'Android NDK was not found. Pass -AndroidNdk or set ANDROID_NDK_HOME / ANDROID_NDK_ROOT / ANDROID_HOME.'
}

$RootDir = $PSScriptRoot
$SourceDir = Join-Path $RootDir 'source'
$BuildRootDir = Join-Path $RootDir 'build'
$BuildAbiDir = Join-Path $BuildRootDir $AndroidAbi
$TempDir = Join-Path $SourceDir '.tmp'
$PatchFile = Join-Path $RootDir 'patches\hashcat-android-ndk.patch'
$PatchApplied = $false

$Msys2Bash = Resolve-Msys2Bash $Msys2Bash
$AndroidNdk = Resolve-AndroidNdk $AndroidNdk
$NdkToolchainBin = Join-Path $AndroidNdk 'toolchains\llvm\prebuilt\windows-x86_64\bin'

if (-not (Test-Path $SourceDir)) { throw "hashcat source directory is missing: $SourceDir" }
if (-not (Test-Path (Join-Path $SourceDir 'src\Makefile'))) { throw "hashcat Makefile is missing in source: $SourceDir" }
if (-not (Test-Path (Join-Path $NdkToolchainBin "aarch64-linux-android$AndroidApi-clang.cmd"))) {
    throw "Android NDK clang is missing in: $NdkToolchainBin"
}

try {
    if (Test-Path $PatchFile) {
        Push-Location $SourceDir
        try {
            git apply --check $PatchFile *> $null
            if ($LASTEXITCODE -eq 0) {
                Write-Output "Applying wrapper Android patch: $PatchFile"
                git apply $PatchFile
                if ($LASTEXITCODE -ne 0) { throw "git apply failed for: $PatchFile" }
                $PatchApplied = $true
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

    New-Item -ItemType Directory -Force -Path $TempDir | Out-Null

    $sourceForMsys = $SourceDir -replace '^([A-Za-z]):', '/$1' -replace '\\', '/'
    $ndkBinForMsys = $NdkToolchainBin -replace '^([A-Za-z]):', '/$1' -replace '\\', '/'
    $tempForMsys = $TempDir -replace '^([A-Za-z]):', '/$1' -replace '\\', '/'

    $makeCommand = @(
        'set -e',
        "cd $sourceForMsys",
        "export PATH=${ndkBinForMsys}:`$PATH",
        "export TMPDIR=$tempForMsys",
        "export TMP=$tempForMsys",
        "export TEMP=$tempForMsys",
        "make hashcat modules bridges feeds UNAME=Android IS_AARCH64=1 VERSION_TAG=$VersionTag CC=aarch64-linux-android$AndroidApi-clang CXX=aarch64-linux-android$AndroidApi-clang++ AR=llvm-ar"
    ) -join '; '

    $skipNoticeRegex = 'Skipping (freethreaded|regular) plugin (72000|73000|74000)'
    $skipNoticeRegex += '|To use -m 7[234]000, you must install'
    $skipNoticeRegex += '|To use it, you must install Rust'
    $skipNoticeRegex += '|Otherwise, you can safely ignore this warning'
    $skipNoticeRegex += '|For more information, see .docs/hashcat-(python|rust)-plugin-requirements'
    $skipNoticeRegex += '|Skipping generic attack-mode 8 plugin'
    $filteredCommand = "$makeCommand 2>&1 | grep -v -E '$skipNoticeRegex'; exit `${PIPESTATUS[0]}"

    & $Msys2Bash -lc $filteredCommand
    if ($LASTEXITCODE -ne 0) { throw "hashcat Android build failed with exit code $LASTEXITCODE" }

    if (Test-Path $BuildAbiDir) { Remove-Item -Recurse -Force -Path $BuildAbiDir }
    New-Item -ItemType Directory -Force -Path $BuildAbiDir | Out-Null

    $frontend = Join-Path $SourceDir 'hashcat'
    if (-not (Test-Path $frontend)) { throw "hashcat frontend was not produced: $frontend" }
    Copy-Item -Force -Path $frontend -Destination (Join-Path $BuildAbiDir 'hashcat')

    foreach ($name in @('modules', 'bridges', 'feeds')) {
        $src = Join-Path $SourceDir $name
        $dst = Join-Path $BuildAbiDir $name
        if (-not (Test-Path $src)) { throw "Required native output directory is missing: $src" }
        New-Item -ItemType Directory -Force -Path $dst | Out-Null
        Get-ChildItem -Path $src -Filter '*.so' -File | ForEach-Object {
            Copy-Item -Force -Path $_.FullName -Destination (Join-Path $dst $_.Name)
        }
    }

    foreach ($name in @('OpenCL', 'rules', 'tunings', 'pcfg')) {
        $src = Join-Path $SourceDir $name
        $dst = Join-Path $BuildAbiDir $name
        if (-not (Test-Path $src)) { throw "Required runtime asset directory is missing: $src" }
        if (Test-Path $dst) { Remove-Item -Recurse -Force -Path $dst }
        Copy-Item -Recurse -Force -Path $src -Destination $dst
    }

    $hcstat = Join-Path $SourceDir 'hashcat.hcstat2'
    if (Test-Path $hcstat) {
        Copy-Item -Force -Path $hcstat -Destination (Join-Path $BuildAbiDir 'hashcat.hcstat2')
    }

    [pscustomobject][ordered]@{
        Root = $RootDir
        Source = $SourceDir
        AndroidAbi = $AndroidAbi
        Build = $BuildAbiDir
        FrontendBytes = (Get-Item (Join-Path $BuildAbiDir 'hashcat')).Length
        Modules = (Get-ChildItem (Join-Path $BuildAbiDir 'modules') -Filter '*.so' -File | Measure-Object).Count
        Bridges = (Get-ChildItem (Join-Path $BuildAbiDir 'bridges') -Filter '*.so' -File | Measure-Object).Count
        Feeds = (Get-ChildItem (Join-Path $BuildAbiDir 'feeds') -Filter '*.so' -File | Measure-Object).Count
        OpenCL = (Get-ChildItem (Join-Path $BuildAbiDir 'OpenCL') -Recurse -File | Measure-Object).Count
    } | Format-List
}
finally {
    if ($PatchApplied) {
        Write-Output 'Reverting temporary wrapper Android patch.'
        git -C $SourceDir checkout -- src/Makefile src/dynloader.c
    }
}