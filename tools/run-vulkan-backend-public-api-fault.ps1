<#
.SYNOPSIS
Run production Vulkan API-fault cleanup diagnostics on Android hardware.

.DESCRIPTION
Stages the test-only API-fault executable for one supported Android ARM ABI,
runs the five fixed failure scenarios against a physical Vulkan GPU and checks
the final native contract. Each scenario must propagate its synthetic VkResult,
perform real vkDeviceWaitIdle cleanup and complete a healthy four-stage
recovery device under the same VkInstance. Transcripts contain no ADB serial.

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
$build = Join-Path $root 'build\diagnostic-multimode\vulkan-backend-public-api-fault-20261102'
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
    'vulkan-backend-public-api-fault-arm64'
}
else {
    'vulkan-backend-public-api-fault-armv7'
}

$local = Join-Path $build $exe
$remote = "/data/local/tmp/$exe"

if (-not (Test-Path -LiteralPath $local -PathType Leaf)) {
    throw "Missing Vulkan API-fault executable: $local"
}

function Invoke-BackendApiFault {
    <#
    .SYNOPSIS
    Execute one requested Vulkan API contract and capture native exit status.

    .DESCRIPTION
    Runs the staged diagnostic through ADB, strips carriage returns from the
    transcript and separates the exact Android process exit marker from body.
    #>
    param ([string]$Api)

    $command = "$remote --minimum $Api" + ' 2>&1; echo API_FAULT_NATIVE_EXIT=$?'
    $response = @(& adb -s $Serial shell $command)

    if ($LASTEXITCODE -ne 0 -or $response.Count -lt 2) {
        throw 'ADB did not return a complete Vulkan API-fault transcript.'
    }

    $lines = @($response | ForEach-Object { $_.TrimEnd([char]13) })
    $exitMarker = $lines[-1].Trim()

    if ($exitMarker -cnotmatch '^API_FAULT_NATIVE_EXIT=([0-9]+)$') {
        throw "Missing native API-fault exit status: $exitMarker"
    }

    return @{
        ExitCode = [int]$Matches[1]
        Body = ([string]::Join([char]10, $lines[0..($lines.Count - 2)])).TrimEnd()
    }
}

function Assert-BackendApiFault {
    <#
    .SYNOPSIS
    Validate the five fixed failure/recovery scenarios in one transcript.

    .DESCRIPTION
    Requires one injected event, one real cleanup wait and one successful
    recovery for each resource-create/submission site. The wait-failure case
    must report one real queue submission before the synthetic fence result.
    #>
    param ([string]$Api, [hashtable]$Result)

    if ($Result.ExitCode -ne 0) {
        throw "Native Vulkan API-fault diagnostic failed: $($Result.Body)"
    }

    $lines = @($Result.Body -split [char]10)
    $summary = "BACKEND_API_FAULT_DIAGNOSTIC_PASS api=$Api scenarios=5 injected=5 cleanup_wait_idle=5 recovered_devices=5 recovered_dispatches=20 verified_words=1280 real_submit_before_wait_fault=1 real_driver_failures=0 password_inputs=0 scheduled=0"

    if ($lines[-1] -cne $summary) {
        throw "Vulkan API-fault summary is incomplete: $($lines[-1])"
    }

    foreach ($site in @(
        'create_buffer',
        'create_shader_module',
        'create_compute_pipeline',
        'queue_submit',
        'fence_wait'
    )) {
        if (@($lines | Where-Object {
            $_.StartsWith("BACKEND_API_FAULT_INJECT site=$site ")
        }).Count -ne 1) {
            throw "Missing or duplicate injected API-fault event for $site."
        }

        if (@($lines | Where-Object {
            $_.StartsWith("BACKEND_API_FAULT_RECOVERY_PASS site=$site ")
        }).Count -ne 1) {
            throw "Missing or duplicate Vulkan recovery event for $site."
        }
    }

    if (@($lines | Where-Object {
        $_ -match '^BACKEND_API_FAULT_WAIT_IDLE call=1 result=0 real_driver_called=1$'
    }).Count -ne 5) {
        throw 'Expected five successful production cleanup wait-idle calls.'
    }

    $expectedWaitDisarm = 'BACKEND_API_FAULT_DISARM site=fence_wait attempts=1 real_calls=0 real_submissions=1 real_waits=0 wait_idle_calls=1 wait_idle_result=0 injected=1 blocked=0 first_error=-4 real_driver_failure=0'

    if (@($lines | Where-Object {
        $_ -ceq $expectedWaitDisarm
    }).Count -ne 1) {
        throw 'Fence-wait injection did not preserve the real prior queue submission.'
    }

    if ($Result.Body.Contains('scheduled=1') -or
        $Result.Body.Contains('password_inputs=1') -or
        $Result.Body.Contains('real_driver_failure=1')) {
        throw 'Diagnostic unexpectedly claims hash scheduling or a physical driver failure.'
    }
}

& adb -s $Serial push $local $remote | Out-Null

if ($LASTEXITCODE -ne 0) {
    throw 'Failed to stage Vulkan API-fault diagnostic.'
}

& adb -s $Serial shell chmod 755 $remote | Out-Null

if ($LASTEXITCODE -ne 0) {
    throw 'Failed to make Vulkan API-fault diagnostic executable.'
}

$apis = @('1.0')

if ($TestCore11) {
    $apis += '1.1'
}

foreach ($api in $apis) {
    $result = Invoke-BackendApiFault -Api $api

    Assert-BackendApiFault -Api $api -Result $result

    $report = Join-Path $build "$ReportPrefix-$api.log"
    [System.IO.File]::WriteAllText(
        $report,
        $result.Body + [char]10,
        [System.Text.UTF8Encoding]::new($false)
    )

    Write-Output "PASS $chosen Vulkan $api production API-fault cleanup/recovery"
}

& adb -s $Serial shell rm -f $remote | Out-Null

Write-Output "PASS Vulkan production API-fault run for $ReportPrefix"
