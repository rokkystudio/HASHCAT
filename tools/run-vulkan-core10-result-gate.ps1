<#
.SYNOPSIS
Verify fail-closed Vulkan submission decisions on real Android GPUs.

.DESCRIPTION
Runs a standalone Vulkan 1.0/1.1 buffer diagnostic using the actual
Android process ABI and genuine GPU fence/copy events. After two real
public-integer copies per VkDevice, a clearly labeled application-only
VkResult tests terminal state persistence and three refused further
submission requests. No actual GPU device loss is induced or claimed.

The script saves exact UTF-8 LF-native driver diagnostics and six
rejected CLI controls per connected Android ABI. The production
HASHCAT scheduler and cryptographic kernels are not exercised.

.PARAMETER Serial
Actual connected Android device serial for ADB access.

.PARAMETER Abi
Supported ARM ABI or auto selection from ro.product.cpu.abilist.

.PARAMETER ReportPrefix
Lowercase public identifier used in archived diagnostic filenames.

.PARAMETER TestCore11
Also verify an explicitly requested Vulkan 1.1 instance.

.PARAMETER TestNoncoherent
Also require true HOST_VISIBLE, noncoherent memory; no implicit
coherent fallback is permitted.
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
    throw 'ReportPrefix must use lowercase letters, numbers and dashes only.'
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$output = Join-Path $root 'build\diagnostic-multimode\vulkan-core10-result-gate-20261101'

New-Item -Path $output -ItemType Directory -Force | Out-Null

$abiOutput = @(& adb -s $Serial shell getprop ro.product.cpu.abilist)

if ($LASTEXITCODE -ne 0 -or $abiOutput.Count -ne 1) {
    throw 'The selected physical Android device is disconnected or did not return one ABI capability list.'
}

$reported = $abiOutput[0].Trim()

if ([string]::IsNullOrWhiteSpace($reported)) {
    throw 'The connected Android device returned an empty ABI capability list.'
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
        throw 'The connected device does not advertise a supported ARM ABI.'
    }
}
else {
    $chosen = $Abi
}

if ($available -notcontains $chosen) {
    throw "The Android device does not support selected ABI $chosen."
}

$exe = if ($chosen -eq 'arm64-v8a') {
    'vulkan-core10-result-gate-arm64'
}
else {
    'vulkan-core10-result-gate-armv7'
}

$local = Join-Path $output $exe
$remote = "/data/local/tmp/hc-vulkan-result-gate-20261101-$ReportPrefix"
$binary = "$remote/$exe"

if (-not (Test-Path -LiteralPath $local -PathType Leaf)) {
    throw "Native Vulkan result gate executable is absent: $local"
}

function Invoke-ResultGate {
    <#
    .SYNOPSIS
    Execute one Android Vulkan diagnostic and retain its process status.

    .DESCRIPTION
    Captures native stdout/stderr through ADB, preserves driver and
    policy observations, normalizes CRLF to LF and parses an exact
    tagged native-process exit code without guessing success.
    #>
    param ([string[]]$Arguments)

    $command = "$binary $($Arguments -join ' ')" + ' 2>&1; echo RESULT_GATE_NATIVE_EXIT=$?'
    $result = @(& adb -s $Serial shell $command)

    if ($LASTEXITCODE -ne 0 -or $result.Count -lt 2) {
        throw 'ADB did not return a complete Vulkan result-gate process.'
    }

    $lines = @($result | ForEach-Object { $_.TrimEnd([char]13) })
    $marker = $lines[-1].Trim()

    if ($marker -cnotmatch '^RESULT_GATE_NATIVE_EXIT=([0-9]+)$') {
        throw "Native Vulkan result-gate process marker is missing: $marker"
    }

    return @{
        ExitCode = [int]$Matches[1]
        Body = ([string]::Join([char]10, $lines[0..($lines.Count - 2)])).TrimEnd()
    }
}

