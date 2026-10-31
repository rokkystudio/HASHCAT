<#
.SYNOPSIS
Run the hashcat backend Vulkan 1.0 catalogue on a real Android GPU.

.DESCRIPTION
Selects an Android ABI from the device-reported ABI list, executes
the real backend_ctx Vulkan discovery code twice per requested API,
checks invalid requests and writes LF-only public hardware logs.
No password algorithm or hashcat kernel scheduler is executed.

.PARAMETER Serial
Connected ADB device serial.

.PARAMETER Abi
Explicit ARM ABI or auto from ro.product.cpu.abilist.

.PARAMETER ReportPrefix
Lowercase public archive prefix identifying the device and process ABI.

.PARAMETER TestCore11
Also require and test Vulkan 1.1 instance and physical-device selection.
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
$directory = Join-Path $root 'build\diagnostic-multimode\vulkan-backend-catalog-20261031'
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
    'vulkan-backend-catalog-arm64'
}
else {
    'vulkan-backend-catalog-armv7'
}

$local = Join-Path $directory $name
$remote = '/data/local/tmp/hc-vulkan-backend-catalog-20261031'
$binary = "$remote/$name"

if (-not (Test-Path -LiteralPath $local -PathType Leaf)) {
    throw "Missing compiled Vulkan backend diagnostic: $local"
}

function Invoke-Catalogue {
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

function Save-Catalogue {
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
        'TEST=vulkan-backend-catalog',
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

function Assert-Catalogue {
    <#
    .SYNOPSIS
    Verify two real hashcat backend_ctx Vulkan instance lifecycles.

    .DESCRIPTION
    Requires two instance/physical-device opens, exact API selection,
    twelve rejected invalid host calls, two driver limit refusals and
    two complete catalogue closes,
    without registering a Vulkan password-processing device.
    #>
    param (
        [string]$Mode,
        [hashtable]$Result
    )

    if ($Result.ExitCode -ne 0) {
        throw "Vulkan backend_ctx discovery failed: $($Result.Body)"
    }

    $completion = "BACKEND_VULKAN_DIAGNOSTIC_PASS api=$Mode trials=2 selections=2 invalid_controls=12 scheduled=0"

    if ($Result.Body.Contains($completion) -ne $true) {
        throw 'Missing completed Vulkan backend_ctx discovery result.'
    }

    foreach ($prefix in @(
        'DEVICE_CATALOG_OPEN ',
        'DEVICE_CATALOG_SELECT ',
        'DEVICE_CATALOG_CLOSE ',
        'BACKEND_VULKAN_CATALOG_READY ',
        'BACKEND_VULKAN_INVALID_CONTROLS ',
        'BACKEND_VULKAN_LIMIT_REJECT ',
        'BACKEND_VULKAN_SELECT_PASS ',
        'BACKEND_VULKAN_CLOSE_PASS '
    )) {
        if (@([regex]::Matches($Result.Body, "(?m)^$prefix")).Count -ne 2) {
            throw "Incomplete Vulkan backend lifecycle: $prefix"
        }
    }

    foreach ($trial in @(0, 1)) {
        foreach ($fragment in @(
            "BACKEND_VULKAN_SELECT_PASS trial=$trial devices=",
            "BACKEND_VULKAN_CLOSE_PASS trial=$trial",
            "requested_api=$Mode effective_api=$Mode",
            'BACKEND_VULKAN_INVALID_CONTROLS rejected=6 selections=0',
            'BACKEND_VULKAN_LIMIT_REJECT result=-8 selections=0'
        )) {
            if (-not $Result.Body.Contains($fragment)) {
                throw "Incomplete backend-owned Vulkan catalogue trial: $fragment"
            }
        }
    }
}

Write-Output "BACKEND_CATALOG_ABI=$chosen source=ro.product.cpu.abilist"

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
        $result = Invoke-Catalogue -Arguments @('--minimum', $mode)

        Assert-Catalogue -Mode $mode -Result $result
        Save-Catalogue -Mode $mode -Result $result

        Write-Output "PASS $chosen Vulkan $mode backend_ctx catalogue two trials"
    }

    $cases = @(
        @{ Name = 'missing-minimum'; Arguments = @() },
        @{ Name = 'unsupported-api'; Arguments = @('--minimum','1.2') },
        @{ Name = 'extra-argument'; Arguments = @('--minimum','1.0','unexpected') }
    )
    $negative = @(
        'TEST=vulkan-backend-catalog-invalid-inputs',
        "ABI=$chosen",
        "INVALID_CASES=$($cases.Count)"
    )

    foreach ($case in $cases) {
        $result = Invoke-Catalogue -Arguments $case.Arguments

        if ($result.ExitCode -ne 2) {
            throw "Invalid Vulkan backend input was accepted: $($case.Name)"
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
