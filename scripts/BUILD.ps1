param(
    [ValidateSet('android', 'android-arm64-v8a', 'windows-x64', 'linux-x64', 'macos-x64', 'macos-arm64', 'macos-universal', 'all')]
    [string[]]$Target = @('android-arm64-v8a', 'windows-x64'),
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
$expandedTargetList = [System.Collections.Generic.List[string]]::new()
foreach ($item in $Target) {
    switch ($item) {
        'android' { [void]$expandedTargetList.Add('android-arm64-v8a') }
        'all' {
            foreach ($expanded in @('android-arm64-v8a', 'windows-x64', 'linux-x64', 'macos-x64', 'macos-arm64', 'macos-universal')) {
                [void]$expandedTargetList.Add($expanded)
            }
        }
        default { [void]$expandedTargetList.Add($item) }
    }
}

$ExpandedTargets = @($expandedTargetList | Select-Object -Unique)
$AndroidRequested = $ExpandedTargets -contains 'android-arm64-v8a'
$DesktopTargets = @($ExpandedTargets | Where-Object { $_ -ne 'android-arm64-v8a' })

function Invoke-DesktopTargets([string[]]$RequestedTargets) {
    $RequestedTargets = @($RequestedTargets | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    if ($RequestedTargets.Count -eq 0) { return }

    $desktopScript = Join-Path $ScriptDir 'BUILD-DESKTOP.ps1'
    if (-not (Test-Path -LiteralPath $desktopScript)) {
        throw "Desktop build script is missing: $desktopScript"
    }

    $desktopParams = @{
        Target = $RequestedTargets
    }
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

$ScriptDir = $PSScriptRoot
$RootDir = (Resolve-Path (Join-Path $ScriptDir '..')).Path

$script:HashcatBuildMutex = [System.Threading.Mutex]::new($false, 'Global\HASHCAT_WRAPPER_D_PROJECTS_HASHCAT')
$script:HashcatBuildLockTaken = $false
function Release-HashcatBuildLock {
    if ($script:HashcatBuildLockTaken) {
        $script:HashcatBuildMutex.ReleaseMutex() | Out-Null
        $script:HashcatBuildLockTaken = $false
    }
    if ($script:HashcatBuildMutex) {
        $script:HashcatBuildMutex.Dispose()
        $script:HashcatBuildMutex = $null
    }
}
trap {
    Release-HashcatBuildLock
    break
}
$script:HashcatBuildLockTaken = $script:HashcatBuildMutex.WaitOne(0)
if (-not $script:HashcatBuildLockTaken) {
    throw 'Another HASHCAT build/update/clean process is already running. Close/stop the other run and try again.'
}

$SourceDir = Join-Path $RootDir 'source'
$BuildRootDir = Join-Path $RootDir 'build'
$BuildAbiDir = Join-Path $BuildRootDir $AndroidAbi
$TempDir = Join-Path $SourceDir '.tmp'
$PatchFile = Join-Path $RootDir 'patches\hashcat-android-ndk.patch'
$PatchApplied = $false

if (-not $AndroidRequested) {
    try { Invoke-DesktopTargets $DesktopTargets } finally { Release-HashcatBuildLock }
    return
}

$AndroidNdk = Resolve-AndroidNdk $AndroidNdk
if ([string]::IsNullOrWhiteSpace($AndroidNdk)) {
    if ($DesktopTargets.Count -gt 0) {
        Write-Warning "Android NDK was not found; skipping android-arm64-v8a and building desktop target(s): $($DesktopTargets -join ', ')"
        try { Invoke-DesktopTargets $DesktopTargets } finally { Release-HashcatBuildLock }
        return
    }

    throw 'Android NDK was not found. Pass -AndroidNdk or set ANDROID_NDK_HOME / ANDROID_NDK_ROOT / ANDROID_HOME.'
}

$Msys2Bash = Resolve-Msys2Bash $Msys2Bash
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
        'mkdir -p obj modules bridges feeds',
        "export PATH=${ndkBinForMsys}:`$PATH",
        "export TMPDIR=$tempForMsys",
        "export TMP=$tempForMsys",
        "export TEMP=$tempForMsys",
        "make hashcat modules bridges feeds UNAME=Android IS_AARCH64=1 VERSION_TAG=$VersionTag CC=aarch64-linux-android$AndroidApi-clang CXX=aarch64-linux-android$AndroidApi-clang++ AR=llvm-ar PYTHON_MP_SKIP_SO=true PYTHON_SP_SKIP_SO=true"
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
        git -C $SourceDir checkout -- src/Makefile src/dynloader.c modules feeds bridges obj
    }
}
if ($DesktopTargets.Count -gt 0) {
    Invoke-DesktopTargets $DesktopTargets
}
Release-HashcatBuildLock
