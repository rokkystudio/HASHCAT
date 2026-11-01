<#
.SYNOPSIS
Verify public Vulkan 1.0 GPU buffer transfers on real Android hardware.

.DESCRIPTION
Selects a compiled Android ARM ABI using the device-reported ABI list,
executes two independent Vulkan sessions per requested core API and
memory coherence mode, and archives exact LF-only native diagnostics.
A Vulkan noncoherent-memory refusal is recorded as an explicit
unsupported outcome, never reported as a successful transfer.
No shader, cryptographic hash or HASHCAT scheduler is executed.

.PARAMETER Serial
ADB serial for the physical Android device.

.PARAMETER Abi
Requested ABI or auto selection from the actual Android ABI list.

.PARAMETER ReportPrefix
Lowercase identifier used in public archived filenames.

.PARAMETER TestCore11
Also run the explicitly selected Vulkan 1.1 instance/API mode.

.PARAMETER TestNoncoherent
Also require genuine HOST_VISIBLE noncoherent Vulkan buffer memory.
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
    throw 'Public report prefix requires only lowercase letters, digits or dashes.'
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$directory = Join-Path $root 'build\diagnostic-multimode\vulkan-core10-transfer-20261101'
$reported = (& adb -s $Serial shell getprop ro.product.cpu.abilist).Trim()

if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($reported)) {
    throw 'Android did not report its actual process ABI support.'
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
        throw 'No supported Android ARM ABI is available.'
    }
}
else {
    $chosen = $Abi
}

if ($available -notcontains $chosen) {
    throw "The physical Android device does not support ABI $chosen."
}

$name = if ($chosen -eq 'arm64-v8a') {
    'vulkan-core10-transfer-arm64'
}
else {
    'vulkan-core10-transfer-armv7'
}

$local = Join-Path $directory $name
$remote = "/data/local/tmp/hc-public-vulkan-transfer-20261101-$ReportPrefix"
$binary = "$remote/$name"

if (-not (Test-Path -LiteralPath $local -PathType Leaf)) {
    throw "The Vulkan transfer executable is not built: $local"
}

function Invoke-Transfer {
    <#
    .SYNOPSIS
    Capture one native Android Vulkan transfer test and its process status.

    .DESCRIPTION
    Invokes the exact supplied native options and parses the tagged
    Android process exit marker, preserving native driver diagnostics.
    Exit status 3 explicitly means driver-incompatible required memory.
    #>
    param (
        [string[]]$Arguments
    )

    $command = "$binary $($Arguments -join ' ')" + ' 2>&1; echo PUBLIC_TRANSFER_EXIT=$?'
    $output = @(& adb -s $Serial shell $command)

    if ($LASTEXITCODE -ne 0 -or $output.Count -lt 2) {
        throw 'ADB returned an incomplete native Vulkan transfer result.'
    }

    $lines = @($output | ForEach-Object { $_.TrimEnd([char]13) })
    $marker = $lines[-1].Trim()

    if ($marker -cnotmatch '^PUBLIC_TRANSFER_EXIT=([0-9]+)$') {
        throw "Missing actual Vulkan transfer exit marker: $marker"
    }

    return @{
        ExitCode = [int]$Matches[1]
        Body = ([string]::Join([char]10, $lines[0..($lines.Count - 2)])).TrimEnd()
    }
}

