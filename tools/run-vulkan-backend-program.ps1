<#
.SYNOPSIS
Run the host-only Vulkan program graph validator on Android.

.DESCRIPTION
Deploys the architecture-matched validator built from the production
backend_vulkan_program source. The process exercises accepted one/two-
stage declarations and fourteen explicit malformed or unsupported
cases. It does not link libvulkan.so, create a VkInstance, access a GPU,
or invoke HASHCAT hashing code.

Writes one UTF-8 LF-only process transcript suitable for independent
offline verification.

.PARAMETER Serial
ADB serial of the physical Android device.

.PARAMETER Abi
Explicit supported ABI or auto-select from ro.product.cpu.abilist.

.PARAMETER ReportPrefix
Lowercase stable prefix for the saved process transcript.
#>
param (
    [Parameter(Mandatory = $true)]
    [string]$Serial,

    [ValidateSet('auto', 'arm64-v8a', 'armeabi-v7a')]
    [string]$Abi = 'auto',

    [Parameter(Mandatory = $true)]
    [string]$ReportPrefix
)

$ErrorActionPreference = 'Stop'

if ($ReportPrefix -cnotmatch '^[a-z0-9-]+$') {
    throw 'ReportPrefix must contain only lowercase letters, numbers and dashes.'
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$build = Join-Path $root 'build\diagnostic-multimode\vulkan-backend-program-20261101'

$abiOutput = @(& adb -s $Serial shell getprop ro.product.cpu.abilist)

if ($LASTEXITCODE -ne 0 -or $abiOutput.Count -ne 1) {
    throw 'Android device did not return one ABI list.'
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
        throw 'Android device does not advertise a supported ARM ABI.'
    }
}
else {
    $chosen = $Abi
}

if ($available -notcontains $chosen) {
    throw "Selected ABI is not advertised by the device: $chosen"
}

$exe = if ($chosen -eq 'arm64-v8a') {
    'vulkan-backend-program-arm64'
}
else {
    'vulkan-backend-program-armv7'
}

$local = Join-Path $build $exe
$remote = "/data/local/tmp/hc-vulkan-program-$ReportPrefix"
$binary = "$remote/$exe"

if (-not (Test-Path -LiteralPath $local -PathType Leaf)) {
    throw "Program validator binary is absent: $local"
}

try {
    & adb -s $Serial shell mkdir -p $remote | Out-Null
    & adb -s $Serial push $local $binary | Out-Null

    if ($LASTEXITCODE -ne 0) {
        throw 'Cannot deploy Vulkan program graph validator.'
    }

    & adb -s $Serial shell chmod 700 $binary | Out-Null

    $command = "$binary 2>&1; echo PROGRAM_NATIVE_EXIT=`$?"
    $response = @(& adb -s $Serial shell $command)

    if ($LASTEXITCODE -ne 0 -or $response.Count -lt 2) {
        throw 'Program validator returned incomplete Android output.'
    }

    $lines = @($response | ForEach-Object { $_.TrimEnd([char]13) })
    $last = $lines[-1].Trim()

    if ($last -cne 'PROGRAM_NATIVE_EXIT=0') {
        throw "Program validator native exit is not zero: $last"
    }

    $body = [string]::Join([char]10, $lines[0..($lines.Count - 2)])

    if ($body -notmatch '(?m)^BACKEND_VULKAN_PROGRAM_DIAGNOSTIC_PASS accepted=2 structural_rejects=10 limit_rejects=4 driver_calls=0 gpu_dispatches=0 password_inputs=0 scheduled=0$') {
        throw 'Program graph completion marker is missing.'
    }

    if (@($lines | Where-Object {
        $_.StartsWith('BACKEND_VULKAN_PROGRAM_CASE_PASS ')
    }).Count -ne 16) {
        throw 'Program graph did not report all 16 validation cases.'
    }

    if ($body -match 'vkCreate|vkQueue|vkCmd|DEVICE_CATALOG|OpenCL Info:') {
        throw 'Host-only program graph transcript contains an unexpected GPU/backend event.'
    }

    $metadata = @(
        'TEST=vulkan-backend-program-graph',
        "ABI=$chosen",
        'DRIVER_CALLS=0',
        'GPU_DISPATCHES=0',
        'ADB_EXIT_CODE=0',
        $body
    )

    $report = Join-Path $build "$ReportPrefix-program-graph.log"

    [System.IO.File]::WriteAllText(
        $report,
        ([string]::Join([char]10, $metadata)) + [char]10,
        [System.Text.UTF8Encoding]::new($false)
    )

    Write-Output "PASS Android $chosen Vulkan program graph contract: 2 accepted, 14 rejected, zero GPU calls"
}
finally {
    & adb -s $Serial shell rm -rf $remote 2>$null | Out-Null
}
