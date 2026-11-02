<#
.SYNOPSIS
Build production Vulkan executor API-fault diagnostics for Android ARM.

.DESCRIPTION
Links the unchanged four-stage backend_vulkan_kernel implementation with a
test-only ld.lld --wrap driver for vkCreateBuffer, vkCreateShaderModule,
vkCreateComputePipelines, vkQueueSubmit, vkWaitForFences and vkDeviceWaitIdle.
Only the dedicated diagnostic binaries contain these wrappers. Synthetic
errors exercise production cleanup and same-instance recovery without
enabling HASHCAT Vulkan hashing or inducing a physical GPU failure.

.PARAMETER AndroidNdk
Android NDK containing clang and llvm-readelf.

.PARAMETER AndroidApi
Minimum Android API level; Vulkan requires at least 24.

.PARAMETER OutputDirectory
Directory for ARM64 and ARMv7 diagnostic executables.
#>
param (
    [Parameter(Mandatory = $true)]
    [string]$AndroidNdk,

    [int]$AndroidApi = 24,

    [string]$OutputDirectory = ''
)

$ErrorActionPreference = 'Stop'

if ($AndroidApi -lt 24) {
    throw 'Vulkan diagnostics require Android API 24 or later.'
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
    $OutputDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-backend-public-api-fault-20261102'
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

$wrapFlags = @(
    '-Wl,--wrap=vkCreateBuffer',
    '-Wl,--wrap=vkCreateShaderModule',
    '-Wl,--wrap=vkCreateComputePipelines',
    '-Wl,--wrap=vkQueueSubmit',
    '-Wl,--wrap=vkWaitForFences',
    '-Wl,--wrap=vkDeviceWaitIdle'
)

$sources = @(
    (Join-Path $root 'tools\vulkan-backend-public-api-fault-diagnostic.c'),
    (Join-Path $root 'tools\vulkan-backend-public-api-fault-driver.c'),
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
    $binary = Join-Path $directory "vulkan-backend-public-api-fault-$($item.Abi)"

    & $clang $target $flags $sources $wrapFlags -o $binary -lvulkan

    if ($LASTEXITCODE -ne 0) {
        throw "Vulkan backend API-fault diagnostic failed to link for $($item.Abi)."
    }

    $symbols = @(& $readelf -W --symbols $binary)

    if ($LASTEXITCODE -ne 0) {
        throw "Unable to inspect API-fault symbols for $($item.Abi)."
    }

    foreach ($symbol in @(
        '__wrap_vkCreateBuffer',
        '__wrap_vkCreateShaderModule',
        '__wrap_vkCreateComputePipelines',
        '__wrap_vkQueueSubmit',
        '__wrap_vkWaitForFences',
        '__wrap_vkDeviceWaitIdle'
    )) {
        if (-not ($symbols -match [regex]::Escape($symbol))) {
            throw "Missing test-only linker wrapper $symbol for $($item.Abi)."
        }
    }

    $dynamic = @(& $readelf -W --dynamic $binary)

    if ($LASTEXITCODE -ne 0 -or
        -not ($dynamic -match 'Shared library: \[libvulkan.so\]')) {
        throw "Vulkan loader was not linked for $($item.Abi)."
    }

    $artifacts += $binary

    Write-Output "PASS Android API $AndroidApi $($item.Abi) production Vulkan API-fault diagnostic"
}

foreach ($artifact in $artifacts) {
    $digest = (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash.ToLowerInvariant()
    $size = (Get-Item -LiteralPath $artifact).Length

    Write-Output "BACKEND_API_FAULT_ARTIFACT $(Split-Path $artifact -Leaf) bytes=$size sha256=$digest"
}

Write-Output "PASS $($artifacts.Count) Android production Vulkan API-fault binaries"
