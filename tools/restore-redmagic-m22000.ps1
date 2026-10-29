# Recover original mode 22000 files if ADB disconnected during a benchmark.
# This script refuses devices other than RedMagic serial 91809037.
$ErrorActionPreference = 'Continue'

$serial = '91809037'
$package = 'fuck.system.hashcat'
$localScript = Join-Path $PSScriptRoot 'restore-m22000-after-test.sh'
$remoteScript = '/data/local/tmp/restore-m22000-after-test.sh'

if (-not (Test-Path -LiteralPath $localScript)) {
    throw "Restore script missing: $localScript"
}

$state = & adb -s $serial get-state 2>$null
if ($LASTEXITCODE -ne 0 -or (($state -join '') -notmatch '^device\s*$')) {
    throw "RedMagic $serial is not connected to ADB. Reconnect it with USB debugging enabled before retrying."
}

& adb -s $serial push $localScript $remoteScript 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw 'Could not upload restore script; no app files have been modified by this attempt.'
}

$result = & adb -s $serial shell run-as $package sh $remoteScript
$code = $LASTEXITCODE
$result | ForEach-Object { Write-Output $_ }

if ($code -ne 0 -or (($result -join [Environment]::NewLine) -notmatch 'RESTORE_VERIFIED:')) {
    throw 'Restore did not verify. Preserve phone backups and retry after checking connectivity.'
}

# Clean only the specific test files staged by this experiment.
& adb -s $serial shell rm -f /data/local/tmp/hc-safe-22000 /data/local/tmp/module_22000.safe.so /data/local/tmp/m22000-pure.safe.cl /data/local/tmp/hc-safe-22000-test.sh $remoteScript
if ($LASTEXITCODE -ne 0) {
    Write-Warning 'App files were restored, but temporary staging cleanup was incomplete.'
}

Write-Output "RESTORE_COMPLETE for RedMagic $serial"
