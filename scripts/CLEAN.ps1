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
    return ''
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

$ScriptDir = $PSScriptRoot
$RootDir = (Resolve-Path (Join-Path $ScriptDir '..')).Path
$SourceDir = Join-Path $RootDir 'source'
$BuildRootDir = Join-Path $RootDir 'build'
$TempDir = Join-Path $SourceDir '.tmp'

$Msys2Bash = Resolve-Msys2Bash $Msys2Bash
$AndroidNdk = Resolve-AndroidNdk $AndroidNdk

if ((Test-Path $SourceDir) -and -not [string]::IsNullOrWhiteSpace($Msys2Bash) -and -not [string]::IsNullOrWhiteSpace($AndroidNdk)) {
    $NdkToolchainBin = Join-Path $AndroidNdk 'toolchains\llvm\prebuilt\windows-x86_64\bin'
    $sourceForMsys = $SourceDir -replace '^([A-Za-z]):', '/$1' -replace '\\', '/'
    $ndkBinForMsys = $NdkToolchainBin -replace '^([A-Za-z]):', '/$1' -replace '\\', '/'
    $cmd = @(
        'set +e',
        "cd $sourceForMsys",
        "export PATH=${ndkBinForMsys}:`$PATH",
        "make clean UNAME=Android IS_AARCH64=1 VERSION_TAG=$VersionTag CC=aarch64-linux-android$AndroidApi-clang CXX=aarch64-linux-android$AndroidApi-clang++ AR=llvm-ar >/dev/null 2>&1 || true"
    ) -join '; '
    & $Msys2Bash -lc $cmd
}

if (Test-Path $SourceDir) {
    foreach ($p in @(
        (Join-Path $SourceDir 'hashcat'),
        (Join-Path $SourceDir 'modules'),
        (Join-Path $SourceDir 'bridges'),
        (Join-Path $SourceDir 'feeds'),
        (Join-Path $SourceDir 'obj'),
        $TempDir
    )) {
        if (Test-Path $p) { Remove-Item -Recurse -Force -Path $p }
    }
    git -C $SourceDir checkout -- src/Makefile src/dynloader.c modules feeds bridges obj 2>$null
}

if (Test-Path $BuildRootDir) { Remove-Item -Recurse -Force -Path $BuildRootDir }
New-Item -ItemType Directory -Force -Path $BuildRootDir | Out-Null

[pscustomobject][ordered]@{
    Source = $SourceDir
    BuildRoot = $BuildRootDir
    BuildRootEmpty = ((Get-ChildItem $BuildRootDir -Force -ErrorAction SilentlyContinue | Measure-Object).Count -eq 0)
} | Format-List