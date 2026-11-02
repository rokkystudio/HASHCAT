<#
.SYNOPSIS
Run the production four-stage real Vulkan fence-timeout diagnostic.

.DESCRIPTION
Stages one Android ARM diagnostic executable, submits the public four-stage
Compute program without associating the owned fence, requires the real
vkWaitForFences call to return VK_TIMEOUT with timeout zero, and verifies that
production cleanup waits for device idle. A fresh VkDevice under the same
VkInstance must then execute four dispatches and verify 256 public uint32 words.

.PARAMETER Serial
Connected physical Android ADB serial. The value is not written to transcripts.

.PARAMETER Abi
Exact ARM ABI or auto-select from the device ABI list.

.PARAMETER ReportPrefix
Stable lowercase filename prefix.

.PARAMETER TestCore11
Also run the requested Vulkan 1.1 instance contract.
#>
param (
    [Parameter(Mandatory = $true)]
    [string]$Serial,

    [ValidateSet('auto', 'arm64-v8a', 'armeabi-v7a')]
    [string]$Abi = 'auto',

    [Parameter(Mandatory = $true)]
    [string]$ReportPrefix,

    [switch]$TestCore11
)

$ErrorActionPreference = 'Stop'

if ($ReportPrefix -cnotmatch '^[a-z0-9-]+$') {
    throw 'ReportPrefix must contain lowercase letters, digits and dashes.'
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$build = Join-Path $root 'build\diagnostic-multimode\vulkan-backend-public-fence-timeout-20261102'
$abiOutput = @(& adb -s $Serial shell getprop ro.product.cpu.abilist)

if ($LASTEXITCODE -ne 0 -or $abiOutput.Count -ne 1) {
    throw 'ADB did not return one device ABI list.'
}

$available = @($abiOutput[0].Trim() -split ',')

if ($Abi -eq 'auto') {
    if ($available -contains 'arm64-v8a') {
        $chosen = 'arm64-v8a'
    }
    elseif ($available -contains 'armeabi-v7a') {
        $chosen = 'armeabi-v7a'
    }
    else {
        throw 'Android hardware advertises no compatible ARM ABI.'
    }
}
else {
    $chosen = $Abi
}

if ($available -notcontains $chosen) {
    throw "Android hardware does not advertise the selected ABI $chosen."
}

$exe = if ($chosen -eq 'arm64-v8a') {
    'vulkan-backend-public-fence-timeout-arm64'
}
else {
    'vulkan-backend-public-fence-timeout-armv7'
}

$local = Join-Path $build $exe
$remote = "/data/local/tmp/$exe"

if (-not (Test-Path -LiteralPath $local -PathType Leaf)) {
    throw "Missing Vulkan fence-timeout executable: $local"
}

function Invoke-BackendFenceTimeout {
    <#
    .SYNOPSIS
    Execute one requested Vulkan API contract and capture native exit status.

    .DESCRIPTION
    Runs the staged native executable through ADB, normalizes carriage returns
    and separates the Android process exit marker from the diagnostic body.
    #>
    param ([string]$Api)

    $command = "$remote --minimum $Api" + ' 2>&1; echo FENCE_TIMEOUT_NATIVE_EXIT=$?'
    $response = @(& adb -s $Serial shell $command)

    if ($LASTEXITCODE -ne 0 -or $response.Count -lt 2) {
        throw 'ADB did not return a complete fence-timeout transcript.'
    }

    $lines = @($response | ForEach-Object { $_.TrimEnd([char]13) })
    $exitMarker = $lines[-1].Trim()

    if ($exitMarker -cnotmatch '^FENCE_TIMEOUT_NATIVE_EXIT=([0-9]+)$') {
        throw "Missing native fence-timeout exit status: $exitMarker"
    }

    return @{
        ExitCode = [int]$Matches[1]
        Body = ([string]::Join([char]10, $lines[0..($lines.Count - 2)])).TrimEnd()
    }
}

function Assert-BackendFenceTimeout {
    <#
    .SYNOPSIS
    Validate one real fence timeout, production cleanup and healthy recovery.

    .DESCRIPTION
    Requires an actual vkWaitForFences result of VK_TIMEOUT, a complete owned
    resource inventory with successful vkDeviceWaitIdle cleanup and the exact
    four-stage recovery summary. Synthetic driver-result claims are forbidden.
    #>
    param ([string]$Api, [hashtable]$Result)

    if ($Result.ExitCode -ne 0) {
        throw "Native fence-timeout diagnostic failed: $($Result.Body)"
    }

    $lines = @($Result.Body -split [char]10)
    $wait = 'BACKEND_PUBLIC_FENCE_WAIT submitted=1 fence_submitted=0 timeout_ns=0 driver_call=1 result=2 scheduled=0'
    $release = 'BACKEND_PUBLIC_FENCE_RELEASE submitted=1 device=1 fence=1 command_buffer=1 pipelines=4 buffers=5 wait_idle=0 completed=1 cleanup_result=0 scheduled=0'
    $recovery = 'BACKEND_PUBLIC_FENCE_RECOVERY_PASS timeout=1 new_device=1 dispatches=4 verified_words=256 driver_wait_timeout=1 password_inputs=0 scheduled=0'
    $summary = "BACKEND_PUBLIC_FENCE_DIAGNOSTIC_PASS api=$Api real_queue_submissions=2 real_fence_timeouts=1 cleanup_wait_idle=1 recovered_devices=1 recovered_dispatches=4 verified_words=256 synthetic_driver_results=0 password_inputs=0 scheduled=0"

    foreach ($expected in @($wait, $release, $recovery, $summary)) {
        if (@($lines | Where-Object { $_ -ceq $expected }).Count -ne 1) {
            throw "Missing exact fence-timeout event: $expected"
        }
    }

    if ($lines[-1] -cne $summary) {
        throw "Fence-timeout summary is not the final native event: $($lines[-1])"
    }

    if ($Result.Body.Contains('synthetic_driver_results=1') -or
        $Result.Body.Contains('password_inputs=1') -or
        $Result.Body.Contains('scheduled=1')) {
        throw 'Fence-timeout diagnostic contains an invalid synthetic or scheduler claim.'
    }
}

& adb -s $Serial push $local $remote | Out-Null

if ($LASTEXITCODE -ne 0) {
    throw 'Failed to stage Vulkan fence-timeout diagnostic.'
}

& adb -s $Serial shell chmod 755 $remote | Out-Null

if ($LASTEXITCODE -ne 0) {
    throw 'Failed to make Vulkan fence-timeout diagnostic executable.'
}

$apis = @('1.0')

if ($TestCore11) {
    $apis += '1.1'
}

foreach ($api in $apis) {
    $result = Invoke-BackendFenceTimeout -Api $api

    Assert-BackendFenceTimeout -Api $api -Result $result

    $report = Join-Path $build "$ReportPrefix-$api.log"
    [System.IO.File]::WriteAllText(
        $report,
        $result.Body + [char]10,
        [System.Text.UTF8Encoding]::new($false)
    )

    Write-Output "PASS $chosen Vulkan $api real fence timeout and recovery"
}

& adb -s $Serial shell rm -f $remote | Out-Null

Write-Output "PASS Vulkan real fence-timeout run for $ReportPrefix"
