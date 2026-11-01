<#
.SYNOPSIS
Verify real Vulkan 1.0 buffer copying after every acquired-resource fault.

.DESCRIPTION
Selects an actual Android ARM ABI reported by ADB and runs ten explicit
fault/recovery points for each requested Vulkan API and memory policy.
Each native trial retains one VkInstance after an intentionally failed
VkDevice acquisition, then creates two independent VkDevices and copies
public 32-bit integers using GPU queue/fence synchronization. Stores
LF-normalized native Android evidence with exit status for each case.

.PARAMETER Serial
ADB device serial of the physical Android GPU.

.PARAMETER Abi
Explicit ABI or auto selection from the device's reported ABI list.

.PARAMETER ReportPrefix
Public lowercase filename prefix for this device/process combination.

.PARAMETER TestCore11
Also verify the Vulkan 1.1 API mode.

.PARAMETER TestNoncoherent
Also require genuinely noncoherent HOST_VISIBLE memory; an unsupported
driver result fails the test rather than silently substituting memory.
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
    throw 'ReportPrefix requires lowercase letters, digits or dashes.'
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$build = Join-Path $root 'build\diagnostic-multimode\vulkan-core10-transfer-recovery-20261101'

New-Item -ItemType Directory -Path $build -Force | Out-Null

$reported = (& adb -s $Serial shell getprop ro.product.cpu.abilist).Trim()

if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($reported)) {
    throw 'The physical Android device did not supply an ABI list.'
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
        throw 'No supported Android ARM process ABI is available.'
    }
}
else {
    $chosen = $Abi
}

if ($available -notcontains $chosen) {
    throw "Requested ABI $chosen is not supported by the physical device."
}

$exe = if ($chosen -eq 'arm64-v8a') {
    'vulkan-core10-transfer-recovery-arm64'
}
else {
    'vulkan-core10-transfer-recovery-armv7'
}

$local = Join-Path $build $exe
$remote = "/data/local/tmp/hc-vulkan-public-recovery-20261101-$ReportPrefix"
$binary = "$remote/$exe"

if (-not (Test-Path -LiteralPath $local -PathType Leaf)) {
    throw "The native fault-recovery ELF was not built: $local"
}

$resources = @(
    'device',
    'command_pool',
    'command_buffer',
    'fence',
    'source_buffer',
    'source_memory',
    'source_mapping',
    'destination_buffer',
    'destination_memory',
    'destination_mapping'
)

function Invoke-Recovery {
    <#
    .SYNOPSIS
    Capture one native Android recovery process and its actual exit code.

    .DESCRIPTION
    Executes supplied literal CLI options and parses the native shell
    exit marker. Normalizes CRLF to LF while retaining every driver
    diagnostic and public integer verification event.
    #>
    param (
        [string[]]$Arguments
    )

    $cmd = "$binary $($Arguments -join ' ')" + ' 2>&1; echo RECOVERY_PROCESS_EXIT=$?'
    $output = @(& adb -s $Serial shell $cmd)

    if ($LASTEXITCODE -ne 0 -or $output.Count -lt 2) {
        throw 'ADB returned an incomplete Vulkan recovery process.'
    }

    $lines = @($output | ForEach-Object { $_.TrimEnd([char]13) })
    $marker = $lines[-1].Trim()

    if ($marker -cnotmatch '^RECOVERY_PROCESS_EXIT=([0-9]+)$') {
        throw "Incorrect Vulkan native recovery exit marker: $marker"
    }

    return @{
        ExitCode = [int]$Matches[1]
        Body = ([string]::Join([char]10, $lines[0..($lines.Count - 2)])).TrimEnd()
    }
}

