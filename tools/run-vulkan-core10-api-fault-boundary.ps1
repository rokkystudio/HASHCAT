<#
.SYNOPSIS
Test explicitly injected Vulkan API call results on actual Android GPUs.

.DESCRIPTION
Runs a dedicated Android API24 executable linked with test-only
--wrap=vkQueueSubmit and --wrap=vkWaitForFences. After real Vulkan
buffer transfers, one API call returns a test-generated negative
VkResult before reaching the driver. The strict gate and API boundary
must reject all later submission attempts, then a separately created
VkDevice performs another real public-data GPU copy.

Archives exact LF-normalized native output and process exit status.
This diagnostic does not induce actual device loss or execute the
production HASHCAT backend, hash computations or shaders.

.PARAMETER Serial
ADB serial of the real physical Android device.

.PARAMETER Abi
Explicit supported ABI or auto discovery from ro.product.cpu.abilist.

.PARAMETER ReportPrefix
Lowercase prefix identifying the tested Android process configuration.

.PARAMETER TestCore11
Also run the explicitly requested Vulkan core 1.1 API mode.

.PARAMETER TestNoncoherent
Also require genuine HOST_VISIBLE noncoherent GPU memory without
fallback to coherent memory.
#>
param (
    [Parameter(Mandatory = $true)]
    [string]$Serial,

    [ValidateSet('auto', 'arm64-v8a', 'armeabi-v7a')]
    [string]$Abi = 'auto',

    [Parameter(Mandatory = $true)]
    [string]$ReportPrefix,

    [switch]$TestCore11,

    [switch]$TestNoncoherent
)

$ErrorActionPreference = 'Stop'

if ($ReportPrefix -cnotmatch '^[a-z0-9-]+$') {
    throw 'ReportPrefix must contain only lowercase letters, numbers and dashes.'
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$build = Join-Path $root 'build\diagnostic-multimode\vulkan-core10-api-fault-boundary-20261101'

New-Item -Path $build -ItemType Directory -Force | Out-Null

$abiOutput = @(& adb -s $Serial shell getprop ro.product.cpu.abilist)

if ($LASTEXITCODE -ne 0 -or $abiOutput.Count -ne 1) {
    throw 'The physical Android device did not return one valid ABI list.'
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
        throw 'The Android device does not advertise an ARM test ABI.'
    }
}
else {
    $chosen = $Abi
}

if ($available -notcontains $chosen) {
    throw "The selected Android ABI is not supported: $chosen"
}

$exe = if ($chosen -eq 'arm64-v8a') {
    'vulkan-core10-api-fault-boundary-arm64'
}
else {
    'vulkan-core10-api-fault-boundary-armv7'
}

$local = Join-Path $build $exe
$remote = "/data/local/tmp/hc-vulkan-api-fault-20261101-$ReportPrefix"
$binary = "$remote/$exe"

if (-not (Test-Path -LiteralPath $local -PathType Leaf)) {
    throw "The native Vulkan API boundary executable is absent: $local"
}

function Invoke-NativeBoundary {
    <#
    .SYNOPSIS
    Capture one native Vulkan API-boundary test and its actual exit code.

    .DESCRIPTION
    Executes exact literal command arguments and records driver and
    test-interposer output through ADB. Normalizes native CRLF to LF
    and requires a tagged native process exit marker.
    #>
    param ([string[]]$Arguments)

    $command = "$binary $($Arguments -join ' ')" + ' 2>&1; echo API_FAULT_NATIVE_EXIT=$?'
    $response = @(& adb -s $Serial shell $command)

    if ($LASTEXITCODE -ne 0 -or $response.Count -lt 2) {
        throw 'The Android Vulkan native process returned incomplete output.'
    }

    $lines = @($response | ForEach-Object { $_.TrimEnd([char]13) })
    $last = $lines[-1].Trim()

    if ($last -cnotmatch '^API_FAULT_NATIVE_EXIT=([0-9]+)$') {
        throw "Android VkResult API boundary exit marker is invalid: $last"
    }

    return @{
        ExitCode = [int]$Matches[1]
        Body = ([string]::Join([char]10, $lines[0..($lines.Count - 2)])).TrimEnd()
    }
}

