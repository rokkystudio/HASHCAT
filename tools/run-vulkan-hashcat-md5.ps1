<#
.SYNOPSIS
Run the production Vulkan MD5 known-answer stage inside Android HASHCAT -I.

.DESCRIPTION
Stages one complete opt-in Android HASHCAT frontend, module_00000.so and OpenCL
assets in an isolated ADB directory. Information mode must complete the existing
public Vulkan checks and the MD5 SPIR-V known-answer stage, preserve ordinary
OpenCL discovery and exit zero. No password attack, benchmark or hashing
scheduler execution is started.

.PARAMETER Serial
Serial of a physically attached Android device. It is not written to the log.

.PARAMETER Abi
One ABI advertised by the selected Android device.

.PARAMETER ReportPrefix
Lowercase identifier used for the LF-normalized integration transcript.
#>
param (
    [Parameter(Mandatory = $true)]
    [string]$Serial,

    [Parameter(Mandatory = $true)]
    [ValidateSet('arm64-v8a', 'armeabi-v7a')]
    [string]$Abi,

    [Parameter(Mandatory = $true)]
    [string]$ReportPrefix
)

$ErrorActionPreference = 'Stop'

if ($ReportPrefix -cnotmatch '^[a-z0-9-]+$') {
    throw 'ReportPrefix must contain lowercase letters, numbers or dashes only.'
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$artifacts = Join-Path $root 'build\diagnostic-multimode\vulkan-hashcat-md5-20261102'
$diagnostics = Join-Path $root 'build\diagnostic-multimode\vulkan-backend-md5-20261102'
$source = Join-Path $artifacts $Abi
$frontend = Join-Path $source 'hashcat'
$module = Join-Path $source 'modules\module_00000.so'
$opencl = Join-Path $source 'OpenCL'
$remote = "/data/local/tmp/hc-vulkan-md5-info-20261102-$ReportPrefix"

foreach ($file in @($frontend, $module, $opencl)) {
    if (-not (Test-Path -LiteralPath $file)) {
        throw "Missing Android HASHCAT runtime asset: $file"
    }
}

$reported = @(& adb -s $Serial shell getprop ro.product.cpu.abilist)

if ($LASTEXITCODE -ne 0 -or $reported.Count -ne 1) {
    throw 'ADB did not return one device ABI list.'
}

$available = @($reported[0].Trim() -split ',')

if ($available -notcontains $Abi) {
    throw "Android device does not advertise ABI $Abi."
}

New-Item -Path $diagnostics -ItemType Directory -Force | Out-Null

try {
    & adb -s $Serial shell mkdir -p "$remote/modules" "$remote/rules" "$remote/tunings" | Out-Null

    if ($LASTEXITCODE -ne 0) {
        throw 'Cannot prepare isolated HASHCAT information directory.'
    }

    foreach ($pair in @(
        @($frontend, "$remote/hashcat"),
        @($module, "$remote/modules/module_00000.so"),
        @($opencl, $remote)
    )) {
        & adb -s $Serial push $pair[0] $pair[1] | Out-Null

        if ($LASTEXITCODE -ne 0) {
            throw "Cannot stage native HASHCAT asset: $($pair[0])"
        }
    }

    & adb -s $Serial shell chmod 700 "$remote/hashcat" | Out-Null

    if ($LASTEXITCODE -ne 0) {
        throw 'Cannot execute the staged HASHCAT frontend.'
    }

    $command = "LD_LIBRARY_PATH=$remote $remote/hashcat -I 2>&1; echo HASHCAT_MD5_EXIT=" + '$?'
    $captured = @(& adb -s $Serial shell $command)

    if ($LASTEXITCODE -ne 0 -or $captured.Count -lt 2) {
        throw 'ADB did not capture a complete HASHCAT information process.'
    }

    $lines = @($captured | ForEach-Object { $_.TrimEnd([char]13) })
    $marker = $lines[-1].Trim()

    if ($marker -cnotmatch '^HASHCAT_MD5_EXIT=([0-9]+)$') {
        throw "Native HASHCAT exit marker is invalid: $marker"
    }

    $exitCode = [int]$Matches[1]
    $body = [string]::Join([char]10, $lines)

    if ($exitCode -ne 0) {
        throw "Native HASHCAT -I failed with exit $exitCode : $body"
    }

    foreach ($pattern in @(
        'hashcat \(v7\.1\.2\) starting in backend information mode',
        '(?m)^BACKEND_VULKAN_CATALOG_READY devices=1 requested_api=1\.0 scheduled=0$',
        '(?m)^BACKEND_VULKAN_PROGRAM_DISPATCH_PASS stages=4 buffers=5 words=64 dispatches=4 verified_words=256 .* password_inputs=0 scheduled=0$',
        '(?m)^BACKEND_VULKAN_MD5_READY index=0 .* api=1\.0 shader_words=1706 descriptors=2 buffers=2 vectors=4 block_bytes=64 local=4 groups=1 program_validated=1 dynamic_resources=1 .* password_inputs=0 target_digests=0 scheduled=0$',
        '(?m)^BACKEND_VULKAN_MD5_DISPATCH_PASS vectors=4 blocks=4 rounds=64 dispatches=1 verified_digest_words=16 sentinel_words=48 .* password_inputs=0 target_digests=0 scheduled=0$',
        '(?m)^BACKEND_VULKAN_MD5_CLOSE index=0 result=0 released=1 password_inputs=0 target_digests=0 scheduled=0$',
        'Vulkan 1\.0 MD5 SPIR-V known-answer vectors dispatched and verified; hashing remains disabled\.',
        'OpenCL Info:',
        'Backend Device ID #01',
        '(?m)^HASHCAT_MD5_EXIT=0$'
    )) {
        if ($body -notmatch $pattern) {
            throw "Incomplete HASHCAT Vulkan MD5 integration proof $pattern : $body"
        }
    }

    $file = Join-Path $diagnostics "$ReportPrefix-hashcat-md5-info.log"
    $log = @(
        'TEST=vulkan-hashcat-md5',
        "DEVICE=$ReportPrefix",
        "ABI=$Abi",
        'COMMAND=-I',
        "ADB_EXIT_CODE=$exitCode",
        $body
    )

    [System.IO.File]::WriteAllText(
        $file,
        ([string]::Join([char]10, $log)) + [char]10,
        [System.Text.UTF8Encoding]::new($false)
    )

    Write-Output "PASS full hashcat -I Vulkan MD5 integration $ReportPrefix $Abi"
    Write-Output 'PASS one cryptographic MD5 dispatch, four public vectors, 16 digest words and normal OpenCL discovery'
    Write-Output "HASHCAT_MD5_INFO_LOG=$file"
}
finally {
    & adb -s $Serial shell rm -rf $remote 2>$null | Out-Null
}
