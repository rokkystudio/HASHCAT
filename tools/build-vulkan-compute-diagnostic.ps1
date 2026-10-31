<#
.SYNOPSIS
Builds the standalone Android ARM64 Vulkan Compute diagnostic.

.DESCRIPTION
Compiles two synthetic compute shaders for Vulkan 1.1 with and without
optimization, validates four SPIR-V modules, and builds an ARM64 Vulkan
executable with the specified Android NDK. The script never runs a GPU,
installs an application, or modifies hashcat's modules and OpenCL files.

.PARAMETER AndroidNdk
Exact path to an installed Android NDK with shader-tools and LLVM.

.PARAMETER AndroidApi
Target Android API level used for the standalone ARM64 executable.

.PARAMETER OutputDirectory
Optional directory for locally generated diagnostic artifacts.
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
        throw "Required Android NDK tool is missing: $tool"
    }
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-compute-20261031'
}
elseif (-not [System.IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory = Join-Path $root $OutputDirectory
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$destination = (Resolve-Path $OutputDirectory).Path

$shaderCases = @(
    [pscustomobject]@{
        Source = 'vulkan-compute-diagnostic.comp'
        Name = 'vulkan-compute'
    },
    [pscustomobject]@{
        Source = 'vulkan-compute-shared32k.comp'
        Name = 'vulkan-compute-shared32k'
    }
)

foreach ($case in $shaderCases) {
    $source = Join-Path $PSScriptRoot $case.Source

    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) {
        throw "Synthetic shader source is missing: $source"
    }

    foreach ($optimization in @('O0', 'O')) {
        $output = Join-Path $destination "$($case.Name)-$optimization.spv"

        & $glslc --target-env=vulkan1.1 "-$optimization" -Werror -o $output $source

        if ($LASTEXITCODE -ne 0) {
            throw "Shader compilation failed: $source ($optimization)"
        }

        & $spirvVal --target-env vulkan1.1 $output

        if ($LASTEXITCODE -ne 0) {
            throw "SPIR-V validation failed: $output"
        }

        Write-Output "PASS SPIR-V: $($case.Name) $optimization"
    }
}

$source = Join-Path $PSScriptRoot 'vulkan-compute-diagnostic.c'
$output = Join-Path $destination 'vulkan-compute-diagnostic-arm64'

& $clang "--target=aarch64-linux-android$AndroidApi" -std=gnu11 -O2 -Wall -Wextra -Werror $source -o $output -lvulkan

if ($LASTEXITCODE -ne 0) {
    throw 'Android ARM64 Vulkan executable build failed'
}

Write-Output 'PASS Android ARM64 Vulkan Compute executable'

foreach ($file in @(
    'vulkan-compute-diagnostic-arm64',
    'vulkan-compute-O0.spv',
    'vulkan-compute-O.spv',
    'vulkan-compute-shared32k-O0.spv',
    'vulkan-compute-shared32k-O.spv'
)) {
    $artifact = Join-Path $destination $file
    $sha256 = (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash.ToLowerInvariant()
    $length = (Get-Item -LiteralPath $artifact).Length

    Write-Output "$file bytes=$length sha256=$sha256"
}

Write-Output 'NO GPU EXECUTION OR HASHCAT INSTALLATION PERFORMED BY BUILD SCRIPT'