function Assert-ApiBoundary {
    <#
    .SYNOPSIS
    Require a genuine linked Vulkan API return and strict terminal gate.

    .DESCRIPTION
    Validates two real VkInstance lifetimes, two genuine prior GPU
    copies, an interposed negative VkResult from vkQueueSubmit or
    vkWaitForFences, three host-side denied submissions, three denied
    Vulkan boundary calls, safe old-device release and a new working
    VkDevice using the same parent VkInstance. All test-generated
    statuses are labeled as such, never as real GPU device loss.
    #>
    param (
        [string]$Api,
        [string]$Memory,
        [string]$Site,
        [string]$ResultMode,
        [hashtable]$Native
    )

    if ($Native.ExitCode -ne 0) {
        throw "Vulkan test failed: $($Native.Body)"
    }

    $mode = if ($Memory -eq 'prefer') { 0 } else { 1 }
    $error = if ($ResultMode -eq 'device-lost') { -4 } else { -2 }
    $source = if ($Site -eq 'submit') { 'queue_submit' } else { 'fence_wait' }
    $realSubmitted = if ($Site -eq 'wait') { 1 } else { 0 }
    $waitCount = if ($Site -eq 'wait') { 2 } else { 1 }
    $body = $Native.Body
    $lines = @($body -split [char]10)

    $summary = "API_FAULT_DIAGNOSTIC_PASS api=$Api mode=$mode fault_site=$Site result=$error trials=2 injected=2 actual_driver_loss=0 live_devices=4 completed_gpu_copies=6 verified_words=2190 verified_bytes=8760 blocked_host=6 blocked_driver_calls=6 shaders=0"

    if ($lines[-1] -cne $summary) {
        throw "Native Vulkan fault boundary summary did not match: $summary"
    }

    $counts = @{
        'DEVICE_CATALOG_OPEN ' = 2
        'DEVICE_CATALOG_CLOSE ' = 2
        'TRANSFER_OPEN ' = 4
        'TRANSFER_CLOSE ' = 4
        'TRANSFER_COPY ' = 6
        'API_FAULT_REAL_DRIVER ' = 4
        'API_FAULT_ARM ' = 2
        'API_FAULT_INJECT ' = 2
        'API_FAULT_RETURNED ' = 2
        'API_FAULT_BLOCK ' = 6
        'API_FAULT_BOUNDARY_DENIED ' = 6
        'API_FAULT_HOST_DENIED ' = 6
        'API_FAULT_COUNTS ' = 2
        'API_FAULT_DISARM ' = 2
        'API_FAULT_GOOD_COPY ' = 6
        'API_FAULT_NEW_DEVICE_PASS ' = 2
        'API_FAULT_NEW_DEVICE_CLOSED ' = 2
        'API_FAULT_TRIAL_PASS ' = 2
        'RESULT_GATE_TERMINAL ' = 2
    }

    foreach ($prefix in $counts.Keys) {
        $actual = @($lines | Where-Object { $_.StartsWith($prefix) }).Count

        if ($actual -ne $counts[$prefix]) {
            throw "Vulkan API interception event $prefix expected=$($counts[$prefix]) observed=$actual"
        }
    }

    $inject = if ($Site -eq 'submit') {
        "API_FAULT_INJECT site=queue_submit result=$error real_driver_called=0 real_submissions=0"
    }
    else {
        "API_FAULT_INJECT site=fence_wait result=$error real_driver_called=0 prior_real_submission=1"
    }

    foreach ($event in @(
        "API_FAULT_ARM site=$source result=$error origin=test_linker_wrap real_device_loss=0",
        $inject,
        "API_FAULT_DISARM site=$source result=$error submit_attempts=3 real_submissions=$realSubmitted wait_attempts=$waitCount real_waits=0 injected=1 blocked=3 real_device_loss=0"
    )) {
        if (@($lines | Where-Object { $_ -ceq $event }).Count -ne 2) {
            throw "Expected two independent physical Vulkan API hooks: $event"
        }
    }

    foreach ($trial in @(0, 1)) {
        foreach ($event in @(
            "API_FAULT_REAL_DRIVER trial=$trial site=fence_status observed=1",
            "API_FAULT_REAL_DRIVER trial=$trial site=fence_wait observed=2",
            "API_FAULT_RETURNED trial=$trial result=$error origin=test_linker_wrap real_driver_loss=0 copies=2 output_unchanged=1024",
            "API_FAULT_COUNTS trial=$trial submit_attempts=3 real_submissions=$realSubmitted wait_attempts=$waitCount real_waits=0 injected=1 blocked=3 host_blocked=3 sticky=1",
            "API_FAULT_NEW_DEVICE_PASS trial=$trial fresh_device=1 copies=1 words=64 bytes=256 instance=1 valid=1",
            "API_FAULT_NEW_DEVICE_CLOSED trial=$trial instance=1 owner=0",
            "API_FAULT_TRIAL_PASS trial=$trial injected=1 actual_driver_loss=0 completed_gpu_copies=3 verified_words=1095 verified_bytes=4380 blocked_host=3 blocked_driver_calls=3 devices=2 instance=1 shaders=0"
        )) {
            if (@($lines | Where-Object { $_ -ceq $event }).Count -ne 1) {
                throw "Incorrect Vulkan driver/gate event: $event"
            }
        }

        foreach ($attempt in @(1, 2, 3)) {
            $deny = "API_FAULT_HOST_DENIED trial=$trial attempt=$attempt error=$error copies=2 output_unchanged=1024"

            if (@($lines | Where-Object { $_ -ceq $deny }).Count -ne 1) {
                throw "Host policy failed to deny a GPU transfer: $deny"
            }
        }

        foreach ($event in @(
            "API_FAULT_BOUNDARY_DENIED trial=$trial site=queue_submit attempt=1 result=$error real_driver_called=0",
            "API_FAULT_BOUNDARY_DENIED trial=$trial site=queue_submit attempt=2 result=$error real_driver_called=0",
            "API_FAULT_BOUNDARY_DENIED trial=$trial site=fence_wait attempt=1 result=$error real_driver_called=0"
        )) {
            if (@($lines | Where-Object { $_ -ceq $event }).Count -ne 1) {
                throw "An API call was not stopped at the Vulkan test boundary: $event"
            }
        }
    }

    if ($Memory -eq 'noncoherent') {
        if (@($lines | Where-Object {
            $_.StartsWith('TRANSFER_CLOSE ') -and
            $_ -match ' flushes=3 invalidates=2 wait_result=0$'
        }).Count -ne 2) {
            throw 'The interrupted source mapping did not flush noncoherent memory.'
        }
    }

    $cursor = 0

    foreach ($trial in @(0, 1)) {
        $catalog = $body.IndexOf('DEVICE_CATALOG_OPEN ', $cursor)
        $good = $body.IndexOf("API_FAULT_GOOD_COPY trial=$trial phase=before round=1 ", $cursor)
        $armed = $body.IndexOf("API_FAULT_ARM site=$source ", $cursor)
        $faulted = $body.IndexOf("API_FAULT_INJECT site=$source ", $cursor)
        $denied = $body.IndexOf("API_FAULT_BOUNDARY_DENIED trial=$trial site=fence_wait ", $cursor)
        $disarmed = $body.IndexOf('API_FAULT_DISARM ', $cursor)
        $closed = $body.IndexOf("API_FAULT_OLD_DEVICE_CLOSED trial=$trial ", $cursor)
        $fresh = $body.IndexOf("API_FAULT_NEW_DEVICE_PASS trial=$trial ", $cursor)
        $freshClosed = $body.IndexOf("API_FAULT_NEW_DEVICE_CLOSED trial=$trial ", $cursor)
        $instanceClosed = $body.IndexOf('DEVICE_CATALOG_CLOSE ', $cursor)

        if ($catalog -lt $cursor -or $good -lt $catalog -or
            $armed -lt $good -or $faulted -lt $armed -or
            $denied -lt $faulted -or $disarmed -lt $denied -or
            $closed -lt $disarmed -or $fresh -lt $closed -or
            $freshClosed -lt $fresh -or $instanceClosed -lt $freshClosed) {
            throw "Invalid Vulkan fault, device recreation or instance ownership order in trial $trial."
        }

        $window = $body.Substring($faulted, $disarmed - $faulted)

        if ($window.Contains('TRANSFER_COPY ') -or
            $window.Contains('API_FAULT_GOOD_COPY ')) {
            throw 'A completed public GPU copy was recorded after the test terminal error.'
        }

        $cursor = $instanceClosed + 1
    }
}