function Assert-Transfer {
    <#
    .SYNOPSIS
    Validate two genuine Vulkan transfer sessions or explicit driver refusal.

    .DESCRIPTION
    Verifies the two physical-device catalogue lifecycles, actual
    memory ownership, queue/fence progress, exact public transfer
    counts and cache synchronization. In strict noncoherent mode,
    the native refusal code 3 is accepted only with two explicit
    VK_ERROR_FEATURE_NOT_PRESENT memory-capability records.
    #>
    param (
        [string]$Mode,
        [string]$Memory,
        [hashtable]$Result
    )

    $policy = if ($Memory -eq 'prefer') { 0 } else { 1 }
    $rows = @($Result.Body -split [char]10)

    foreach ($prefix in @(
        'DEVICE_CATALOG_OPEN ',
        'DEVICE_CATALOG_SELECT ',
        'DEVICE_CATALOG_CLOSE ',
        'TRANSFER_INVALID_OPEN '
    )) {
        if (@([regex]::Matches($Result.Body, "(?m)^$prefix")).Count -ne 2) {
            throw "Missing real Vulkan resource lifecycle: $prefix"
        }
    }

    if ($Result.ExitCode -eq 3) {
        if ($Memory -ne 'noncoherent') {
            throw 'A preferred-memory transfer unexpectedly refused all GPU memory.'
        }

        $completion = "TRANSFER_DIAGNOSTIC_UNSUPPORTED api=$Mode mode=$policy trials=2 refusals=2 copied_bytes=0 shaders=0"

        if (@($rows | Where-Object { $_ -ceq $completion }).Count -ne 1 -or
            @($rows | Where-Object { $_ -match '^TRANSFER_MEMORY_UNAVAILABLE ' }).Count -ne 2 -or
            @($rows | Where-Object { $_ -match '^TRANSFER_OPEN_FAILED .*result=-8$' }).Count -ne 2 -or
            @($rows | Where-Object { $_ -match '^TRANSFER_UNSUPPORTED_TRIAL ' }).Count -ne 2 -or
            @($rows | Where-Object { $_ -match '^TRANSFER_COPY ' }).Count -ne 0) {
            throw 'Unsupported Vulkan memory result lacks exact driver refusals.'
        }

        return 'unsupported'
    }

    if ($Result.ExitCode -ne 0) {
        throw "Native Android Vulkan transfer failed: $($Result.Body)"
    }

    $completion = "TRANSFER_DIAGNOSTIC_PASS api=$Mode mode=$policy trials=2 copies=10 verified_words=2706 copied_bytes=10824 invalid=26 shaders=0"

    if (@($rows | Where-Object { $_ -ceq $completion }).Count -ne 1) {
        throw 'Missing complete Vulkan public transfer proof.'
    }

    foreach ($prefix in @(
        'TRANSFER_OPEN ',
        'TRANSFER_INVALID_ACTIVE ',
        'TRANSFER_TRIAL_PASS ',
        'TRANSFER_CLOSE '
    )) {
        if (@([regex]::Matches($Result.Body, "(?m)^$prefix")).Count -ne 2) {
            throw "Incomplete native Vulkan mapped-buffer lifecycle: $prefix"
        }
    }

    foreach ($prefix in @('TRANSFER_COPY ', 'TRANSFER_VERIFIED ')) {
        if (@([regex]::Matches($Result.Body, "(?m)^$prefix")).Count -ne 10) {
            throw "Incomplete actual GPU buffer copies: $prefix"
        }
    }

    $opened = $Result.Body.IndexOf('DEVICE_CATALOG_OPEN ')
    $buffers = $Result.Body.IndexOf('TRANSFER_OPEN ')
    $closed = $Result.Body.IndexOf('TRANSFER_CLOSE ')
    $instance = $Result.Body.IndexOf('DEVICE_CATALOG_CLOSE ')

    if ($opened -lt 0 -or $buffers -lt $opened -or
        $closed -lt $buffers -or $instance -lt $closed) {
        throw 'VkBuffer, VkDevice and VkInstance destruction order is invalid.'
    }

    foreach ($trial in @(0, 1)) {
        foreach ($line in @(
            "TRANSFER_INVALID_OPEN trial=$trial rejected=7 owned=0",
            "TRANSFER_INVALID_ACTIVE trial=$trial rejected=6 copies=0",
            "TRANSFER_TRIAL_PASS trial=$trial mode=$policy copies=5 verified_words=1353 copied_bytes=5412 invalid=13 shaders=0"
        )) {
            if (@($rows | Where-Object { $_ -ceq $line }).Count -ne 1) {
                throw "Missing exact public transfer GPU verification: $line"
            }
        }
    }

    if ($Memory -eq 'noncoherent') {
        foreach ($line in @($rows | Where-Object { $_ -match '^TRANSFER_CLOSE ' })) {
            if ($line -notmatch ' flushes=5 invalidates=5 wait_result=0$') {
                throw 'Required noncoherent GPU flush/invalidate did not occur.'
            }
        }
    }

    return 'passed'
}

Write-Output "TRANSFER_TEST_ABI=$chosen source=ro.product.cpu.abilist"

try {
    & adb -s $Serial shell mkdir -p $remote | Out-Null

    if ($LASTEXITCODE -ne 0) {
        throw 'Cannot create Android Vulkan transfer test directory.'
    }

    & adb -s $Serial push $local $binary | Out-Null

    if ($LASTEXITCODE -ne 0) {
        throw 'Cannot stage the native Vulkan transfer executable.'
    }

    & adb -s $Serial shell chmod 700 $binary | Out-Null

    if ($LASTEXITCODE -ne 0) {
        throw 'Cannot execute the native Vulkan transfer test.'
    }

    $apis = @('1.0')

    if ($TestCore11) {
        $apis += '1.1'
    }

    $memories = @('prefer')

    if ($TestNoncoherent) {
        $memories += 'noncoherent'
    }

    foreach ($api in $apis) {
        foreach ($memory in $memories) {
            $result = Invoke-Transfer -Arguments @('--minimum', $api, '--memory', $memory)
            $outcome = Assert-Transfer -Mode $api -Memory $memory -Result $result

            $metadata = @(
                'TEST=vulkan-core10-transfer',
                "ABI=$chosen",
                "API=$api",
                "MEMORY_MODE=$memory",
                "ADB_EXIT_CODE=$($result.ExitCode)",
                $result.Body
            )

            [System.IO.File]::WriteAllText(
                (Join-Path $directory "$ReportPrefix-$api-$memory.log"),
                ([string]::Join([char]10, $metadata)) + [char]10,
                [System.Text.UTF8Encoding]::new($false)
            )

            Write-Output "PASS $chosen Vulkan $api $memory outcome=$outcome native_exit=$($result.ExitCode)"
        }
    }

    $invalid = @(
        @{ Name = 'missing-minimum'; Arguments = @() },
        @{ Name = 'unsupported-api'; Arguments = @('--minimum','1.2','--memory','prefer') },
        @{ Name = 'unknown-memory'; Arguments = @('--minimum','1.0','--memory','unknown') }
    )

    $negative = @(
        'TEST=vulkan-core10-transfer-invalid-inputs',
        "ABI=$chosen",
        "INVALID_CASES=$($invalid.Count)"
    )

    foreach ($case in $invalid) {
        $result = Invoke-Transfer -Arguments $case.Arguments

        if ($result.ExitCode -ne 2) {
            throw "Malformed native Vulkan transfer control was accepted: $($case.Name)"
        }

        $negative += "INVALID_CASE=$($case.Name) exit=2"
        Write-Output "PASS $chosen invalid native command $($case.Name)"
    }

    [System.IO.File]::WriteAllText(
        (Join-Path $directory "$ReportPrefix-negative-controls.log"),
        ([string]::Join([char]10, $negative)) + [char]10,
        [System.Text.UTF8Encoding]::new($false)
    )
}
finally {
    & adb -s $Serial shell rm -rf $remote 2>$null | Out-Null
}
