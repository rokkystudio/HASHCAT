<#
.SYNOPSIS
Verify controlled Vulkan four-stage cleanup and same-instance recovery.

.DESCRIPTION
Runs the production public-resource fault diagnostic on a physical
Android GPU. Each requested Vulkan API opens one real VkInstance,
visits 37 explicitly numbered cleanup checkpoints, releases the
partially acquired Vulkan/host resources and creates a new working
four-stage VkDevice under the same instance. Each healthy execution
checks 256 public uint32 GPU results against CPU arithmetic.

The injected initialization errors are test-control decisions. No
Vulkan driver return is replaced; point 36 aborts before vkQueueSubmit,
and point 37 aborts after successful vkQueueSubmit while ordinary
cleanup waits for idle. Neither point simulates physical device loss.

Writes UTF-8 LF-normalized ADB transcripts and three invalid-CLI
controls. No HASHCAT password candidate or cryptographic kernel runs.

.PARAMETER Serial
Connected physical Android ADB device serial; never written to logs.

.PARAMETER Abi
Exact supported Android ARM ABI or auto-select from reported ABI list.

.PARAMETER ReportPrefix
Stable lowercase test profile identifier used in evidence filenames.

.PARAMETER TestCore11
Also run the explicitly requested Vulkan 1.1 API contract.
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
$build = Join-Path $root 'build\diagnostic-multimode\vulkan-backend-public-fault-20261102'
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
    'vulkan-backend-public-fault-arm64'
}
else {
    'vulkan-backend-public-fault-armv7'
}

$local = Join-Path $build $exe
$remote = "/data/local/tmp/hc-backend-public-fault-20261102-$ReportPrefix"
$native = "$remote/$exe"

if (-not (Test-Path -LiteralPath $local -PathType Leaf)) {
    throw "Missing Vulkan public fault diagnostic executable: $local"
}

function Invoke-NativeFault {
    <#
    .SYNOPSIS
    Execute one native Android test and capture its exact process exit.

    .DESCRIPTION
    Runs literal native arguments through ADB, records native stdout
    and stderr and reads the final exit status from the Android shell.
    Does not reinterpret test-injected errors as driver errors.
    #>
    param ([string[]]$Arguments)

    $command = "$native $($Arguments -join ' ')" + ' 2>&1; echo FAULT_NATIVE_EXIT=$?'
    $response = @(& adb -s $Serial shell $command)

    if ($LASTEXITCODE -ne 0 -or $response.Count -lt 2) {
        throw 'ADB did not return a complete fault-recovery transcript.'
    }

    $lines = @($response | ForEach-Object { $_.TrimEnd([char]13) })
    $exitMarker = $lines[-1].Trim()

    if ($exitMarker -cnotmatch '^FAULT_NATIVE_EXIT=([0-9]+)$') {
        throw "Missing exact native diagnostic exit status: $exitMarker"
    }

    return @{
        ExitCode = [int]$Matches[1]
        Body = ([string]::Join([char]10, $lines[0..($lines.Count - 2)])).TrimEnd()
    }
}

