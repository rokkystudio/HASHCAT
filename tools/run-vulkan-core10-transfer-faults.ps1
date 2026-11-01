<#
.SYNOPSIS
Verify fault cleanup at every real Vulkan 1.0 buffer acquisition.

.DESCRIPTION
Runs the standalone public-integer Vulkan buffer test on actual Android
ARM64 or ARMv7 hardware with explicit fault-after points 1 through 10.
Each point interrupts after successfully acquiring a real VkDevice,
command resource, buffer, device-memory allocation or CPU mapping.
Requires exact resource release counts, native exit 42, VkDevice
destruction before VkInstance and no GPU queue copy submission.

All hardware logs are saved with LF line endings and no device serial.
This script never invokes the HASHCAT password-processing backend.

.PARAMETER Serial
ADB serial of the selected physical Android device.

.PARAMETER Abi
Explicit Android ABI or auto selection from ro.product.cpu.abilist.

.PARAMETER ReportPrefix
Lowercase name for public hardware log filenames.

.PARAMETER TestCore11
Also run the 10 acquisition-fault points with requested Vulkan API 1.1.
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
    throw 'ReportPrefix requires lowercase letters, digits and dashes only.'
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$build = Join-Path $root 'build\diagnostic-multimode\vulkan-core10-transfer-20261101'
$archive = Join-Path $root 'build\diagnostic-multimode\vulkan-core10-transfer-faults-20261101'

New-Item -Path $archive -ItemType Directory -Force | Out-Null

$reported = (& adb -s $Serial shell getprop ro.product.cpu.abilist).Trim()

if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($reported)) {
    throw 'The connected Android device did not report an ABI list.'
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
        throw 'No tested Android ARM ABI is available.'
    }
}
else {
    $chosen = $Abi
}

if ($available -notcontains $chosen) {
    throw "Requested ABI $chosen is not supported by the actual Android device."
}

$exe = if ($chosen -eq 'arm64-v8a') {
    'vulkan-core10-transfer-arm64'
}
else {
    'vulkan-core10-transfer-armv7'
}

$local = Join-Path $build $exe
$remote = "/data/local/tmp/hc-vulkan-public-faults-20261101-$ReportPrefix"
$program = "$remote/$exe"

