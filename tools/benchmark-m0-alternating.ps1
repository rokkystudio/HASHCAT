param(
    [Parameter(Mandatory = $true)]
    [string]$BaseHashcat,
    [Parameter(Mandatory = $true)]
    [string]$CurrentHashcat,
    [ValidateRange(1, 20)]
    [int]$Pairs = 6,
    [ValidateRange(0, 99999)]
    [int]$Mode = 0,
    [ValidateRange(1, 4)]
    [int]$WorkloadProfile = 2,
    [string]$OutputPath = ''
)

# Alternating base/current benchmark.
#
# A single sample cannot separate a real change from autotune and clock drift
# (the recorded OpenCL baseline itself shows up to 17 % spread on mode 22000).
# This runner therefore measures the two binaries in alternating order and
# flips the order every pair:
#
#   pair 1: base, current
#   pair 2: current, base
#   ...
#
# The report prints every sample, the per-pair ratio and the median ratio.

$ErrorActionPreference = 'Continue'
[System.Threading.Thread]::CurrentThread.CurrentCulture = [System.Globalization.CultureInfo]::InvariantCulture

function Resolve-Hashcat([string]$Path)
{
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "hashcat executable not found: $Path"
    }

    return (Resolve-Path -LiteralPath $Path).Path
}

function Invoke-Speed([string]$Executable, [int]$Mode, [int]$WorkloadProfile)
{
    $directory = Split-Path -Parent $Executable

    Push-Location -LiteralPath $directory

    try
    {
        $arguments = @('-b', '-m', "$Mode", '-w', "$WorkloadProfile", '--backend-ignore-cuda', '--backend-ignore-hip')

        $lines = (& $Executable @arguments 2>&1) | ForEach-Object { "$_" }
        $exitCode = $LASTEXITCODE
    }
    finally
    {
        Pop-Location
    }

    $speed = $null

    foreach ($line in $lines)
    {
        if ($line -match '^Speed\.#01\.{2,}:\s+([0-9]+(?:\.[0-9]+)?)\s*([kMGTP]?H)/s')
        {
            $multiplier = switch ($matches[2])
            {
                'H'  { 0.001 }
                'kH' { 1.0 }
                'MH' { 1000.0 }
                'GH' { 1000000.0 }
                default { 1.0 }
            }

            $speed = [double]$matches[1] * $multiplier
        }
    }

    return [pscustomobject]@{
        ExitCode = $exitCode
        SpeedKhs = $speed
    }
}

function Get-Median([double[]]$Values)
{
    $sorted = @($Values | Sort-Object)
    $count = $sorted.Count

    if ($count -eq 0) { return 0.0 }
    if (($count % 2) -eq 1) { return [double]$sorted[[int](($count - 1) / 2)] }

    return ([double]$sorted[$count / 2 - 1] + [double]$sorted[$count / 2]) / 2.0
}

$basePath = Resolve-Hashcat $BaseHashcat
$currentPath = Resolve-Hashcat $CurrentHashcat

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path (Split-Path -Parent $PSScriptRoot) "build\m0-alternating-$((Get-Date).ToString('yyyyMMdd-HHmmss')).json"
}

$samples = [System.Collections.Generic.List[object]]::new()

for ($pair = 1; $pair -le $Pairs; $pair++)
{
    $order = if (($pair % 2) -eq 1) { @('base', 'current') } else { @('current', 'base') }

    foreach ($kind in $order)
    {
        $executable = if ($kind -eq 'base') { $basePath } else { $currentPath }

        Write-Output "pair $pair/$Pairs $kind $executable"

        $result = Invoke-Speed $executable $Mode $WorkloadProfile

        if ($null -eq $result.SpeedKhs)
        {
            throw "no Speed.#01 line from $kind (exit $($result.ExitCode))"
        }

        $samples.Add([pscustomobject][ordered]@{
            pair = $pair
            kind = $kind
            speed_khs = $result.SpeedKhs
        })
    }
}

$baseSamples = @($samples | Where-Object { $_.kind -eq 'base' } | ForEach-Object { $_.speed_khs })
$currentSamples = @($samples | Where-Object { $_.kind -eq 'current' } | ForEach-Object { $_.speed_khs })

$pairRatios = @()

for ($pair = 1; $pair -le $Pairs; $pair++)
{
    $base = ($samples | Where-Object { $_.pair -eq $pair -and $_.kind -eq 'base' }).speed_khs
    $current = ($samples | Where-Object { $_.pair -eq $pair -and $_.kind -eq 'current' }).speed_khs
    $ratio = $current / $base

    $pairRatios += [pscustomobject][ordered]@{
        pair = $pair
        base_khs = $base
        current_khs = $current
        current_vs_base_percent = ($ratio - 1.0) * 100.0
    }
}

$baseMedian = Get-Median $baseSamples
$currentMedian = Get-Median $currentSamples
$medianDeltaPercent = (($currentMedian / $baseMedian) - 1.0) * 100.0

$report = [pscustomobject][ordered]@{
    mode = $Mode
    workload_profile = $WorkloadProfile
    pairs = $Pairs
    base_executable = $basePath
    current_executable = $currentPath
    base_median_khs = $baseMedian
    current_median_khs = $currentMedian
    median_delta_percent = $medianDeltaPercent
    samples = $samples
    pair_ratios = $pairRatios
}

$report | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $OutputPath -Encoding utf8

Write-Output ''
Write-Output ("base median    : {0:N1} kH/s" -f $baseMedian)
Write-Output ("current median : {0:N1} kH/s" -f $currentMedian)
Write-Output ("median delta   : {0:+0.0;-0.0} %" -f $medianDeltaPercent)
Write-Output ''

foreach ($row in $pairRatios)
{
    Write-Output ("pair {0}: {1,12:N1} / {2,12:N1} kH/s = {3:+0.0;-0.0} %" -f $row.pair, $row.base_khs, $row.current_khs, $row.current_vs_base_percent)
}

Write-Output ''
Write-Output "report: $OutputPath"
