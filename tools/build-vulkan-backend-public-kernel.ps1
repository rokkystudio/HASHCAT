<#
.SYNOPSIS
Build the first real hashcat backend-owned public SPIR-V Compute kernel.

.DESCRIPTION
Compiles the actual production HASHCAT backend.c in default and opt-in
Vulkan modes and links a standalone diagnostic with the exact
backend_vulkan_kernel_verify implementation included in the full
HASHCAT Android executable. The embedded SPIR-V 1.0 shader dispatches
two 32-thread workgroups on public integers and verifies all 64
results independently on CPU. No cryptographic or password kernel is
loaded, and the ordinary HASHCAT scheduler is unchanged.

.PARAMETER AndroidNdk
Android NDK containing clang and llvm-readelf.

.PARAMETER AndroidApi
Minimum Android API level; Vulkan requires at least 24.

.PARAMETER OutputDirectory
Output directory for the native public-kernel diagnostics and backend objects.
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
    $OutputDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-backend-public-kernel-20261101'
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
    (Join-Path $root 'tools\vulkan-backend-public-kernel-diagnostic.c'),
    (Join-Path $root 'source\src\backend_vulkan.c'),
    (Join-Path $root 'source\src\backend_vulkan_session.c'),
    (Join-Path $root 'source\src\backend_vulkan_kernel.c'),
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
    $object = Join-Path $directory "hashcat-backend-vulkan-$($item.Abi).o"
    $binaryName = "vulkan-backend-public-kernel-$($item.Abi)"
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
        throw "Vulkan backend public SPIR-V kernel diagnostic failed to link for $($item.Abi)."
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
    Write-Output "PASS Android API $AndroidApi $($item.Abi) real Vulkan 1.0 SPIR-V Compute kernel linked"

    $artifacts += @($object, $binary)
}

foreach ($artifact in $artifacts) {
    $digest = (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash.ToLowerInvariant()
    $size = (Get-Item -LiteralPath $artifact).Length

    Write-Output "BACKEND_KERNEL_ARTIFACT $(Split-Path $artifact -Leaf) bytes=$size sha256=$digest"
}

Write-Output "PASS $($artifacts.Count) actual Android backend and Vulkan public Compute kernel build products"