function Assert-ResultGate {
    <#
    .SYNOPSIS
    Require genuine GPU copies followed by a strict terminal host gate.

    .DESCRIPTION
    Verifies two actual Vulkan instance/device lifetimes, real pending
    fence VkResults, four completed public GPU buffer transfers, exact
    test-only terminal error classification, six refused future
    submissions with untouched output, and five host-only error
    classification controls. Explicitly forbids post-terminal copies.
    #>
    param (
        [string]$Api,
        [string]$Memory,
        [string]$Terminal,
        [hashtable]$Result
    )

    if ($Result.ExitCode -ne 0) {
        throw "Native Android Vulkan result gate failed: $($Result.Body)"
    }

    $mode = if ($Memory -eq 'prefer') { 0 } else { 1 }
    $observed = switch ($Terminal) {
        'device-lost' { -4 }
        'device-memory' { -2 }
        'unexpected-positive' { 5 }
    }
    $errorResult = if ($observed -eq 5) { -3 } else { $observed }
    $lines = @($Result.Body -split [char]10)

    $summary = "RESULT_GATE_DIAGNOSTIC_PASS api=$Api mode=$mode terminal=$Terminal trials=2 injected=2 actual_driver_loss=0 real_copies=4 verified_words=2062 verified_bytes=8248 nonfatal=4 blocked=6 host_controls=5 shaders=0"

    if ($lines[-1] -cne $summary) {
        throw "Invalid terminal policy completion record: $summary"
    }

    $counts = @{
        'DEVICE_CATALOG_OPEN ' = 2
        'DEVICE_CATALOG_CLOSE ' = 2
        'TRANSFER_OPEN ' = 2
        'TRANSFER_COPY ' = 4
        'TRANSFER_CLOSE ' = 2
        'RESULT_GATE_REAL_DRIVER ' = 4
        'RESULT_GATE_GPU_VERIFIED ' = 4
        'RESULT_GATE_TEST_INJECT ' = 2
        'RESULT_GATE_TRIAL_PASS ' = 2
        'RESULT_GATE_OPEN ' = 7
        'RESULT_GATE_ALLOW ' = 4
        'RESULT_GATE_OBSERVE ' = 12
        'RESULT_GATE_TERMINAL ' = 7
        'RESULT_GATE_BLOCK ' = 11
        'RESULT_GATE_REFUSED ' = 6
        'RESULT_GATE_STICKY ' = 2
        'RESULT_GATE_CLOSE ' = 7
        'RESULT_GATE_POLICY_ONLY_PASS ' = 5
    }

    foreach ($key in $counts.Keys) {
        $actual = @($lines | Where-Object { $_.StartsWith($key) }).Count

        if ($actual -ne $counts[$key]) {
            throw "Invalid event count for $key actual=$actual required=$($counts[$key])"
        }
    }

    foreach ($trial in @(0, 1)) {
        foreach ($line in @(
            "RESULT_GATE_REAL_DRIVER trial=$trial site=fence_status observed=1",
            "RESULT_GATE_REAL_DRIVER trial=$trial site=fence_wait observed=2",
            "RESULT_GATE_TEST_INJECT trial=$trial mode=$Terminal origin=application_only observed=$observed driver_error=0",
            "RESULT_GATE_STICKY trial=$trial first_error=$errorResult later_success_ignored=1 blocked=3",
            "RESULT_GATE_TRIAL_PASS trial=$trial mode=$Terminal injected=1 real_driver_loss=0 completed_copies=2 verified_words=1031 verified_bytes=4124 allowed=2 blocked=3 nonfatal=2 successes=4 shaders=0"
        )) {
            if (@($lines | Where-Object { $_ -ceq $line }).Count -ne 1) {
                throw "Invalid driver/terminal policy event: $line"
            }
        }

        foreach ($round in @(0, 1)) {
            $words = if ($round -eq 0) { 7 } else { 1024 }
            $bytes = $words * 4
            $line = "RESULT_GATE_GPU_VERIFIED trial=$trial round=$round words=$words bytes=$bytes fence=0 match=1 shaders=0"

            if (@($lines | Where-Object { $_ -ceq $line }).Count -ne 1) {
                throw "Missing actual completed GPU buffer comparison: $line"
            }
        }

        for ($attempt = 1; $attempt -le 3; $attempt++) {
            $line = "RESULT_GATE_REFUSED trial=$trial attempt=$attempt error=$errorResult copies=2 bytes=4124 output_unchanged=1024"

            if (@($lines | Where-Object { $_ -ceq $line }).Count -ne 1) {
                throw "An attempted submission was not refused: $line"
            }
        }

        $opened = $Result.Body.IndexOf('DEVICE_CATALOG_OPEN ', 0)
        if ($opened -lt 0) { throw 'No real Vulkan instance was created.' }
    }

    foreach ($line in @(
        'RESULT_GATE_POLICY_ONLY_PASS case=timeout_from_status origin=synthetic observed=2 first_error=-3 blocked=1 closed=1',
        'RESULT_GATE_POLICY_ONLY_PASS case=not_ready_from_wait origin=synthetic observed=1 first_error=-3 blocked=1 closed=1',
        'RESULT_GATE_POLICY_ONLY_PASS case=incomplete_transfer origin=synthetic observed=5 first_error=-3 blocked=1 closed=1',
        'RESULT_GATE_POLICY_ONLY_PASS case=host_memory_error origin=synthetic observed=-1 first_error=-1 blocked=1 closed=1',
        'RESULT_GATE_POLICY_ONLY_PASS case=invalid_site origin=synthetic observed=0 first_error=-3 blocked=1 closed=1'
    )) {
        if (@($lines | Where-Object { $_ -ceq $line }).Count -ne 1) {
            throw "Invalid host-only VkResult policy control: $line"
        }
    }

    if ($Memory -eq 'noncoherent') {
        foreach ($line in @($lines | Where-Object { $_.StartsWith('TRANSFER_CLOSE ') })) {
            if ($line -notmatch ' flushes=2 invalidates=2 wait_result=0$') {
                throw 'The Vulkan noncoherent memory was not synchronized.'
            }
        }
    }

    $cursor = 0

    foreach ($trial in @(0, 1)) {
        $instance = $Result.Body.IndexOf('DEVICE_CATALOG_OPEN ', $cursor)
        $submitted = $Result.Body.IndexOf("RESULT_GATE_GPU_VERIFIED trial=$trial round=1 ", $cursor)
        $injected = $Result.Body.IndexOf("RESULT_GATE_TEST_INJECT trial=$trial ", $cursor)
        $refused = $Result.Body.IndexOf("RESULT_GATE_REFUSED trial=$trial attempt=3 ", $cursor)
        $closed = $Result.Body.IndexOf('TRANSFER_CLOSE ', $cursor)
        $released = $Result.Body.IndexOf('DEVICE_CATALOG_CLOSE ', $cursor)

        if ($instance -lt $cursor -or $submitted -lt $instance -or
            $injected -lt $submitted -or $refused -lt $injected -or
            $closed -lt $refused -or $released -lt $closed) {
            throw "Vulkan GPU policy/device/instance lifecycle is not ordered for trial $trial."
        }

        $window = $Result.Body.Substring($injected, $closed - $injected)

        if ($window.Contains('TRANSFER_COPY ') -or
            $window.Contains('RESULT_GATE_ALLOW ')) {
            throw "An extra Vulkan queue submission was permitted after terminal error in trial $trial."
        }

        $cursor = $released + 1
    }
}

