<#
.SYNOPSIS
Tests the portable three-stage Vulkan 1.0 Compute path on one Android device.

.DESCRIPTION
Discovers Android ABI support through ro.product.cpu.abilist and chooses
the ARM64 or ARMv7 API24 executable without manufacturer/model profiles.
Uploads three public SPIR-V 1.0 shaders per optimization and tests default
core Vulkan 1.0 plus an explicit core Vulkan 1.0 requirement.

-TestCore11 additionally exercises the explicitly requested Vulkan 1.1 API.
-TestNoncoherentStaging exercises real HOST_VISIBLE noncoherent memory
or records a precise incompatible-memory rejection without fallback.

Every successful run verifies the exact API, six allocation plans,
flush/invalidate behavior and all three CPU reference checksums. Captures
LF-only local evidence logs and always removes temporary Android files.

.PARAMETER Serial
Exact ADB serial identifying the device.

.PARAMETER ArtifactDirectory
Directory containing two native executables and six SPIR-V 1.0 modules.

.PARAMETER Abi
auto selects a declared ABI, preferring ARM64, or explicitly requires
arm64-v8a or armeabi-v7a.

.PARAMETER ReportPrefix
ASCII-only prefix used to name local evidence logs.

.PARAMETER TestCore11
Also test explicit Vulkan core 1.1. Not required for Vulkan 1.0 devices.

.PARAMETER TestNoncoherentStaging
Test actual noncoherent host staging memory, or record exact incompatibility.
#>
param (
    [Parameter(Mandatory = $true)]
    [string]$Serial,

    [string]$ArtifactDirectory = '',

    [ValidateSet('auto', 'arm64-v8a', 'armeabi-v7a')]
    [string]$Abi = 'auto',

    [string]$ReportPrefix = 'portable',

    [switch]$TestCore11,

    [switch]$TestNoncoherentStaging
)

$ErrorActionPreference = 'Stop'

