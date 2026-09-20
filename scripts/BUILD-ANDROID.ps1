param(
    [string]$AndroidNdk = '',
    [string]$AndroidApi = '26',
    [string]$AndroidAbi = 'arm64-v8a',
    [string]$Msys2Bash = '',
    [string]$Msys2Root = '',
    [string]$MakeJobs = '8',
    [string]$VersionTag = 'v7.1.2',
    [bool]$Package = $true,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'BUILD.ps1') -Target android-arm64-v8a -AndroidNdk $AndroidNdk -AndroidApi $AndroidApi -AndroidAbi $AndroidAbi -Msys2Bash $Msys2Bash -Msys2Root $Msys2Root -MakeJobs $MakeJobs -VersionTag $VersionTag -Package:$Package -Clean:$Clean
