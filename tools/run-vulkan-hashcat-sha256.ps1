<#
.SYNOPSIS
Run the production Vulkan SHA-256 batch inside Android HASHCAT -I.

.DESCRIPTION
Stages one complete opt-in Android HASHCAT runtime in an isolated ADB
directory. Information mode must execute the existing MD5 stage, then the
variable-length SHA-256 common-executor batch, preserve ordinary OpenCL
discovery and exit zero. No benchmark or password attack is started.

.PARAMETER Serial
Connected physical Android ADB serial. It is not written to the transcript.

.PARAMETER Abi
One ABI advertised by the selected Android device.

.PARAMETER ReportPrefix
Lowercase identifier for the LF-normalized integration transcript.
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
$artifacts = Join-Path $root 'build\diagnostic-multimode\vulkan-hashcat-sha256-20261102'
$diagnostics = Join-Path $root 'build\diagnostic-multimode\vulkan-backend-sha256-20261102'
$source = Join-Path $artifacts $Abi
$frontend = Join-Path $source 'hashcat'
$module = Join-Path $source 'modules\module_00000.so'
$opencl = Join-Path $source 'OpenCL'
$remote = "/data/local/tmp/hc-vulkan-sha256-info-20261102-$ReportPrefix"

foreach ($file in @($frontend, $module, $opencl)) {
    if (-not (Test-Path -LiteralPath $file)) {
        throw "Missing Android HASHCAT runtime asset: $file"
    }
}

$reported = @(& adb -s $Serial shell getprop ro.product.cpu.abilist)

if ($LASTEXITCODE -ne 0 -or $reported.Count -ne 1) {
    throw 'ADB did not return one device ABI list.'
}

if (@($reported[0].Trim() -split ',') -notcontains $Abi) {
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
        throw 'Cannot execute staged HASHCAT frontend.'
    }

    $command = "LD_LIBRARY_PATH=$remote $remote/hashcat -I 2>&1; echo HASHCAT_SHA256_EXIT=" + '$?'
    $captured = @(& adb -s $Serial shell $command)

    if ($LASTEXITCODE -ne 0 -or $captured.Count -lt 2) {
        throw 'ADB did not capture a complete HASHCAT information process.'
    }

    $lines = @($captured | ForEach-Object { $_.TrimEnd([char]13) })
    $marker = $lines[-1].Trim()

    if ($marker -cnotmatch '^HASHCAT_SHA256_EXIT=([0-9]+)$') {
        throw "Native HASHCAT exit marker is invalid: $marker"
    }

    $exitCode = [int]$Matches[1]
    $body = [string]::Join([char]10, $lines)

    if ($exitCode -ne 0) {
        throw "Native HASHCAT -I failed with exit $exitCode : $body"
    }

    foreach ($pattern in @(
        'hashcat \(v7\.1\.2\) starting in backend information mode',
        'Vulkan 1\.0 MD5 SPIR-V known-answer vectors dispatched and verified; hashing remains disabled\.',
        '(?m)^BACKEND_VULKAN_PROGRAM_ACCEPT stages=1 buffers=2 bytes=192 heterogeneous=1 descriptors_per_stage=2$',
        '(?m)^BACKEND_VULKAN_SHA256_READY index=0 .* api=1\.0 shader_words=3219 descriptors=2 buffers=2 input_bytes=192 output_bytes=128 messages=4 local=32 groups=1 program_validated=1 dynamic_resources=1 .* scheduled=0$',
        '(?m)^BACKEND_VULKAN_SHA256_DISPATCH_PASS messages=4 input_bytes=192 output_bytes=128 groups=1 dispatches=1 digests=4 password_generation=0 target_digests=0 scheduled=0$',
        '(?m)^BACKEND_VULKAN_SHA256_CLOSE index=0 result=0 released=1 password_generation=0 target_digests=0 scheduled=0$',
        '(?m)^BACKEND_VULKAN_SHA256_VERIFY_PASS messages=4 one_block=2 two_block=2 verified_digest_bytes=128 password_generation=0 target_digests=0 scheduled=0$',
        'Vulkan 1\.0 SHA-256 SPIR-V variable-length batch dispatched and verified; hashing remains disabled\.',
        'OpenCL Info:',
        'Backend Device ID #01',
        '(?m)^HASHCAT_SHA256_EXIT=0$'
    )) {
        if ($body -notmatch $pattern) {
            throw "Incomplete HASHCAT Vulkan SHA-256 integration proof $pattern : $body"
        }
    }

    $file = Join-Path $diagnostics "$ReportPrefix-hashcat-sha256-info.log"
    $log = @(
        'TEST=vulkan-hashcat-sha256',
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

    Write-Output "PASS full hashcat -I Vulkan SHA-256 integration $ReportPrefix $Abi"
    Write-Output 'PASS four variable-length SHA-256 messages, two padding shapes and normal OpenCL discovery'
    Write-Output "HASHCAT_SHA256_INFO_LOG=$file"
}
finally {
    & adb -s $Serial shell rm -rf $remote 2>$null | Out-Null
}
