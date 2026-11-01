<#
.SYNOPSIS
Verify genuine Vulkan 1.0 fence NOT_READY/TIMEOUT states on Android GPUs.

.DESCRIPTION
Selects an ABI from the actual connected Android device, executes
two independent Vulkan logical-device lifecycles per API/memory mode,
and requires the real driver's vkGetFenceStatus, zero-timeout waits
and bounded waits to report the exact expected nonfatal VkResult.
Each native process also submits ten public-integer buffer copies,
verifies the signaled fence and performs explicit fence resets.

Vulkan device loss is never provoked or simulated; unexpected Vulkan
errors cause test failure without fallback. Archives LF-normalized
real driver diagnostics but does not execute HASHCAT hashing work.

.PARAMETER Serial
Actual connected Android device serial for ADB operations.

.PARAMETER Abi
Android process ABI or auto selection from ro.product.cpu.abilist.

.PARAMETER ReportPrefix
Lowercase identifier for public Android hardware result files.

.PARAMETER TestCore11
Also request Vulkan core API 1.1 with the same Vulkan 1.0 imports.

.PARAMETER TestNoncoherent
Also require a compatible noncoherent HOST_VISIBLE memory type,
with no automatic fallback when the GPU lacks that memory mode.
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
    throw 'ReportPrefix must contain only lowercase letters, digits and dashes.'
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$output = Join-Path $root 'build\diagnostic-multimode\vulkan-core10-transfer-fence-status-20261101'

New-Item -Path $output -ItemType Directory -Force | Out-Null

$reported = (& adb -s $Serial shell getprop ro.product.cpu.abilist).Trim()

if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($reported)) {
    throw 'The Android device did not report an ABI list.'
}

$available = @($reported -split ',')

if ($Abi -eq 'auto') {
    if ($available -contains 'arm64-v8a') {
        $chosen = 'arm64-v8a'
    }
    elseif ($available -contains 'armeabi-v7a') {
        $chosen = 'armeabi-v7a'
    }
    else {
        throw 'The connected Android device has no supported ARM ABI.'
    }
}
else {
    $chosen = $Abi
}

if ($available -notcontains $chosen) {
    throw "Requested ABI $chosen is not supported by this Android device."
}

$exe = if ($chosen -eq 'arm64-v8a') {
    'vulkan-core10-transfer-fence-status-arm64'
}
else {
    'vulkan-core10-transfer-fence-status-armv7'
}

$local = Join-Path $output $exe
$remote = "/data/local/tmp/hc-real-vulkan-fence-status-20261101-$ReportPrefix"
$binary = "$remote/$exe"

if (-not (Test-Path -LiteralPath $local -PathType Leaf)) {
    throw "The native Vulkan fence-status ELF does not exist: $local"
}

function Invoke-Fence {
    <#
    .SYNOPSIS
    Capture one real Vulkan Android process and its exit status.

    .DESCRIPTION
    Runs the exact supplied native arguments, retains every Vulkan
    driver event, normalizes CRLF to LF and extracts the tagged
    Android process exit marker.
    #>
    param (
        [string[]]$Arguments
    )

    $cmd = "$binary $($Arguments -join ' ')" + ' 2>&1; echo FENCE_NATIVE_EXIT=$?'
    $result = @(& adb -s $Serial shell $cmd)

    if ($LASTEXITCODE -ne 0 -or $result.Count -lt 2) {
        throw 'ADB returned incomplete native Vulkan fence diagnostics.'
    }

    $lines = @($result | ForEach-Object { $_.TrimEnd([char]13) })
    $marker = $lines[-1].Trim()

    if ($marker -cnotmatch '^FENCE_NATIVE_EXIT=([0-9]+)$') {
        throw "The native Android process exit marker is invalid: $marker"
    }

    return @{
        ExitCode = [int]$Matches[1]
        Body = ([string]::Join([char]10, $lines[0..($lines.Count - 2)])).TrimEnd()
    }
}

