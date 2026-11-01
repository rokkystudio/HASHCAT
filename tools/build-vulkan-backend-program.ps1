<#
.SYNOPSIS
Build the host-only Vulkan program-graph validator for Android ARM.

.DESCRIPTION
Compiles source/src/backend_vulkan_program.c together with the dedicated
diagnostic for Android API24 ARM64 and ARMv7. The validator checks only
explicit SPIR-V headers, buffer dependencies and VkPhysicalDeviceLimits
provided by the caller. No Vulkan loader library is linked and no GPU
command can be issued by these executables.

Both ABIs compile with -Wall -Wextra -Werror. The generated binaries
are placed in an ignored diagnostic directory and are intended only
for contract verification before the program graph is wired into the
existing production Compute executor.

.PARAMETER AndroidNdk
Android NDK root containing LLVM Clang.

.PARAMETER AndroidApi
Minimum Android API encoded in the native target triple.

.PARAMETER OutputDirectory
Optional destination for the two diagnostic executables.
#>
param (
    [Parameter(Mandatory = $true)]
    [string]$AndroidNdk,

    [int]$AndroidApi = 24,

    [string]$OutputDirectory = ''
)

$ErrorActionPreference = 'Stop'

if ($AndroidApi -lt 24) {
    throw 'Vulkan program graph validation requires Android API 24 or later.'
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$llvm = Join-Path $AndroidNdk 'toolchains\llvm\prebuilt\windows-x86_64\bin'
$clang = Join-Path $llvm 'clang.exe'
$readelf = Join-Path $llvm 'llvm-readelf.exe'

foreach ($tool in @($clang, $readelf)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "Required Android NDK tool is absent: $tool"
    }
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-backend-program-20261101'
}
elseif (-not [System.IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory = Join-Path $root $OutputDirectory
}

New-Item -Path $OutputDirectory -ItemType Directory -Force | Out-Null
$directory = (Resolve-Path -LiteralPath $OutputDirectory).Path

$specs = @(
    @{ Abi = 'arm64'; Target = "aarch64-linux-android$AndroidApi"; Class = 'ELF64'; Machine = 'AArch64' },
    @{ Abi = 'armv7'; Target = "armv7a-linux-androideabi$AndroidApi"; Class = 'ELF32'; Machine = 'ARM' }
)

foreach ($spec in $specs) {
    $destination = Join-Path $directory "vulkan-backend-program-$($spec.Abi)"

    $arguments = @(
        "--target=$($spec.Target)",
        '-std=gnu99',
        '-Wall',
        '-Wextra',
        '-Werror',
        '-Wno-typedef-redefinition',
        '-fPIE',
        '-pie',
        "-I$(Join-Path $root 'source\include')",
        "-I$(Join-Path $root 'source\OpenCL')",
        (Join-Path $root 'source\src\backend_vulkan_program.c'),
        (Join-Path $root 'tools\vulkan-backend-program-diagnostic.c'),
        '-o',
        $destination
    )

    & $clang @arguments

    if ($LASTEXITCODE -ne 0) {
        throw "Vulkan program graph diagnostic failed to compile for $($spec.Abi)."
    }

    $header = (& $readelf -W --file-header $destination) -join "`n"
    $notes = (& $readelf -W --notes $destination) -join "`n"
    $dynamic = (& $readelf -W --dynamic $destination) -join "`n"

    if ($header -notmatch "Class:\s+$($spec.Class)\b" -or
        $header -notmatch "Machine:\s+$([regex]::Escape($spec.Machine))\b") {
        throw "Unexpected Android ELF architecture for $($spec.Abi)."
    }

    if ($notes -notmatch 'NT_ANDROID_TYPE_IDENT' -or
        $notes -notmatch 'description data: 18 00 00 00') {
        throw "Android API24 note is absent for $($spec.Abi)."
    }

    if ($dynamic -match 'libvulkan\.so') {
        throw "Host-only program graph validator unexpectedly links libvulkan.so for $($spec.Abi)."
    }

    Write-Output "PROGRAM_GRAPH_ARTIFACT abi=$($spec.Abi) sha256=$((Get-FileHash $destination -Algorithm SHA256).Hash) bytes=$((Get-Item $destination).Length)"
}

Write-Output 'PASS two Android API24 program-graph validators without Vulkan loader linkage'
