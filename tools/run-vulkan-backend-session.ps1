<#
.SYNOPSIS
Run the backend-owned Vulkan 1.0 Compute session on a real Android GPU.

.DESCRIPTION
Selects an Android ABI from the device-reported ABI list, opens a
Vulkan instance, physical-device catalogue and logical Compute session
twice per requested API. Checks empty fence-completed submissions,
resource ownership, invalid requests and explicit GPU limits.
Writes LF-only public hardware logs. Does not execute GPU shaders,
password algorithms or hashcat's kernel scheduler.

.PARAMETER Serial
Connected ADB device serial.

.PARAMETER Abi
Explicit ARM ABI or auto from ro.product.cpu.abilist.

.PARAMETER ReportPrefix
Lowercase public archive prefix identifying the device and process ABI.

.PARAMETER TestCore11
Also test Vulkan 1.1 physical-device and empty queue-session selection.
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
    throw 'Invalid public report prefix.'
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$directory = Join-Path $root 'build\diagnostic-multimode\vulkan-backend-session-20261101'
$reported = (& adb -s $Serial shell getprop ro.product.cpu.abilist).Trim()

if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($reported)) {
    throw 'Android CPU ABI property is unavailable.'
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
        throw 'Neither compiled Vulkan backend ABI is supported.'
    }
}
else {
    $chosen = $Abi
}

if ($available -notcontains $chosen) {
    throw "The device does not advertise ABI $chosen."
}

$name = if ($chosen -eq 'arm64-v8a') {
    'vulkan-backend-session-arm64'
}
else {
    'vulkan-backend-session-armv7'
}

$local = Join-Path $directory $name
$remote = '/data/local/tmp/hc-vulkan-backend-session-20261101'
$binary = "$remote/$name"

if (-not (Test-Path -LiteralPath $local -PathType Leaf)) {
    throw "Missing compiled Vulkan backend diagnostic: $local"
}

function Invoke-Session {
    <#
    .SYNOPSIS
    Return the exact native Android exit code and UTF-8 diagnostic body.
    #>
    param (
        [string[]]$Arguments
    )

    $command = "$binary $($Arguments -join ' ')" + ' 2>&1; echo HC_BACKEND_EXIT=$?'
    $output = @(& adb -s $Serial shell $command)

    if ($LASTEXITCODE -ne 0 -or $output.Count -lt 2) {
        throw 'ADB did not report a complete Vulkan backend result.'
    }

    $lines = @($output | ForEach-Object { $_.TrimEnd([char]13) })
    $marker = $lines[-1].Trim()

    if ($marker -cnotmatch '^HC_BACKEND_EXIT=([0-9]+)$') {
        throw "Missing native Vulkan backend exit status: $marker"
    }

    return @{
        ExitCode = [int]$Matches[1]
        Body = ([string]::Join([char]10, $lines[0..($lines.Count - 2)])).TrimEnd()
    }
}

function Save-Session {
    <#
    .SYNOPSIS
    Store one complete native Vulkan transcript with public run metadata.

    .DESCRIPTION
    Uses LF-only UTF-8 without BOM. Neither the ADB serial nor absolute
    host paths appear in the archived evidence file.
    #>
    param (
        [string]$Mode,
        [hashtable]$Result
    )

    $records = @(
        'TEST=vulkan-backend-session',
        "ABI=$chosen",
        "API=$Mode",
        "ADB_EXIT_CODE=$($Result.ExitCode)",
        $Result.Body
    )

    [System.IO.File]::WriteAllText(
        (Join-Path $directory "$ReportPrefix-$Mode.log"),
        ([string]::Join([char]10, $records)) + [char]10,
        [System.Text.UTF8Encoding]::new($false)
    )
}

