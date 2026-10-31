<#
.SYNOPSIS
Exercises a reusable three-stage Vulkan Compute command buffer on Android.

.DESCRIPTION
Stages the isolated ARM64 executable and six public SPIR-V shaders, executes
both shader optimization variants with submission-specific synthetic data,
checks every reported intermediate and final CPU/GPU comparison, and requires
identical optimizer results for each submission. Invalid CLI inputs and a
missing SPIR-V file must fail without dispatching any GPU workload.
The seven staged files are deleted from /data/local/tmp in the finally block.
Two GPU execution logs and a strict four-case error log are recorded locally.
No hashcat process, application assets or cryptographic GPU shaders are used.

.PARAMETER Serial
Explicit ADB serial of the connected Android device.

.PARAMETER ArtifactDirectory
Directory containing the seven artifacts of the replay build script.

.PARAMETER ReportPrefix
Filename prefix used for local replay log files; no device serial is saved.

.PARAMETER Submissions
Number of sequential re-submissions of the same recorded command buffer.
Accepted values are integers from 2 through 16.
#>
param (
    [Parameter(Mandatory = $true)]
    [string]$Serial,

    [string]$ArtifactDirectory = '',

    [string]$ReportPrefix = 'vulkan-replay',

    [ValidateRange(2, 16)]
    [int]$Submissions = 4
)

$ErrorActionPreference = 'Stop'

if ($ReportPrefix -cnotmatch '^[A-Za-z0-9_-]+$') {
    throw 'ReportPrefix must contain only ASCII letters, digits, underscores or hyphens.'
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

if ([string]::IsNullOrWhiteSpace($ArtifactDirectory)) {
    $ArtifactDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-resubmit-20261031'
}
elseif (-not [System.IO.Path]::IsPathRooted($ArtifactDirectory)) {
    $ArtifactDirectory = Join-Path $root $ArtifactDirectory
}

$directory = (Resolve-Path $ArtifactDirectory).Path
$remote = '/data/local/tmp'
$binary = 'vulkan-resubmit-arm64'
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
        throw "Required Vulkan command-reuse artifact is missing: $name"
    }
}

$state = @(& adb -s $Serial get-state 2>&1)

if ($LASTEXITCODE -ne 0 -or ($state -join '').Trim() -ne 'device') {
    throw "Explicit Android device is not available: $Serial"
}

$firstOptimizationChecksums = $null
$failureEvidence = @()