Write-Output "API_FAULT_ABI=$chosen source=ro.product.cpu.abilist"

try {
    & adb -s $Serial shell mkdir -p $remote | Out-Null

    if ($LASTEXITCODE -ne 0) { throw 'Cannot prepare the Android Vulkan test path.' }

    & adb -s $Serial push $local $binary | Out-Null

    if ($LASTEXITCODE -ne 0) { throw 'Cannot deploy the Android Vulkan native ELF.' }

    & adb -s $Serial shell chmod 700 $binary | Out-Null

    if ($LASTEXITCODE -ne 0) { throw 'Cannot execute the Android Vulkan native ELF.' }

    $apis = @('1.0')
    if ($TestCore11) { $apis += '1.1' }

    $policies = @('prefer')
    if ($TestNoncoherent) { $policies += 'noncoherent' }

    foreach ($api in $apis) {
        foreach ($policy in $policies) {
            foreach ($site in @('submit', 'wait')) {
                foreach ($terminal in @('device-lost', 'device-memory')) {
                    $result = Invoke-NativeBoundary -Arguments @(
                        '--minimum', $api,
                        '--memory', $policy,
                        '--fault-site', $site,
                        '--result', $terminal
                    )

                    Assert-ApiBoundary -Api $api -Memory $policy -Site $site -ResultMode $terminal -Native $result

                    $metadata = @(
                        'TEST=vulkan-core10-api-fault-boundary',
                        "ABI=$chosen",
                        "API=$api",
                        "MEMORY_MODE=$policy",
                        "FAULT_SITE=$site",
                        "TEST_RESULT=$terminal",
                        "ADB_EXIT_CODE=$($result.ExitCode)",
                        $result.Body
                    )

                    [System.IO.File]::WriteAllText(
                        (Join-Path $build "$ReportPrefix-$api-$policy-$site-$terminal.log"),
                        ([string]::Join([char]10, $metadata)) + [char]10,
                        [System.Text.UTF8Encoding]::new($false)
                    )

                    Write-Output "PASS Vulkan $api $chosen $policy $site $terminal injected=2 real_copies=6 blocked=12"
                }
            }
        }
    }

    $invalid = @(
        @{ Name = 'missing-options'; Args = @() },
        @{ Name = 'unsupported-api'; Args = @('--minimum','1.2','--memory','prefer','--fault-site','submit','--result','device-lost') },
        @{ Name = 'invalid-memory'; Args = @('--minimum','1.0','--memory','unknown','--fault-site','submit','--result','device-lost') },
        @{ Name = 'invalid-site'; Args = @('--minimum','1.0','--memory','prefer','--fault-site','transfer','--result','device-lost') },
        @{ Name = 'invalid-result'; Args = @('--minimum','1.0','--memory','prefer','--fault-site','submit','--result','timeout') },
        @{ Name = 'wrong-flag'; Args = @('--api','1.0','--memory','prefer','--fault-site','submit','--result','device-lost') },
        @{ Name = 'extra-argument'; Args = @('--minimum','1.0','--memory','prefer','--fault-site','submit','--result','device-lost','extra') }
    )

    $negative = @(
        'TEST=vulkan-core10-api-fault-boundary-invalid-inputs',
        "ABI=$chosen",
        "INVALID_CASES=$($invalid.Count)"
    )

    foreach ($case in $invalid) {
        $result = Invoke-NativeBoundary -Arguments $case.Args

        if ($result.ExitCode -ne 2) {
            throw "Malformed native Vulkan API test options were accepted: $($case.Name)"
        }

        $negative += "INVALID_CASE=$($case.Name) exit=2"
        Write-Output "PASS native Vulkan fault option rejected: $($case.Name)"
    }

    [System.IO.File]::WriteAllText(
        (Join-Path $build "$ReportPrefix-negative-controls.log"),
        ([string]::Join([char]10, $negative)) + [char]10,
        [System.Text.UTF8Encoding]::new($false)
    )
}
finally {
    & adb -s $Serial shell rm -rf $remote 2>$null | Out-Null
}
