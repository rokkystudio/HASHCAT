<#
.SYNOPSIS
Builds a portable Vulkan 1.0 synthetic GPU staging-buffer diagnostic.

.DESCRIPTION
Builds a standalone Android AArch64 program with strict Clang warnings and
two unchanged public stage-A Vulkan SPIR-V shaders, one optimized and one
unoptimized. Both shader modules are validated for the Vulkan 1.0 target.
The build does not execute GPU operations, change hashcat code or install apps.

.PARAMETER AndroidNdk
Path to the Android NDK root containing clang, glslc and spirv-val.

.PARAMETER AndroidApi
Android platform API level of the standalone ARM64 executable.

.PARAMETER OutputDirectory
Local artifact directory for one executable and two SPIR-V binaries.
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
$llvm = Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin'
$shaderTools = Join-Path $ndk 'shader-tools\windows-x86_64'
$clang = Join-Path $llvm 'clang.exe'
$glslc = Join-Path $shaderTools 'glslc.exe'
$spirvVal = Join-Path $shaderTools 'spirv-val.exe'

foreach ($tool in @($clang, $glslc, $spirvVal)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "Vulkan compiler or shader validator missing: $tool"
    }
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-staging-20261031'
}
elseif (-not [System.IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory = Join-Path $root $OutputDirectory
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$directory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$binary = 'vulkan-staging-arm64'
$autoSource = Join-Path $PSScriptRoot 'vulkan-auto-config.c'
$policySource = Join-Path $PSScriptRoot 'vulkan-staging-memory.c'
$stagingSource = Join-Path $PSScriptRoot 'vulkan-staging-diagnostic.c'

& $clang "--target=aarch64-linux-android$AndroidApi" -std=gnu11 -O2 -Wall -Wextra -Werror $autoSource $policySource $stagingSource -o (Join-Path $directory $binary) -lvulkan

if ($LASTEXITCODE -ne 0) {
    throw 'Vulkan staging AArch64 compilation failed.'
}

Write-Output 'PASS strict ARM64 Vulkan transfer-mode executable'

$artifacts = @($binary)

foreach ($mode in @('O0', 'O')) {
    $name = "stage-a-$mode-vk1.0.spv"
    $source = Join-Path $PSScriptRoot 'vulkan-pipeline-stage-a.comp'
    $output = Join-Path $directory $name

    & $glslc --target-env=vulkan1.0 "-$mode" -Werror -o $output $source

    if ($LASTEXITCODE -ne 0) {
        throw "Vulkan 1.0 stage-A shader compilation failed: $mode"
    }

    & $spirvVal --target-env vulkan1.0 $output

    if ($LASTEXITCODE -ne 0) {
        throw "Vulkan 1.0 stage-A shader validation failed: $mode"
    }

    Write-Output "PASS Vulkan 1.0 SPIR-V stage-A $mode"

    $artifacts += $name
}

foreach ($name in $artifacts) {
    $file = Join-Path $directory $name
    $hash = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
    $bytes = (Get-Item -LiteralPath $file).Length

    Write-Output "$name bytes=$bytes sha256=$hash"
}

Write-Output 'BUILD COMPLETE: standalone Vulkan 1.0 staging executable and two public synthetic shaders'
