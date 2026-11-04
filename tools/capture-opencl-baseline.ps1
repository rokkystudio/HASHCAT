<#
.SYNOPSIS
Records the OpenCL device list and benchmark speeds of a hashcat executable.

.DESCRIPTION
Produces the machine-readable half of the OpenCL baseline that
tools/check-opencl-regression.ps1 compares against. The script runs
"hashcat -I" and one benchmark per requested hash mode, stores the raw
outputs as evidence and merges the result into the baseline JSON without
touching the other host sections.

Two transports are supported:
    local   HashcatExecutable is the hashcat binary of this host.
    adb     HashcatExecutable is a local directory that contains the Android
            runtime (hashcat, modules/, OpenCL/); it is pushed to the device
            and executed there through "su -c".

The baseline JSON keeps one section per host under "hosts". Re-capturing a
host replaces only that section, so the PC and the Android baseline are
recorded independently.

.EXAMPLE
powershell -File tools/capture-opencl-baseline.ps1 `
    -HashcatExecutable build\windows-x64\hashcat.exe `
    -HostName pc -Label 'Windows 10 x64 / NVIDIA GeForce RTX 3060 Ti'

.EXAMPLE
powershell -File tools/capture-opencl-baseline.ps1 `
    -HashcatExecutable build\arm64-v8a -Transport adb -AdbSerial 91809037 `
    -HostName android -Label 'REDMAGIC 7S Pro (NX709S), Android 13'
#>
param(
  [Parameter(Mandatory = $true)]
  [string]$HashcatExecutable,

  [Parameter(Mandatory = $true)]
  [string]$HostName,

  [Parameter(Mandatory = $true)]
  [string]$Label,

  [string]$Baseline = 'docs/opencl-baseline.json',

  [ValidateSet('local', 'adb')]
  [string]$Transport = 'local',

  [string]$AdbPath = 'adb',

  [string]$AdbSerial = '',

  [ValidateRange(0, 255)]
  [int]$Device = 0,

  [int[]]$Modes = @(0, 1000, 22000),

  [ValidateRange(0, 4)]
  [int]$WorkloadProfile = 2,

  [string]$RawOutputDirectory = 'build/opencl-baseline'
)

$ErrorActionPreference = 'Stop'

$utf8NoBom = [System.Text.UTF8Encoding]::new($false)

[Console]::OutputEncoding = $utf8NoBom
$OutputEncoding = $utf8NoBom

# hashcat prints speeds as "14042.2 MH/s"; parsing must not depend on the
# machine locale (a comma-decimal locale would misread the dot).
[System.Threading.Thread]::CurrentThread.CurrentCulture = [System.Globalization.CultureInfo]::InvariantCulture

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

# The OpenCL baseline must not silently switch to CUDA or HIP when those
# interfaces are present: on a PC both backends expose the same physical GPU
# and "hashcat -b" benchmarks the first one and skips the other as a duplicate.
$gateArguments = @('--backend-ignore-cuda', '--backend-ignore-hip')

function Resolve-RepoPath([string]$Path)
{
  if ([System.IO.Path]::IsPathRooted($Path)) { return $Path }

  return (Join-Path $repoRoot $Path)
}

$Baseline           = Resolve-RepoPath $Baseline
$RawOutputDirectory = Resolve-RepoPath $RawOutputDirectory

function Stop-Capture([string]$Message)
{
  Write-Host "ERROR: $Message" -ForegroundColor Red

  exit 1
}

function Convert-DeviceType([string]$Type, [string]$Backend)
{
  if ([string]::IsNullOrWhiteSpace($Type))
  {
    if ($Backend -eq 'cuda' -or $Backend -eq 'hip' -or $Backend -eq 'metal') { return 'GPU' }

    return 'ACCEL'
  }

  switch -Regex ($Type.Trim())
  {
    '^GPU$'                 { return 'GPU' }
    '^CPU$'                 { return 'CPU' }
    '^FPGA$'                { return 'FPGA' }
    '^ACCEL$|^Accelerator$' { return 'ACCEL' }
    default                 { return 'ACCEL' }
  }
}

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

  foreach ($device in $devices)
  {
    $device.type = Convert-DeviceType $device.type $device.backend
  }

  return $devices
}

function Get-BenchmarkSpeeds([string[]]$Lines)
{
  $speeds = @{}

  foreach ($line in $Lines)
  {
    if ($line -match '^Speed\.#(\d+)\.{2,}:\s+([0-9]+(?:\.[0-9]+)?)\s*([kMGTP]?H)/s')
    {
      $multiplier = switch ($matches[3])
      {
        'H'  { 0.001 }
        'kH' { 1.0 }
        'MH' { 1000.0 }
        'GH' { 1000000.0 }
        'TH' { 1000000000.0 }
        default { 1.0 }
      }

      $speeds[[int]$matches[1]] = [math]::Round([double]$matches[2] * $multiplier, 3)
    }
  }

  return $speeds
}

