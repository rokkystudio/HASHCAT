param(
    [string]$AndroidNdk = '',
    [string]$AndroidApi = '26',
    [string]$AndroidAbi = 'arm64-v8a',
    [string]$Msys2Bash = '',
    [string]$Msys2Root = '',
    [string]$WslDistro = '',
    [string]$MakeJobs = '8',
    [string]$VersionTag = 'v7.1.2',
    [bool]$Package = $true,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'BUILD.ps1') -Target all -AndroidNdk $AndroidNdk -AndroidApi $AndroidApi -AndroidAbi $AndroidAbi -Msys2Bash $Msys2Bash -Msys2Root $Msys2Root -WslDistro $WslDistro -MakeJobs $MakeJobs -VersionTag $VersionTag -Package:$Package -Clean:$Clean
