param(
    [ValidateSet('all', 'arm64-v8a', 'armeabi-v7a', 'x86', 'x86_64')]
    [string[]]$Abi = @('all'),
    [string]$Msys2Bash = '',
    [string]$Msys2Root = '',
    [string]$MakeJobs = '8',
    [string]$AndroidNdk = '',
    [string]$AndroidApi = '26',
    [string]$VersionTag = 'v7.1.2',
    [bool]$Package = $true,
    [switch]$NoVulkan,
    [switch]$Clean
)

$targets = [System.Collections.Generic.List[string]]::new()
foreach ($item in $Abi) {
    switch ($item) {
        'all' {
            foreach ($target in @('android-arm64-v8a', 'android-armeabi-v7a', 'android-x86_64', 'android-x86')) {
                if (-not $targets.Contains($target)) { [void]$targets.Add($target) }
            }
        }
        'arm64-v8a' { if (-not $targets.Contains('android-arm64-v8a')) { [void]$targets.Add('android-arm64-v8a') } }
        'armeabi-v7a' { if (-not $targets.Contains('android-armeabi-v7a')) { [void]$targets.Add('android-armeabi-v7a') } }
        'x86_64' { if (-not $targets.Contains('android-x86_64')) { [void]$targets.Add('android-x86_64') } }
        'x86' { if (-not $targets.Contains('android-x86')) { [void]$targets.Add('android-x86') } }
    }
}

# Vulkan is part of the normal Android runtime since M3. -NoVulkan keeps the
# old OpenCL-only layout available for debugging and bisects.
& (Join-Path $PSScriptRoot 'BUILD.ps1') -Target @($targets) -AndroidNdk $AndroidNdk -AndroidApi $AndroidApi -Msys2Bash $Msys2Bash -Msys2Root $Msys2Root -MakeJobs $MakeJobs -VersionTag $VersionTag -Package:$Package -EnableVulkan:(-not $NoVulkan) -Clean:$Clean