# Runs adb and captures stdout/stderr without letting native progress output
# abort the capture.
function Invoke-Adb([string[]]$Arguments)
{
  $ErrorActionPreference = 'Continue'

  $lines = & $AdbPath @Arguments 2>&1 | ForEach-Object { "$_" }

  return [pscustomobject]@{
    exit_code = $LASTEXITCODE
    lines     = @($lines)
  }
}

# --------------------------------------------------------- transport

$remoteRoot   = $null
$remoteSerial = $null
$executablePath = $null

if ($Transport -eq 'adb')
{
  if ([string]::IsNullOrWhiteSpace($AdbSerial)) { Stop-Capture '-AdbSerial is required with -Transport adb' }

  if (-not (Test-Path -LiteralPath $HashcatExecutable -PathType Container))
  {
    Stop-Capture "Android runtime directory not found: $HashcatExecutable"
  }

  $runtimePath = (Resolve-Path -LiteralPath $HashcatExecutable).Path

  if (-not (Test-Path -LiteralPath (Join-Path $runtimePath 'hashcat') -PathType Leaf))
  {
    Stop-Capture "Android runtime directory has no hashcat binary: $runtimePath"
  }

  $state = Invoke-Adb @('-s', $AdbSerial, 'get-state')

  if ($state.exit_code -ne 0 -or (($state.lines -join '').Trim() -ne 'device'))
  {
    Stop-Capture "ADB device is not ready: $AdbSerial"
  }

  $remoteSerial = $AdbSerial
  $remoteRoot   = "/data/local/tmp/hc-baseline-$AdbSerial"

  Write-Host "Staging $runtimePath to $remoteRoot" -ForegroundColor Cyan

  [void](Invoke-Adb @('-s', $AdbSerial, 'shell', "rm -rf $remoteRoot"))

  $created = Invoke-Adb @('-s', $AdbSerial, 'shell', "mkdir -p $remoteRoot")

  if ($created.exit_code -ne 0) { Stop-Capture "Cannot create $remoteRoot on $AdbSerial" }

  # adb push copies directories recursively; stage the whole runtime so that
  # hashcat finds modules, OpenCL sources, hashcat.hcstat2, rules and tunings.
  foreach ($runtimeItem in Get-ChildItem -LiteralPath $runtimePath)
  {
    $pushed = Invoke-Adb @('-s', $AdbSerial, 'push', $runtimeItem.FullName, "$remoteRoot/$($runtimeItem.Name)")

    if ($pushed.exit_code -ne 0) { Stop-Capture "Failed to stage $($runtimeItem.Name)" }
  }

  $runnerText = @'
#!/system/bin/sh
set -eu
ROOT="$1"
shift
mkdir -p "$ROOT/home" "$ROOT/home/cache" "$ROOT/home/data"
export HOME="$ROOT/home"
export TMPDIR="$ROOT/home"
export XDG_CACHE_HOME="$ROOT/home/cache"
export XDG_DATA_HOME="$ROOT/home/data"
export HASHCAT_SHARED_FOLDER="$ROOT"
export LD_LIBRARY_PATH="$ROOT:/vendor/lib64:/system/lib64"
cd "$ROOT"
exec "$@"
'@

  $runnerLocal = Join-Path $env:TEMP ("hc-baseline-runner-" + [guid]::NewGuid().ToString('N') + ".sh")

  [System.IO.File]::WriteAllText($runnerLocal, ($runnerText -replace "`r`n", "`n"), $utf8NoBom)

  $runnerPushed = Invoke-Adb @('-s', $AdbSerial, 'push', $runnerLocal, "$remoteRoot/hc-run.sh")

  Remove-Item -LiteralPath $runnerLocal -Force

  if ($runnerPushed.exit_code -ne 0) { Stop-Capture 'Failed to stage the device runner' }

  $chmod = Invoke-Adb @('-s', $AdbSerial, 'shell', "su -c 'chmod 755 $remoteRoot/hashcat $remoteRoot/hc-run.sh'")

  if ($chmod.exit_code -ne 0) { Stop-Capture 'Failed to mark the staged runner executable (root required)' }
}

if ($Transport -eq 'local')
{
  if (-not (Test-Path -LiteralPath $HashcatExecutable -PathType Leaf))
  {
    Stop-Capture "hashcat executable not found: $HashcatExecutable"
  }

  $executablePath = (Resolve-Path -LiteralPath $HashcatExecutable).Path
}

