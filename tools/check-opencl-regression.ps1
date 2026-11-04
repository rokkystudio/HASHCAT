<#
.SYNOPSIS
Compares a hashcat executable against the recorded OpenCL baseline.

.DESCRIPTION
This is the regression gate for the native part of HASHCAT. A green gate means
the backend device list and the benchmark speeds still match the recorded
baseline, so any later change to the native tree can be checked with a single
command.

The baseline is the machine-readable JSON recorded in docs/ next to the
human-readable report. Passing the report path resolves the JSON sibling
automatically. Device composition is compared on identifier, name and type.
Benchmark speeds are compared per device with two thresholds: a drop above
-WarnPercent is reported as a warning, a drop above -FailPercent fails.

Exit codes
    0  device composition and benchmark speeds match the baseline
    1  the backend device list (identifier, name or type) differs
    2  a benchmark speed dropped beyond -FailPercent, or a benchmark did not
       complete and therefore cannot be compared
    3  usage or input error, e.g. a missing executable or an unreadable
       baseline; never used for a real regression

.EXAMPLE
powershell -File tools/check-opencl-regression.ps1 `
    -HashcatExecutable build\windows-x64\hashcat.exe `
    -Baseline docs/opencl-baseline.json

.EXAMPLE
powershell -File tools/check-opencl-regression.ps1 `
    -HashcatExecutable build\windows-x64\hashcat.exe `
    -Baseline docs/opencl-baseline-2026-11-04.md `
    -Device 1 -WarnPercent 10 -FailPercent 30
#>
param(
  [Parameter(Mandatory = $true)]
  [string]$HashcatExecutable,

  [Parameter(Mandatory = $true)]
  [string]$Baseline,

  [ValidateRange(0, 255)]
  [int]$Device = 0,

  [ValidateSet('pc', 'android')]
  [string]$HostName = 'pc',

  [ValidateRange(1, 99)]
  [int]$WarnPercent = 10,

  [ValidateRange(1, 100)]
  [int]$FailPercent = 30,

  [switch]$SkipBenchmark
)

$ErrorActionPreference = 'Stop'

$utf8NoBom = [System.Text.UTF8Encoding]::new($false)

[Console]::OutputEncoding = $utf8NoBom
$OutputEncoding = $utf8NoBom

# hashcat prints speeds as "14042.2 MH/s"; parsing and formatting must not
# depend on the machine locale (a comma-decimal locale would misread the dot).
[System.Threading.Thread]::CurrentThread.CurrentCulture = [System.Globalization.CultureInfo]::InvariantCulture

# The baseline is the OpenCL view: on a PC with both CUDA and OpenCL the same
# physical GPU appears twice and "hashcat -b" would benchmark the CUDA copy.
$gateArguments = @('--backend-ignore-cuda', '--backend-ignore-hip')

function Write-Failure([string]$Message)
{
  Write-Host "ERROR: $Message" -ForegroundColor Red

  exit 3
}

# Normalizes the human readable -I type string to the four contract values.
function Convert-DeviceType([string]$Type, [string]$Backend)
{
  if ([string]::IsNullOrWhiteSpace($Type))
  {
    # CUDA, HIP and Metal report no type; every device they expose is a GPU.
    if ($Backend -eq 'cuda' -or $Backend -eq 'hip' -or $Backend -eq 'metal') { return 'GPU' }

    return 'ACCEL'
  }

  switch -Regex ($Type.Trim())
  {
    '^GPU$'                            { return 'GPU' }
    '^CPU$'                            { return 'CPU' }
    '^FPGA$'                           { return 'FPGA' }
    '^ACCEL$|^Accelerator$'            { return 'ACCEL' }
    default                            { return 'ACCEL' }
  }
}

# Parses the "hashcat -I" device sections into id, name, type and vendor id.
function Get-BackendDevices([string[]]$Lines)
{
  $devices = @()

  $backend = 'unknown'
  $current = $null

  foreach ($line in $Lines)
  {
    if ($line -match '^(CUDA|HIP|Metal|OpenCL|Vulkan) Info:')
    {
      $backend = $matches[1].ToLowerInvariant()

      $current = $null

      continue
    }

    if ($line -match '^\s*Backend Device ID #(\d+)')
    {
      $current = [pscustomobject][ordered]@{
        id        = [int]$matches[1]
        name      = ''
        type      = ''
        vendor_id = 0
        backend   = $backend
      }

      $devices += $current

      continue
    }

    if ($null -eq $current) { continue }

    if ($line -match '^\s+Type\.{2,}:\s*(.+?)\s*$')
    {
      $current.type = $matches[1]
    }
    elseif ($line -match '^\s+Vendor\.ID\.{2,}:\s*(\d+)')
    {
      $current.vendor_id = [long]$matches[1]
    }
    elseif ($line -match '^\s+Name\.{2,}:\s*(.+?)\s*$')
    {
      if ([string]::IsNullOrEmpty($current.name)) { $current.name = $matches[1] }
    }
  }

  foreach ($backendDevice in $devices)
  {
    $backendDevice.type = Convert-DeviceType $backendDevice.type $backendDevice.backend
  }

  return $devices
}

# Runs hashcat from its own directory, where it resolves OpenCL/ and modules/.
function Invoke-Hashcat([string[]]$Arguments)
{
  $ErrorActionPreference = 'Continue'

  Push-Location -LiteralPath (Split-Path -Parent $executablePath)

  try
  {
    $lines = & $executablePath @Arguments 2>&1 | ForEach-Object { "$_" }
  }
  finally
  {
    Pop-Location
  }

  return [pscustomobject]@{
    exit_code = $LASTEXITCODE
    lines     = @($lines)
  }
}

# Runs one benchmark and returns the speed of every reported device in kH/s.
function Get-BenchmarkSpeeds([int]$Mode, [int]$WorkloadProfile, [int]$DeviceId)
{
  $arguments = @('-b', '-m', "$Mode")

  # Workload profile 0 means "hashcat default" and passes no -w at all.
  if ($WorkloadProfile -gt 0) { $arguments += @('-w', "$WorkloadProfile") }

  if ($DeviceId -gt 0) { $arguments += @('-d', "$DeviceId") }

  $arguments += $gateArguments

  $result = Invoke-Hashcat $arguments

  $output = $result.lines
  $exitCode = $result.exit_code

  $speeds = @{}

  foreach ($line in $output)
  {
    if ($line -match '^Speed\.#(\d+)\.{2,}:\s+([0-9]+(?:\.[0-9]+)?)\s*([kMGTP]?H)/s')
    {
      $unit = $matches[3]

      $multiplier = switch ($unit)
      {
        'H'  { 0.001 }
        'kH' { 1.0 }
        'MH' { 1000.0 }
        'GH' { 1000000.0 }
        'TH' { 1000000000.0 }
        default { 1.0 }
      }

      $speeds[[int]$matches[1]] = [double]$matches[2] * $multiplier
    }
  }

  return [pscustomobject]@{
    exit_code = $exitCode
    speeds    = $speeds
    output    = $output
  }
}

# ---------------------------------------------------------------- inputs

if (-not (Test-Path -LiteralPath $HashcatExecutable -PathType Leaf))
{
  Write-Failure "hashcat executable not found: $HashcatExecutable"
}

$executablePath = (Resolve-Path -LiteralPath $HashcatExecutable).Path

$baselinePath = $Baseline

if ($Baseline -match '\.md$')
{
  $jsonSibling = [System.IO.Path]::ChangeExtension($Baseline, '.json')

  if (Test-Path -LiteralPath $jsonSibling)
  {
    $baselinePath = $jsonSibling
  }
  else
  {
    $baselinePath = Join-Path (Split-Path -Parent $Baseline) 'opencl-baseline.json'
  }
}

if (-not (Test-Path -LiteralPath $baselinePath -PathType Leaf))
{
  Write-Failure "baseline not found: $baselinePath"
}

try
{
  $document = Get-Content -LiteralPath $baselinePath -Raw | ConvertFrom-Json
}
catch
{
  Write-Failure "baseline is not valid JSON: $baselinePath ($($_.Exception.Message))"
}

if ($document.schema -ne 1)
{
  Write-Failure "unsupported baseline schema '$($document.schema)' in $baselinePath"
}

$hostSection = $document.hosts.$HostName

if ($null -eq $hostSection)
{
  Write-Failure "baseline $baselinePath has no host section '$HostName'"
}

if ($hostSection.transport -ne 'local')
{
  Write-Failure "host section '$HostName' was recorded over '$($hostSection.transport)'; run this gate on that host"
}

# ------------------------------------------------------------ devices

$info = Invoke-Hashcat (@('-I') + $gateArguments)

if ($info.exit_code -ne 0)
{
  Write-Failure "hashcat -I exited with $($info.exit_code)"
}

$actualDevices = @(Get-BackendDevices $info.lines)

$expectedDevices = @($hostSection.devices)

$deviceErrors = @()

foreach ($expected in $expectedDevices)
{
  $actual = $actualDevices | Where-Object { $_.id -eq [int]$expected.id } | Select-Object -First 1

  if ($null -eq $actual)
  {
    $deviceErrors += "device #$($expected.id) '$($expected.name)' ($($expected.type)) is missing"

    continue
  }

  if ($actual.name -ne $expected.name)
  {
    $deviceErrors += "device #$($expected.id) name changed: expected '$($expected.name)', got '$($actual.name)'"
  }

  if ($actual.type -ne $expected.type)
  {
    $deviceErrors += "device #$($expected.id) type changed: expected '$($expected.type)', got '$($actual.type)'"
  }
}

foreach ($actual in $actualDevices)
{
  if (@($expectedDevices | Where-Object { [int]$_.id -eq $actual.id }).Count -eq 0)
  {
    $deviceErrors += "device #$($actual.id) '$($actual.name)' ($($actual.type)) is not part of the baseline"
  }
}

if ($deviceErrors.Count -gt 0)
{
  Write-Host "DEVICE COMPOSITION MISMATCH ($HostName)" -ForegroundColor Red

  foreach ($message in $deviceErrors) { Write-Host "  - $message" -ForegroundColor Red }

  exit 1
}

Write-Host "Devices: $($expectedDevices.Count) match the baseline" -ForegroundColor Green

# ---------------------------------------------------------- benchmarks

if ($SkipBenchmark)
{
  Write-Host 'Benchmarks: skipped (-SkipBenchmark)'

  exit 0
}

$benchmarks = @($hostSection.benchmarks)

if ($Device -gt 0)
{
  $benchmarks = @($benchmarks | Where-Object { [int]$_.device_id -eq $Device })

  if ($benchmarks.Count -eq 0)
  {
    Write-Host "No baseline benchmark recorded for device #$Device; nothing to compare" -ForegroundColor Yellow
  }
}

$speedFailures = @()

foreach ($benchmark in $benchmarks)
{
  $mode = [int]$benchmark.mode
  $workload = if ($null -ne $benchmark.workload_profile) { [int]$benchmark.workload_profile } else { 0 }
  $deviceId = [int]$benchmark.device_id

  $result = Get-BenchmarkSpeeds $mode $workload $deviceId

  if ($result.exit_code -ne 0)
  {
    $speedFailures += "-m $mode device #$deviceId benchmark exited with $($result.exit_code)"

    continue
  }

  $current = $result.speeds[$deviceId]

  if ($null -eq $current)
  {
    $speedFailures += "-m $mode device #$deviceId produced no Speed line"

    continue
  }

  $reference = [double]$benchmark.speed_khs

  $dropPercent = (($reference - $current) / $reference) * 100.0

  $summary = ('-m {0} device #{1}: {2:N1} kH/s vs {3:N1} kH/s baseline ({4:+0.0;-0.0}%)' -f $mode, $deviceId, $current, $reference, ($dropPercent * -1.0))

  if ($dropPercent -gt $FailPercent)
  {
    $speedFailures += $summary

    Write-Host $summary -ForegroundColor Red
  }
  elseif ($dropPercent -gt $WarnPercent)
  {
    Write-Host "WARNING: $summary" -ForegroundColor Yellow
  }
  else
  {
    Write-Host "OK: $summary" -ForegroundColor Green
  }
}

if ($speedFailures.Count -gt 0)
{
  Write-Host "SPEED DEGRADATION ($HostName)" -ForegroundColor Red

  foreach ($message in $speedFailures) { Write-Host "  - $message" -ForegroundColor Red }

  exit 2
}

exit 0
