<#
.SYNOPSIS
Builds portable Vulkan capability discovery and baseline synthetic shaders.

.DESCRIPTION
Compiles one standalone Vulkan 1.0-compatible Android ARM64 diagnostic and
six variants of the three existing public synthetic GLSL shaders.
Checks all six shader modules with spirv-val. The programs do not load
hashcat, parse passwords, or alter Android application files.

.PARAMETER AndroidNdk
Exact Android NDK root containing Clang, glslc and spirv-val.

.PARAMETER AndroidApi
Minimum Android platform level for the ARM64 diagnostics.

.PARAMETER OutputDirectory
Optional local directory for the seven reproducible build artifacts.
#>
param (
    [Parameter(Mandatory = $true)]
    [string]$AndroidNdk,

    [int]$AndroidApi = 26,

    [string]$OutputDirectory = ''
)

$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$ndk = (Resolve-Path -LiteralPath $AndroidNdk).Path
$shaderTools = Join-Path $ndk 'shader-tools\windows-x86_64'
$llvmBin = Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin'
$glslc = Join-Path $shaderTools 'glslc.exe'
$spirvVal = Join-Path $shaderTools 'spirv-val.exe'
$clang = Join-Path $llvmBin 'clang.exe'

foreach ($tool in @($glslc, $spirvVal, $clang)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "Android NDK compiler or SPIR-V validator unavailable: $tool"
    }
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-autoconfig-20261031'
}
elseif (-not [System.IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory = Join-Path $root $OutputDirectory
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$directory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$artifacts = @()

$probe = Join-Path $directory 'vulkan-autoconfig-arm64'
$autoSource = Join-Path $PSScriptRoot 'vulkan-auto-config.c'
$diagnosticSource = Join-Path $PSScriptRoot 'vulkan-auto-config-diagnostic.c'

& $clang "--target=aarch64-linux-android$AndroidApi" -std=gnu11 -O2 -Wall -Wextra -Werror $autoSource $diagnosticSource -o $probe -lvulkan

if ($LASTEXITCODE -ne 0) {
    throw 'Vulkan 1.0 auto-configuration ARM64 build failed.'
}

$artifacts += 'vulkan-autoconfig-arm64'

Write-Output 'PASS auto-config AArch64 CPU/device discovery executable'

foreach ($stage in @('a', 'b', 'c')) {
    $shaderSource = Join-Path $PSScriptRoot "vulkan-pipeline-stage-$stage.comp"

    foreach ($optimization in @('O0', 'O')) {
        $name = "stage-$stage-$optimization-vk1.0.spv"
        $output = Join-Path $directory $name

        & $glslc --target-env=vulkan1.0 "-$optimization" -Werror -o $output $shaderSource

        if ($LASTEXITCODE -ne 0) {
            throw "Vulkan 1.0 shader compilation failed: $name"
        }

        & $spirvVal --target-env vulkan1.0 $output

        if ($LASTEXITCODE -ne 0) {
            throw "Vulkan 1.0 shader validation failed: $name"
        }

        $artifacts += $name

        Write-Output "PASS Vulkan 1.0 shader baseline: $name"
    }
}

foreach ($name in $artifacts) {
    $file = Join-Path $directory $name
    $hash = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
    $bytes = (Get-Item -LiteralPath $file).Length

    Write-Output "$name bytes=$bytes sha256=$hash"
}

Write-Output 'BUILD COMPLETE: one standalone ARM64 executable and six Vulkan 1.0 SPIR-V shaders'