if (-not (Test-Path -LiteralPath $local -PathType Leaf)) {
    throw "Native Vulkan transfer binary does not exist: $local"
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

function Invoke-FaultNative {
    <#
    .SYNOPSIS
    Run one real native Vulkan fault trial and extract its exit status.

    .DESCRIPTION
    Captures the driver's complete process output from ADB, checks a
    uniquely tagged native process exit marker and returns the LF-ready
    body together with the integer exit code.
    #>
    param (
        [string[]]$Arguments
    )

    $command = "$program $($Arguments -join ' ') 2>&1; echo TRANSFER_FAULT_EXIT=" + '$?'
    $output = @(& adb -s $Serial shell $command)

    if ($LASTEXITCODE -ne 0 -or $output.Count -lt 2) {
        throw 'Android did not return a complete Vulkan fault process result.'
    }

    $lines = @($output | ForEach-Object { $_.TrimEnd([char]13) })
    $marker = $lines[-1].Trim()

    if ($marker -cnotmatch '^TRANSFER_FAULT_EXIT=([0-9]+)$') {
        throw "Invalid native Vulkan fault exit marker: $marker"
    }

    return @{
        ExitCode = [int]$Matches[1]
        Body = ([string]::Join([char]10, $lines[0..($lines.Count - 2)])).TrimEnd()
    }
}

function Assert-FaultNative {
    <#
    .SYNOPSIS
    Verify exact Vulkan handle acquisition and release at one fault point.

    .DESCRIPTION
    Requires two genuine physical-device catalogue opens, two
    intentionally interrupted partial VkDevice creations, exact
    object-release cardinalities, repeated safe owner teardown and
    native exit 42. Rejects any GPU copy or unexpected successful open.
    #>
    param (
        [string]$Api,
        [int]$Point,
        [hashtable]$Result
    )

    if ($Result.ExitCode -ne 42) {
        throw "Fault $Point did not produce the required native exit 42: $($Result.Body)"
    }

    $resource = $resources[$Point - 1]
    $mapped = if ($Point -ge 10) { 2 } elseif ($Point -ge 7) { 1 } else { 0 }
    $buffers = if ($Point -ge 8) { 2 } elseif ($Point -ge 5) { 1 } else { 0 }
    $memory = if ($Point -ge 9) { 2 } elseif ($Point -ge 6) { 1 } else { 0 }
    $fence = [int]($Point -ge 4)
    $command = [int]($Point -ge 3)
    $pool = [int]($Point -ge 2)

    $expected = @(
        "TRANSFER_INJECT_FAULT point=$Point acquisition=$Point resource=$resource result=-3",
        'TRANSFER_OPEN_FAILED index=0 mode=0 result=-3',
        'TRANSFER_CLOSE index=0 bytes=0 copies=0 copied_bytes=0 flushes=0 invalidates=0 wait_result=0',
        "TRANSFER_RELEASE point=$Point acquired=$Point mapped=$mapped buffers=$buffers memory=$memory fence=$fence command=$command pool=$pool device=1"
    )

    $lines = @($Result.Body -split [char]10)

    foreach ($event in $expected) {
        if (@($lines | Where-Object { $_ -ceq $event }).Count -ne 2) {
            throw "Incorrect Vulkan resource cleanup for fault ${Point}: $event"
        }
    }

    foreach ($prefix in @(
        'VULKAN_LOADER_VERSION=',
        'DEVICE_CATALOG_OPEN ',
        'DEVICE_CATALOG_SELECT ',
        'DEVICE_CATALOG_CLOSE ',
        'TRANSFER_INVALID_OPEN ',
        'TRANSFER_FAULT_TRIAL '
    )) {
        if (@([regex]::Matches(
            $Result.Body,
            "(?m)^$([regex]::Escape($prefix))"
        )).Count -ne 2) {
            throw "Fault $Point lacks two real Vulkan instance/device lifecycles: $prefix"
        }
    }

    foreach ($trial in @(0, 1)) {
        $event = "TRANSFER_FAULT_TRIAL trial=$trial point=$Point injected=1 owned=0"

        if (@($lines | Where-Object { $_ -ceq $event }).Count -ne 1) {
            throw "Fault $Point has inconsistent partial ownership for trial $trial"
        }
    }

    $summary = "TRANSFER_DIAGNOSTIC_FAULT api=$Api mode=0 point=$Point trials=2 injected=2 copies=0 shaders=0"

    if ($lines[-1] -cne $summary) {
        throw "Fault $Point does not have the required native completion record."
    }

    foreach ($prefix in @('TRANSFER_COPY ', 'TRANSFER_VERIFIED ', 'TRANSFER_OPEN ')) {
        if (@($lines | Where-Object { $_.StartsWith($prefix) }).Count -ne 0) {
            throw "Fault $Point unexpectedly used or completed a GPU buffer transfer."
        }
    }

    $cursor = 0

    for ($trial = 0; $trial -lt 2; $trial++) {
        $begin = $Result.Body.IndexOf('DEVICE_CATALOG_OPEN ', $cursor)
        $inject = $Result.Body.IndexOf("TRANSFER_INJECT_FAULT point=$Point ", $cursor)
        $release = $Result.Body.IndexOf("TRANSFER_RELEASE point=$Point ", $cursor)
        $end = $Result.Body.IndexOf('DEVICE_CATALOG_CLOSE ', $cursor)

        if ($begin -lt $cursor -or $inject -lt $begin -or
            $release -lt $inject -or $end -lt $release) {
            throw "Fault $Point released a dependent Vulkan device after VkInstance."
        }

        $cursor = $end + 1
    }
}

Write-Output "TRANSFER_FAULT_ABI=$chosen source=ro.product.cpu.abilist"

try {
    & adb -s $Serial shell mkdir -p $remote | Out-Null

    if ($LASTEXITCODE -ne 0) {
        throw 'Cannot create the remote Vulkan fault diagnostics directory.'
    }

    & adb -s $Serial push $local $program | Out-Null

    if ($LASTEXITCODE -ne 0) {
        throw 'Cannot deploy the native Vulkan fault diagnostic.'
    }

    & adb -s $Serial shell chmod 700 $program | Out-Null

    if ($LASTEXITCODE -ne 0) {
        throw 'Cannot execute the native Vulkan fault diagnostic.'
    }

    $apis = @('1.0')

    if ($TestCore11) {
        $apis += '1.1'
    }

    foreach ($api in $apis) {
        for ($point = 1; $point -le $resources.Count; $point++) {
            $result = Invoke-FaultNative -Arguments @(
                '--minimum', $api,
                '--memory', 'prefer',
                '--fault-after', $point
            )

            Assert-FaultNative -Api $api -Point $point -Result $result

            $metadata = @(
                'TEST=vulkan-core10-transfer-faults',
                "ABI=$chosen",
                "API=$api",
                "FAULT_POINT=$point",
                'ADB_EXIT_CODE=42',
                $result.Body
            )

            [System.IO.File]::WriteAllText(
                (Join-Path $archive "$ReportPrefix-$api-fault-$point.log"),
                ([string]::Join([char]10, $metadata)) + [char]10,
                [System.Text.UTF8Encoding]::new($false)
            )

            Write-Output "PASS Vulkan $api $chosen acquired-point=$point resource=$($resources[$point - 1]) two complete cleanups"
        }
    }

    $invalid = @(
        @{ Name = 'zero-fault'; Args = @('--minimum','1.0','--memory','prefer','--fault-after','0') },
        @{ Name = 'fault-out-of-range'; Args = @('--minimum','1.0','--memory','prefer','--fault-after','11') },
        @{ Name = 'fault-nonnumeric'; Args = @('--minimum','1.0','--memory','prefer','--fault-after','abc') },
        @{ Name = 'fault-noncoherent-mode'; Args = @('--minimum','1.0','--memory','noncoherent','--fault-after','1') },
        @{ Name = 'fault-extra-argument'; Args = @('--minimum','1.0','--memory','prefer','--fault-after','1','extra') }
    )

    $negative = @(
        'TEST=vulkan-core10-transfer-fault-invalid-inputs',
        "ABI=$chosen",
        "INVALID_CASES=$($invalid.Count)"
    )

    foreach ($case in $invalid) {
        $result = Invoke-FaultNative -Arguments $case.Args

        if ($result.ExitCode -ne 2) {
            throw "Invalid fault option was accepted: $($case.Name)"
        }

        $negative += "INVALID_CASE=$($case.Name) exit=2"
        Write-Output "PASS Vulkan native fault-option rejection $($case.Name)"
    }

    [System.IO.File]::WriteAllText(
        (Join-Path $archive "$ReportPrefix-fault-negative-controls.log"),
        ([string]::Join([char]10, $negative)) + [char]10,
        [System.Text.UTF8Encoding]::new($false)
    )
}
finally {
    & adb -s $Serial shell rm -rf $remote 2>$null | Out-Null
}
