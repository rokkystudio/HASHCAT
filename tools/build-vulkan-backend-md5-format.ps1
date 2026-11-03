<#
.SYNOPSIS
Build the common-executor Vulkan MD5 format batch diagnostic for Android ARM.

.DESCRIPTION
Compiles production backend.c with and without Vulkan discovery and links
six public MD5 message/salt known-answer vectors against the existing Vulkan
MD5 SPIR-V and common dynamic executor. Both Android ARM ABIs are supported.
The diagnostic does not use candidate rules, target hashes or attack scheduling.

.PARAMETER AndroidNdk
Android NDK containing clang and llvm-readelf.

.PARAMETER AndroidApi
Minimum Android API level; Vulkan requires at least 24.

.PARAMETER GlslangCompiler
glslangValidator capable of emitting Vulkan 1.0 SPIR-V.

.PARAMETER OutputDirectory
Directory for SPIR-V and ARM diagnostic executables.
#>
param (
    [Parameter(Mandatory = $true)]
    [string]$AndroidNdk,

    [int]$AndroidApi = 24,

    [Parameter(Mandatory = $true)]
    [string]$GlslangCompiler,

    [string]$OutputDirectory = ''
)

$ErrorActionPreference = 'Stop'

if ($AndroidApi -lt 24) {
    throw 'Vulkan MD5 format diagnostics require Android API 24 or later.'
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$ndk = (Resolve-Path -LiteralPath $AndroidNdk).Path
$glslang = (Resolve-Path -LiteralPath $GlslangCompiler).Path
$llvm = Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin'
$clang = Join-Path $llvm 'clang.exe'
$readelf = Join-Path $llvm 'llvm-readelf.exe'

foreach ($tool in @($clang, $readelf, $glslang)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "Android build tool not found: $tool"
    }
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-backend-md5-format-20261102'
}
elseif (-not [System.IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory = Join-Path $root $OutputDirectory
}

New-Item -Path $OutputDirectory -ItemType Directory -Force | Out-Null
$directory = (Resolve-Path -LiteralPath $OutputDirectory).Path
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

$diagnosticSources = @(
    (Join-Path $root 'tools\vulkan-backend-md5-format-diagnostic.c'),
    (Join-Path $root 'source\src\backend_vulkan.c'),
    (Join-Path $root 'source\src\backend_vulkan_session.c'),
    (Join-Path $root 'source\src\backend_vulkan_kernel.c'),
    (Join-Path $root 'source\src\backend_vulkan_md5_format.c'),
    (Join-Path $root 'source\src\backend_vulkan_program.c'),
    (Join-Path $root 'tools\vulkan-core10-device-catalog.c'),
    (Join-Path $root 'tools\vulkan-core11-policy.c'),
    (Join-Path $root 'tools\vulkan-auto-config.c')
)

$artifacts = @()

foreach ($item in @(
    @{ Abi = 'arm64'; Target = "aarch64-linux-android$AndroidApi" },
    @{ Abi = 'armv7'; Target = "armv7a-linux-androideabi$AndroidApi" }
)) {
    $target = "--target=$($item.Target)"
    $backendObject = Join-Path $directory "hashcat-backend-md5-format-$($item.Abi).o"
    $binary = Join-Path $directory "vulkan-backend-md5-format-$($item.Abi)"

    & $clang $target $flags -fPIC -c (Join-Path $root 'source\src\backend.c') -o $backendObject

    if ($LASTEXITCODE -ne 0) {
        throw "Production HASHCAT backend.c with Vulkan MD5 format failed for $($item.Abi)."
    }

    $defaultFlags = @($flags | Where-Object { $_ -ne '-DWITH_VULKAN_DISCOVERY' })

    & $clang $target $defaultFlags -fsyntax-only (Join-Path $root 'source\src\backend.c')

    if ($LASTEXITCODE -ne 0) {
        throw "Default Android HASHCAT backend.c compilation failed for $($item.Abi)."
    }

    & $clang $target $flags $diagnosticSources -o $binary -lvulkan

    if ($LASTEXITCODE -ne 0) {
        throw "Vulkan MD5 format diagnostic failed to link for $($item.Abi)."
    }

    $dynamic = @(& $readelf -W --dynamic $binary)
    $symbols = @(& $readelf -W --dyn-syms $binary)

    if ($LASTEXITCODE -ne 0 -or
        -not ($dynamic -match 'Shared library: \[libvulkan.so\]') -or
        $symbols -match '__wrap_vk') {
        throw "Vulkan loader contract failed for MD5 format $($item.Abi)."
    }

    $artifacts += @($backendObject, $binary)

    Write-Output "PASS Android API $AndroidApi $($item.Abi) common-executor MD5 format diagnostic"
}

foreach ($artifact in $artifacts) {
    $digest = (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash.ToLowerInvariant()
    $size = (Get-Item -LiteralPath $artifact).Length

    Write-Output "BACKEND_MD5_FORMAT_ARTIFACT $(Split-Path $artifact -Leaf) bytes=$size sha256=$digest"
}

Write-Output 'PASS common-executor Vulkan MD5 format build products'