function Assert-Session {
    <#
    .SYNOPSIS
    Verify two real backend-owned Vulkan Compute sessions and device instances.

    .DESCRIPTION
    Requires exact API negotiation, one real queue selection and one
    fence-completed empty submission per session. Confirms invalid host
    calls, an explicit GPU limit refusal, active-instance close refusal,
    and destruction of each logical device before its owning instance.
    #>
    param (
        [string]$Mode,
        [hashtable]$Result
    )

    if ($Result.ExitCode -ne 0) {
        throw "Native Vulkan session failed: $($Result.Body)"
    }

    $completion = "BACKEND_SESSION_DIAGNOSTIC_PASS api=$Mode trials=2 invalid=8 owner_rejects=4 unsupported=2 empty_submissions=2 scheduled=0"

    if (@([regex]::Matches(
        $Result.Body, "(?m)^$([regex]::Escape($completion))$"
    )).Count -ne 1) {
        throw 'Vulkan Compute session completion event is missing.'
    }

    $events = @(
        'DEVICE_CATALOG_OPEN ',
        'DEVICE_CATALOG_SELECT ',
        'DEVICE_CATALOG_CLOSE ',
        'BACKEND_SESSION_INVALID_CONTROLS ',
        'BACKEND_SESSION_OWNER_CONTROLS ',
        'BACKEND_VULKAN_SESSION_READY ',
        'BACKEND_VULKAN_SESSION_CLOSE ',
        'BACKEND_SESSION_TRIAL_PASS ',
        'REJECT_COMPUTE '
    )

    foreach ($prefix in $events) {
        if (@([regex]::Matches($Result.Body, "(?m)^$prefix")).Count -ne 2) {
            throw "Incomplete real Vulkan session ownership: $prefix"
        }
    }

    $rows = @($Result.Body -split [char]10)

    foreach ($trial in @(0, 1)) {
        foreach ($line in @(
            "BACKEND_SESSION_INVALID_CONTROLS trial=$trial invalid=4 unsupported=1 active=0",
            "BACKEND_SESSION_OWNER_CONTROLS trial=$trial rejects=2 active=1",
            "BACKEND_SESSION_TRIAL_PASS trial=$trial invalid=4 owner_rejects=2 empty_submissions=1 scheduled=0"
        )) {
            if (@($rows | Where-Object { $_ -ceq $line }).Count -ne 1) {
                throw "Missing exact Vulkan session control: $line"
            }
        }
    }

    $readies = @([regex]::Matches(
        $Result.Body,
        "(?m)^BACKEND_VULKAN_SESSION_READY index=(\d+) queue_family=(\d+) effective_api=$([regex]::Escape($Mode)) command_buffers=1 empty_submissions=1 features=0 extensions=0 scheduled=0$"
    ))
    $closes = @([regex]::Matches(
        $Result.Body,
        '(?m)^BACKEND_VULKAN_SESSION_CLOSE index=(\d+) queue_family=(\d+) empty_submissions=1 wait_result=0$'
    ))

    if ($readies.Count -ne 2 -or $closes.Count -ne 2) {
        throw 'Actual logical-device session creation or cleanup is incomplete.'
    }

    for ($i = 0; $i -lt 2; $i++) {
        if ($readies[$i].Groups[1].Value -ne $closes[$i].Groups[1].Value -or
            $readies[$i].Groups[2].Value -ne $closes[$i].Groups[2].Value) {
            throw 'Vulkan logical-device and queue ownership do not match.'
        }
    }

    $opened = $Result.Body.IndexOf('DEVICE_CATALOG_OPEN ')
    $ready = $Result.Body.IndexOf('BACKEND_VULKAN_SESSION_READY ')
    $closed = $Result.Body.IndexOf('BACKEND_VULKAN_SESSION_CLOSE ')
    $instanceClosed = $Result.Body.IndexOf('DEVICE_CATALOG_CLOSE ')

    if ($opened -lt 0 -or $ready -lt $opened -or
        $closed -lt $ready -or $instanceClosed -lt $closed) {
        throw 'Vulkan session and VkInstance lifecycle are out of order.'
    }
}

Write-Output "BACKEND_SESSION_ABI=$chosen source=ro.product.cpu.abilist"

try {
    & adb -s $Serial shell mkdir -p $remote | Out-Null

    if ($LASTEXITCODE -ne 0) {
        throw 'Cannot create Android catalogue directory.'
    }

    & adb -s $Serial push $local $binary | Out-Null

    if ($LASTEXITCODE -ne 0) {
        throw 'Cannot stage Android catalogue executable.'
    }

    & adb -s $Serial shell chmod 700 $binary | Out-Null

    if ($LASTEXITCODE -ne 0) {
        throw 'Cannot execute Android catalogue executable.'
    }

    $modes = @('1.0')

    if ($TestCore11) {
        $modes += '1.1'
    }

    foreach ($mode in $modes) {
        $result = Invoke-Session -Arguments @('--minimum', $mode)

        Assert-Session -Mode $mode -Result $result
        Save-Session -Mode $mode -Result $result

        Write-Output "PASS $chosen Vulkan $mode backend_ctx Compute queue session two trials"
    }

    $cases = @(
        @{ Name = 'missing-minimum'; Arguments = @() },
        @{ Name = 'unsupported-api'; Arguments = @('--minimum','1.2') },
        @{ Name = 'extra-argument'; Arguments = @('--minimum','1.0','unexpected') }
    )
    $negative = @(
        'TEST=vulkan-backend-session-invalid-inputs',
        "ABI=$chosen",
        "INVALID_CASES=$($cases.Count)"
    )

    foreach ($case in $cases) {
        $result = Invoke-Session -Arguments $case.Arguments

        if ($result.ExitCode -ne 2) {
            throw "Invalid Vulkan session diagnostic input was accepted: $($case.Name)"
        }

        $negative += "INVALID_CASE=$($case.Name) exit=2"
        Write-Output "PASS $chosen malformed $($case.Name)"
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
