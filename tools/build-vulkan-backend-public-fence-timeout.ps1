<#
.SYNOPSIS
Build real Vulkan fence-timeout diagnostics for Android ARM.

.DESCRIPTION
Links the production four-stage Vulkan executor with a dedicated diagnostic
that submits public Compute work without associating the owned fence, then
observes the real driver's VK_TIMEOUT from a zero-duration vkWaitForFences.
No Vulkan entry point is wrapped or replaced. The production cleanup waits for
device idle before the same-instance recovery device is created.

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
    $OutputDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-backend-public-fence-timeout-20261102'
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

$sources = @(
    (Join-Path $root 'tools\vulkan-backend-public-fence-timeout-diagnostic.c'),
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
    $binary = Join-Path $directory "vulkan-backend-public-fence-timeout-$($item.Abi)"

    & $clang $target $flags $sources -o $binary -lvulkan

    if ($LASTEXITCODE -ne 0) {
        throw "Vulkan fence-timeout diagnostic failed to link for $($item.Abi)."
    }

    $dynamic = @(& $readelf -W --dynamic $binary)

    if ($LASTEXITCODE -ne 0 -or
        -not ($dynamic -match 'Shared library: \[libvulkan.so\]')) {
        throw "Vulkan loader was not linked for $($item.Abi)."
    }

    $symbols = @(& $readelf -W --dyn-syms $binary)

    if ($LASTEXITCODE -ne 0 -or
        $symbols -match '__wrap_vk' -or
        -not ($symbols -match 'UND.*vkWaitForFences') -or
        -not ($symbols -match 'UND.*vkDeviceWaitIdle')) {
        throw "Fence-timeout diagnostic unexpectedly wraps or omits Vulkan loader calls for $($item.Abi)."
    }

    $artifacts += $binary

    Write-Output "PASS Android API $AndroidApi $($item.Abi) real Vulkan fence-timeout diagnostic"
}

foreach ($artifact in $artifacts) {
    $digest = (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash.ToLowerInvariant()
    $size = (Get-Item -LiteralPath $artifact).Length

    Write-Output "BACKEND_FENCE_TIMEOUT_ARTIFACT $(Split-Path $artifact -Leaf) bytes=$size sha256=$digest"
}

Write-Output "PASS $($artifacts.Count) Android real Vulkan fence-timeout binaries"
