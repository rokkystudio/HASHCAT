<#
.SYNOPSIS
Build the common-executor Vulkan SHA-256 batch diagnostic for Android ARM.

.DESCRIPTION
Compiles the variable-length SHA-256 Compute shader for Vulkan 1.0, requires
the embedded SPIR-V header to reproduce byte-for-byte, compiles production
backend.c with and without Vulkan discovery, and links a diagnostic against
the common backend_vulkan_kernel dynamic executor. No separate SHA-256 Vulkan
resource owner is linked.

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
    throw 'Vulkan SHA-256 diagnostics require Android API 24 or later.'
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
    $OutputDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-backend-sha256-20261102'
}
elseif (-not [System.IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory = Join-Path $root $OutputDirectory
}

New-Item -Path $OutputDirectory -ItemType Directory -Force | Out-Null
$directory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$module = Join-Path $directory 'vulkan-backend-sha256.spv'
$header = Join-Path $root 'source\include\backend_vulkan_sha256_spirv.h'
$headerSha = (Get-FileHash -LiteralPath $header -Algorithm SHA256).Hash

& $glslang -V --target-env vulkan1.0 -S comp -e main -o $module (Join-Path $root 'tools\vulkan-backend-sha256.comp')

if ($LASTEXITCODE -ne 0) {
    throw 'SHA-256 Compute stage failed Vulkan 1.0 GLSL compilation.'
}

& python -B (Join-Path $root 'tools\generate-vulkan-backend-sha256-kernel.py') --spirv $module --header $header

if ($LASTEXITCODE -ne 0 -or
    (Get-FileHash -LiteralPath $header -Algorithm SHA256).Hash -ne $headerSha) {
    throw 'Embedded SHA-256 SPIR-V bytes are not reproducible.'
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

$diagnosticSources = @(
    (Join-Path $root 'tools\vulkan-backend-sha256-diagnostic.c'),
    (Join-Path $root 'source\src\backend_vulkan.c'),
    (Join-Path $root 'source\src\backend_vulkan_session.c'),
    (Join-Path $root 'source\src\backend_vulkan_kernel.c'),
    (Join-Path $root 'source\src\backend_vulkan_program.c'),
    (Join-Path $root 'tools\vulkan-core10-device-catalog.c'),
    (Join-Path $root 'tools\vulkan-core11-policy.c'),
    (Join-Path $root 'tools\vulkan-auto-config.c')
)

$artifacts = @($module)

foreach ($item in @(
    @{ Abi = 'arm64'; Target = "aarch64-linux-android$AndroidApi" },
    @{ Abi = 'armv7'; Target = "armv7a-linux-androideabi$AndroidApi" }
)) {
    $target = "--target=$($item.Target)"
    $backendObject = Join-Path $directory "hashcat-backend-sha256-$($item.Abi).o"
    $binary = Join-Path $directory "vulkan-backend-sha256-$($item.Abi)"

    & $clang $target $flags -fPIC -c (Join-Path $root 'source\src\backend.c') -o $backendObject

    if ($LASTEXITCODE -ne 0) {
        throw "Production HASHCAT backend.c with Vulkan SHA-256 failed for $($item.Abi)."
    }

    $defaultFlags = @($flags | Where-Object { $_ -ne '-DWITH_VULKAN_DISCOVERY' })

    & $clang $target $defaultFlags -fsyntax-only (Join-Path $root 'source\src\backend.c')

    if ($LASTEXITCODE -ne 0) {
        throw "Default Android HASHCAT backend.c compilation failed for $($item.Abi)."
    }

    & $clang $target $flags $diagnosticSources -o $binary -lvulkan

    if ($LASTEXITCODE -ne 0) {
        throw "Vulkan SHA-256 diagnostic failed to link for $($item.Abi)."
    }

    $dynamic = @(& $readelf -W --dynamic $binary)
    $symbols = @(& $readelf -W --dyn-syms $binary)

    if ($LASTEXITCODE -ne 0 -or
        -not ($dynamic -match 'Shared library: \[libvulkan.so\]') -or
        $symbols -match '__wrap_vk') {
        throw "Vulkan loader contract failed for SHA-256 $($item.Abi)."
    }

    $artifacts += @($backendObject, $binary)

    Write-Output "PASS Android API $AndroidApi $($item.Abi) common-executor SHA-256 diagnostic"
}

foreach ($artifact in $artifacts) {
    $digest = (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash.ToLowerInvariant()
    $size = (Get-Item -LiteralPath $artifact).Length

    Write-Output "BACKEND_SHA256_ARTIFACT $(Split-Path $artifact -Leaf) bytes=$size sha256=$digest"
}

Write-Output 'PASS common-executor Vulkan SHA-256 build products'