function Assert-Recovery {
    <#
    .SYNOPSIS
    Require two real Vulkan fault/reopen/copy/reopen lifecycles.

    .DESCRIPTION
    Validates native exit zero, exact driver-independent recovery
    counters, real GPU queue/fence copy events, the explicitly
    requested fault point and destruction of each logical device
    before the shared parent VkInstance is closed.
    #>
    param (
        [string]$Api,
        [string]$Memory,
        [int]$Point,
        [hashtable]$Result
    )

    if ($Result.ExitCode -ne 0) {
        throw "Native GPU recovery failed: $($Result.Body)"
    }

    $mode = if ($Memory -eq 'prefer') { 0 } else { 1 }
    $summary = "RECOVERY_DIAGNOSTIC_PASS api=$Api mode=$mode point=$Point trials=2 faults=2 reopened=4 copies=12 verified_words=4754 copied_bytes=19016 shaders=0"
    $lines = @($Result.Body -split [char]10)

    if ($lines[-1] -cne $summary) {
        throw "Native Vulkan recovery completion is incomplete: $summary"
    }

    $counts = @{
        'DEVICE_CATALOG_OPEN ' = 2
        'DEVICE_CATALOG_SELECT ' = 2
        'DEVICE_CATALOG_CLOSE ' = 2
        'TRANSFER_INJECT_FAULT ' = 2
        'TRANSFER_RELEASE ' = 2
        'TRANSFER_OPEN_FAILED ' = 2
        'RECOVERY_FAULT_CONFIRMED ' = 2
        'TRANSFER_OPEN ' = 4
        'TRANSFER_CLOSE ' = 6
        'RECOVERY_REOPEN ' = 4
        'TRANSFER_COPY ' = 12
        'RECOVERY_VERIFIED ' = 12
        'RECOVERY_PHASE_PASS ' = 4
        'RECOVERY_PHASE_CLOSED ' = 4
        'RECOVERY_TRIAL_PASS ' = 2
    }

    foreach ($key in $counts.Keys) {
        $actual = @($lines | Where-Object { $_.StartsWith($key) }).Count

        if ($actual -ne $counts[$key]) {
            throw "Incomplete real Vulkan recovery event $key actual=$actual expected=$($counts[$key])"
        }
    }

    $maps = if ($Point -ge 10) { 2 } elseif ($Point -ge 7) { 1 } else { 0 }
    $buffers = if ($Point -ge 8) { 2 } elseif ($Point -ge 5) { 1 } else { 0 }
    $memory = if ($Point -ge 9) { 2 } elseif ($Point -ge 6) { 1 } else { 0 }
    $fence = [int]($Point -ge 4)
    $command = [int]($Point -ge 3)
    $pool = [int]($Point -ge 2)

    $release = "TRANSFER_RELEASE point=$Point acquired=$Point mapped=$maps buffers=$buffers memory=$memory fence=$fence command=$command pool=$pool device=1"
    $inject = "TRANSFER_INJECT_FAULT point=$Point acquisition=$Point resource=$($resources[$Point - 1]) result=-3"

    foreach ($line in @($inject, $release)) {
        if (@($lines | Where-Object { $_ -ceq $line }).Count -ne 2) {
            throw "Partial Vulkan owner recovery does not match real acquisition: $line"
        }
    }

    foreach ($trial in @(0, 1)) {
        foreach ($line in @(
            "RECOVERY_FAULT_CONFIRMED trial=$trial point=$Point result=-3 owner=0 instance=1 copies=0",
            "RECOVERY_TRIAL_PASS trial=$trial fault=$Point failed=1 reopened=2 copies=6 verified_words=2377 copied_bytes=9508 shaders=0"
        )) {
            if (@($lines | Where-Object { $_ -ceq $line }).Count -ne 1) {
                throw "Incomplete Vulkan fault recovery trial: $line"
            }
        }

        foreach ($phase in @(0, 1)) {
            foreach ($line in @(
                "RECOVERY_REOPEN trial=$trial phase=$phase point=$Point owner=1 acquisitions=10 copied=0",
                "RECOVERY_PHASE_CLOSED trial=$trial phase=$phase owner=0 instance=1"
            )) {
                if (@($lines | Where-Object { $_ -ceq $line }).Count -ne 1) {
                    throw "Incomplete Vulkan device reopen/close phase: $line"
                }
            }
        }
    }

    $cursor = 0

    foreach ($trial in @(0, 1)) {
        $catalogue = $Result.Body.IndexOf('DEVICE_CATALOG_OPEN ', $cursor)
        $fault = $Result.Body.IndexOf('TRANSFER_INJECT_FAULT ', $cursor)
        $release = $Result.Body.IndexOf('TRANSFER_RELEASE ', $cursor)
        $recovery = $Result.Body.IndexOf("RECOVERY_REOPEN trial=$trial phase=0 ", $cursor)
        $closed0 = $Result.Body.IndexOf("RECOVERY_PHASE_CLOSED trial=$trial phase=0 ", $cursor)
        $recovery1 = $Result.Body.IndexOf("RECOVERY_REOPEN trial=$trial phase=1 ", $cursor)
        $closed1 = $Result.Body.IndexOf("RECOVERY_PHASE_CLOSED trial=$trial phase=1 ", $cursor)
        $instanceClose = $Result.Body.IndexOf('DEVICE_CATALOG_CLOSE ', $cursor)

        if ($catalogue -lt $cursor -or $fault -lt $catalogue -or
            $release -lt $fault -or $recovery -lt $release -or
            $closed0 -lt $recovery -or $recovery1 -lt $closed0 -or
            $closed1 -lt $recovery1 -or $instanceClose -lt $closed1) {
            throw "VkInstance was released before its Vulkan recovery devices in trial $trial."
        }

        $cursor = $instanceClose + 1
    }
}

