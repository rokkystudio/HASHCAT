<#
.SYNOPSIS
Checks automatic Vulkan 1.0 compute configuration on one Android device.

.DESCRIPTION
Stages one standalone ARM64 program and two versions of the original public
stage-A synthetic shader. The Vulkan program detects compatible queues,
compute limits and memory types at runtime, then performs one GPU dispatch
and checks every output word against a CPU reference.

The script validates both shader optimization modes, exact memory binding
alignment and two expected input errors, writes local evidence logs and
removes all staged files. It never requires GPU name or vendor-specific
settings, installs an app, or runs hashcat.

.PARAMETER Serial
Exact ADB serial of the connected Android device to test.

.PARAMETER ArtifactDirectory
Local directory containing the seven reproducible build outputs.

.PARAMETER ReportPrefix
ASCII-only name identifying local evidence files, not a device serial.
#>
param (
    [Parameter(Mandatory = $true)]
    [string]$Serial,

    [string]$ArtifactDirectory = '',

    [string]$ReportPrefix = 'vulkan-auto'
)

$ErrorActionPreference = 'Stop'

if ($ReportPrefix -cnotmatch '^[A-Za-z0-9_-]+$') {
    throw 'ReportPrefix must contain ASCII letters, digits, hyphens or underscores.'
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

if ([string]::IsNullOrWhiteSpace($ArtifactDirectory)) {
    $ArtifactDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-autoconfig-20261031'
}
elseif (-not [System.IO.Path]::IsPathRooted($ArtifactDirectory)) {
    $ArtifactDirectory = Join-Path $root $ArtifactDirectory
}

$directory = (Resolve-Path -LiteralPath $ArtifactDirectory).Path
$remote = '/data/local/tmp'
$executable = 'vulkan-autoconfig-arm64'
$names = @($executable, 'stage-a-O0-vk1.0.spv', 'stage-a-O-vk1.0.spv')

foreach ($name in $names) {
    if (-not (Test-Path -LiteralPath (Join-Path $directory $name) -PathType Leaf)) {
        throw "Required portable Vulkan build artifact missing: $name"
    }
}

$state = @(& adb -s $Serial get-state 2>&1)

if ($LASTEXITCODE -ne 0 -or ($state -join '').Trim() -ne 'device') {
    throw "Explicit Android device unavailable: $Serial"
}

try {
    foreach ($name in $names) {
        & adb -s $Serial push (Join-Path $directory $name) ($remote + '/' + $name) | Out-Null

        if ($LASTEXITCODE -ne 0) {
            throw "Failed to copy portable Vulkan test file: $name"
        }
    }

    & adb -s $Serial shell chmod 700 ($remote + '/' + $executable)

    if ($LASTEXITCODE -ne 0) {
        throw 'Cannot execute Android Vulkan capability probe.'
    }

    foreach ($optimization in @('O0', 'O')) {
        $arguments = @(
            '-s', $Serial, 'shell',
            ($remote + '/' + $executable),
            ($remote + "/stage-a-$optimization-vk1.0.spv")
        )

        $lines = @(& adb @arguments 2>&1)
        $exitCode = $LASTEXITCODE
        $output = ($lines | Out-String).Trim()
        $log = Join-Path $directory "$ReportPrefix-$optimization.log"

        @(
            'TEST=portable-vulkan1.0-auto-config',
            "OPTIMIZATION=$optimization",
            "ADB_EXIT_CODE=$exitCode",
            $output
        ) | Set-Content -LiteralPath $log -Encoding UTF8

        if ($exitCode -ne 0) {
            throw "Automatic Vulkan device and memory selection failed: $optimization, exit=$exitCode; $output"
        }

        foreach ($required in @(
            'PASS_POLICY preferred_coherent_type=',
            'PASS_POLICY fallback_noncoherent_type=',
            'PASS_POLICY noncoherent_atom=256',
            'PASS_POLICY coherent_offset_1=',
            'PASS_POLICY unsupported_memory_error=',
            'PASS_POLICY invalid_requirements_error=',
            'PASS_POLICY overflow_error=',
            'WORKLOAD_PROFILE vulkan_min=1.0',
            'ACCEPT_COMPUTE index=',
            'PASS_VULKAN_GPU_STAGE device_index='
        )) {
            if (-not $output.Contains($required)) {
                throw "Missing Vulkan auto-configuration evidence: $required"
            }
        }

        $plans = [regex]::Matches(
            $output,
            '(?m)^AUTO_MEMORY type=(\d+) coherent=([01]) bytes=(\d+) compatible_bits=0x([0-9a-f]{8}) atom=(\d+)\r?$'
        )

        if ($plans.Count -ne 3) {
            throw "Expected two synthetic policy plans and one real Vulkan plan, got $($plans.Count)"
        }

        $final = $plans[2].Groups
        $memoryType = [int]::Parse($final[1].Value)
        $coherent = [int]::Parse($final[2].Value)
        $allocatedBytes = [long]::Parse($final[3].Value)
        $memoryBits = [Convert]::ToUInt32($final[4].Value, 16)
        $atom = [long]::Parse($final[5].Value)

        if ($memoryType -lt 0 -or
            $memoryType -ge 32 -or
            ($memoryBits -band ([uint32]1 -shl $memoryType)) -eq 0 -or
            $atom -le 0 -or
            ($atom -band ($atom - 1)) -ne 0 -or
            $allocatedBytes -le 0) {
            throw 'Vulkan returned an incompatible memory type or invalid cache atom.'
        }

        $layoutRecords = [regex]::Matches(
            $output,
            '(?m)^AUTO_BUFFER index=(\d+) offset=(\d+) required=(\d+) driver_alignment=(\d+) effective_alignment=(\d+) compatible_bits=0x([0-9a-f]{8})\r?$'
        )

        if ($layoutRecords.Count -ne 9) {
            throw "Expected five policy-test ranges and four real buffer ranges: got $($layoutRecords.Count)"
        }

        $end = [long]0

        for ($index = 0; $index -lt 4; $index++) {
            $record = $layoutRecords[$layoutRecords.Count - 4 + $index].Groups
            $bufferIndex = [int]$record[1].Value
            $offset = [long]::Parse($record[2].Value)
            $required = [long]::Parse($record[3].Value)
            $driverAlignment = [long]::Parse($record[4].Value)
            $effectiveAlignment = [long]::Parse($record[5].Value)

            if ($bufferIndex -ne $index -or $offset -lt $end -or
                $required -le 0 -or $driverAlignment -le 0 -or
                $effectiveAlignment -lt $driverAlignment -or
                ($offset % $driverAlignment) -ne 0 -or
                ($offset % $effectiveAlignment) -ne 0) {
                throw "Automatic Vulkan memory range invalid at index $index"
            }

            $end = $offset + $required
        }

        if ($end -ne $allocatedBytes) {
            throw 'Allocated bytes differ from the last correctly aligned buffer end.'
        }

        $result = [regex]::Match(
            $output,
            '(?m)^PASS_VULKAN_AUTO_CONFIG device_index=(\d+) queue_family=(\d+) memory_type=(\d+) coherent=([01]) allocation_bytes=(\d+) buffers=4 verified_cpu_words=14336 gpu_dispatches=1 verified_gpu_words=2048\r?$'
        )

        if (-not $result.Success -or
            [int]$result.Groups[3].Value -ne $memoryType -or
            [int]$result.Groups[4].Value -ne $coherent -or
            [long]$result.Groups[5].Value -ne $allocatedBytes) {
            throw 'Vulkan GPU verification does not match the selected memory plan.'
        }

        Write-Output "PASS Vulkan1.0 ${optimization}: 2048 GPU words, 14336 mapped CPU words, memory type $memoryType, coherent=$coherent, allocation=$allocatedBytes"
    }

    $negativeRecords = @()

    foreach ($case in @('no-shader-argument', 'missing-shader-file')) {
        $arguments = @('-s', $Serial, 'shell', ($remote + '/' + $executable))

        if ($case -eq 'missing-shader-file') {
            $arguments += ($remote + '/missing-portable-shader.spv')
        }

        $saved = $ErrorActionPreference

        try {
            $ErrorActionPreference = 'Continue'
            $errorLines = @(& adb @arguments 2>&1)
            $errorCode = $LASTEXITCODE
        }
        finally {
            $ErrorActionPreference = $saved
        }

        $errorOutput = ($errorLines | Out-String).Trim()
        $expected = if ($case -eq 'no-shader-argument') { 'Usage:' } else { 'shader fopen' }

        if ($errorCode -ne 2 -or -not $errorOutput.Contains($expected)) {
            throw "Portable Vulkan invalid input not rejected: ${case}; exit=$errorCode"
        }

        $negativeRecords += "NEGATIVE $case exit=2"
        Write-Output "PASS rejected ${case}: exit code 2"
    }

    $negativeLog = Join-Path $directory "$ReportPrefix-negative-controls.log"

    @(
        'TEST=portable-vulkan1.0-auto-config',
        'INVALID_INPUT_CONTROLS=2'
    ) + $negativeRecords | Set-Content -LiteralPath $negativeLog -Encoding UTF8

    Write-Output 'PASS portable Vulkan configuration using API-reported capabilities, two real GPU runs and two input rejections'
}
finally {
    foreach ($name in $names) {
        & adb -s $Serial shell rm -f ($remote + '/' + $name) 2>$null | Out-Null
    }
}
