param(
    [Parameter(Mandatory = $true)]
    [string]$Serial,
    [ValidateSet('arm64-v8a', 'armeabi-v7a')]
    [string]$Abi = 'arm64-v8a',
    [string]$BuildDir = '',
    [ValidateRange(1, 100)]
    [int]$Iterations = 10,
    [string]$ReportPath = '',
    [switch]$SkipPush
)

# adb writes its progress to stderr, which PowerShell would turn into a
# terminating error under 'Stop'. Every adb call is followed by an explicit
# exit-code check instead.
$ErrorActionPreference = 'Continue'
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
[Console]::InputEncoding = $utf8NoBom
[Console]::OutputEncoding = $utf8NoBom
$OutputEncoding = $utf8NoBom

$RootDir = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

if ([string]::IsNullOrWhiteSpace($BuildDir)) {
    $BuildDir = Join-Path $RootDir "build\diagnostic-multimode\vulkan-runtime-demo\$Abi"
}

if ([string]::IsNullOrWhiteSpace($ReportPath)) {
    $ReportPath = Join-Path $RootDir "build\vulkan-runtime-$Serial-$Abi.log"
}

$RemoteDir = '/data/local/tmp/vkrt'

foreach ($required in @('hashcat', 'libc++_shared.so', 'hashcat.hcstat2')) {
    $candidate = Join-Path $BuildDir $required
    if (-not (Test-Path -LiteralPath $candidate)) {
        throw "Missing runtime file: $candidate. Build the Vulkan diagnostic target first."
    }
}

if (-not $SkipPush) {
    & adb -s $Serial shell "mkdir -p $RemoteDir && rm -rf $RemoteDir/*" 2>$null
    if ($LASTEXITCODE -ne 0) { throw "adb shell failed for $Serial" }

    foreach ($name in @('modules', 'bridges', 'rules', 'tunings', 'OpenCL')) {
        $candidate = Join-Path $BuildDir $name
        if (-not (Test-Path -LiteralPath $candidate)) { throw "Missing runtime directory: $candidate" }
        & adb -s $Serial push $candidate "$RemoteDir/" 2>$null | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "adb push $name failed" }
    }

    foreach ($name in @('hashcat', 'libc++_shared.so', 'hashcat.hcstat2')) {
        & adb -s $Serial push (Join-Path $BuildDir $name) "$RemoteDir/$name" 2>$null | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "adb push $name failed" }
    }

    & adb -s $Serial shell "chmod 755 $RemoteDir/hashcat" 2>$null
}

$runCommand = "cd $RemoteDir && HASHCAT_VULKAN_RUNTIME_ITERATIONS=$Iterations LD_LIBRARY_PATH=. ./hashcat -I --backend-ignore-opencl; echo RC=`$?"

Write-Output "Running Vulkan runtime check on $Serial ($Iterations iteration(s))"

$lines = (& adb -s $Serial shell $runCommand 2>&1) | ForEach-Object { $_.ToString() }

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $ReportPath) | Out-Null

$lines | Set-Content -LiteralPath $ReportPath -Encoding utf8

$jsonPath = [System.IO.Path]::ChangeExtension($ReportPath, '.json')

$jsonLines = (& adb -s $Serial shell "cd $RemoteDir && LD_LIBRARY_PATH=. ./hashcat --backend-info-json --backend-ignore-opencl 2>/dev/null" 2>&1) | ForEach-Object { $_.ToString() }

$jsonLines | Set-Content -LiteralPath $jsonPath -Encoding utf8

$verifier = Join-Path $PSScriptRoot 'test-vulkan-runtime-check.py'

& python -B $verifier --log $ReportPath --json $jsonPath

$exitCode = $LASTEXITCODE

Write-Output "Report: $ReportPath"
Write-Output "JSON:   $jsonPath"

exit $exitCode
