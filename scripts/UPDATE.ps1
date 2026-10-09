param(
    [string]$SourceRef = '',
    [bool]$BuildAfterUpdate = $false,
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

$ScriptDir = $PSScriptRoot
$RootDir = (Resolve-Path (Join-Path $ScriptDir '..')).Path
$SourceDir = Join-Path $RootDir 'source'

$requiredPaths = @(
    'src',
    'src/Makefile',
    'src/main.c',
    'src/modules',
    'include',
    'deps',
    'OpenCL',
    'rules',
    'tunings',
    'hashcat.hcstat2'
)
$missing = @()
foreach ($rel in $requiredPaths) {
    $path = Join-Path $SourceDir ($rel -replace '/', [IO.Path]::DirectorySeparatorChar)
    if (-not (Test-Path $path)) { $missing += $rel }
}
if ($missing.Count -gt 0) {
    throw "Source tree is incomplete. Missing: $($missing -join ', ')"
}

if (-not [string]::IsNullOrWhiteSpace($SourceRef)) {
    $head = (& git -C $RootDir rev-parse HEAD).Trim()
    $expected = (& git -C $RootDir rev-parse "$SourceRef^{commit}" 2>$null)
    if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($expected)) {
        throw "SourceRef cannot be resolved in HASHCAT repo: $SourceRef"
    }
    if ($head -ne $expected.Trim()) {
        throw "HASHCAT HEAD $head does not match SourceRef $($expected.Trim())."
    }
}

$summary = [ordered]@{
    Root = $RootDir
    Source = $SourceDir
    SourceRef = if ([string]::IsNullOrWhiteSpace($SourceRef)) { '(working tree)' } else { $SourceRef }
    Commit = (& git -C $RootDir rev-parse HEAD).Trim()
    OpenCLFiles = (Get-ChildItem (Join-Path $SourceDir 'OpenCL') -Recurse -File | Measure-Object).Count
    RuleFiles = (Get-ChildItem (Join-Path $SourceDir 'rules') -Recurse -File | Measure-Object).Count
    TuningFiles = (Get-ChildItem (Join-Path $SourceDir 'tunings') -Recurse -File | Measure-Object).Count
    Status = (& git -C $RootDir status -sb | Out-String).Trim()
}
[pscustomobject]$summary | Format-List

if ($BuildAfterUpdate) {
    $buildScript = Join-Path $ScriptDir 'BUILD.ps1'
    if (-not (Test-Path $buildScript)) { throw "BUILD.ps1 is missing: $buildScript" }
    & $buildScript -AndroidAbi $AndroidAbi -Msys2Bash $Msys2Bash -AndroidNdk $AndroidNdk -AndroidApi $AndroidApi -VersionTag $VersionTag -SourceRef $SourceRef
    if ($LASTEXITCODE -ne 0) { throw "BUILD.ps1 failed with exit code $LASTEXITCODE" }
}
