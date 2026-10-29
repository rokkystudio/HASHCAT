param(
  [Parameter(Mandatory=$true)][ValidateSet('91809037')][string]$Serial,
  [Parameter(Mandatory=$true)][ValidateSet(0,100,1000,1400,1700,1800,22000)][int]$Mode,
  [ValidateRange(0,4)][int]$WorkloadProfile = 0
)
$ErrorActionPreference = 'Continue'
$m5 = '{0:D5}' -f $Mode
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$remote = "/data/local/tmp/hc-modebench-$m5"
$runner = '/data/local/tmp/run-hashcat-modes-isolated.sh'
$runtime = 'fuck.system.hashcat'
$state = & adb -s $Serial get-state 2>$null
if ($LASTEXITCODE -ne 0 -or (($state -join '').Trim() -ne 'device')) { throw "ADB unavailable: $Serial" }
$model = (& adb -s $Serial shell getprop ro.product.model 2>$null) -join ''
if ($LASTEXITCODE -ne 0 -or ($model.Trim() -ne 'NX709S')) {
  throw "Unexpected target model '$model' for serial $Serial"
}
& adb -s $Serial shell run-as $runtime ls files/hashcat/runtime/modules/module_00000.so 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) { throw "Hashcat app runtime is unavailable on $Serial" }
& adb -s $Serial shell ls -ld $remote 2>$null | Out-Null
if ($LASTEXITCODE -eq 0) { throw "Stale staged directory exists: $remote" }
$frontend = Join-Path $root 'build/arm64-v8a/hashcat'
$module = Join-Path $root "build/arm64-v8a/modules/module_$m5.so"
$sources = @(Get-ChildItem (Join-Path $root 'build/arm64-v8a/OpenCL') -Filter "m$m5*.cl" -File)
if (!(Test-Path $frontend) -or !(Test-Path $module) -or $sources.Count -lt 1) { throw "Missing build artifacts for $Mode" }
$logDir = Join-Path $root 'build/diagnostic-multimode'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
$logFile = Join-Path $logDir "$($model.Trim().Replace(' ','_'))-m$m5.log"
Write-Output "TARGET:$Serial MODEL:$model MODE:$Mode SOURCES:$($sources.Count)"
try {
  & adb -s $Serial shell mkdir -p "$remote/OpenCL" | Out-Null
  if ($LASTEXITCODE -ne 0) { throw 'Staging directory failed' }
  foreach ($pair in @(@($frontend, "$remote/hashcat"),@($module, "$remote/module_$m5.so"),@((Join-Path $root 'tools/run-hashcat-modes-isolated.sh'),$runner))) {
    & adb -s $Serial push $pair[0] $pair[1] 2>$null | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Failed to stage: $($pair[0])" }
  }
  foreach ($source in $sources) {
    & adb -s $Serial push $source.FullName "$remote/OpenCL/$($source.Name)" 2>$null | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Failed to stage: $($source.Name)" }
  }
  $suffix = if ($WorkloadProfile -gt 0) { "-w$WorkloadProfile" } else { '' }
  $remoteResultDir = '/data/user/0/fuck.system.hashcat/cache/hc-mode-results'
  $remoteLog = "$remoteResultDir/m$m5$suffix.log"
  $remoteExit = "$remoteResultDir/m$m5$suffix.exit"
  & adb -s $Serial shell run-as $runtime mkdir -p $remoteResultDir 2>$null | Out-Null
  & adb -s $Serial shell run-as $runtime rm -f $remoteLog $remoteExit 2>$null | Out-Null
  $logFile = Join-Path $logDir "$($model.Trim().Replace(' ','_'))-m$m5$suffix-device.log"
  $exitFile = Join-Path $logDir "$($model.Trim().Replace(' ','_'))-m$m5$suffix.exit"
  Write-Output "RUNNING_ISOLATED:$Serial MODE:$Mode WORKLOAD:$WorkloadProfile"
  & adb -s $Serial shell run-as $runtime sh $runner $Mode $WorkloadProfile 2>$null | Out-Null
  $adbCode = $LASTEXITCODE
  $deviceExit = ((& adb -s $Serial shell run-as $runtime cat $remoteExit 2>$null) -join '').Trim()
  $deviceLog = & adb -s $Serial shell run-as $runtime cat $remoteLog 2>$null
  if ($deviceLog) { [System.IO.File]::WriteAllLines($logFile, [string[]]$deviceLog, [System.Text.UTF8Encoding]::new($false)) }
  if ($deviceExit -match '^\d+$') { [System.IO.File]::WriteAllText($exitFile, "$deviceExit`n", [System.Text.UTF8Encoding]::new($false)) }
  $benchmarkSuccess = ($deviceExit -eq '0') -and (($deviceLog | Select-String -Pattern '^Speed\.#01') -ne $null)
  Write-Output "MODE_RESULT:$Mode DEVICE_EXIT:$deviceExit ADB_EXIT:$adbCode HASHCAT_BENCHMARK_SUCCESS:$benchmarkSuccess LOG:$logFile"
  if (-not $benchmarkSuccess) { throw "No verified hashcat benchmark completion for mode $Mode (device exit '$deviceExit', ADB $adbCode); see $logFile" }
  & adb -s $Serial shell run-as $runtime rm -f $remoteLog $remoteExit 2>$null | Out-Null

}
finally {
  & adb -s $Serial shell rm -rf $remote 2>$null | Out-Null
  if ($LASTEXITCODE -ne 0) { Write-Warning "Could not clean remote staging $remote; reconnect and inspect" }
}
