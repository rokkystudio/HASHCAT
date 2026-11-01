<#
.SYNOPSIS
Run a real public SPIR-V Compute kernel inside the Android HASHCAT -I process.

.DESCRIPTION
Stages an isolated experimental HASHCAT executable, its original
module_00000.so and complete OpenCL runtime assets in an ADB temporary
directory. The -I information-mode path verifies one empty Vulkan queue
session and then dispatches a 64-word public-data SPIR-V 1.0 Compute
shader through backend_vulkan_kernel_verify. A nonzero native exit,
missing shader dispatch or missing normal OpenCL GPU information fails
the test. No HASHCAT hashing operation, password attack or APK is run.

Outputs one public LF-normalized native transcript with exact
provenance metadata. The temporary Android directory is always removed.

.PARAMETER Serial
Serial of a physically attached Android device.

.PARAMETER Abi
One actually supported target ABI: arm64-v8a or armeabi-v7a.

.PARAMETER ReportPrefix
Lowercase public test identifier, for example pixel-arm64.
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
$artifacts = Join-Path $root 'build\diagnostic-multimode\vulkan-hashcat-public-kernel-20261101'
$diagnostics = Join-Path $root 'build\diagnostic-multimode\vulkan-backend-public-kernel-20261101'
$source = Join-Path $artifacts $Abi
$frontend = Join-Path $source 'hashcat'
$module = Join-Path $source 'modules\module_00000.so'
$opencl = Join-Path $source 'OpenCL'
$remote = "/data/local/tmp/hc-public-compute-hashcat-info-20261101-$ReportPrefix"

foreach ($file in @($frontend, $module, $opencl)) {
    if (-not (Test-Path -LiteralPath $file)) {
        throw "Missing isolated native HASHCAT runtime asset: $file"
    }
}

$reported = @(& adb -s $Serial shell getprop ro.product.cpu.abilist)

if ($LASTEXITCODE -ne 0 -or $reported.Count -ne 1) {
    throw 'The Android device does not expose an unambiguous ABI list.'
}

$available = @($reported[0].Trim() -split ',')

if ($available -notcontains $Abi) {
    throw "The physically connected Android device does not support ABI $Abi."
}

New-Item -Path $diagnostics -ItemType Directory -Force | Out-Null

try {
    & adb -s $Serial shell mkdir -p "$remote/modules" "$remote/rules" "$remote/tunings" | Out-Null

    if ($LASTEXITCODE -ne 0) {
        throw 'Cannot prepare an isolated Android HASHCAT information directory.'
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
        throw 'Cannot execute the staged native HASHCAT frontend.'
    }

    $command = "LD_LIBRARY_PATH=$remote $remote/hashcat -I 2>&1; echo HASHCAT_KERNEL_EXIT=`$?"
    $captured = @(& adb -s $Serial shell $command)

    if ($LASTEXITCODE -ne 0 -or $captured.Count -lt 2) {
        throw 'ADB did not capture a complete Android HASHCAT information process.'
    }

    $lines = @($captured | ForEach-Object { $_.TrimEnd([char]13) })
    $marker = $lines[-1].Trim()

    if ($marker -cnotmatch '^HASHCAT_KERNEL_EXIT=([0-9]+)$') {
        throw "Native HASHCAT process exit marker is invalid: $marker"
    }

    $exitCode = [int]$Matches[1]
    $body = [string]::Join([char]10, $lines)

    if ($exitCode -ne 0) {
        throw "Native HASHCAT -I failed with exit $exitCode : $body"
    }

    foreach ($pattern in @(
        'hashcat \(v7\.1\.2\) starting in backend information mode',
        '(?m)^BACKEND_VULKAN_CATALOG_READY devices=1 requested_api=1\.0 scheduled=0$',
        '(?m)^BACKEND_VULKAN_SESSION_READY index=0 .* empty_submissions=1 .* scheduled=0$',
        '(?m)^BACKEND_VULKAN_KERNEL_READY index=0 .* shader_words=326 descriptors=2 local=32 groups=2 .* scheduled=0$',
        '(?m)^BACKEND_VULKAN_KERNEL_DISPATCH_PASS kernel=public_u32_transform words=64 local=32 groups=2 verified=64 .* shaders=1 password_inputs=0 scheduled=0$',
        '(?m)^BACKEND_VULKAN_KERNEL_CLOSE index=0 result=0 released=1 scheduled=0$',
        'Vulkan 1\.0 public SPIR-V Compute kernel dispatched and verified',
        'OpenCL Info:',
        'Backend Device ID #01',
        '(?m)^HASHCAT_KERNEL_EXIT=0$'
    )) {
        if ($body -notmatch $pattern) {
            throw "Incomplete real HASHCAT Vulkan kernel proof $pattern : $body"
        }
    }

    $file = Join-Path $diagnostics "$ReportPrefix-hashcat-info.log"
    $log = @(
        'TEST=vulkan-hashcat-public-kernel',
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

    Write-Output "PASS full hashcat -I Vulkan 1.0 SPIR-V public kernel $ReportPrefix $Abi"
    Write-Output 'PASS genuine vkCmdDispatch(2,1,1), CPU-verified 64 words and normal OpenCL GPU discovery'
    Write-Output "HASHCAT_KERNEL_INFO_LOG=$file"
}
finally {
    & adb -s $Serial shell rm -rf $remote 2>$null | Out-Null
}