Write-Output "RESULT_GATE_TEST_ABI=$chosen source=ro.product.cpu.abilist"

try {
    & adb -s $Serial shell mkdir -p $remote | Out-Null

    if ($LASTEXITCODE -ne 0) { throw 'Failed to create Android diagnostics folder.' }

    & adb -s $Serial push $local $binary | Out-Null

    if ($LASTEXITCODE -ne 0) { throw 'Failed to stage Android policy executable.' }

    & adb -s $Serial shell chmod 700 $binary | Out-Null

    if ($LASTEXITCODE -ne 0) { throw 'Failed to authorize native Android executable.' }

    $apis = @('1.0')
    if ($TestCore11) { $apis += '1.1' }

    $policies = @('prefer')
    if ($TestNoncoherent) { $policies += 'noncoherent' }

    foreach ($api in $apis) {
        foreach ($policy in $policies) {
            foreach ($terminal in @('device-lost', 'device-memory', 'unexpected-positive')) {
                $result = Invoke-ResultGate -Arguments @(
                    '--minimum', $api,
                    '--memory', $policy,
                    '--terminal', $terminal
                )

                Assert-ResultGate -Api $api -Memory $policy -Terminal $terminal -Result $result

                $log = @(
                    'TEST=vulkan-core10-result-gate',
                    "ABI=$chosen",
                    "API=$api",
                    "MEMORY_MODE=$policy",
                    "TERMINAL=$terminal",
                    "ADB_EXIT_CODE=$($result.ExitCode)",
                    $result.Body
                )

                [System.IO.File]::WriteAllText(
                    (Join-Path $output "$ReportPrefix-$api-$policy-$terminal.log"),
                    ([string]::Join([char]10, $log)) + [char]10,
                    [System.Text.UTF8Encoding]::new($false)
                )

                Write-Output "PASS Vulkan $api $chosen $policy test-only=$terminal actual_copies=4 blocked=6"
            }
        }
    }

    $invalid = @(
        @{ Name = 'missing-options'; Args = @() },
        @{ Name = 'unsupported-api'; Args = @('--minimum','1.2','--memory','prefer','--terminal','device-lost') },
        @{ Name = 'unknown-memory'; Args = @('--minimum','1.0','--memory','invalid','--terminal','device-lost') },
        @{ Name = 'unknown-terminal'; Args = @('--minimum','1.0','--memory','prefer','--terminal','error') },
        @{ Name = 'wrong-flag'; Args = @('--api','1.0','--memory','prefer','--terminal','device-lost') },
        @{ Name = 'extra-argument'; Args = @('--minimum','1.0','--memory','prefer','--terminal','device-lost','extra') }
    )

    $negative = @(
        'TEST=vulkan-core10-result-gate-invalid-inputs',
        "ABI=$chosen",
        "INVALID_CASES=$($invalid.Count)"
    )

    foreach ($case in $invalid) {
        $result = Invoke-ResultGate -Arguments $case.Args

        if ($result.ExitCode -ne 2) {
            throw "Malformed native VkResult policy control succeeded: $($case.Name)"
        }

        $negative += "INVALID_CASE=$($case.Name) exit=2"
        Write-Output "PASS rejected Vulkan terminal CLI $($case.Name)"
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
