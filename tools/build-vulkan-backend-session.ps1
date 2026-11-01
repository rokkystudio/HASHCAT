<#
.SYNOPSIS
Build hashcat backend-owned Vulkan Compute session probes for Android ARM.

.DESCRIPTION
Compiles the production backend.c with Vulkan discovery and session
verification enabled. Links a standalone native Vulkan 1.0 device-session
diagnostic against actual backend_vulkan.c, backend_vulkan_session.c and
public device discovery sources. Tests a real empty Compute command-buffer
submission, without shader or cryptographic execution. Targets Android
API 24 for ARM64/ARMv7 and checks Vulkan 1.0-only loader imports.

The production build flag ENABLE_VULKAN_DISCOVERY=1 links the same
components into a real hashcat backend, without adding a Vulkan
password-processing device or kernel.

.PARAMETER AndroidNdk
Android NDK containing clang and llvm-readelf.

.PARAMETER AndroidApi
Minimum Android API level; Vulkan requires at least 24.

.PARAMETER OutputDirectory
Output directory for the native diagnostics and compiled backend objects.
#>
param (
    [Parameter(Mandatory = $true)]
    [string]$AndroidNdk,

    [int]$AndroidApi = 24,

    [string]$OutputDirectory = ''
)

$ErrorActionPreference = 'Stop'

if ($AndroidApi -lt 24) {
    throw 'Vulkan Compute session verification requires Android API 24 or later.'
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$ndk = (Resolve-Path -LiteralPath $AndroidNdk).Path
$llvm = Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin'
$clang = Join-Path $llvm 'clang.exe'
$readelf = Join-Path $llvm 'llvm-readelf.exe'

foreach ($tool in @($clang, $readelf)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "Android compiler tool not found: $tool"
    }
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-backend-session-20261101'
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
    '-std=gnu99', '-O2', '-Wall', '-Wextra', '-Werror',
    '-Wno-typedef-redefinition', '-DWITH_VULKAN_DISCOVERY'
) + $includes

$diagnosticSources = @(
    (Join-Path $root 'tools\vulkan-backend-session-diagnostic.c'),
    (Join-Path $root 'source\src\backend_vulkan.c'),
    (Join-Path $root 'source\src\backend_vulkan_session.c'),
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
    $object = Join-Path $directory "hashcat-backend-vulkan-$($item.Abi).o"
    $binaryName = "vulkan-backend-session-$($item.Abi)"
    $binary = Join-Path $directory $binaryName

    & $clang $target $flags -fPIC -c (Join-Path $root 'source\src\backend.c') -o $object

    if ($LASTEXITCODE -ne 0) {
        throw "Production HASHCAT backend.c failed to compile for $($item.Abi)."
    }

    $defaultFlags = @($flags | Where-Object { $_ -ne '-DWITH_VULKAN_DISCOVERY' })

    & $clang $target $defaultFlags -fsyntax-only (Join-Path $root 'source\src\backend.c')

    if ($LASTEXITCODE -ne 0) {
        throw "Default Android HASHCAT backend.c compilation failed for $($item.Abi)."
    }

    & $clang $target $flags $diagnosticSources -o $binary -lvulkan

    if ($LASTEXITCODE -ne 0) {
        throw "Vulkan backend session diagnostic failed to link for $($item.Abi)."
    }

    $symbols = @(& $readelf -W --dyn-syms $binary)

    if ($LASTEXITCODE -ne 0 -or
        $symbols -match 'UND.*vkEnumerateInstanceVersion' -or
        -not ($symbols -match 'UND.*vkGetInstanceProcAddr')) {
        throw "Vulkan 1.0 entry-point contract failed for $($item.Abi)."
    }

    $dynamic = @(& $readelf -W --dynamic $binary)

    if ($LASTEXITCODE -ne 0 -or
        -not ($dynamic -match 'Shared library: \[libvulkan.so\]')) {
        throw "Vulkan loader was not linked for $($item.Abi)."
    }

    Write-Output "PASS Android API $AndroidApi $($item.Abi) default hashcat backend.c compilation"
    Write-Output "PASS Android API $AndroidApi $($item.Abi) hashcat backend.c compiled with Vulkan discovery"
    Write-Output "PASS Android API $AndroidApi $($item.Abi) independent Vulkan Compute session linked"

    $artifacts += @($object, $binary)
}

foreach ($artifact in $artifacts) {
    $digest = (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash.ToLowerInvariant()
    $size = (Get-Item -LiteralPath $artifact).Length

    Write-Output "BACKEND_SESSION_ARTIFACT $(Split-Path $artifact -Leaf) bytes=$size sha256=$digest"
}

Write-Output "PASS $($artifacts.Count) actual Android backend and Vulkan Compute session build products"
