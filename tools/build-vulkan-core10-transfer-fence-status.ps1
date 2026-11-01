<#
.SYNOPSIS
Build real Vulkan 1.0 fence timeout and GPU-transfer diagnostics for Android.

.DESCRIPTION
Links standalone ARM64/ARMv7 Android API24 executables that poll a
real initially unsignaled VkFence and require driver-returned
VK_NOT_READY and VK_TIMEOUT. Each native session then completes
five public integer GPU copies using the existing Vulkan 1.0
queue/memory and fence synchronization. No Vulkan device loss is
induced, no Vulkan 1.1 loader symbol is imported, and no HASHCAT
kernel, shader or cryptographic workload is executed.

.PARAMETER AndroidNdk
Path to the installed Android NDK with clang and llvm-readelf.

.PARAMETER AndroidApi
Explicit Android minimum API, at least 24.

.PARAMETER OutputDirectory
Optional isolated output directory for the two native fence-status ELFs.
#>
param (
    [Parameter(Mandatory = $true)]
    [string]$AndroidNdk,

    [int]$AndroidApi = 24,

    [string]$OutputDirectory = ''
)

$ErrorActionPreference = 'Stop'

if ($AndroidApi -lt 24) {
    throw 'Vulkan 1.0 mapped-buffer transfers require Android API 24 or later.'
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$ndk = (Resolve-Path -LiteralPath $AndroidNdk).Path
$bin = Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin'
$clang = Join-Path $bin 'clang.exe'
$readelf = Join-Path $bin 'llvm-readelf.exe'

foreach ($tool in @($clang, $readelf)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "Android compiler tool is missing: $tool"
    }
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-core10-transfer-fence-status-20261101'
}
elseif (-not [System.IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory = Join-Path $root $OutputDirectory
}

New-Item -Path $OutputDirectory -ItemType Directory -Force | Out-Null
$output = (Resolve-Path -LiteralPath $OutputDirectory).Path

$sources = @(
    (Join-Path $root 'tools\vulkan-core10-transfer-fence-status-diagnostic.c'),
    (Join-Path $root 'tools\vulkan-core10-transfer-session.c'),
    (Join-Path $root 'tools\vulkan-core10-device-catalog.c'),
    (Join-Path $root 'tools\vulkan-core11-policy.c'),
    (Join-Path $root 'tools\vulkan-auto-config.c')
)

foreach ($item in @(
    @{ Abi = 'arm64'; Target = "aarch64-linux-android$AndroidApi" },
    @{ Abi = 'armv7'; Target = "armv7a-linux-androideabi$AndroidApi" }
)) {
    $target = "--target=$($item.Target)"
    $destination = Join-Path $output "vulkan-core10-transfer-fence-status-$($item.Abi)"

    $arguments = @(
        $target, '-std=gnu99', '-O2',
        '-Wall', '-Wextra', '-Werror',
        "-I$(Join-Path $root 'tools')"
    ) + $sources + @('-o', $destination, '-lvulkan')

    & $clang @arguments

    if ($LASTEXITCODE -ne 0) {
        throw "Vulkan 1.0 real fence-status diagnostic failed to build for $($item.Abi)."
    }

    $header = @(& $readelf -W --file-header $destination)

    if ($LASTEXITCODE -ne 0) {
        throw "Android ELF header could not be inspected for $($item.Abi)."
    }

    $expectedClass = if ($item.Abi -eq 'arm64') { 'ELF64' } else { 'ELF32' }
    $expectedMachine = if ($item.Abi -eq 'arm64') { 'AArch64' } else { 'ARM' }

    if (-not ($header -match "Class:\s+$expectedClass") -or
        -not ($header -match "Machine:\s+$expectedMachine")) {
        throw "Unexpected Android ABI or ELF class for $($item.Abi)."
    }

    $notes = @(& $readelf -W --notes $destination)

    if ($LASTEXITCODE -ne 0 -or
        -not ($notes -match 'NT_ANDROID_TYPE_IDENT') -or
        -not ($notes -match 'description data: 18 00 00 00')) {
        throw "Android minimum API 24 ELF note is missing for $($item.Abi)."
    }

    $symbols = @(& $readelf -W --dyn-syms $destination)

    if ($LASTEXITCODE -ne 0 -or
        $symbols -match 'UND.*vkEnumerateInstanceVersion') {
        throw "Vulkan 1.0 loader symbol contract failed for $($item.Abi)."
    }

    foreach ($method in @(
        'vkGetInstanceProcAddr',
        'vkCreateDevice',
        'vkCreateBuffer',
        'vkAllocateMemory',
        'vkCmdCopyBuffer',
        'vkQueueSubmit',
        'vkWaitForFences',
        'vkFlushMappedMemoryRanges',
        'vkInvalidateMappedMemoryRanges'
    )) {
        if (-not ($symbols -match "UND.*$method")) {
            throw "The Vulkan 1.0 import $method is missing for $($item.Abi)."
        }
    }

    $dynamic = @(& $readelf -W --dynamic $destination)

    if ($LASTEXITCODE -ne 0 -or
        -not ($dynamic -match 'Shared library: \[libvulkan.so\]')) {
        throw "Vulkan loader was not linked for $($item.Abi)."
    }

    Write-Output "PASS Android API $AndroidApi $($item.Abi) standalone Vulkan transfer ELF"
    Write-Output "PASS exact Vulkan 1.0 import and host-memory synchronization symbols $($item.Abi)"

    $digest = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash.ToLowerInvariant()
    $bytes = (Get-Item -LiteralPath $destination).Length

    Write-Output "FENCE_BUILD_ARTIFACT $(Split-Path $destination -Leaf) bytes=$bytes sha256=$digest"
}

Write-Output 'PASS two independent Android Vulkan 1.0 real fence-status executables'
