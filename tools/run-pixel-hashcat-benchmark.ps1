param(
  [Parameter(Mandatory=$true)][ValidateSet(0,100,1000,1400,1700,1800,22000)][int]$Mode,
  [ValidateRange(1,4)][int]$WorkloadProfile = 0
)
$ErrorActionPreference = 'Continue'
$serial = '46281FDAS002AL'
$stage = '/data/local/tmp/hc-pixel-hashcat-20261030'
$m5 = '{0:D5}' -f $Mode
$state = & adb -s $serial get-state 2>$null
if ($LASTEXITCODE -ne 0 -or (($state -join '').Trim() -ne 'device')) { throw 'Pixel offline' }
$model = ((& adb -s $serial shell getprop ro.product.model) -join '').Trim()
if ($model -ne 'Pixel 9 Pro XL') { throw "Wrong Pixel model: $model" }
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$module = Join-Path $root "build/arm64-v8a/modules/module_$m5.so"
$sources = @(Get-ChildItem (Join-Path $root 'build/arm64-v8a/OpenCL') -Filter "m$m5*.cl" -File)
if (!(Test-Path $module) -or $sources.Count -lt 1) { throw "Missing local artifacts for mode $Mode" }
& adb -s $serial shell mkdir -p "$stage/modules" "$stage/OpenCL" "$stage/logs" 2>$null | Out-Null
& adb -s $serial push $module "$stage/modules/module_$m5.so" 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Module staging failed' }
foreach ($source in $sources) {
  & adb -s $serial push $source.FullName "$stage/OpenCL/$($source.Name)" 2>$null | Out-Null
  if ($LASTEXITCODE -ne 0) { throw "OpenCL staging failed: $($source.Name)" }
}
$runnerLocal = Join-Path $root 'tools/run-pixel-hashcat-mode.sh'
$runnerRemote = '/data/local/tmp/run-pixel-hashcat-mode.sh'
& adb -s $serial push $runnerLocal $runnerRemote 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Runner staging failed' }
$suffix = if ($WorkloadProfile -gt 0) { "-w$WorkloadProfile" } else { '' }
$remoteLog = "$stage/logs/m$m5$suffix.log"
$remoteExit = "$stage/logs/m$m5$suffix.exit"
& adb -s $serial shell rm -f $remoteLog $remoteExit 2>$null | Out-Null
$logdir = Join-Path $root 'build/diagnostic-multimode'
New-Item -ItemType Directory -Force -Path $logdir | Out-Null
$log = Join-Path $logdir "Pixel_9_Pro_XL-m$m5$suffix-device.log"
$exitFile = Join-Path $logdir "Pixel_9_Pro_XL-m$m5$suffix.exit"
Write-Output "PIXEL_BENCHMARK_BEGIN:mode=$Mode workload=$WorkloadProfile"
& adb -s $serial shell sh $runnerRemote $Mode $WorkloadProfile 2>$null | Out-Null
$adbCode = $LASTEXITCODE
$deviceExit = ((& adb -s $serial shell cat $remoteExit 2>$null) -join '').Trim()
$deviceLog = & adb -s $serial shell cat $remoteLog 2>$null
if ($deviceLog) { [System.IO.File]::WriteAllLines($log, [string[]]$deviceLog, [System.Text.UTF8Encoding]::new($false)) }
if ($deviceExit -match '^\d+$') { [System.IO.File]::WriteAllText($exitFile, "$deviceExit`n", [System.Text.UTF8Encoding]::new($false)) }
Write-Output "PIXEL_MODE_RESULT:$Mode DEVICE_EXIT:$deviceExit ADB_EXIT:$adbCode LOG:$log"
if ($deviceExit -notmatch '^0$') { throw "Pixel mode $Mode failed with device exit '$deviceExit' (ADB $adbCode)" }
