<#
.SYNOPSIS
Tests automatic Vulkan direct mapping and GPU staging transfers on Android.

.DESCRIPTION
Stages a standalone Vulkan 1.0 program and two public SPIR-V shader variants.
Runs automatic and explicit staging modes for both variants. The program
selects all memory types and compute queues from the runtime Vulkan API,
executes one synthetic shader and compares all GPU output words with the CPU.

This script validates mode selection, the exact number of Vulkan copies,
output checksums, real buffer memory requirements and four malformed CLI
controls. It stores local logs and removes all temporary staged device files.
No hashcat process or cryptographic GPU shader is executed.

.PARAMETER Serial
Exact ADB identifier of one connected Android device.

.PARAMETER ArtifactDirectory
Directory containing the three outputs from build-vulkan-staging.ps1.

.PARAMETER ReportPrefix
ASCII-only local log prefix, not an ADB device identifier.
#>
param (
    [Parameter(Mandatory = $true)]
    [string]$Serial,

    [string]$ArtifactDirectory = '',

    [string]$ReportPrefix = 'vulkan-staging'
)

$ErrorActionPreference = 'Stop'

if ($ReportPrefix -cnotmatch '^[A-Za-z0-9_-]+$') {
    throw 'ReportPrefix must use ASCII letters, digits, underscores and hyphens.'
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

if ([string]::IsNullOrWhiteSpace($ArtifactDirectory)) {
    $ArtifactDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-staging-20261031'
}
elseif (-not [System.IO.Path]::IsPathRooted($ArtifactDirectory)) {
    $ArtifactDirectory = Join-Path $root $ArtifactDirectory
}

$directory = (Resolve-Path -LiteralPath $ArtifactDirectory).Path
$remote = '/data/local/tmp'
$binary = 'vulkan-staging-arm64'
$names = @($binary, 'stage-a-O0-vk1.0.spv', 'stage-a-O-vk1.0.spv')

foreach ($name in $names) {
    if (-not (Test-Path -LiteralPath (Join-Path $directory $name) -PathType Leaf)) {
        throw "Required synthetic Vulkan staging build artifact absent: $name"
    }
}

$device = @(& adb -s $Serial get-state 2>&1)

if ($LASTEXITCODE -ne 0 -or ($device -join '').Trim() -ne 'device') {
    throw "Specified Android device is unavailable: $Serial"
}

try {
    foreach ($name in $names) {
        & adb -s $Serial push (Join-Path $directory $name) ($remote + '/' + $name) | Out-Null

        if ($LASTEXITCODE -ne 0) {
            throw "Unable to stage Vulkan transfer test artifact: $name"
        }
    }

    & adb -s $Serial shell chmod 700 ($remote + '/' + $binary)

    if ($LASTEXITCODE -ne 0) {
        throw 'Cannot execute standalone Vulkan transfer test.'
    }

    $runCount = 0

    foreach ($mode in @('auto', 'staging')) {
        foreach ($optimization in @('O0', 'O')) {
            $arguments = @(
                '-s', $Serial, 'shell',
                ($remote + '/' + $binary),
                ($remote + "/stage-a-$optimization-vk1.0.spv"),
                '--mode', $mode
            )
            $lines = @(& adb @arguments 2>&1)
            $exitCode = $LASTEXITCODE
            $output = ($lines | Out-String).Trim()
            $log = Join-Path $directory "$ReportPrefix-$mode-$optimization.log"

            @(
                'TEST=portable-vulkan1.0-staging',
                "REQUESTED_MODE=$mode",
                "OPTIMIZATION=$optimization",
                "ADB_EXIT_CODE=$exitCode",
                $output
            ) | Set-Content -LiteralPath $log -Encoding UTF8

            if ($exitCode -ne 0) {
                throw "Vulkan transfer execution failed: mode=$mode, optimization=$optimization, exit=$exitCode; $output"
            }

            foreach ($policy in @(
                'PASS_POLICY gpu_local_preference=0',
                'PASS_POLICY compatible_nonlocal_type=1',
                'PASS_POLICY noncoherent_staging_memory_type=1',
                'PASS_POLICY no_host_visible_staging_result=-8',
                'PASS_POLICY incompatible_gpu_memory_result=-8',
                'PASS_POLICY supported_direct_mapping=1',
                'PASS_POLICY device_local_only_selects_staging=1',
                'PASS_POLICY explicit_staging=1',
                'PASS_POLICY no_gpu_storage_type_result=-8',
                'ACCEPT_COMPUTE index='
            )) {
                if (-not $output.Contains($policy)) {
                    throw "Missing portable Vulkan transfer policy result: $policy"
                }
            }

            $transfers = [regex]::Matches(
                $output,
                '(?m)^AUTO_TRANSFER mode=(direct|staging) .+\r?$'
            )

            if ($transfers.Count -ne 4) {
                throw "Expected 3 synthetic and 1 actual transfer decision, got $($transfers.Count)"
            }

            $actualMode = $transfers[3].Groups[1].Value

            if ($mode -eq 'staging' -and $actualMode -ne 'staging') {
                throw 'Explicit Vulkan staging mode unexpectedly used direct mapping.'
            }

            $expectedCopies = if ($actualMode -eq 'staging') { 1 } else { 0 }
            $expectedBuffers = if ($actualMode -eq 'staging') { 4 } else { 2 }

            $requirements = [regex]::Matches(
                $output,
                '(?m)^BUFFER_REQUIREMENTS role=(gpu-input|gpu-output|staging-upload|staging-download) required=(\d+) alignment=(\d+) memory_bits=0x([0-9a-f]{8})\r?$'
            )
            $allocations = [regex]::Matches(
                $output,
                '(?m)^BUFFER_ALLOCATION role=(gpu-input|gpu-output|staging-upload|staging-download) memory_type=(\d+) cpu_mapped=([01]) coherent=([01]) bytes=(\d+)\r?$'
            )

            if ($requirements.Count -ne $expectedBuffers -or
                $allocations.Count -ne $expectedBuffers) {
                throw "Vulkan transfer resources do not match actual path: mode=$actualMode"
            }

            $expectedRoles = @('gpu-input', 'gpu-output')

            if ($actualMode -eq 'staging') {
                $expectedRoles += @('staging-upload', 'staging-download')
            }

            for ($index = 0; $index -lt $expectedBuffers; $index++) {
                $require = $requirements[$index].Groups
                $allocate = $allocations[$index].Groups
                $requiredBytes = [long]::Parse($require[2].Value)
                $alignment = [long]::Parse($require[3].Value)
                $allowed = [Convert]::ToUInt32($require[4].Value, 16)
                $memoryType = [int]::Parse($allocate[2].Value)
                $mapped = [int]::Parse($allocate[3].Value)
                $actualBytes = [long]::Parse($allocate[5].Value)
                $expectedMapped = if ($actualMode -eq 'staging' -and $index -lt 2) { 0 } else { 1 }

                if ($require[1].Value -cne $expectedRoles[$index] -or
                    $allocate[1].Value -cne $expectedRoles[$index] -or
                    $requiredBytes -lt 8192 -or
                    $actualBytes -ne $requiredBytes -or
                    $alignment -le 0 -or
                    ($alignment -band ($alignment - 1)) -ne 0 -or
                    $memoryType -lt 0 -or $memoryType -ge 32 -or
                    ($allowed -band ([uint32]1 -shl $memoryType)) -eq 0 -or
                    $mapped -ne $expectedMapped) {
                    throw "Vulkan runtime buffer requirements or mapping invalid: role=$($expectedRoles[$index])"
                }
            }

            $result = [regex]::Match(
                $output,
                '(?m)^PASS_VULKAN_TRANSFER mode=(direct|staging) device=(\d+) queue=(\d+) words=2048 checksum=([0-9a-f]{16}) uploads=([01]) downloads=([01]) gpu_dispatches=1\r?$'
            )

            if (-not $result.Success -or
                $result.Groups[1].Value -ne $actualMode -or
                $result.Groups[4].Value -ne '00d6d6e4837edebc' -or
                [int]$result.Groups[5].Value -ne $expectedCopies -or
                [int]$result.Groups[6].Value -ne $expectedCopies) {
                throw "Vulkan transfer compute result or copy count mismatch: $mode / $optimization"
            }

            $runCount++

            Write-Output "PASS requested=$mode optimization=$optimization selected=${actualMode}: 2048 GPU words, $expectedCopies upload, $expectedCopies download"
        }
    }

    $negativeRecords = @()

    foreach ($case in @('no-arguments', 'missing-shader', 'unknown-mode', 'wrong-flag')) {
        $arguments = @('-s', $Serial, 'shell', ($remote + '/' + $binary))
        $reason = 'Usage:'

        if ($case -eq 'missing-shader') {
            $arguments += @(
                ($remote + '/missing-public-shader.spv'), '--mode', 'auto'
            )
            $reason = 'shader fopen'
        }
        elseif ($case -eq 'unknown-mode') {
            $arguments += @(
                ($remote + '/stage-a-O-vk1.0.spv'), '--mode', 'bad-mode'
            )
        }
        elseif ($case -eq 'wrong-flag') {
            $arguments += @(
                ($remote + '/stage-a-O-vk1.0.spv'), '--bad-flag', 'staging'
            )
        }

        $saved = $ErrorActionPreference

        try {
            $ErrorActionPreference = 'Continue'
            $errors = @(& adb @arguments 2>&1)
            $code = $LASTEXITCODE
        }
        finally {
            $ErrorActionPreference = $saved
        }

        $message = ($errors | Out-String).Trim()

        if ($code -ne 2 -or -not $message.Contains($reason)) {
            throw "Invalid Vulkan transfer CLI was accepted: $case, exit=$code; $message"
        }

        $negativeRecords += "INVALID $case exit=2"
        Write-Output "PASS rejected ${case}: exit code 2"
    }

    if ($runCount -ne 4 -or $negativeRecords.Count -ne 4) {
        throw 'Expected four real GPU runs and four input rejection tests.'
    }

    $log = Join-Path $directory "$ReportPrefix-negative-controls.log"

    @(
        'TEST=portable-vulkan1.0-staging',
        'INVALID_INPUT_CONTROLS=4'
    ) + $negativeRecords | Set-Content -LiteralPath $log -Encoding UTF8

    Write-Output 'PASS four real synthetic GPU executions and four input rejections using API-driven device selection'
}
finally {
    foreach ($name in $names) {
        & adb -s $Serial shell rm -f ($remote + '/' + $name) 2>$null | Out-Null
    }
}
