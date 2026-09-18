param(
    [string]$Remote = 'origin',
    [string]$Branch = '',
    [bool]$BuildAfterUpdate = $false
)

$ErrorActionPreference = 'Stop'
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
[Console]::InputEncoding = $utf8NoBom
[Console]::OutputEncoding = $utf8NoBom
$OutputEncoding = $utf8NoBom

$RootDir = $PSScriptRoot
$SourceDir = Join-Path $RootDir 'source'
if (-not (Test-Path (Join-Path $SourceDir '.git'))) { throw "source is not a git checkout: $SourceDir" }

Push-Location $SourceDir
try {
    git fetch $Remote
    if ([string]::IsNullOrWhiteSpace($Branch)) { $Branch = git branch --show-current }
    if ([string]::IsNullOrWhiteSpace($Branch)) { throw 'Cannot determine current branch. Pass -Branch explicitly.' }
    git pull --ff-only $Remote $Branch
    git status -sb
}
finally { Pop-Location }

if ($BuildAfterUpdate) { & (Join-Path $RootDir 'BUILD.ps1') }