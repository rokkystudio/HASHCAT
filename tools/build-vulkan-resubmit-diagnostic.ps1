<#
.SYNOPSIS
Builds the reusable-command-buffer Vulkan Compute diagnostic for Android.

.DESCRIPTION
Compiles six synthetic Vulkan 1.1 SPIR-V shaders, validates every shader,
and builds the Android ARM64 program that re-submits one recorded three-stage
compute command buffer with changing public data. No GPU is launched here.

.PARAMETER AndroidNdk
Exact Android NDK root path containing shader-tools and LLVM.

.PARAMETER AndroidApi
Android API level used to compile the ARM64 Vulkan executable.

.PARAMETER OutputDirectory
Optional local destination for all generated executable and SPIR-V files.
#>
param (
    [Parameter(Mandatory = $true)]
    [string]$AndroidNdk,

    [int]$AndroidApi = 26,

    [string]$OutputDirectory = ''
)

$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$ndk = (Resolve-Path $AndroidNdk).Path
$shaderTools = Join-Path $ndk 'shader-tools\windows-x86_64'
$llvmBin = Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin'
$glslc = Join-Path $shaderTools 'glslc.exe'
$spirvVal = Join-Path $shaderTools 'spirv-val.exe'
$clang = Join-Path $llvmBin 'clang.exe'

foreach ($tool in @($glslc, $spirvVal, $clang)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "Android NDK compiler or validator is missing: $tool"
    }
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-resubmit-20261031'
}
elseif (-not [System.IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory = Join-Path $root $OutputDirectory
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$destination = (Resolve-Path $OutputDirectory).Path
$artifacts = [System.Collections.Generic.List[string]]::new()

foreach ($stage in @('a', 'b', 'c')) {
    $source = Join-Path $PSScriptRoot "vulkan-pipeline-stage-$stage.comp"

    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Vulkan shader source is missing: $source"
    }

    foreach ($optimization in @('O0', 'O')) {
        $name = "stage-$stage-$optimization.spv"
        $output = Join-Path $destination $name

        & $glslc --target-env=vulkan1.1 "-$optimization" -Werror -o $output $source

        if ($LASTEXITCODE -ne 0) {
            throw "Vulkan shader compilation failed: $source ($optimization)"
        }

        & $spirvVal --target-env vulkan1.1 $output

        if ($LASTEXITCODE -ne 0) {
            throw "SPIR-V shader validation failed: $name"
        }

        [void]$artifacts.Add($name)

        Write-Output "PASS Vulkan 1.1 SPIR-V: stage $stage, optimization $optimization"
    }
}

$runner = Join-Path $PSScriptRoot 'vulkan-resubmit-diagnostic.c'
$binaryName = 'vulkan-resubmit-arm64'
$binary = Join-Path $destination $binaryName

& $clang "--target=aarch64-linux-android$AndroidApi" -std=gnu11 -O2 -Wall -Wextra -Werror $runner -o $binary -lvulkan

if ($LASTEXITCODE -ne 0) {
    throw 'Reusable-command-buffer ARM64 Vulkan compilation failed'
}

[void]$artifacts.Add($binaryName)

Write-Output 'PASS reusable-command-buffer Vulkan ARM64 executable'

foreach ($name in $artifacts) {
    $path = Join-Path $destination $name
    $sha256 = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    $size = (Get-Item -LiteralPath $path).Length

    Write-Output "$name bytes=$size sha256=$sha256"
}

Write-Output 'BUILD COMPLETE: six original Vulkan SPIR-V shaders and replay-enabled ARM64 executable'