try {
    foreach ($name in $names) {
        & adb -s $Serial push (Join-Path $directory $name) ($remote + '/' + $name) | Out-Null

        if ($LASTEXITCODE -ne 0) {
            throw "Cannot stage Vulkan replay artifact: $name"
        }
    }

    & adb -s $Serial shell chmod 700 ($remote + '/' + $binary)

    if ($LASTEXITCODE -ne 0) {
        throw 'Cannot execute standalone replay diagnostic.'
    }

    foreach ($optimization in $optimizations) {
        $args = @('-s', $Serial, 'shell', ($remote + '/' + $binary))

        foreach ($stage in $stages) {
            $args += ($remote + "/stage-$stage-$optimization.spv")
        }

        $args += @('--submissions', "$Submissions")
        $lines = @(& adb @args 2>&1)
        $exitCode = $LASTEXITCODE
        $output = ($lines | Out-String).Trim()
        $log = Join-Path $directory "$ReportPrefix-$optimization.log"

        @(
            'PIPELINE=three-stage-command-reuse',
            "OPTIMIZATION=$optimization",
            "SUBMISSIONS=$Submissions",
            "ADB_EXIT_CODE=$exitCode",
            $output
        ) | Set-Content -LiteralPath $log -Encoding UTF8

        Write-Output "=== Vulkan replay $optimization ==="
        Write-Output $output
        Write-Output "ADB_EXIT_CODE=$exitCode"

        if ($exitCode -ne 0) {
            throw "Vulkan command reuse failed: $optimization, code=$exitCode"
        }

        $pattern = '(?m)^PASS Vulkan replay submission=(\d+): 2048 elements, 12288 correct 32-bit words; stageA=([0-9a-f]{16}) stageB=([0-9a-f]{16}) output=([0-9a-f]{16})\r?$'
        $matches = [regex]::Matches($output, $pattern)

        if ($matches.Count -ne $Submissions) {
            throw "Expected $Submissions verified replay checksums, got $($matches.Count)"
        }

        $checksums = @()
        $outputs = @{}

        for ($index = 0; $index -lt $Submissions; $index++) {
            $match = $matches[$index]

            if ([int]$match.Groups[1].Value -ne $index) {
                throw "Missing or out-of-order Vulkan GPU submission $index"
            }

            $triplet = @(
                $match.Groups[2].Value,
                $match.Groups[3].Value,
                $match.Groups[4].Value
            )
            $checksums += ,$triplet
            $finalChecksum = $triplet[2]

            if ($outputs.ContainsKey($finalChecksum)) {
                throw "Distinct replay inputs produced a duplicate output checksum"
            }

            $outputs[$finalChecksum] = $true

            Write-Output "PASS $optimization submission=$index stageA=$($triplet[0]) stageB=$($triplet[1]) output=$finalChecksum"
        }

        $summary = "PASS Vulkan command reuse: $Submissions sequential submissions, 3 dispatches/submission, 12288 verified words/submission"

        if (-not $output.Contains($summary)) {
            throw 'Device did not confirm recording and reusing one command buffer.'
        }

        if ($null -ne $firstOptimizationChecksums) {
            for ($index = 0; $index -lt $Submissions; $index++) {
                for ($stage = 0; $stage -lt 3; $stage++) {
                    if ($checksums[$index][$stage] -ne $firstOptimizationChecksums[$index][$stage]) {
                        throw "Vulkan optimizer mismatch: submission=$index stage=$stage"
                    }
                }
            }
        }
        else {
            $firstOptimizationChecksums = $checksums
        }
    }

    $invalid = @(
        [pscustomobject]@{ Label = 'zero-submissions'; Value = '0' },
        [pscustomobject]@{ Label = 'too-many-submissions'; Value = '17' },
        [pscustomobject]@{ Label = 'non-numeric-submissions'; Value = 'invalid' }
    )

    foreach ($case in $invalid) {
        $arguments = @(
            '-s', $Serial, 'shell', ($remote + '/' + $binary),
            ($remote + '/stage-a-O.spv'),
            ($remote + '/stage-b-O.spv'),
            ($remote + '/stage-c-O.spv'),
            '--submissions', $case.Value
        )

        $savedErrorAction = $ErrorActionPreference

        try {
            $ErrorActionPreference = 'Continue'
            $negativeOutput = @(& adb @arguments 2>&1)
            $negativeCode = $LASTEXITCODE
        }
        finally {
            $ErrorActionPreference = $savedErrorAction
        }

        if ($negativeCode -ne 2 -or ($negativeOutput -join ' ') -notmatch 'Invalid submissions count') {
            throw "Invalid replay argument was not rejected: $($case.Label), code=$negativeCode"
        }

        $failureEvidence += "NEGATIVE $($case.Label) exit=2"

        Write-Output "PASS expected rejection: $($case.Label), exit code 2"
    }

    $missingArgs = @(
        '-s', $Serial, 'shell', ($remote + '/' + $binary),
        ($remote + '/missing-replay-stage.spv'),
        ($remote + '/stage-b-O.spv'),
        ($remote + '/stage-c-O.spv'),
        '--submissions', '4'
    )
    $savedErrorAction = $ErrorActionPreference

    try {
        $ErrorActionPreference = 'Continue'
        $missingOutput = @(& adb @missingArgs 2>&1)
        $missingCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $savedErrorAction
    }

    if ($missingCode -ne 2 -or ($missingOutput -join ' ') -notmatch 'shader fopen') {
        throw "Missing SPIR-V stage not rejected: code=$missingCode"
    }

    $failureEvidence += 'NEGATIVE missing-spirv-stage exit=2'

    if ($failureEvidence.Count -ne 4) {
        throw 'Required four Vulkan input rejection controls were not recorded.'
    }

    $failureLog = Join-Path $directory "$ReportPrefix-negative-controls.log"

    @(
        'PIPELINE=three-stage-command-reuse',
        'ERROR_CONTROLS=4'
    ) + $failureEvidence | Set-Content -LiteralPath $failureLog -Encoding UTF8

    Write-Output 'PASS expected rejection: missing SPIR-V stage, exit code 2'
    Write-Output "PASS $($Submissions * 2) actual GPU submissions, per-stage CPU comparisons, fence reuse and four negative controls"
}
finally {
    foreach ($name in $names) {
        & adb -s $Serial shell rm -f ($remote + '/' + $name) 2>$null | Out-Null
    }
}
