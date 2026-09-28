param(
    [string]$RepoUrl = 'https://github.com/hashcat/hashcat.git',
    [string]$Remote = 'origin',
    [string]$SourceRef = 'v7.1.2',
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

$ScriptDir = $PSScriptRoot
$RootDir = (Resolve-Path (Join-Path $ScriptDir '..')).Path
$SourceDir = Join-Path $RootDir 'source'

<#
.SYNOPSIS
Runs a Git command in the upstream source checkout.

.DESCRIPTION
Executes Git in the requested working directory and treats a non-zero exit code as a workspace error.
#>
function Invoke-Git([string[]]$GitArgs, [string]$WorkingDirectory = $SourceDir) {
    Push-Location $WorkingDirectory
    try {
        & git @GitArgs
        if ($LASTEXITCODE -ne 0) { throw "git $($GitArgs -join ' ') failed with exit code $LASTEXITCODE" }
    }
    finally { Pop-Location }
}

$updateMutex = [System.Threading.Mutex]::new($false, 'Global\HASHCAT_WRAPPER_D_PROJECTS_HASHCAT_ANDROID_RUNTIME_V2')
$lockTaken = $false

try {
    $lockTaken = $updateMutex.WaitOne(0)
    if (-not $lockTaken) {
        throw 'Another HASHCAT build/update/clean process is already running. Close/stop the other run and try again.'
    }

    if ([string]::IsNullOrWhiteSpace($SourceRef)) { throw 'SourceRef must not be empty.' }

    if (-not (Test-Path (Join-Path $SourceDir '.git'))) {
    if (Test-Path $SourceDir) {
        $entries = Get-ChildItem $SourceDir -Force -ErrorAction SilentlyContinue
        if (($entries | Measure-Object).Count -gt 0) {
            if (-not $ForceReset) { throw "source exists but is not a git checkout: $SourceDir" }
            Remove-Item -Recurse -Force $SourceDir
        }
    }

    Write-Output "Initializing upstream hashcat checkout in: $SourceDir"
    New-Item -ItemType Directory -Force -Path $SourceDir | Out-Null
    & git -C $SourceDir init
    if ($LASTEXITCODE -ne 0) { throw "git init failed with exit code $LASTEXITCODE" }
}

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

Write-Output "Fetching pinned upstream source ref: $SourceRef"
Invoke-Git -GitArgs @('fetch',$Remote,$SourceRef,'--depth','1','--prune')
Invoke-Git -GitArgs @('checkout','--detach','FETCH_HEAD')

$resolvedSourceRef = (& git -C $SourceDir rev-parse "$SourceRef^{commit}" 2>$null)
if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($resolvedSourceRef)) {
    throw "Fetched SourceRef cannot be resolved to a commit: $SourceRef"
}

$currentCommit = (& git -C $SourceDir rev-parse HEAD).Trim()
if ($currentCommit -ne $resolvedSourceRef.Trim()) {
    throw "source HEAD $currentCommit does not match SourceRef $($resolvedSourceRef.Trim())."
}

Write-Output 'Removing generated, untracked and ignored files from the pinned source checkout.'
Invoke-Git -GitArgs @('clean','-fdx')

if (Test-Path (Join-Path $SourceDir '.gitmodules')) {
    Write-Output 'Updating submodules recursively.'
    Invoke-Git -GitArgs @('submodule','sync','--recursive')
    Invoke-Git -GitArgs @('submodule','update','--init','--recursive','--depth','1')
}

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
    throw "Updated source is incomplete. Missing: $($missing -join ', ')"
}

$summary = [ordered]@{
    Source = $SourceDir
    Remote = (& git -C $SourceDir remote get-url $Remote).Trim()
    SourceRef = $SourceRef
    Commit = (& git -C $SourceDir rev-parse HEAD).Trim()
    Shallow = (& git -C $SourceDir rev-parse --is-shallow-repository).Trim()
    Tags = ((& git -C $SourceDir tag | Measure-Object).Count)
    OpenCLFiles = (Get-ChildItem (Join-Path $SourceDir 'OpenCL') -Recurse -File | Measure-Object).Count
    RuleFiles = (Get-ChildItem (Join-Path $SourceDir 'rules') -Recurse -File | Measure-Object).Count
    TuningFiles = (Get-ChildItem (Join-Path $SourceDir 'tunings') -Recurse -File | Measure-Object).Count
    Status = (& git -C $SourceDir status -sb | Out-String).Trim()
}
[pscustomobject]$summary | Format-List

    if ($BuildAfterUpdate) {
        $buildScript = Join-Path $ScriptDir 'BUILD.ps1'
        if (-not (Test-Path $buildScript)) { throw "BUILD.ps1 is missing: $buildScript" }
        & $buildScript -AndroidAbi $AndroidAbi -Msys2Bash $Msys2Bash -AndroidNdk $AndroidNdk -AndroidApi $AndroidApi -VersionTag $VersionTag -SourceRef $SourceRef
        if ($LASTEXITCODE -ne 0) { throw "BUILD.ps1 failed with exit code $LASTEXITCODE" }
    }
}
finally {
    if ($lockTaken) { $updateMutex.ReleaseMutex() | Out-Null }
    if ($updateMutex) { $updateMutex.Dispose() }
}
