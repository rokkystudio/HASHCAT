param(
    [string]$Msys2Bash = '',
    [string]$Msys2Root = '',
    [string]$MakeJobs = '8',
    [string]$AndroidNdk = '',
    [string]$AndroidApi = '26',
    [string]$VersionTag = 'v7.1.2',
    [bool]$Package = $true,
    [switch]$Clean
)
& (Join-Path $PSScriptRoot 'BUILD-ANDROID.ps1') -Abi x86_64 -Msys2Bash $Msys2Bash -Msys2Root $Msys2Root -MakeJobs $MakeJobs -AndroidNdk $AndroidNdk -AndroidApi $AndroidApi -VersionTag $VersionTag -Package:$Package -Clean:$Clean