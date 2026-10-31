<#
.SYNOPSIS
Runs a standalone three-dispatch Vulkan Compute test on one Android device.

.DESCRIPTION
Stages one synthetic ARM64 executable and six Vulkan 1.1 SPIR-V shaders under
/data/local/tmp, executes both optimization variants, and records GPU device
metadata, return codes, intermediate stage checksums and output checksums.
The program itself checks all 12288 words against an independent CPU formula.
The script verifies optimizer parity and deletes temporary files in finally.
It never invokes hashcat, parses credentials, or changes app assets.

.PARAMETER Serial
Explicit ADB serial of the Android GPU device to test.

.PARAMETER ArtifactDirectory
Build directory containing the seven artifacts from the build script.

.PARAMETER ReportPrefix
Short filename prefix for the two locally archived device logs.
#>
param (
    [Parameter(Mandatory = $true)]
    [string]$Serial,

    [string]$ArtifactDirectory = '',

    [string]$ReportPrefix = 'vulkan-pipeline'
)

$ErrorActionPreference = 'Stop'

if ($ReportPrefix -cnotmatch '^[A-Za-z0-9_-]+$') {
    throw 'ReportPrefix must contain only ASCII letters, digits, underscores or hyphens.'
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

if ([string]::IsNullOrWhiteSpace($ArtifactDirectory)) {
    $ArtifactDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-pipeline-20261031'
}
elseif (-not [System.IO.Path]::IsPathRooted($ArtifactDirectory)) {
    $ArtifactDirectory = Join-Path $root $ArtifactDirectory
}

$directory = (Resolve-Path $ArtifactDirectory).Path
$remote = '/data/local/tmp'
$binary = 'vulkan-multistage-arm64'
$stages = @('a', 'b', 'c')
$optimizations = @('O0', 'O')
$names = @($binary)

foreach ($stage in $stages) {
    foreach ($optimization in $optimizations) {
        $names += "stage-$stage-$optimization.spv"
    }
}

foreach ($name in $names) {
    $artifact = Join-Path $directory $name

    if (-not (Test-Path -LiteralPath $artifact -PathType Leaf)) {
        throw "Missing three-stage Vulkan artifact: $artifact"
    }
}

$deviceState = @(& adb -s $Serial get-state 2>&1)
$deviceExit = $LASTEXITCODE

if ($deviceExit -ne 0 -or ($deviceState -join '').Trim() -ne 'device') {
    throw "Explicit Android device is not available: $Serial"
}

$referenceChecksums = $null

try {
    foreach ($name in $names) {
        & adb -s $Serial push (Join-Path $directory $name) ($remote + '/' + $name) | Out-Null

        if ($LASTEXITCODE -ne 0) {
            throw "Failed to stage three-stage Vulkan artifact: $name"
        }
    }

    & adb -s $Serial shell chmod 700 ($remote + '/' + $binary)

    if ($LASTEXITCODE -ne 0) {
        throw 'Cannot execute standalone Vulkan ARM64 program.'
    }

    foreach ($optimization in $optimizations) {
        $args = @(
            '-s', $Serial, 'shell',
            ($remote + '/' + $binary)
        )

        foreach ($stage in $stages) {
            $args += ($remote + "/stage-$stage-$optimization.spv")
        }

        $lines = @(& adb @args 2>&1)
        $exitCode = $LASTEXITCODE
        $output = ($lines | Out-String).Trim()
        $log = Join-Path $directory "$ReportPrefix-$optimization.log"

        @(
            'PIPELINE=three-stage',
            "OPTIMIZATION=$optimization",
            "ADB_EXIT_CODE=$exitCode",
            $output
        ) | Set-Content -LiteralPath $log -Encoding UTF8

        Write-Output "=== three-stage Vulkan $optimization ==="
        Write-Output $output
        Write-Output "ADB_EXIT_CODE=$exitCode"

        if ($exitCode -ne 0) {
            throw "Three-stage Vulkan execution failed at $optimization, exit=$exitCode"
        }

        $result = [regex]::Match(
            $output,
            'PASS Vulkan 3-stage pipeline: 2048 elements, 12288 correct 32-bit words; stageA=([0-9a-f]{16}) stageB=([0-9a-f]{16}) output=([0-9a-f]{16})'
        )

        if (-not $result.Success) {
            throw "Missing Vulkan GPU/CPU stage comparison: $optimization"
        }

        $checksums = @(
            $result.Groups[1].Value,
            $result.Groups[2].Value,
            $result.Groups[3].Value
        )

        if ($null -ne $referenceChecksums) {
            for ($index = 0; $index -lt 3; $index++) {
                if ($checksums[$index] -ne $referenceChecksums[$index]) {
                    throw "Optimized Vulkan pipeline differs at stage index $index"
                }
            }
        }
        else {
            $referenceChecksums = $checksums
        }

        Write-Output "PASS $optimization stageA=$($checksums[0]) stageB=$($checksums[1]) output=$($checksums[2])"
    }

    if ($null -eq $referenceChecksums) {
        throw 'Three-stage GPU checksum comparison was never executed.'
    }

    Write-Output 'PASS both Vulkan optimization variants and all three intermediate/output comparisons'
}
finally {
    foreach ($name in $names) {
        & adb -s $Serial shell rm -f ($remote + '/' + $name) 2>$null | Out-Null
    }
}