Write-Output "RECOVERY_TEST_ABI=$chosen source=ro.product.cpu.abilist"

try {
    & adb -s $Serial shell mkdir -p $remote | Out-Null

    if ($LASTEXITCODE -ne 0) {
        throw 'Cannot create the Android GPU recovery directory.'
    }

    & adb -s $Serial push $local $binary | Out-Null

    if ($LASTEXITCODE -ne 0) {
        throw 'Cannot stage the Vulkan fault recovery executable.'
    }

    & adb -s $Serial shell chmod 700 $binary | Out-Null

    if ($LASTEXITCODE -ne 0) {
        throw 'Cannot execute the Vulkan fault recovery executable.'
    }

    $apis = @('1.0')
    if ($TestCore11) { $apis += '1.1' }

    $policies = @('prefer')
    if ($TestNoncoherent) { $policies += 'noncoherent' }

    foreach ($api in $apis) {
        foreach ($policy in $policies) {
            for ($point = 1; $point -le 10; $point++) {
                $result = Invoke-Recovery -Arguments @(
                    '--minimum', $api,
                    '--memory', $policy,
                    '--fault-after', $point
                )

                Assert-Recovery -Api $api -Memory $policy -Point $point -Result $result

                $metadata = @(
                    'TEST=vulkan-core10-transfer-recovery',
                    "ABI=$chosen",
                    "API=$api",
                    "MEMORY_MODE=$policy",
                    "FAULT_POINT=$point",
                    "ADB_EXIT_CODE=$($result.ExitCode)",
                    $result.Body
                )

                [System.IO.File]::WriteAllText(
                    (Join-Path $build "$ReportPrefix-$api-$policy-recover-$point.log"),
                    ([string]::Join([char]10, $metadata)) + [char]10,
                    [System.Text.UTF8Encoding]::new($false)
                )

                Write-Output "PASS Vulkan $api $chosen memory=$policy fault=$point reopened=4 copies=12"
            }
        }
    }

    $invalid = @(
        @{ Name = 'missing-arguments'; Args = @() },
        @{ Name = 'unsupported-api'; Args = @('--minimum','1.2','--memory','prefer','--fault-after','1') },
        @{ Name = 'zero-fault'; Args = @('--minimum','1.0','--memory','prefer','--fault-after','0') },
        @{ Name = 'fault-out-of-range'; Args = @('--minimum','1.0','--memory','prefer','--fault-after','11') },
        @{ Name = 'fault-nonnumeric'; Args = @('--minimum','1.0','--memory','prefer','--fault-after','abc') },
        @{ Name = 'unsupported-memory-name'; Args = @('--minimum','1.0','--memory','invalid','--fault-after','1') }
    )

    $negative = @(
        'TEST=vulkan-core10-transfer-recovery-invalid-inputs',
        "ABI=$chosen",
        "INVALID_CASES=$($invalid.Count)"
    )

    foreach ($case in $invalid) {
        $result = Invoke-Recovery -Arguments $case.Args

        if ($result.ExitCode -ne 2) {
            throw "Invalid Vulkan recovery option was accepted: $($case.Name)"
        }

        $negative += "INVALID_CASE=$($case.Name) exit=2"

        Write-Output "PASS rejected Vulkan recovery option $($case.Name)"
    }

    [System.IO.File]::WriteAllText(
        (Join-Path $build "$ReportPrefix-recovery-negative-controls.log"),
        ([string]::Join([char]10, $negative)) + [char]10,
        [System.Text.UTF8Encoding]::new($false)
    )
}
finally {
    & adb -s $Serial shell rm -rf $remote 2>$null | Out-Null
}