function Assert-Fence {
    <#
    .SYNOPSIS
    Require exact driver VkResult states and real completed GPU copies.

    .DESCRIPTION
    Checks two independent Vulkan physical-device/instance sessions,
    real VK_NOT_READY and VK_TIMEOUT status values, successful fence
    signaling after each copy, noncoherent memory cache synchronization,
    explicit fence resets, complete public integer data verification
    and VkDevice closure before VkInstance destruction.
    #>
    param (
        [string]$Api,
        [string]$Memory,
        [hashtable]$Result
    )

    if ($Result.ExitCode -ne 0) {
        throw "Android Vulkan fence status/copy failed: $($Result.Body)"
    }

    $mode = if ($Memory -eq 'prefer') { 0 } else { 1 }
    $lines = @($Result.Body -split [char]10)
    $summary = "FENCE_DIAGNOSTIC_PASS api=$Api mode=$mode trials=2 copies=10 verified_words=2706 copied_bytes=10824 not_ready=12 zero_timeouts=12 bounded_timeouts=2 signaled=10 zero_success=10 resets=10 shaders=0"

    if ($lines[-1] -cne $summary) {
        throw 'The Vulkan fence-status native completion record is incomplete.'
    }

    $counts = @{
        'DEVICE_CATALOG_OPEN ' = 2
        'DEVICE_CATALOG_SELECT ' = 2
        'DEVICE_CATALOG_CLOSE ' = 2
        'TRANSFER_OPEN ' = 2
        'TRANSFER_CLOSE ' = 2
        'FENCE_SESSION_OPEN ' = 2
        'FENCE_UNSIGNALED ' = 12
        'FENCE_TIMED_WAIT ' = 2
        'FENCE_PENDING_NO_COPY ' = 2
        'TRANSFER_COPY ' = 10
        'FENCE_SIGNALED ' = 10
        'FENCE_COPY_VERIFIED ' = 10
        'FENCE_RESET ' = 10
        'FENCE_TRIAL_PASS ' = 2
    }

    foreach ($key in $counts.Keys) {
        $actual = @($lines | Where-Object { $_.StartsWith($key) }).Count

        if ($actual -ne $counts[$key]) {
            throw "Missing actual Vulkan fence result $key actual=$actual expected=$($counts[$key])"
        }
    }

    $words = @(1, 7, 64, 257, 1024)

    foreach ($trial in @(0, 1)) {
        foreach ($line in @(
            "FENCE_SESSION_OPEN trial=$trial mode=$mode owner=1 copies=0",
            "FENCE_TIMED_WAIT trial=$trial timeout_ns=1000000 result=2",
            "FENCE_PENDING_NO_COPY trial=$trial output_unchanged=1024 copies=0",
            "FENCE_TRIAL_PASS trial=$trial copies=5 verified_words=1353 copied_bytes=5412 not_ready=6 zero_timeouts=6 bounded_timeouts=1 signaled=5 zero_success=5 resets=5 shaders=0"
        )) {
            if (@($lines | Where-Object { $_ -ceq $line }).Count -ne 1) {
                throw "Incomplete Vulkan real status proof: $line"
            }
        }

        for ($step = 0; $step -le 5; $step++) {
            $unsignaled = "FENCE_UNSIGNALED trial=$trial step=$step status=1 wait_zero=2"

            if (@($lines | Where-Object { $_ -ceq $unsignaled }).Count -ne 1) {
                throw "Missing driver-returned VK_NOT_READY/VK_TIMEOUT: $unsignaled"
            }
        }

        for ($round = 0; $round -lt $words.Count; $round++) {
            $bytes = $words[$round] * 4

            foreach ($line in @(
                "FENCE_SIGNALED trial=$trial round=$round status=0 wait_zero=0",
                "FENCE_COPY_VERIFIED trial=$trial round=$round words=$($words[$round]) bytes=$bytes valid=1",
                "FENCE_RESET trial=$trial round=$round result=0"
            )) {
                if (@($lines | Where-Object { $_ -ceq $line }).Count -ne 1) {
                    throw "Missing signaled GPU fence/verified data: $line"
                }
            }
        }
    }

    if ($Memory -eq 'noncoherent') {
        foreach ($closed in @($lines | Where-Object { $_.StartsWith('TRANSFER_CLOSE ') })) {
            if ($closed -notmatch ' flushes=5 invalidates=5 wait_result=0$') {
                throw 'The GPU did not synchronize actual noncoherent memory.'
            }
        }
    }

    $cursor = 0

    foreach ($trial in @(0, 1)) {
        $opened = $Result.Body.IndexOf('DEVICE_CATALOG_OPEN ', $cursor)
        $started = $Result.Body.IndexOf("FENCE_SESSION_OPEN trial=$trial ", $cursor)
        $pending = $Result.Body.IndexOf("FENCE_UNSIGNALED trial=$trial step=0 ", $cursor)
        $copied = $Result.Body.IndexOf('TRANSFER_COPY ', $cursor)
        $completed = $Result.Body.IndexOf("FENCE_TRIAL_PASS trial=$trial ", $cursor)
        $deviceClosed = $Result.Body.IndexOf('TRANSFER_CLOSE ', $cursor)
        $instanceClosed = $Result.Body.IndexOf('DEVICE_CATALOG_CLOSE ', $cursor)

        if ($opened -lt $cursor -or $started -lt $opened -or
            $pending -lt $started -or $copied -lt $pending -or
            $completed -lt $copied -or $deviceClosed -lt $completed -or
            $instanceClosed -lt $deviceClosed) {
            throw "Vulkan fence lifecycle / VkInstance teardown order is invalid in trial $trial."
        }

        $cursor = $instanceClosed + 1
    }
}

