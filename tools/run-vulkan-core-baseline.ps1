<#
.SYNOPSIS
Tests a Vulkan 1.0 Compute baseline on an explicitly selected Android device.

.DESCRIPTION
Selects the ARM64 or ARMv7 Vulkan 1.0 executable using the Android device's
declared ABI capabilities; an explicit -Abi requirement is checked and
rejected when unsupported. Both binaries target Android API 24. Stages the
selected executable and public SPIR-V 1.0 shaders, then executes baseline
and optional compatibility tests without vendor-specific device profiles.

With -TestCore11, also executes an explicitly required Vulkan 1.1 variant.
The flag is optional so devices supporting only Vulkan 1.0 can pass the
standard test without requiring features from newer API versions.

With -TestNoncoherentStaging, explicitly requests Vulkan 1.0 staging memory
that has HOST_VISIBLE without HOST_COHERENT. Devices with compatible types
must complete real flush/Compute/fence/invalidate and exact CPU verification.
Devices without compatible memory return a precise allocation-type rejection
and are recorded as unsupported, without silent fallback.

Every run checks selected API versions, memory requirements and GPU output.
Writes LF-only local logs and removes all temporary Android device files.

.PARAMETER Serial
Exact ADB identifier of the connected Android device to test.

.PARAMETER ArtifactDirectory
Directory containing ARM64/ARMv7 diagnostics and two SPIR-V 1.0 files.

.PARAMETER Abi
Select the actual device ABI automatically (arm64-v8a before armeabi-v7a),
or explicitly require arm64-v8a or armeabi-v7a. Unsupported requested ABIs
return an error and no program is uploaded to the device.

.PARAMETER ReportPrefix
ASCII-only label for the locally saved test reports.

.PARAMETER TestCore11
Also run the optional core Vulkan 1.1 requirement. Only use this switch when
the device supports at least Vulkan 1.1; it is not a prerequisite for 1.0.

.PARAMETER TestNoncoherentStaging
Exercise actual noncoherent HOST_VISIBLE memory on core Vulkan 1.0 when
available, or record exact unsupported memoryTypeBits when unavailable.
#>
param (
    [Parameter(Mandatory = $true)]
    [string]$Serial,

    [string]$ArtifactDirectory = '',

    [ValidateSet('auto', 'arm64-v8a', 'armeabi-v7a')]
    [string]$Abi = 'auto',

    [string]$ReportPrefix = 'vulkan-core10',

    [switch]$TestCore11,

    [switch]$TestNoncoherentStaging
)

$ErrorActionPreference = 'Stop'

if ($ReportPrefix -cnotmatch '^[A-Za-z0-9_-]+$') {
    throw 'ReportPrefix may contain ASCII letters, digits, underscores or hyphens.'
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

if ([string]::IsNullOrWhiteSpace($ArtifactDirectory)) {
    $ArtifactDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-core-baseline-20261031'
}
elseif (-not [System.IO.Path]::IsPathRooted($ArtifactDirectory)) {
    $ArtifactDirectory = Join-Path $root $ArtifactDirectory
}

$directory = (Resolve-Path -LiteralPath $ArtifactDirectory).Path
$remote = '/data/local/tmp'

$state = @(& adb -s $Serial get-state 2>&1)

if ($LASTEXITCODE -ne 0 -or ($state -join '').Trim() -ne 'device') {
    throw "Selected Android device is not connected: $Serial"
}

$reported = @(& adb -s $Serial shell getprop ro.product.cpu.abilist 2>&1)

if ($LASTEXITCODE -ne 0 -or $reported.Count -ne 1) {
    throw 'Android device ABI capabilities are unavailable.'
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
        throw "No compatible Android ARM64 or ARMv7 ABI: $($abis -join ',')"
    }
}
else {
    if ($abis -notcontains $Abi) {
        throw "Requested Android ABI $Abi is unsupported; reported: $($abis -join ',')"
    }

    $selectedAbi = $Abi
}

$executable = if ($selectedAbi -eq 'arm64-v8a') {
    'vulkan-core-baseline-arm64'
}
else {
    'vulkan-core-baseline-armv7'
}

$names = @($executable, 'stage-a-O0-vk1.0.spv', 'stage-a-O-vk1.0.spv')

foreach ($name in $names) {
    if (-not (Test-Path -LiteralPath (Join-Path $directory $name) -PathType Leaf)) {
        throw "Required portable Vulkan 1.0 artifact unavailable: $name"
    }
}

Write-Output "SELECT_ANDROID_ABI=$selectedAbi source=ro.product.cpu.abilist"

