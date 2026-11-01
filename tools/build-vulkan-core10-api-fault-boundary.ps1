<#
.SYNOPSIS
Build test-linked Vulkan 1.0 API boundary error diagnostics for Android.

.DESCRIPTION
Compiles ARM64/ARMv7 Android API24 diagnostic executables with an
explicit linker-only --wrap=vkQueueSubmit / --wrap=vkWaitForFences.
Actual public GPU copies pass through to the Vulkan driver. After
the linker test is armed, one selected API call returns a deliberately
test-controlled VkResult to the real buffer transfer function.
The fail-closed policy forbids further submissions and the test
verifies a separately opened healthy VkDevice. No real GPU loss,
HASHCAT scheduler, shader or password processing is involved.

.PARAMETER AndroidNdk
Path to the installed Android NDK with clang and llvm-readelf.

.PARAMETER AndroidApi
Explicit Android minimum API, at least 24.

.PARAMETER OutputDirectory
Optional output directory for the two native API-boundary test ELFs.
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
    $OutputDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-core10-api-fault-boundary-20261101'
}
elseif (-not [System.IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory = Join-Path $root $OutputDirectory
}

New-Item -Path $OutputDirectory -ItemType Directory -Force | Out-Null
$output = (Resolve-Path -LiteralPath $OutputDirectory).Path

$sources = @(
    (Join-Path $root 'tools\vulkan-core10-api-fault-boundary-diagnostic.c'),
    (Join-Path $root 'tools\vulkan-core10-api-fault-driver.c'),
    (Join-Path $root 'tools\vulkan-core10-result-gate.c'),
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
    $destination = Join-Path $output "vulkan-core10-api-fault-boundary-$($item.Abi)"

    $arguments = @(
        $target, '-std=gnu99', '-O2',
        '-Wall', '-Wextra', '-Werror',
        "-I$(Join-Path $root 'tools')"
    ) + $sources + @(
        '-o', $destination, '-lvulkan',
        '-Wl,--wrap=vkQueueSubmit',
        '-Wl,--wrap=vkWaitForFences'
    )

    & $clang @arguments

    if ($LASTEXITCODE -ne 0) {
        throw "Vulkan 1.0 API boundary diagnostic failed to build for $($item.Abi)."
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
        'vkGetFenceStatus',
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

    Write-Output "API_FAULT_BUILD_ARTIFACT $(Split-Path $destination -Leaf) bytes=$bytes sha256=$digest"
}

Write-Output 'PASS two independent Android Vulkan 1.0 API boundary fault executables'