Write-Output "FENCE_TEST_ABI=$chosen source=ro.product.cpu.abilist"

try {
    & adb -s $Serial shell mkdir -p $remote | Out-Null

    if ($LASTEXITCODE -ne 0) {
        throw 'Cannot create the Android Vulkan fence diagnostic directory.'
    }

    & adb -s $Serial push $local $binary | Out-Null

    if ($LASTEXITCODE -ne 0) {
        throw 'Cannot deploy the Android Vulkan fence diagnostic.'
    }

    & adb -s $Serial shell chmod 700 $binary | Out-Null

    if ($LASTEXITCODE -ne 0) {
        throw 'Cannot execute the Android Vulkan fence diagnostic.'
    }

    $apis = @('1.0')
    if ($TestCore11) { $apis += '1.1' }

    $policies = @('prefer')
    if ($TestNoncoherent) { $policies += 'noncoherent' }

    foreach ($api in $apis) {
        foreach ($policy in $policies) {
            $result = Invoke-Fence -Arguments @(
                '--minimum', $api,
                '--memory', $policy
            )

            Assert-Fence -Api $api -Memory $policy -Result $result

            $metadata = @(
                'TEST=vulkan-core10-transfer-fence-status',
                "ABI=$chosen",
                "API=$api",
                "MEMORY_MODE=$policy",
                "ADB_EXIT_CODE=$($result.ExitCode)",
                $result.Body
            )

            [System.IO.File]::WriteAllText(
                (Join-Path $output "$ReportPrefix-$api-$policy.log"),
                ([string]::Join([char]10, $metadata)) + [char]10,
                [System.Text.UTF8Encoding]::new($false)
            )

            Write-Output "PASS actual Android Vulkan $api $chosen $policy VK_NOT_READY=12 VK_TIMEOUT=14 copies=10"
        }
    }

    $invalid = @(
        @{ Name = 'missing-options'; Args = @() },
        @{ Name = 'unsupported-api'; Args = @('--minimum','1.2','--memory','prefer') },
        @{ Name = 'unsupported-memory'; Args = @('--minimum','1.0','--memory','unknown') },
        @{ Name = 'wrong-flag'; Args = @('--api','1.0','--memory','prefer') },
        @{ Name = 'extra-parameter'; Args = @('--minimum','1.0','--memory','prefer','extra') }
    )

    $negative = @(
        'TEST=vulkan-core10-transfer-fence-status-invalid-inputs',
        "ABI=$chosen",
        "INVALID_CASES=$($invalid.Count)"
    )

    foreach ($case in $invalid) {
        $result = Invoke-Fence -Arguments $case.Args

        if ($result.ExitCode -ne 2) {
            throw "Malformed Vulkan fence status option was accepted: $($case.Name)"
        }

        $negative += "INVALID_CASE=$($case.Name) exit=2"
        Write-Output "PASS rejected Vulkan fence CLI option $($case.Name)"
    }

    [System.IO.File]::WriteAllText(
        (Join-Path $output "$ReportPrefix-negative-controls.log"),
        ([string]::Join([char]10, $negative)) + [char]10,
        [System.Text.UTF8Encoding]::new($false)
    )
}
finally {
    & adb -s $Serial shell rm -rf $remote 2>$null | Out-Null
}
