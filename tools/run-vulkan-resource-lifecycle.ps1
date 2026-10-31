<#
.SYNOPSIS
Tests Vulkan resource lifetime and deterministic failure cleanup on Android.

.DESCRIPTION
Stages one standalone ARM64 executable and two public synthetic SPIR-V
modules, executes regular resource creation/destruction cycles on each
shader optimization, and injects a failure at each of 14 real Vulkan resource
acquisition steps. Each failure must return 42 after exactly balanced cleanup.
Five invalid-input cases must return 2 before creating Vulkan objects.
Logs are stored locally and all staged device files are removed in finally.
No GPU work is submitted, and hashcat files are never accessed.

.PARAMETER Serial
Explicit ADB serial of the Android Vulkan device to test.

.PARAMETER ArtifactDirectory
Directory containing the three output files from the build script.

.PARAMETER ReportPrefix
ASCII filename prefix for device-specific logs without an ADB serial.

.PARAMETER Cycles
Number of complete Vulkan object graph create/destroy passes (1-16).
#>
param (
    [Parameter(Mandatory = $true)]
    [string]$Serial,

    [string]$ArtifactDirectory = '',

    [string]$ReportPrefix = 'vulkan-lifecycle',

    [ValidateRange(1, 16)]
    [int]$Cycles = 4
)

$ErrorActionPreference = 'Stop'

