param(
    [string]$RepoUrl = 'https://github.com/hashcat/hashcat.git',
    [string]$Remote = 'origin',
    [string]$Branch = '',
    [bool]$BuildAfterUpdate = $false,
    [bool]$ForceReset = $false,
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

$RootDir = $PSScriptRoot
$SourceDir = Join-Path $RootDir 'source'

function Invoke-Git([string[]]$GitArgs, [string]$WorkingDirectory = $SourceDir) {
    Push-Location $WorkingDirectory
    try {
        & git @GitArgs
        if ($LASTEXITCODE -ne 0) { throw "git $($GitArgs -join ' ') failed with exit code $LASTEXITCODE" }
    }
    finally { Pop-Location }
}

if (-not (Test-Path (Join-Path $SourceDir '.git'))) {
    if (Test-Path $SourceDir) {
        $entries = Get-ChildItem $SourceDir -Force -ErrorAction SilentlyContinue
        if (($entries | Measure-Object).Count -gt 0) {
            throw "source exists but is not a git checkout: $SourceDir"
        }
    }
    else {
        New-Item -ItemType Directory -Force -Path (Split-Path $SourceDir -Parent) | Out-Null
    }

    Write-Output "Cloning upstream hashcat source into: $SourceDir"
    & git clone $RepoUrl $SourceDir
    if ($LASTEXITCODE -ne 0) { throw "git clone failed with exit code $LASTEXITCODE" }
}

# Make sure origin points to the intended upstream. This script manages only the
# upstream checkout in source/, not the wrapper repository itself.
$currentRemote = ''
try { $currentRemote = (& git -C $SourceDir remote get-url $Remote 2>$null) } catch {}
if ([string]::IsNullOrWhiteSpace($currentRemote)) {
    Invoke-Git -GitArgs @('remote','add',$Remote,$RepoUrl)
}
elseif ($currentRemote.Trim() -ne $RepoUrl) {
    Invoke-Git -GitArgs @('remote','set-url',$Remote,$RepoUrl)
}

$dirty = (& git -C $SourceDir status --porcelain)
if ($dirty -and -not $ForceReset) {
    throw "source has local changes. Re-run with -ForceReset `$true to discard them."
}
if ($dirty -and $ForceReset) {
    Write-Output 'Discarding local source changes because -ForceReset is true.'
    Invoke-Git -GitArgs @('reset','--hard','HEAD')
    Invoke-Git -GitArgs @('clean','-fdx')
}

Write-Output 'Fetching upstream source, tags and pruning deleted refs.'
Invoke-Git -GitArgs @('fetch',$Remote,'--tags','--prune')

$isShallow = (& git -C $SourceDir rev-parse --is-shallow-repository).Trim()
if ($isShallow -eq 'true') {
    Write-Output 'Repository is shallow; unshallowing to get complete history.'
    Invoke-Git -GitArgs @('fetch','--unshallow',$Remote)
}

if ([string]::IsNullOrWhiteSpace($Branch)) {
    $Branch = (& git -C $SourceDir branch --show-current).Trim()
}
if ([string]::IsNullOrWhiteSpace($Branch)) {
    $remoteHead = (& git -C $SourceDir remote show $Remote | Select-String 'HEAD branch' | Select-Object -First 1).Line
    if ($remoteHead -match 'HEAD branch:\s*(\S+)') { $Branch = $Matches[1] }
}
if ([string]::IsNullOrWhiteSpace($Branch)) { $Branch = 'master' }

Write-Output "Updating branch: $Branch"
Invoke-Git -GitArgs @('checkout',$Branch)
Invoke-Git -GitArgs @('pull','--ff-only',$Remote,$Branch)

if (Test-Path (Join-Path $SourceDir '.gitmodules')) {
    Write-Output 'Updating submodules recursively.'
    Invoke-Git -GitArgs @('submodule','sync','--recursive')
    Invoke-Git -GitArgs @('submodule','update','--init','--recursive')
}

# Verify that the source checkout contains every source/runtime directory needed
# for building and packaging the Android runtime. These are source files, not
# generated build outputs.
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
    'pcfg',
    'hashcat.hcstat2'
)
$missing = @()
foreach ($rel in $requiredPaths) {
    $path = Join-Path $SourceDir ($rel -replace '/', [IO.Path]::DirectorySeparatorChar)
    if (-not (Test-Path $path)) { $missing += $rel }
}
if ($missing.Count -gt 0) {
    throw "Updated source is incomplete. Missing: $($missing -join ', ')"
}

$summary = [ordered]@{
    Source = $SourceDir
    Remote = (& git -C $SourceDir remote get-url $Remote).Trim()
    Branch = (& git -C $SourceDir branch --show-current).Trim()
    Commit = (& git -C $SourceDir rev-parse HEAD).Trim()
    Tags = ((& git -C $SourceDir tag | Measure-Object).Count)
    OpenCLFiles = (Get-ChildItem (Join-Path $SourceDir 'OpenCL') -Recurse -File | Measure-Object).Count
    RuleFiles = (Get-ChildItem (Join-Path $SourceDir 'rules') -Recurse -File | Measure-Object).Count
    TuningFiles = (Get-ChildItem (Join-Path $SourceDir 'tunings') -Recurse -File | Measure-Object).Count
    PcfgFiles = (Get-ChildItem (Join-Path $SourceDir 'pcfg') -Recurse -File | Measure-Object).Count
    Status = (& git -C $SourceDir status -sb | Out-String).Trim()
}
[pscustomobject]$summary | Format-List

if ($BuildAfterUpdate) {
    $buildScript = Join-Path $RootDir 'BUILD.ps1'
    if (-not (Test-Path $buildScript)) { throw "BUILD.ps1 is missing: $buildScript" }
    & $buildScript -AndroidAbi $AndroidAbi -Msys2Bash $Msys2Bash -AndroidNdk $AndroidNdk -AndroidApi $AndroidApi -VersionTag $VersionTag
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}