function Assert-FaultRecovery {
    <#
    .SYNOPSIS
    Verify 37 distinct partial-cleanup paths and 37 real GPU recoveries.

    .DESCRIPTION
    Requires ordered, simulated fault triggers, complete released-job
    reports, successful new four-stage Vulkan sessions and exact CPU
    output verification for every checkpoint. Also checks that both
    submission-boundary controls are reported without driver-loss claims.
    #>
    param ([string]$Api, [hashtable]$Result)

    if ($Result.ExitCode -ne 0) {
        throw "Native fault recovery diagnostic failed: $($Result.Body)"
    }

    $lines = @($Result.Body -split [char]10)
    $summary = "BACKEND_PUBLIC_FAULT_DIAGNOSTIC_PASS api=$Api checkpoints=37 injected=37 recovered_devices=37 recovered_dispatches=148 verified_words=9472 abandoned_before_submit=1 abandoned_after_submit=1 real_driver_losses=0 password_inputs=0 scheduled=0"

    if ($lines[-1] -cne $summary) {
        throw "Native fault recovery summary is incomplete: $($lines[-1])"
    }

    $counts = @{
        'DEVICE_CATALOG_OPEN ' = 1
        'DEVICE_CATALOG_CLOSE ' = 1
        'BACKEND_PUBLIC_FAULT_TRIGGER ' = 37
        'BACKEND_PUBLIC_FAULT_RELEASE ' = 37
        'BACKEND_PUBLIC_FAULT_RECOVERY_PASS ' = 37
        'BACKEND_VULKAN_PROGRAM_ACCEPT ' = 74
        'BACKEND_VULKAN_PROGRAM_DISPATCH_PASS ' = 37
        'BACKEND_VULKAN_PROGRAM_CLOSE ' = 74
        'DEVICE_CATALOG_SELECT ' = 74
    }

    foreach ($prefix in $counts.Keys) {
        $actual = @($lines | Where-Object { $_.StartsWith($prefix) }).Count

        if ($actual -ne $counts[$prefix]) {
            throw "Expected $($counts[$prefix]) $prefix events; got $actual"
        }
    }

    for ($point = 1; $point -le 37; $point++) {
        $trigger = @($lines | Where-Object {
            $_.StartsWith("BACKEND_PUBLIC_FAULT_TRIGGER point=$point ")
        })
        $release = @($lines | Where-Object {
            $_.StartsWith("BACKEND_PUBLIC_FAULT_RELEASE point=$point ")
        })
        $recovery = @($lines | Where-Object {
            $_ -ceq "BACKEND_PUBLIC_FAULT_RECOVERY_PASS point=$point injected=1 new_device=1 dispatches=4 verified_words=256 scheduled=0"
        })

        if ($trigger.Count -ne 1 -or $release.Count -ne 1 -or
            $recovery.Count -ne 1) {
            throw "Controlled Vulkan fault point $point has missing or duplicate events."
        }

        $submitted = if ($point -eq 37) { 1 } else { 0 }
        $expectedTrigger = "^BACKEND_PUBLIC_FAULT_TRIGGER point=$point resource=[a-z_]+ simulated=1 submitted=$submitted result=-3$"

        if ($trigger[0] -cnotmatch $expectedTrigger) {
            throw "Controlled Vulkan fault point $point did not report a real checkpoint."
        }

        if ($release[0] -cnotmatch "^BACKEND_PUBLIC_FAULT_RELEASE point=$point reached=$point hit=1 submitted=$submitted .* wait_idle=0 completed=1 cleanup_result=0 simulated=1$") {
            throw "Vulkan fault point $point was not fully released."
        }

        $triggerIndex = [array]::IndexOf($lines, $trigger[0])
        $releaseIndex = [array]::IndexOf($lines, $release[0])
        $recoveryIndex = [array]::IndexOf($lines, $recovery[0])

        if ($triggerIndex -ge $releaseIndex -or
            $releaseIndex -ge $recoveryIndex) {
            throw "Vulkan partial release and GPU recovery are out of order at point $point."
        }
    }

    if (@($lines | Where-Object {
        $_ -ceq 'BACKEND_PUBLIC_FAULT_CONTROLS_PASS invalid_points=2 invalid_context=3 gpu_submissions=0 scheduled=0'
    }).Count -ne 1) {
        throw 'Invalid resource fault controls were not rejected.'
    }

    if ($Result.Body.Contains('scheduled=1') -or
        $Result.Body.Contains('password_inputs=1') -or
        $Result.Body.Contains('real_driver_losses=1')) {
        throw 'Fault test unexpectedly claims hash scheduling or real device loss.'
    }
}

New-Item -ItemType Directory -Force -Path $build | Out-Null

try {
    & adb -s $Serial shell mkdir -p $remote | Out-Null

    if ($LASTEXITCODE -ne 0) { throw 'Failed to create remote Vulkan test path.' }

    & adb -s $Serial push $local $native | Out-Null

    if ($LASTEXITCODE -ne 0) { throw 'Failed to deploy Android Vulkan diagnostic.' }

    & adb -s $Serial shell chmod 700 $native | Out-Null

    if ($LASTEXITCODE -ne 0) { throw 'Failed to make Android Vulkan diagnostic executable.' }

    $apis = @('1.0')

    if ($TestCore11) { $apis += '1.1' }

    foreach ($api in $apis) {
        $result = Invoke-NativeFault -Arguments @('--minimum', $api)

        Assert-FaultRecovery -Api $api -Result $result

        $entry = @(
            'TEST=vulkan-backend-public-fault',
            "ABI=$chosen",
            "API=$api",
            "ADB_EXIT_CODE=$($result.ExitCode)",
            $result.Body
        )

        [System.IO.File]::WriteAllText(
            (Join-Path $build "$ReportPrefix-$api.log"),
            ([string]::Join([char]10, $entry)) + [char]10,
            [System.Text.UTF8Encoding]::new($false)
        )

        Write-Output "PASS Android $chosen Vulkan $api 37 controlled Vulkan cleanups + 37 fresh GPU recoveries + 9472 checked words"
    }

    $invalid = @(
        @{ Name = 'missing-arguments'; Args = @() },
        @{ Name = 'unsupported-api'; Args = @('--minimum','1.2') },
        @{ Name = 'wrong-flag'; Args = @('--api','1.0') }
    )

    $negative = @(
        'TEST=vulkan-backend-public-fault-invalid-inputs',
        "ABI=$chosen",
        "INVALID_CASES=$($invalid.Count)"
    )

    foreach ($case in $invalid) {
        $result = Invoke-NativeFault -Arguments $case.Args

        if ($result.ExitCode -ne 2) {
            throw "Malformed Vulkan diagnostic CLI succeeded: $($case.Name)"
        }

        $negative += "INVALID_CASE=$($case.Name) exit=2"
    }

    [System.IO.File]::WriteAllText(
        (Join-Path $build "$ReportPrefix-negative-controls.log"),
        ([string]::Join([char]10, $negative)) + [char]10,
        [System.Text.UTF8Encoding]::new($false)
    )

    Write-Output "PASS Android $chosen three native Vulkan fault diagnostic invalid-input controls rejected"
}
finally {
    & adb -s $Serial shell rm -rf $remote 2>$null | Out-Null
}