if ($ReportPrefix -cnotmatch '^[A-Za-z0-9_-]+$') {
    throw 'ReportPrefix must contain only ASCII letters, digits, underscores or hyphens.'
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

if ([string]::IsNullOrWhiteSpace($ArtifactDirectory)) {
    $ArtifactDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-lifecycle-20261031'
}
elseif (-not [System.IO.Path]::IsPathRooted($ArtifactDirectory)) {
    $ArtifactDirectory = Join-Path $root $ArtifactDirectory
}

$directory = (Resolve-Path -LiteralPath $ArtifactDirectory).Path
$remote = '/data/local/tmp'
$binary = 'vulkan-resource-lifecycle-arm64'
$optimizations = @('O0', 'O')
$names = @($binary, 'lifecycle-O0.spv', 'lifecycle-O.spv')
$steps = @(
    'instance',
    'device',
    'buffer',
    'device-memory',
    'memory-map',
    'descriptor-layout',
    'pipeline-layout',
    'shader-module',
    'compute-pipeline',
    'descriptor-pool',
    'descriptor-set',
    'command-pool',
    'command-buffer',
    'fence'
)

foreach ($name in $names) {
    if (-not (Test-Path -LiteralPath (Join-Path $directory $name) -PathType Leaf)) {
        throw "Vulkan lifecycle diagnostic artifact is missing: $name"
    }
}

$deviceState = @(& adb -s $Serial get-state 2>&1)

if ($LASTEXITCODE -ne 0 -or ($deviceState -join '').Trim() -ne 'device') {
    throw "Explicit Android device is not available: $Serial"
}

try {
    foreach ($name in $names) {
        & adb -s $Serial push (Join-Path $directory $name) ($remote + '/' + $name) | Out-Null

        if ($LASTEXITCODE -ne 0) {
            throw "Failed to stage Vulkan lifecycle artifact: $name"
        }
    }

    & adb -s $Serial shell chmod 700 ($remote + '/' + $binary)

    if ($LASTEXITCODE -ne 0) {
        throw 'Cannot execute standalone Vulkan resource lifecycle diagnostic.'
    }

    foreach ($optimization in $optimizations) {
        $arguments = @(
            '-s', $Serial, 'shell',
            ($remote + '/' + $binary),
            ($remote + "/lifecycle-$optimization.spv"),
            '--cycles', "$Cycles"
        )

        $lines = @(& adb @arguments 2>&1)
        $exitCode = $LASTEXITCODE
        $output = ($lines | Out-String).Trim()
        $log = Join-Path $directory "$ReportPrefix-$optimization.log"

        @(
            'PIPELINE=resource-lifecycle',
            "OPTIMIZATION=$optimization",
            "CYCLES=$Cycles",
            "ADB_EXIT_CODE=$exitCode",
            $output
        ) | Set-Content -LiteralPath $log -Encoding UTF8

        Write-Output "=== Vulkan resource lifecycle $optimization ==="
        Write-Output $output
        Write-Output "ADB_EXIT_CODE=$exitCode"

        if ($exitCode -ne 0) {
            throw "Vulkan lifecycle regular run failed: $optimization, exit=$exitCode"
        }

        $pattern = '(?m)^PASS Vulkan resource cycle=(\d+) created=14 released=14 shader_dispatched=0\r?$'
        $matches = [regex]::Matches($output, $pattern)

        if ($matches.Count -ne $Cycles) {
            throw "Vulkan lifecycle did not report $Cycles balanced cycles: $optimization"
        }

        for ($index = 0; $index -lt $Cycles; $index++) {
            if ([int]$matches[$index].Groups[1].Value -ne $index) {
                throw "Vulkan resource cycle is missing or out of order: $index"
            }
        }

        $summary = "PASS Vulkan resource lifecycle: $Cycles cycles, 14 resources/cycle, all resources released; GPU submissions=0"

        if (-not $output.Contains($summary)) {
            throw 'Vulkan device did not report balanced resource lifetime.'
        }

        Write-Output "PASS $optimization $Cycles cycles; 14 resources created and released per cycle"
    }

    $faultEvidence = @()

    foreach ($step in $steps) {
        $index = [array]::IndexOf($steps, $step)
        $count = $index + 1
        $arguments = @(
            '-s', $Serial, 'shell',
            ($remote + '/' + $binary),
            ($remote + '/lifecycle-O.spv'),
            '--cycles', "$Cycles",
            '--fail-after', $step
        )

        # PowerShell 5.1 emits native stderr as a nonterminating ErrorRecord.
        # These executions intentionally write an error and return exit 42.
        $savedPreference = $ErrorActionPreference

        try {
            $ErrorActionPreference = 'Continue'
            $lines = @(& adb @arguments 2>&1)
            $exitCode = $LASTEXITCODE
        }
        finally {
            $ErrorActionPreference = $savedPreference
        }

        $output = ($lines | Out-String).Trim()
        $injected = "INJECTED_FAILURE stage=$step code=42"
        $cleanup = "CLEANUP_OK cycle=0 created=$count released=$count fail_after=$step shader_dispatched=0"

        if ($exitCode -ne 42 -or -not $output.Contains($injected) -or -not $output.Contains($cleanup)) {
            throw "Injected Vulkan resource failure was not fully cleaned: $step, exit=$exitCode; $output"
        }

        $faultEvidence += "NEGATIVE $step exit=42 created=$count released=$count"

        Write-Output "PASS Vulkan fault ${step}: $count acquired, $count released, exit=42"
    }

    $invalid = @(
        [pscustomobject]@{ Label = 'zero-cycles'; Shader = 'lifecycle-O.spv'; Cycles = '0'; Step = ''; Reason = 'Invalid cycles count' },
        [pscustomobject]@{ Label = 'too-many-cycles'; Shader = 'lifecycle-O.spv'; Cycles = '17'; Step = ''; Reason = 'Invalid cycles count' },
        [pscustomobject]@{ Label = 'noninteger-cycles'; Shader = 'lifecycle-O.spv'; Cycles = 'invalid'; Step = ''; Reason = 'Invalid cycles count' },
        [pscustomobject]@{ Label = 'unknown-failure-step'; Shader = 'lifecycle-O.spv'; Cycles = '1'; Step = 'not-a-vulkan-step'; Reason = 'Unknown Vulkan failure injection step' },
        [pscustomobject]@{ Label = 'missing-shader'; Shader = 'missing-lifecycle-shader.spv'; Cycles = '1'; Step = ''; Reason = 'shader fopen' }
    )

    foreach ($case in $invalid) {
        $arguments = @(
            '-s', $Serial, 'shell',
            ($remote + '/' + $binary),
            ($remote + '/' + $case.Shader),
            '--cycles', $case.Cycles
        )

        if (-not [string]::IsNullOrEmpty($case.Step)) {
            $arguments += @('--fail-after', $case.Step)
        }

        $savedPreference = $ErrorActionPreference

        try {
            $ErrorActionPreference = 'Continue'
            $lines = @(& adb @arguments 2>&1)
            $exitCode = $LASTEXITCODE
        }
        finally {
            $ErrorActionPreference = $savedPreference
        }

        $output = ($lines | Out-String).Trim()

        if ($exitCode -ne 2 -or -not $output.Contains($case.Reason)) {
            throw "Vulkan lifecycle invalid input accepted: $($case.Label), exit=$exitCode; $output"
        }

        $faultEvidence += "INVALID $($case.Label) exit=2"

        Write-Output "PASS rejected $($case.Label), exit=2"
    }

    if ($faultEvidence.Count -ne 19) {
        throw "Expected 14 Vulkan resource fault checks and 5 CLI input checks."
    }

    $log = Join-Path $directory "$ReportPrefix-negative-controls.log"

    @(
        'PIPELINE=resource-lifecycle',
        'RESOURCE_FAULT_CONTROLS=14',
        'INVALID_INPUT_CONTROLS=5'
    ) + $faultEvidence | Set-Content -LiteralPath $log -Encoding UTF8

    Write-Output "PASS Vulkan lifecycle: $Cycles complete object graphs per optimization, 14 cleanup fault points and 5 CLI input failures; GPU submissions=0"
}
finally {
    foreach ($name in $names) {
        & adb -s $Serial shell rm -f ($remote + '/' + $name) 2>$null | Out-Null
    }
}
