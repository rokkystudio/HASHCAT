<#
.SYNOPSIS
Runs synthetic Vulkan shared-memory suballocation checks on Android GPUs.

.DESCRIPTION
Stages a standalone ARM64 Vulkan program and six unchanged public synthetic
SPIR-V shaders, then verifies a three-dispatch GPU result with four storage
buffers in exactly one VkDeviceMemory allocation. Both shader optimization
variants are compared with CPU reference checksums and the exact Vulkan
buffer memory-requirement layout is recorded for each device.

Three simulated errors exercise application cleanup without exhausting
device memory or actually losing a GPU. Three invalid CLI inputs must fail
with code 2. All seven temporary files are removed from /data/local/tmp.

.PARAMETER Serial
Exact ADB serial of one connected Android device.

.PARAMETER ArtifactDirectory
Directory containing seven outputs of the Vulkan suballocation build script.

.PARAMETER ReportPrefix
ASCII-only prefix for local logs, excluding private device identifiers.
#>
param (
    [Parameter(Mandatory = $true)]
    [string]$Serial,

    [string]$ArtifactDirectory = '',

    [string]$ReportPrefix = 'vulkan-suballocation'
)

$ErrorActionPreference = 'Stop'

if ($ReportPrefix -cnotmatch '^[A-Za-z0-9_-]+$') {
    throw 'ReportPrefix must consist of ASCII letters, digits, underscores, hyphens.'
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

if ([string]::IsNullOrWhiteSpace($ArtifactDirectory)) {
    $ArtifactDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-suballocation-20261031'
}
elseif (-not [System.IO.Path]::IsPathRooted($ArtifactDirectory)) {
    $ArtifactDirectory = Join-Path $root $ArtifactDirectory
}

$directory = (Resolve-Path -LiteralPath $ArtifactDirectory).Path
$remote = '/data/local/tmp'
$binary = 'vulkan-suballocation-arm64'
$stages = @('a', 'b', 'c')
$optimizations = @('O0', 'O')
$names = @($binary)

foreach ($stage in $stages) {
    foreach ($optimization in $optimizations) {
        $names += "stage-$stage-$optimization.spv"
    }
}

foreach ($name in $names) {
    if (-not (Test-Path -LiteralPath (Join-Path $directory $name) -PathType Leaf)) {
        throw "Missing synthetic Vulkan suballocation build artifact: $name"
    }
}

$state = @(& adb -s $Serial get-state 2>&1)

if ($LASTEXITCODE -ne 0 -or ($state -join '').Trim() -ne 'device') {
    throw "Explicit Android device is not connected through ADB: $Serial"
}

$referenceChecksums = $null

try {
    foreach ($name in $names) {
        & adb -s $Serial push (Join-Path $directory $name) ($remote + '/' + $name) | Out-Null

        if ($LASTEXITCODE -ne 0) {
            throw "Cannot stage Vulkan test artifact on device: $name"
        }
    }

    & adb -s $Serial shell chmod 700 ($remote + '/' + $binary)

    if ($LASTEXITCODE -ne 0) {
        throw 'Cannot execute isolated Vulkan suballocation program.'
    }

    foreach ($optimization in $optimizations) {
        $arguments = @('-s', $Serial, 'shell', ($remote + '/' + $binary))

        foreach ($stage in $stages) {
            $arguments += ($remote + "/stage-$stage-$optimization.spv")
        }

        $lines = @(& adb @arguments 2>&1)
        $code = $LASTEXITCODE
        $output = ($lines | Out-String).Trim()
        $log = Join-Path $directory "$ReportPrefix-$optimization.log"

        @(
            'TEST=shared-allocation',
            "OPTIMIZATION=$optimization",
            "ADB_EXIT_CODE=$code",
            $output
        ) | Set-Content -LiteralPath $log -Encoding UTF8

        Write-Output "=== Vulkan shared allocation $optimization ==="
        Write-Output $output
        Write-Output "ADB_EXIT_CODE=$code"

        if ($code -ne 0) {
            throw "Shared-memory Vulkan GPU execution failed: $optimization, code=$code"
        }

        $layoutPattern = '(?m)^BUFFER_LAYOUT name=(input|stage-a|stage-b|output) bytes=(\d+) required=(\d+) alignment=(\d+) offset=(\d+) end=(\d+) memory_type_bits=0x([0-9a-f]{8})\r?$'
        $layouts = [regex]::Matches($output, $layoutPattern)
        $expectedNames = @('input', 'stage-a', 'stage-b', 'output')
        $expectedBytes = @(8192, 8192, 8192, 32768)

        if ($layouts.Count -ne 4) {
            throw 'Vulkan driver memory layout did not contain exactly four buffers.'
        }

        $lastEnd = [long]0

        for ($index = 0; $index -lt 4; $index++) {
            $record = $layouts[$index].Groups
            $name = $record[1].Value
            $bytes = [long]::Parse($record[2].Value)
            $required = [long]::Parse($record[3].Value)
            $alignment = [long]::Parse($record[4].Value)
            $offset = [long]::Parse($record[5].Value)
            $end = [long]::Parse($record[6].Value)

            if ($name -cne $expectedNames[$index] -or
                $bytes -ne $expectedBytes[$index] -or
                $required -lt $bytes -or
                $alignment -le 0 -or
                $offset -lt $lastEnd -or
                ($offset % $alignment) -ne 0 -or
                $end -ne ($offset + $required)) {
                throw "Vulkan storage memory requirements invalid at buffer index $index"
            }

            $lastEnd = $end
        }

        $allocated = [regex]::Match(
            $output,
            '(?m)^MEMORY_ALLOCATION allocations=1 bytes=(\d+) bound_buffers=4 memory_type=(\d+)\r?$'
        )

        if (-not $allocated.Success -or [long]$allocated.Groups[1].Value -ne $lastEnd) {
            throw 'Vulkan shared-allocation byte count does not match four aligned ranges.'
        }

        $result = [regex]::Match(
            $output,
            'PASS Vulkan 3-stage pipeline: 2048 elements, 12288 correct 32-bit words; stageA=([0-9a-f]{16}) stageB=([0-9a-f]{16}) output=([0-9a-f]{16})'
        )

        if (-not $result.Success) {
            throw 'Shared Vulkan allocation did not pass 12288 CPU/GPU stage comparisons.'
        }

        $checksums = @(
            $result.Groups[1].Value,
            $result.Groups[2].Value,
            $result.Groups[3].Value
        )

        if ($null -ne $referenceChecksums) {
            for ($index = 0; $index -lt 3; $index++) {
                if ($checksums[$index] -ne $referenceChecksums[$index]) {
                    throw "Shared Vulkan allocation differs between optimizations at stage $index"
                }
            }
        }
        else {
            $referenceChecksums = $checksums
        }

        if (-not $output.Contains(
            'CLEANUP_SHARED_MEMORY allocations_freed=1 buffers_bound=4 simulated_device_lost=0 final_status=0'
        )) {
            throw 'Vulkan shared allocation was not freed after the GPU completed.'
        }

        Write-Output "PASS ${optimization}: 4 distinct, aligned buffers in one $lastEnd-byte Vulkan allocation"
    }

    $faults = @(
        [pscustomobject]@{ Name = 'memory-type'; Code = 42; VkResult = -8; Allocation = 0; Bound = 0; Lost = 0 },
        [pscustomobject]@{ Name = 'allocation'; Code = 43; VkResult = -2; Allocation = 0; Bound = 0; Lost = 0 },
        [pscustomobject]@{ Name = 'device-lost'; Code = 44; VkResult = -4; Allocation = 1; Bound = 4; Lost = 1 }
    )

    $negativeRecords = @()

    foreach ($fault in $faults) {
        $arguments = @(
            '-s', $Serial, 'shell', ($remote + '/' + $binary),
            ($remote + '/stage-a-O.spv'),
            ($remote + '/stage-b-O.spv'),
            ($remote + '/stage-c-O.spv'),
            '--simulate', $fault.Name
        )
        $previous = $ErrorActionPreference

        try {
            $ErrorActionPreference = 'Continue'
            $lines = @(& adb @arguments 2>&1)
            $exitCode = $LASTEXITCODE
        }
        finally {
            $ErrorActionPreference = $previous
        }

        $output = ($lines | Out-String).Trim()
        $expectedFailure = "SIMULATED_VULKAN_FAILURE name=$($fault.Name) vkresult=$($fault.VkResult)"
        $expectedCleanup = "CLEANUP_SHARED_MEMORY allocations_freed=$($fault.Allocation) buffers_bound=$($fault.Bound) simulated_device_lost=$($fault.Lost) final_status=$($fault.Code)"

        if ($exitCode -ne $fault.Code -or
            -not $output.Contains($expectedFailure) -or
            -not $output.Contains($expectedCleanup)) {
            throw "Synthetic Vulkan error handling rejected: $($fault.Name), code=$exitCode; $output"
        }

        if ($fault.Name -eq 'device-lost' -and -not $output.Contains('real_fence_wait=0')) {
            throw 'Simulated device loss must occur after real successful GPU fence completion.'
        }

        $negativeRecords += "SIMULATED $($fault.Name) exit=$($fault.Code) vkresult=$($fault.VkResult) allocations_freed=$($fault.Allocation) buffers_bound=$($fault.Bound)"

        Write-Output "PASS simulated Vulkan $($fault.Name), code $exitCode, memory cleanup checked"
    }

    $invalid = @(
        [pscustomobject]@{ Name = 'invalid-simulation'; Stage = 'stage-a-O.spv'; Simulation = 'bad-mode'; Reason = 'Unknown simulated Vulkan failure' },
        [pscustomobject]@{ Name = 'missing-shader'; Stage = 'missing-vulkan-suballocation.spv'; Simulation = ''; Reason = 'shader fopen' },
        [pscustomobject]@{ Name = 'missing-simulate-flag'; Stage = 'stage-a-O.spv'; Simulation = 'bad-flag'; Reason = 'Usage:' }
    )

    foreach ($case in $invalid) {
        $args = @(
            '-s', $Serial, 'shell', ($remote + '/' + $binary),
            ($remote + '/' + $case.Stage),
            ($remote + '/stage-b-O.spv'),
            ($remote + '/stage-c-O.spv')
        )

        if ($case.Name -eq 'missing-simulate-flag') {
            $args += @('--bad-flag', 'allocation')
        }
        elseif ($case.Simulation -ne '') {
            $args += @('--simulate', $case.Simulation)
        }

        $previous = $ErrorActionPreference

        try {
            $ErrorActionPreference = 'Continue'
            $lines = @(& adb @args 2>&1)
            $exitCode = $LASTEXITCODE
        }
        finally {
            $ErrorActionPreference = $previous
        }

        $output = ($lines | Out-String).Trim()

        if ($exitCode -ne 2 -or -not $output.Contains($case.Reason)) {
            throw "Vulkan shared allocation invalid input was not rejected: $($case.Name); $output"
        }

        $negativeRecords += "INVALID $($case.Name) exit=2"

        Write-Output "PASS invalid Vulkan CLI $($case.Name): exit code 2"
    }

    if ($negativeRecords.Count -ne 6) {
        throw 'Expected three simulated memory/device errors and three invalid inputs.'
    }

    $log = Join-Path $directory "$ReportPrefix-negative-controls.log"

    @(
        'TEST=shared-allocation',
        'SIMULATED_ERRORS=3',
        'INVALID_INPUTS=3'
    ) + $negativeRecords | Set-Content -LiteralPath $log -Encoding UTF8

    Write-Output 'PASS 2 real GPU pipelines, 3 simulated Vulkan failures and 3 invalid input controls'
}
finally {
    foreach ($name in $names) {
        & adb -s $Serial shell rm -f ($remote + '/' + $name) 2>$null | Out-Null
    }
}
