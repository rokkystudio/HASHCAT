param(
    [string]$MakeJobs = '8',
    [string]$VersionTag = 'v7.1.2',
    [bool]$Package = $true,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'BUILD.ps1') -Target macos-x64 -MakeJobs $MakeJobs -VersionTag $VersionTag -Package:$Package -Clean:$Clean
