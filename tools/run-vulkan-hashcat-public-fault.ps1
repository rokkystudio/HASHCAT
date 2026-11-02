<#
.SYNOPSIS
Run public Vulkan programs inside HASHCAT after fault-path integration.

.DESCRIPTION
Stages a complete Android HASHCAT executable with its original OpenCL
assets and checks the production information-mode path using the exact
backend_vulkan_kernel source carrying controlled ownership checkpoints.
No diagnostic fault control is passed to hashcat -I, so all temporary
Vulkan objects are created, seven public Compute stages are dispatched,
448 public uint32 results are checked against independent CPU arithmetic,
and all resources are normally released. The program then performs
ordinary OpenCL GPU discovery. No HASHCAT password candidate, attack
scheduler, cryptographic kernel or APK is invoked.

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
$artifacts = Join-Path $root 'build\diagnostic-multimode\vulkan-hashcat-public-fault-20261102'
$diagnostics = Join-Path $root 'build\diagnostic-multimode\vulkan-backend-public-fault-20261102'
$source = Join-Path $artifacts $Abi
$frontend = Join-Path $source 'hashcat'
$module = Join-Path $source 'modules\module_00000.so'
$opencl = Join-Path $source 'OpenCL'
$remote = "/data/local/tmp/hc-public-fault-hashcat-info-20261102-$ReportPrefix"

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
        '(?m)^BACKEND_VULKAN_KERNEL_READY index=0 .* shader_words=326 descriptors=2 local=32 groups=2 program_validated=1 dynamic_resources=1 .* scheduled=0$',
        '(?m)^BACKEND_VULKAN_KERNEL_DISPATCH_PASS kernel=public_u32_transform words=64 local=32 groups=2 verified=64 .* shaders=1 password_inputs=0 scheduled=0$',
        '(?m)^BACKEND_VULKAN_KERNEL_CLOSE index=0 result=0 released=1 scheduled=0$',
        'Vulkan 1\.0 public SPIR-V Compute kernel dispatched and verified',
        '(?m)^BACKEND_VULKAN_CHAIN_READY index=0 .* shader_words_first=326 shader_words_second=363 descriptors=4 sets=2 buffers=3 local=32 groups=2 program_validated=1 dynamic_resources=1 .* scheduled=0$',
        '(?m)^BACKEND_VULKAN_CHAIN_DISPATCH_PASS stage1=public_u32_transform stage2=public_u32_rotate words=64 local=32 groups=2 dispatches=2 verified_first=64 verified_second=64 .* barrier=shader_write_to_shader_read shaders=2 password_inputs=0 scheduled=0$',
        '(?m)^BACKEND_VULKAN_CHAIN_CLOSE index=0 result=0 released=1 scheduled=0$',
        'Vulkan 1\.0 public two-stage SPIR-V Compute chain dispatched and verified',
        '(?m)^BACKEND_VULKAN_PROGRAM_READY index=0 .* api=1\.0 stages=4 buffers=5 descriptor_sets=4 descriptors=8 shader_modules=4 pipelines=4 local=32 groups=2 program_validated=1 dynamic_resources=1 .* scheduled=0$',
        '(?m)^BACKEND_VULKAN_PROGRAM_DISPATCH_PASS stages=4 buffers=5 words=64 dispatches=4 verified_words=256 dependency_barriers=3 descriptors=8 .* password_inputs=0 scheduled=0$',
        '(?m)^BACKEND_VULKAN_PROGRAM_CLOSE index=0 stages=4 result=0 released=1 scheduled=0$',
        'Vulkan 1\.0 public four-stage SPIR-V Compute chain dispatched and verified',
        '(?m)^DEVICE_CATALOG_CLOSE devices=1 selections=4$',
        'OpenCL Info:',
        'Backend Device ID #01',
        '(?m)^HASHCAT_KERNEL_EXIT=0$'
    )) {
        if ($body -notmatch $pattern) {
            throw "Incomplete real HASHCAT Vulkan kernel proof $pattern : $body"
        }
    }

    if (@($lines | Where-Object {
        $_ -ceq 'BACKEND_VULKAN_PROGRAM_ACCEPT stages=1 buffers=2 bytes=256 descriptors_per_stage=2'
    }).Count -ne 1 -or
        @($lines | Where-Object {
        $_ -ceq 'BACKEND_VULKAN_PROGRAM_ACCEPT stages=2 buffers=3 bytes=256 descriptors_per_stage=2'
    }).Count -ne 1 -or
        @($lines | Where-Object {
        $_ -ceq 'BACKEND_VULKAN_PROGRAM_ACCEPT stages=4 buffers=5 bytes=256 descriptors_per_stage=2'
    }).Count -ne 1) {
        throw 'HASHCAT -I did not validate all three declared public Vulkan programs exactly once.'
    }

    if ($body.Contains('BACKEND_PUBLIC_FAULT_TRIGGER') -or
        $body.Contains('BACKEND_PUBLIC_FAULT_RELEASE')) {
        throw 'Ordinary HASHCAT information mode must not enable fault injection.'
    }

    $file = Join-Path $diagnostics "$ReportPrefix-hashcat-fault-info.log"
    $log = @(
        'TEST=vulkan-hashcat-public-fault',
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

    Write-Output "PASS full hashcat -I with four-stage public Vulkan fault support $ReportPrefix $Abi"
    Write-Output 'PASS three graph validations, seven public vkCmdDispatch(2,1,1), 448 CPU-verified words and normal OpenCL GPU discovery'
    Write-Output "HASHCAT_KERNEL_INFO_LOG=$file"
}
finally {
    & adb -s $Serial shell rm -rf $remote 2>$null | Out-Null
}