if ($ReportPrefix -cnotmatch '^[A-Za-z0-9_-]+$') {
    throw 'ReportPrefix must use ASCII letters, digits, hyphens or underscores.'
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

if ([string]::IsNullOrWhiteSpace($ArtifactDirectory)) {
    $ArtifactDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-core-multistage-20261031'
}
elseif (-not [System.IO.Path]::IsPathRooted($ArtifactDirectory)) {
    $ArtifactDirectory = Join-Path $root $ArtifactDirectory
}

$directory = (Resolve-Path -LiteralPath $ArtifactDirectory).Path
$device = @(& adb -s $Serial get-state 2>&1)

if ($LASTEXITCODE -ne 0 -or ($device -join '').Trim() -ne 'device') {
    throw "Requested Android device unavailable: $Serial"
}

$reported = @(& adb -s $Serial shell getprop ro.product.cpu.abilist 2>&1)

if ($LASTEXITCODE -ne 0 -or $reported.Count -ne 1) {
    throw 'Android ABI capability list is not readable.'
}

$abis = @(($reported[0].Trim() -split ',') | Where-Object {
    -not [string]::IsNullOrWhiteSpace($_)
})

if ($Abi -eq 'auto') {
    if ($abis -contains 'arm64-v8a') {
        $selectedAbi = 'arm64-v8a'
    }
    elseif ($abis -contains 'armeabi-v7a') {
        $selectedAbi = 'armeabi-v7a'
    }
    else {
        throw "No supported ARM32 or ARM64 ABI: $($abis -join ',')"
    }
}
else {
    if ($abis -notcontains $Abi) {
        throw "Requested ABI $Abi unsupported; actual ABIs: $($abis -join ',')"
    }

    $selectedAbi = $Abi
}

$executable = if ($selectedAbi -eq 'arm64-v8a') {
    'vulkan-core10-multistage-arm64'
}
else {
    'vulkan-core10-multistage-armv7'
}

$files = @($executable)

foreach ($mode in @('O0', 'O')) {
    foreach ($stage in @('a', 'b', 'c')) {
        $files += "stage-$stage-$mode-vk1.0.spv"
    }
}

foreach ($file in $files) {
    if (-not (Test-Path -LiteralPath (Join-Path $directory $file) -PathType Leaf)) {
        throw "Vulkan 1.0 pipeline artifact missing: $file"
    }
}

Write-Output "SELECT_ANDROID_ABI=$selectedAbi source=ro.product.cpu.abilist"

$remote = '/data/local/tmp'
$remoteExecutable = "$remote/$executable"

try {
    foreach ($file in $files) {
        & adb -s $Serial push (Join-Path $directory $file) "$remote/$file" | Out-Null

        if ($LASTEXITCODE -ne 0) {
            throw "Failed to stage Vulkan 1.0 pipeline file: $file"
        }
    }

    & adb -s $Serial shell chmod 700 $remoteExecutable

    if ($LASTEXITCODE -ne 0) {
        throw 'Android cannot execute the chosen native ABI.'
    }

    $modes = @('default10', 'explicit10')

    if ($TestCore11) {
        $modes += 'required11'
    }

    foreach ($mode in $modes) {
        $expected = if ($mode -eq 'required11') { '1.1' } else { '1.0' }

        foreach ($optimization in @('O0', 'O')) {
            $shaderArgs = @(
                "$remote/stage-a-$optimization-vk1.0.spv",
                "$remote/stage-b-$optimization-vk1.0.spv",
                "$remote/stage-c-$optimization-vk1.0.spv"
            )
            $arguments = @('-s', $Serial, 'shell', $remoteExecutable) + $shaderArgs

            if ($mode -eq 'required11') {
                $arguments += @('--minimum', '1.1')
            }
            elseif ($mode -eq 'explicit10') {
                $arguments += @('--minimum', '1.0')
            }

            $lines = @(& adb @arguments 2>&1)
            $code = $LASTEXITCODE
            $body = ($lines | Out-String).Trim()
            $body = $body.Replace("`r`n", "`n").Replace("`r", '')

            if ($code -ne 0) {
                throw "Three-stage Vulkan $mode -$optimization failed: exit=$code; $body"
            }

            foreach ($required in @(
                "SELECT_INSTANCE_API=$expected ",
                "SELECT_DEVICE_API=$expected ",
                'MULTISTAGE_DEVICE_API effective=',
                'shared_bytes=128 storage_bindings=4 enabled_features=0 extensions=0',
                'MULTISTAGE_STAGING_REQUEST mode=auto',
                'MULTISTAGE_STAGING_MAPPED upload_type=',
                'MULTISTAGE_SYNC index=4 operation=flush coherent=',
                'MULTISTAGE_SYNC index=5 operation=invalidate coherent=',
                'PASS_CORE10_MULTISTAGE elements=2048 verified_words=12288 stageA=de56d6d0aa66c6ec stageB=c1259ad4f024499a output=05de70c62f8d7cfd'
            )) {
                if (-not $body.Contains($required)) {
                    throw "Vulkan Compute missing exact evidence: $required"
                }
            }

            if (@([regex]::Matches(
                $body, '(?m)^MULTISTAGE_PLAN_BUFFER index=\d+'
            )).Count -ne 6 -or
                @([regex]::Matches(
                    $body, '(?m)^MULTISTAGE_REQUIREMENTS index=\d+'
                )).Count -ne 6) {
                throw "The six Vulkan GPU/staging memory buffers were not all planned."
            }

            $record = @(
                'TEST=portable-vulkan1.0-multistage',
                "MODE=$mode",
                "OPTIMIZATION=$optimization",
                "ABI=$selectedAbi",
                'ADB_EXIT_CODE=0',
                $body
            )

            [System.IO.File]::WriteAllText(
                (Join-Path $directory "$ReportPrefix-$mode-$optimization.log"),
                ($record -join "`n") + "`n",
                [System.Text.UTF8Encoding]::new($false)
            )

            Write-Output "PASS $selectedAbi $mode -${optimization}: 3 Compute stages, 12,288 words, 6 buffers"
        }
    }

    if ($TestNoncoherentStaging) {
        foreach ($optimization in @('O0', 'O')) {
            $shaders = @(
                "$remote/stage-a-$optimization-vk1.0.spv",
                "$remote/stage-b-$optimization-vk1.0.spv",
                "$remote/stage-c-$optimization-vk1.0.spv"
            )

            $remoteCommand = (
                $remoteExecutable + ' ' + ($shaders -join ' ') +
                ' --require-noncoherent-staging 2>&1; echo DEVICE_EXIT=$?'
            )

            $lines = @(& adb -s $Serial shell $remoteCommand 2>&1)

            if ($LASTEXITCODE -ne 0) {
                throw "ADB cannot test noncoherent Vulkan staging -$optimization"
            }

            $body = ($lines | Out-String).Trim()
            $body = $body.Replace("`r`n", "`n").Replace("`r", '')
            $exitLine = [regex]::Matches(
                $body, '(?m)^DEVICE_EXIT=(\d+)$'
            )

            if ($exitLine.Count -ne 1) {
                throw 'Noncoherent Vulkan Compute exit status unavailable.'
            }

            $code = [int]$exitLine[0].Groups[1].Value
            $body = [regex]::Replace(
                $body, '(?m)^DEVICE_EXIT=\d+\n?', ''
            ).TrimEnd()

            if (-not $body.Contains('SELECT_INSTANCE_API=1.0 ') -or
                -not $body.Contains(
                    'MULTISTAGE_STAGING_REQUEST mode=noncoherent'
                )) {
                throw 'The Vulkan 1.0 noncoherent memory test did not run.'
            }

            if ($code -eq 0) {
                foreach ($required in @(
                    'MULTISTAGE_STAGING_MAPPED upload_type=',
                    'upload_coherent=0 download_coherent=0',
                    'MULTISTAGE_SYNC index=4 operation=flush coherent=0 driver_call=1',
                    'MULTISTAGE_SYNC index=5 operation=invalidate coherent=0 driver_call=1',
                    'PASS_CORE10_MULTISTAGE elements=2048 verified_words=12288 stageA=de56d6d0aa66c6ec stageB=c1259ad4f024499a output=05de70c62f8d7cfd'
                )) {
                    if (-not $body.Contains($required)) {
                        throw "Real noncoherent Vulkan GPU result invalid: $required"
                    }
                }

                Write-Output "PASS $selectedAbi noncoherent -${optimization}: real flush/invalidate and three correct GPU stages"
            }
            elseif ($code -eq 1) {
                if ($body -notmatch 'REJECT_MULTISTAGE_MEMORY_TYPE index=[45] role=1 bits=0x[0-9a-f]{8}' -or
                    -not $body.Contains(
                        'NO_COMPATIBLE_VULKAN_MULTISTAGE_CONFIGURATION'
                    ) -or
                    $body.Contains('PASS_CORE10_MULTISTAGE')) {
                    throw 'Noncoherent Vulkan test did not fail with an exact memory-type mismatch.'
                }

                Write-Output "UNSUPPORTED $selectedAbi noncoherent -${optimization}: no valid host memory type; no fallback"
            }
            else {
                throw "Unknown noncoherent Vulkan return code $code"
            }

            $record = @(
                'TEST=portable-vulkan1.0-multistage',
                'MODE=noncoherent10',
                "OPTIMIZATION=$optimization",
                "ABI=$selectedAbi",
                "ADB_EXIT_CODE=$code",
                $body
            )

            [System.IO.File]::WriteAllText(
                (Join-Path $directory "$ReportPrefix-noncoherent10-$optimization.log"),
                ($record -join "`n") + "`n",
                [System.Text.UTF8Encoding]::new($false)
            )
        }
    }

    Write-Output 'PASS portable three-stage Vulkan 1.0 device verification complete'
}
finally {
    foreach ($file in $files) {
        & adb -s $Serial shell rm -f "$remote/$file" 2>$null | Out-Null
    }
}
