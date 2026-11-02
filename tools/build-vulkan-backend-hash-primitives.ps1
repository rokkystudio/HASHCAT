<#
.SYNOPSIS
Compile SHA-1, SHA-224 and BLAKE2s-256 Vulkan Compute modules for Android.

.DESCRIPTION
Compiles the production byte-message GPU shaders for Vulkan 1.0 and embeds
canonical SPIR-V 1.0 arrays. Requires every generated header to match the
existing embedded bytes. Compiles the common dynamic Vulkan executor for
Android ARM64 and ARMv7 with warnings treated as errors.

This build does not execute a password attack, Hashcat scheduler workload
or hardware test matrix.

.PARAMETER AndroidNdk
Android NDK containing clang and llvm-readelf.

.PARAMETER GlslangCompiler
glslangValidator executable with Vulkan 1.0 SPIR-V support.

.PARAMETER AndroidApi
Minimum Android API level; must be at least 24.

.PARAMETER OutputDirectory
Directory for intermediate SPIR-V modules and ARM object files.
#>
param (
    [Parameter(Mandatory = $true)]
    [string]$AndroidNdk,

    [Parameter(Mandatory = $true)]
    [string]$GlslangCompiler,

    [int]$AndroidApi = 24,

    [string]$OutputDirectory = ''
)

$ErrorActionPreference = 'Stop'

if ($AndroidApi -lt 24) {
    throw 'Vulkan Compute requires Android API 24 or later.'
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$ndk = (Resolve-Path -LiteralPath $AndroidNdk).Path
$glslang = (Resolve-Path -LiteralPath $GlslangCompiler).Path
$llvm = Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin'
$clang = Join-Path $llvm 'clang.exe'
$readelf = Join-Path $llvm 'llvm-readelf.exe'

foreach ($tool in @($clang, $readelf, $glslang)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "Android compilation tool not found: $tool"
    }
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-hash-primitives'
}
elseif (-not [System.IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory = Join-Path $root $OutputDirectory
}

New-Item -Path $OutputDirectory -ItemType Directory -Force | Out-Null
$directory = (Resolve-Path -LiteralPath $OutputDirectory).Path

foreach ($algorithm in @('sha1', 'sha224', 'blake2s')) {
    $source = Join-Path $root "tools\vulkan-backend-$algorithm.comp"
    $header = Join-Path $root "source\include\backend_vulkan_$($algorithm)_spirv.h"
    $module = Join-Path $directory "vulkan-backend-$algorithm.spv"
    $expected = (Get-FileHash -LiteralPath $header -Algorithm SHA256).Hash

    & $glslang -V --target-env vulkan1.0 -S comp -e main -o $module $source

    if ($LASTEXITCODE -ne 0) {
        throw "Vulkan 1.0 shader compilation failed: $algorithm"
    }

    & python -B (Join-Path $root 'tools\generate-vulkan-backend-hash-kernel.py') --algorithm $algorithm --spirv $module --header $header

    if ($LASTEXITCODE -ne 0 -or
        (Get-FileHash -LiteralPath $header -Algorithm SHA256).Hash -ne $expected) {
        throw "Embedded SPIR-V bytes differ from the compiled $algorithm module."
    }

    Write-Output "PASS reproducible Vulkan SPIR-V 1.0 $algorithm module"
}

$includes = @(
    "-I$(Join-Path $root 'source\include')",
    "-I$(Join-Path $root 'source\OpenCL')",
    "-I$(Join-Path $root 'source\deps\LZMA-SDK\C')",
    "-I$(Join-Path $root 'source\deps\zlib')",
    "-I$(Join-Path $root 'source\deps\zlib\contrib')",
    "-I$(Join-Path $root 'source\deps\OpenCL-Headers')",
    "-I$(Join-Path $root 'tools')"
)

$flags = @(
    '-std=gnu99',
    '-O2',
    '-Wall',
    '-Wextra',
    '-Werror',
    '-Wno-typedef-redefinition',
    '-DWITH_VULKAN_DISCOVERY'
) + $includes

foreach ($target in @(
    @{ Abi = 'arm64'; Triple = "aarch64-linux-android$AndroidApi" },
    @{ Abi = 'armv7'; Triple = "armv7a-linux-androideabi$AndroidApi" }
)) {
    $object = Join-Path $directory "backend-vulkan-hash-$($target.Abi).o"

    & $clang "--target=$($target.Triple)" $flags -fPIC -c (Join-Path $root 'source\src\backend_vulkan_kernel.c') -o $object

    if ($LASTEXITCODE -ne 0) {
        throw "Common Vulkan hash executor compilation failed for $($target.Abi)."
    }

    $symbols = @(& $readelf -W --symbols $object)

    if ($LASTEXITCODE -ne 0 -or
        -not ($symbols -match 'backend_vulkan_hash_batch') -or
        -not ($symbols -match 'backend_vulkan_sha256_hash_batch')) {
        throw "Common Vulkan hash API symbols missing for $($target.Abi)."
    }

    Write-Output "PASS Android API $AndroidApi $($target.Abi) SHA-1/SHA-224/SHA-256/BLAKE2s-256 executor"
    Write-Output "HASH_CORE_ARTIFACT $($target.Abi) sha256=$((Get-FileHash -LiteralPath $object -Algorithm SHA256).Hash.ToLowerInvariant())"
}

Write-Output 'PASS Vulkan hash primitive compilation for both Android ARM ABIs'
