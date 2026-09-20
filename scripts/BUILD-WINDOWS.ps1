param(
    [string]$Msys2Bash = '',
    [string]$Msys2Root = '',
    [string]$MakeJobs = '8',
    [string]$VersionTag = 'v7.1.2',
    [bool]$Package = $true,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'BUILD.ps1') -Target windows-x64 -Msys2Bash $Msys2Bash -Msys2Root $Msys2Root -MakeJobs $MakeJobs -VersionTag $VersionTag -Package:$Package -Clean:$Clean