try {
    foreach ($name in $names) {
        & adb -s $Serial push (Join-Path $directory $name) ($remote + '/' + $name) | Out-Null

        if ($LASTEXITCODE -ne 0) {
            throw "Unable to stage Vulkan 1.0 test artifact: $name"
        }
    }

    & adb -s $Serial shell chmod 700 ($remote + '/' + $executable)

    if ($LASTEXITCODE -ne 0) {
        throw 'Unable to execute Vulkan 1.0 baseline diagnostic.'
    }

    $modes = @('default10', 'explicit10')

    if ($TestCore11) {
        $modes += 'required11'
    }

    $executions = 0

    foreach ($mode in $modes) {
        $expected = if ($mode -eq 'required11') { '1.1' } else { '1.0' }

        foreach ($optimization in @('O0', 'O')) {
            $arguments = @(
                '-s', $Serial, 'shell',
                ($remote + '/' + $executable),
                ($remote + "/stage-a-$optimization-vk1.0.spv")
            )

            if ($mode -eq 'explicit10') {
                $arguments += @('--minimum', '1.0')
            }
            elseif ($mode -eq 'required11') {
                $arguments += @('--minimum', '1.1')
            }

            $lines = @(& adb @arguments 2>&1)
            $exitCode = $LASTEXITCODE
            $text = ($lines | Out-String).Trim()
            $text = $text.Replace("`r`n", "`n").Replace("`r", '')

            if ($exitCode -ne 0) {
                throw "Vulkan Compute $mode -$optimization failed: exit=$exitCode; $text"
            }

            foreach ($required in @(
                'VULKAN_LOADER_VERSION=',
                "SELECT_INSTANCE_API=$expected ",
                "SELECT_DEVICE_API=$expected ",
                'ACCEPT_COMPUTE index=',
                'CORE11_OPTIONAL_FEATURES ',
                ' enabled=0 extensions=0 ',
                'PLAN_BUFFER index=0 label=gpu-input',
                'PLAN_BUFFER index=1 label=gpu-output',
                'PLAN_BUFFER index=2 label=staging-upload',
                'PLAN_BUFFER index=3 label=staging-download'
            )) {
                if (-not $text.Contains($required)) {
                    throw "Vulkan $expected output missing a required fact: $required"
                }
            }

            $result = [regex]::Matches(
                $text,
                '(?m)^PASS_CORE11_COMPUTE device=(\d+) queue=(\d+) words=2048 checksum=00d6d6e4837edebc gpu_buffers=2 staging_buffers=2 copies=2 optional_features=0 extensions=0\n?$'
            )

            if ($result.Count -ne 1) {
                throw "Vulkan $expected GPU output is invalid: $mode -$optimization"
            }

            if (@([regex]::Matches(
                $text, '(?m)^PLAN_BUFFER index=(\d+) label='
            )).Count -ne 4) {
                throw "Vulkan $expected GPU allocation plan is incomplete."
            }

            $header = @(
                'TEST=vulkan-core1.0-baseline-optional-1.1',
                "MODE=$mode",
                "OPTIMIZATION=$optimization",
                "ADB_EXIT_CODE=$exitCode"
            )

            [System.IO.File]::WriteAllText(
                (Join-Path $directory "$ReportPrefix-$mode-$optimization.log"),
                (($header -join "`n") + "`n" + $text + "`n"),
                [System.Text.UTF8Encoding]::new($false)
            )

            $executions++

            Write-Output "PASS $mode -${optimization}: selected Vulkan $expected, 2048 exact GPU outputs, two staging copies"
        }
    }

    if ($TestNoncoherentStaging) {
        foreach ($optimization in @('O0', 'O')) {
            $probe = (
                ($remote + '/' + $executable) + ' ' +
                ($remote + "/stage-a-$optimization-vk1.0.spv") +
                ' --require-noncoherent-staging 2>&1; echo DEVICE_EXIT=$?'
            )

            $lines = @(& adb -s $Serial shell $probe 2>&1)

            if ($LASTEXITCODE -ne 0) {
                throw "ADB failed to execute Vulkan noncoherent staging -$optimization"
            }

            $output = ($lines | Out-String).Trim()
            $output = $output.Replace("`r`n", "`n").Replace("`r", '')

            $returnLine = [regex]::Matches(
                $output, '(?m)^DEVICE_EXIT=(\d+)$'
            )

            if ($returnLine.Count -ne 1) {
                throw "Device return code unavailable for noncoherent -$optimization"
            }

            $exitCode = [int]$returnLine[0].Groups[1].Value
            $payload = [regex]::Replace(
                $output, '(?m)^DEVICE_EXIT=\d+\n?', ''
            ).TrimEnd()

            if (-not $payload.Contains('SELECT_INSTANCE_API=1.0 ') -or
                -not $payload.Contains('CORE10_STAGING_REQUEST mode=noncoherent')) {
                throw "Noncoherent probe did not request core Vulkan 1.0: -$optimization"
            }

            if ($exitCode -eq 0) {
                foreach ($required in @(
                    'CORE10_STAGING_MAPPED input_type=',
                    'input_coherent=0 output_coherent=0',
                    'CORE10_STAGING_SYNC index=2 operation=flush coherent=0 driver_call=1',
                    'CORE10_STAGING_SYNC index=3 operation=invalidate coherent=0 driver_call=1',
                    'PASS_CORE11_COMPUTE device=0 queue=0 words=2048 checksum=00d6d6e4837edebc'
                )) {
                    if (-not $payload.Contains($required)) {
                        throw "Incomplete real noncoherent GPU transfer: $required"
                    }
                }

                if (@([regex]::Matches(
                    $payload, '(?m)^CORE10_STAGING_SYNC index='
                )).Count -ne 2) {
                    throw 'Expected exactly one noncoherent flush and one invalidate.'
                }

                Write-Output "PASS Vulkan 1.0 noncoherent -${optimization}: real flush, Compute, fence, invalidate and CPU reference"
            }
            elseif ($exitCode -eq 1) {
                if ($payload -notmatch 'REJECT_ALLOCATION_TYPE index=[23] role=1 bits=0x[0-9a-f]{8}' -or
                    -not $payload.Contains(
                        'NO_CORE11_COMPATIBLE_SYNTHETIC_COMPUTE_CONFIGURATION'
                    ) -or
                    $payload.Contains('PASS_CORE11_COMPUTE')) {
                    throw "Noncoherent mode failed for a reason other than incompatible memory: -$optimization"
                }

                Write-Output "UNSUPPORTED Vulkan 1.0 noncoherent -${optimization}: no compatible HOST_VISIBLE noncoherent staging type; no fallback"
            }
            else {
                throw "Vulkan noncoherent staging returned unexpected exit=$exitCode"
            }

            $record = @(
                'TEST=vulkan-core1.0-noncoherent-staging',
                'MODE=noncoherent10',
                "OPTIMIZATION=$optimization",
                "ADB_EXIT_CODE=$exitCode",
                $payload
            )

            [System.IO.File]::WriteAllText(
                (Join-Path $directory "$ReportPrefix-noncoherent10-$optimization.log"),
                ($record -join "`n") + "`n",
                [System.Text.UTF8Encoding]::new($false)
            )
        }
    }

    $negative = @()

    foreach ($case in @('no-shader', 'bad-flag', 'unsupported-api', 'missing-file')) {
        $arguments = @(
            '-s', $Serial, 'shell', ($remote + '/' + $executable)
        )
        $expectedText = 'Usage:'

        if ($case -eq 'bad-flag') {
            $arguments += @(
                ($remote + '/stage-a-O-vk1.0.spv'),
                '--wrong-option', '1.0'
            )
        }
        elseif ($case -eq 'unsupported-api') {
            $arguments += @(
                ($remote + '/stage-a-O-vk1.0.spv'),
                '--minimum', '1.2'
            )
        }
        elseif ($case -eq 'missing-file') {
            $arguments += @(
                ($remote + '/missing-public-synthetic-shader.spv')
            )
            $expectedText = 'SHADER_FILE_OPEN_FAILED'
        }

        $savedPreference = $ErrorActionPreference

        try {
            $ErrorActionPreference = 'Continue'
            $errorLines = @(& adb @arguments 2>&1)
            $errorCode = $LASTEXITCODE
        }
        finally {
            $ErrorActionPreference = $savedPreference
        }

        if ($errorCode -ne 2 -or
            -not ($errorLines -join ' ').Contains($expectedText)) {
            throw "Invalid Vulkan 1.0 CLI input was not rejected: $case"
        }

        $negative += "INVALID $case exit=2"

        Write-Output "PASS rejected $case, exit code 2"
    }

    $controls = @(
        'TEST=vulkan-core1.0-baseline-optional-1.1',
        'INVALID_INPUT_CONTROLS=4'
    ) + $negative

    [System.IO.File]::WriteAllText(
        (Join-Path $directory "$ReportPrefix-negative-controls.log"),
        ($controls -join "`n") + "`n",
        [System.Text.UTF8Encoding]::new($false)
    )

    if ($executions -ne 2 * $modes.Count -or $negative.Count -ne 4) {
        throw 'The Vulkan core-version compatibility run is incomplete.'
    }

    Write-Output "PASS Vulkan 1.0 compatible compute: $executions GPU executions, four invalid CLI controls"
}
finally {
    foreach ($name in $names) {
        & adb -s $Serial shell rm -f ($remote + '/' + $name) 2>$null | Out-Null
    }
}