function Invoke-Hashcat([string[]]$Arguments)
{
  # Native stderr must not terminate the capture: hashcat reports JIT warnings
  # and missing optional backends there.
  $ErrorActionPreference = 'Continue'

  if ($Transport -eq 'local')
  {
    # hashcat resolves OpenCL/ and modules/ relative to the working directory.
    Push-Location -LiteralPath (Split-Path -Parent $executablePath)

    try
    {
      $lines = & $executablePath @Arguments 2>&1 | ForEach-Object { "$_" }
    }
    finally
    {
      Pop-Location
    }
  }
  else
  {
    $remoteArguments = ($Arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '

    $command = "su -c 'sh $remoteRoot/hc-run.sh $remoteRoot ./hashcat $remoteArguments'"

    $lines = & $AdbPath -s $remoteSerial shell $command 2>&1 | ForEach-Object { "$_" }
  }

  return [pscustomobject]@{
    exit_code = $LASTEXITCODE
    lines     = @($lines)
  }
}

# ------------------------------------------------------------ capture

if (-not (Test-Path -LiteralPath $RawOutputDirectory))
{
  New-Item -ItemType Directory -Force -Path $RawOutputDirectory | Out-Null
}

$rawPrefix = Join-Path $RawOutputDirectory $HostName

Write-Host "Recording hashcat -V ($HostName)" -ForegroundColor Cyan

$version = Invoke-Hashcat @('-V')

if ($version.exit_code -ne 0) { Stop-Capture "hashcat -V exited with $($version.exit_code) on $HostName" }

$versionTag = ($version.lines | Where-Object { $_ -match '^v?[0-9]+\.[0-9]+\.[0-9]+' } | Select-Object -First 1)

if ([string]::IsNullOrWhiteSpace($versionTag)) { Stop-Capture "hashcat -V produced no version line on $HostName" }

$versionTag = $versionTag.Trim()

Write-Host "Recording hashcat -I ($HostName)" -ForegroundColor Cyan

$info = Invoke-Hashcat (@('-I') + $gateArguments)

if ($info.exit_code -ne 0) { Stop-Capture "hashcat -I exited with $($info.exit_code) on $HostName" }

[System.IO.File]::WriteAllLines("$rawPrefix-info.txt", [string[]]$info.lines, $utf8NoBom)

$devices = @(Get-BackendDevices $info.lines)

if ($devices.Count -eq 0) { Stop-Capture "hashcat -I reported no backend devices on $HostName" }

Write-Host "Devices on $HostName" -ForegroundColor Cyan

foreach ($backendDevice in $devices)
{
  Write-Host ("  #{0} {1} [{2}] backend={3} vendor_id={4}" -f $backendDevice.id, $backendDevice.name, $backendDevice.type, $backendDevice.backend, $backendDevice.vendor_id)
}

$selected = @($devices)

if ($Device -gt 0)
{
  $selected = @($devices | Where-Object { $_.id -eq $Device })

  if ($selected.Count -eq 0) { Stop-Capture "Device #$Device is not present on $HostName" }
}

$benchmarks = @()

foreach ($mode in $Modes)
{
  foreach ($backendDevice in $selected)
  {
    $workloadLabel = if ($WorkloadProfile -gt 0) { "-w $WorkloadProfile" } else { 'default workload' }

    Write-Host "Benchmark -m $mode $workloadLabel device #$($backendDevice.id)" -ForegroundColor Cyan

    $benchmarkArguments = @('-b', '-m', "$mode", '-d', "$($backendDevice.id)")

    # Workload profile 0 records hashcat's own default: on Adreno "-w 2"
    # makes clLinkProgram fail with CL_OUT_OF_HOST_MEMORY for the pure kernel.
    if ($WorkloadProfile -gt 0) { $benchmarkArguments += @('-w', "$WorkloadProfile") }

    $result = Invoke-Hashcat ($benchmarkArguments + $gateArguments)

    $rawName = "$rawPrefix-bench-$mode-device$($backendDevice.id).txt"

    [System.IO.File]::WriteAllLines($rawName, [string[]]$result.lines, $utf8NoBom)

    if ($result.exit_code -ne 0) { Stop-Capture "Benchmark -m $mode exited with $($result.exit_code) on $HostName" }

    $speeds = Get-BenchmarkSpeeds $result.lines

    if (-not $speeds.ContainsKey($backendDevice.id))
    {
      Stop-Capture "Benchmark -m $mode produced no Speed line for device #$($backendDevice.id) on $HostName"
    }

    $benchmarks += [ordered]@{
      mode             = $mode
      workload_profile = $WorkloadProfile
      device_id        = $backendDevice.id
      device_name      = $backendDevice.name
      speed_khs        = [double]$speeds[$backendDevice.id]
    }
  }
}

# -------------------------------------------------------------- merge

$hostSection = [ordered]@{
  label             = $Label
  transport         = $Transport
  serial            = if ($Transport -eq 'adb') { $remoteSerial } else { $null }
  capture_arguments = ($gateArguments -join ' ')
  devices           = @($devices | ForEach-Object {
    [ordered]@{
      id        = $_.id
      name      = $_.name
      type      = $_.type
      vendor_id = $_.vendor_id
      backend   = $_.backend
    }
  })
  benchmarks = @($benchmarks)
}

$hosts = [ordered]@{}

if (Test-Path -LiteralPath $Baseline)
{
  $existing = Get-Content -LiteralPath $Baseline -Raw | ConvertFrom-Json

  foreach ($property in $existing.hosts.PSObject.Properties)
  {
    if ($property.Name -ne $HostName) { $hosts[$property.Name] = $property.Value }
  }
}

$hosts[$HostName] = $hostSection

$document = [ordered]@{
  schema          = 1
  generated_at    = (Get-Date).ToString('yyyy-MM-ddTHH:mm:sszzz')
  git_head        = ((& git -C $repoRoot rev-parse HEAD 2>$null | Out-String)).Trim()
  hashcat_version = $versionTag
  hosts           = $hosts
}

$baselineDirectory = Split-Path -Parent $Baseline

if (-not (Test-Path -LiteralPath $baselineDirectory)) { New-Item -ItemType Directory -Force -Path $baselineDirectory | Out-Null }

# ------------------------------------------------------------- output

function ConvertTo-JsonString([string]$Text)
{
  $builder = New-Object System.Text.StringBuilder

  [void]$builder.Append('"')

  foreach ($character in $Text.ToCharArray())
  {
    if ($character -eq '"')       { [void]$builder.Append('\"');   continue }
    if ($character -eq '\')       { [void]$builder.Append('\\');   continue }
    if ($character -eq "`n")      { [void]$builder.Append('\n');   continue }
    if ($character -eq "`r")      { [void]$builder.Append('\r');   continue }
    if ($character -eq "`t")      { [void]$builder.Append('\t');   continue }

    if ([int]$character -lt 0x20)
    {
      [void]$builder.Append(('\u{0:x4}' -f [int]$character))
    }
    else
    {
      [void]$builder.Append($character)
    }
  }

  [void]$builder.Append('"')

  return $builder.ToString()
}

# Writes two-space indented JSON with a stable key order. ConvertTo-Json in
# Windows PowerShell 5.1 aligns keys with extra spaces, which makes the
# baseline hard to review and to diff.
function ConvertTo-JsonText
{
  param($Value, [int]$Level = 0)

  $indent      = ' ' * ($Level * 2)
  $childIndent = ' ' * (($Level + 1) * 2)

  if ($null -eq $Value) { return 'null' }

  if ($Value -is [bool]) { if ($Value) { return 'true' } return 'false' }

  if ($Value -is [string]) { return (ConvertTo-JsonString $Value) }

  if ($Value -is [int] -or $Value -is [long] -or $Value -is [double] -or $Value -is [decimal])
  {
    return $Value.ToString([System.Globalization.CultureInfo]::InvariantCulture)
  }

  if ($Value -is [System.Management.Automation.PSCustomObject])
  {
    $ordered = [ordered]@{}

    foreach ($property in $Value.PSObject.Properties) { $ordered[$property.Name] = $property.Value }

    return (ConvertTo-JsonText $ordered $Level)
  }

  if ($Value -is [System.Collections.IDictionary])
  {
    $entries = @()

    foreach ($key in $Value.Keys)
    {
      $entries += ($childIndent + (ConvertTo-JsonString ([string]$key)) + ': ' + (ConvertTo-JsonText $Value[$key] ($Level + 1)))
    }

    if ($entries.Count -eq 0) { return '{}' }

    return "{`n" + ($entries -join ",`n") + "`n$indent}"
  }

  if ($Value -is [System.Collections.IEnumerable])
  {
    $items = @($Value)

    if ($items.Count -eq 0) { return '[]' }

    $parts = @()

    foreach ($item in $items)
    {
      $parts += ($childIndent + (ConvertTo-JsonText $item ($Level + 1)))
    }

    return "[`n" + ($parts -join ",`n") + "`n$indent]"
  }

  return (ConvertTo-JsonString ([string]$Value))
}

$json = ConvertTo-JsonText $document

[System.IO.File]::WriteAllText($Baseline, $json + "`n", $utf8NoBom)

Write-Host "Baseline written: $Baseline" -ForegroundColor Green
Write-Host "Raw outputs: $RawOutputDirectory" -ForegroundColor Green
