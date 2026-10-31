<#
.SYNOPSIS
Runs standalone synthetic Vulkan Compute diagnostics on one Android device.

.DESCRIPTION
Stages the isolated ARM64 executable and four Vulkan 1.1 SPIR-V shaders
under /data/local/tmp, compares each GPU computation with the program's
independent CPU reference, and records exact stdout and return codes.
All five temporary device files are deleted in the finally block.
The script never touches any hashcat application or password input.

.PARAMETER Serial
Explicit ADB serial of the Android device to test.

.PARAMETER ArtifactDirectory
Directory containing the five outputs from build-vulkan-compute-diagnostic.ps1.

.PARAMETER ReportPrefix
Basename prefix for four local log files in ArtifactDirectory.
#>
param (
    [Parameter(Mandatory = $true)]
    [string]$Serial,

    [string]$ArtifactDirectory = '',

    [string]$ReportPrefix = 'vulkan'
)

$ErrorActionPreference = 'Stop'

if ($ReportPrefix -cnotmatch '^[A-Za-z0-9_-]+$') {
    throw 'ReportPrefix must contain only ASCII letters, digits, underscores, or hyphens.'
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

if ([string]::IsNullOrWhiteSpace($ArtifactDirectory)) {
    $ArtifactDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-compute-20261031'
}
elseif (-not [System.IO.Path]::IsPathRooted($ArtifactDirectory)) {
    $ArtifactDirectory = Join-Path $root $ArtifactDirectory
}

$directory = (Resolve-Path $ArtifactDirectory).Path
$remote = '/data/local/tmp'
$binary = 'vulkan-compute-diagnostic-arm64'

$cases = @(
    [pscustomobject]@{ Shader = 'vulkan-compute-O0.spv'; Mode = 'baseline'; Optimization = 'O0' },
    [pscustomobject]@{ Shader = 'vulkan-compute-O.spv'; Mode = 'baseline'; Optimization = 'O' },
    [pscustomobject]@{ Shader = 'vulkan-compute-shared32k-O0.spv'; Mode = 'shared32k'; Optimization = 'O0' },
    [pscustomobject]@{ Shader = 'vulkan-compute-shared32k-O.spv'; Mode = 'shared32k'; Optimization = 'O' }
)

$names = @($binary) + @($cases | ForEach-Object { $_.Shader })

foreach ($name in $names) {
    if (-not (Test-Path -LiteralPath (Join-Path $directory $name) -PathType Leaf)) {
        throw "Missing synthetic Vulkan diagnostic artifact: $name"
    }
}

$deviceStatus = @(& adb -s $Serial get-state 2>&1)
$deviceExit = $LASTEXITCODE

if ($deviceExit -ne 0 -or ($deviceStatus -join '').Trim() -ne 'device') {
    throw "Android device not available through ADB: $Serial"
}

$checksums = @{}

try {
    foreach ($name in $names) {
        & adb -s $Serial push (Join-Path $directory $name) ($remote + '/' + $name) | Out-Null

        if ($LASTEXITCODE -ne 0) {
            throw "Failed to stage synthetic Vulkan artifact: $name"
        }
    }

    & adb -s $Serial shell chmod 700 ($remote + '/' + $binary)

    if ($LASTEXITCODE -ne 0) {
        throw 'Cannot execute staged synthetic Vulkan diagnostic.'
    }

    foreach ($case in $cases) {
        $command = @(
            '-s', $Serial, 'shell',
            ($remote + '/' + $binary),
            ($remote + '/' + $case.Shader)
        )

        if ($case.Mode -eq 'shared32k') {
            $command += '--shared-32k'
        }

        $lines = @(& adb @command 2>&1)
        $exitCode = $LASTEXITCODE
        $output = ($lines | Out-String).Trim()
        $label = "$($case.Mode)-$($case.Optimization)"
        $log = Join-Path $directory "$ReportPrefix-$label.log"

        @(
            "MODE=$($case.Mode)",
            "OPTIMIZATION=$($case.Optimization)",
            "ADB_EXIT_CODE=$exitCode",
            $output
        ) | Set-Content -LiteralPath $log -Encoding UTF8

        Write-Output "=== $label ==="
        Write-Output $output
        Write-Output "ADB_EXIT_CODE=$exitCode"

        if ($exitCode -ne 0) {
            throw "Vulkan diagnostic failed: $label, exit code $exitCode"
        }

        $match = [regex]::Match(
            $output,
            'PASS Vulkan Compute mode=(baseline|shared32k): 2048 elements, 8192 correct 32-bit words; FNV1A64=([0-9a-f]{16})'
        )

        if (-not $match.Success -or $match.Groups[1].Value -ne $case.Mode) {
            throw "Vulkan diagnostic did not confirm the expected CPU match: $label"
        }

        $checksum = $match.Groups[2].Value

        if ($checksums.ContainsKey($case.Mode)) {
            if ($checksums[$case.Mode] -ne $checksum) {
                throw "Vulkan optimization variants disagree for mode $($case.Mode)"
            }
        }
        else {
            $checksums[$case.Mode] = $checksum
        }

        Write-Output "PASS ${label}: FNV1A64=$checksum"
    }

    if ($checksums.Count -ne 2) {
        throw 'Expected both baseline and shared32k shader modes.'
    }

    Write-Output 'PASS all four Vulkan Compute tests, CPU comparisons and optimization parity'
}
finally {
    foreach ($name in $names) {
        & adb -s $Serial shell rm -f ($remote + '/' + $name) 2>$null | Out-Null
    }
}
