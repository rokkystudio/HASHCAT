<#
.SYNOPSIS
Builds the standalone Android Vulkan 1.1 resource-lifetime diagnostic.

.DESCRIPTION
Compiles one public synthetic GLSL shader with and without optimization,
validates both Vulkan 1.1 SPIR-V modules, and compiles the AArch64 resource
lifecycle test with strict warnings. This command does not execute Vulkan,
install an app, or modify hashcat files.

.PARAMETER AndroidNdk
Exact Android NDK directory containing shader-tools and Clang.

.PARAMETER AndroidApi
Android API level targeted by the standalone ARM64 executable.

.PARAMETER OutputDirectory
Destination for the three locally generated diagnostic artifacts.
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
        throw "Android NDK compiler or shader validator is missing: $tool"
    }
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-lifecycle-20261031'
}
elseif (-not [System.IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory = Join-Path $root $OutputDirectory
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

$directory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$shaderSource = Join-Path $PSScriptRoot 'vulkan-pipeline-stage-a.comp'
$artifacts = @()

foreach ($optimization in @('O0', 'O')) {
    $name = "lifecycle-$optimization.spv"
    $output = Join-Path $directory $name

    & $glslc --target-env=vulkan1.1 "-$optimization" -Werror -o $output $shaderSource

    if ($LASTEXITCODE -ne 0) {
        throw "Vulkan shader compilation failed for $optimization"
    }

    & $spirvVal --target-env vulkan1.1 $output

    if ($LASTEXITCODE -ne 0) {
        throw "Vulkan SPIR-V validation failed for $optimization"
    }

    $artifacts += $name

    Write-Output "PASS validated Vulkan SPIR-V: $name"
}

$name = 'vulkan-resource-lifecycle-arm64'
$source = Join-Path $PSScriptRoot 'vulkan-resource-lifecycle-diagnostic.c'
$output = Join-Path $directory $name

& $clang "--target=aarch64-linux-android$AndroidApi" -std=gnu11 -O2 -Wall -Wextra -Werror $source -o $output -lvulkan

if ($LASTEXITCODE -ne 0) {
    throw 'Android ARM64 Vulkan resource lifecycle compilation failed'
}

$artifacts += $name

Write-Output 'PASS standalone Android ARM64 resource lifecycle executable'

foreach ($artifact in $artifacts) {
    $file = Join-Path $directory $artifact
    $hash = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
    $length = (Get-Item -LiteralPath $file).Length

    Write-Output "$artifact bytes=$length sha256=$hash"
}

Write-Output 'BUILD COMPLETE: two public SPIR-V shaders and one standalone ARM64 executable'
