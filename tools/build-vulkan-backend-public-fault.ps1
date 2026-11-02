<#
.SYNOPSIS
Build controlled public Vulkan resource cleanup and recovery diagnostics.

.DESCRIPTION
Compiles actual HASHCAT backend.c with and without Vulkan discovery,
then links the full production four-stage Compute executor with a
dedicated fault diagnostic for Android ARM64 and ARMv7. The diagnostic
injects an explicit host initialization error at each of 37 resource
acquisition/submission checkpoints, closes all acquired resources and
reopens a new working Vulkan Compute device under the same VkInstance.
No Vulkan driver error is forged or physical GPU device-loss induced.
The recovery execution processes only public test integers; no HASHCAT
hashing, password candidate or scheduler work is invoked.

.PARAMETER AndroidNdk
Android NDK containing clang and llvm-readelf.

.PARAMETER AndroidApi
Minimum Android API level; Vulkan requires at least 24.

.PARAMETER GlslangCompiler
Explicit GLSL compiler path capable of emitting Vulkan 1.0 SPIR-V.

.PARAMETER OutputDirectory
Output directory for the native public-kernel diagnostics and backend objects.
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
    throw 'Vulkan Compute session verification requires Android API 24 or later.'
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$ndk = (Resolve-Path -LiteralPath $AndroidNdk).Path
$llvm = Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin'
$clang = Join-Path $llvm 'clang.exe'
$readelf = Join-Path $llvm 'llvm-readelf.exe'
$glslang = (Resolve-Path -LiteralPath $GlslangCompiler).Path

foreach ($tool in @($clang, $readelf, $glslang)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "Android compiler tool not found: $tool"
    }
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-backend-public-fault-20261102'
}
elseif (-not [System.IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory = Join-Path $root $OutputDirectory
}

New-Item -Path $OutputDirectory -ItemType Directory -Force | Out-Null
$directory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$firstModule = Join-Path $directory 'vulkan-backend-public-stage1.spv'
$firstHeader = Join-Path $root 'source\include\backend_vulkan_public_spirv.h'
$firstHeaderSha = (Get-FileHash -LiteralPath $firstHeader -Algorithm SHA256).Hash

& $glslang -V --target-env vulkan1.0 -S comp -e main -o $firstModule (Join-Path $root 'tools\vulkan-backend-public-kernel.comp')

if ($LASTEXITCODE -ne 0) {
    throw 'First public Compute stage failed Vulkan 1.0 GLSL compilation.'
}

& python -B (Join-Path $root 'tools\generate-vulkan-backend-public-kernel.py') --spirv $firstModule --header $firstHeader

if ($LASTEXITCODE -ne 0 -or (Get-FileHash -LiteralPath $firstHeader -Algorithm SHA256).Hash -ne $firstHeaderSha) {
    throw 'Embedded first-stage SPIR-V bytes are not reproducible.'
}

$compiled = Join-Path $directory 'vulkan-backend-public-stage2.spv'
$generated = Join-Path $root 'source\include\backend_vulkan_public_stage2_spirv.h'
$before = (Get-FileHash -LiteralPath $generated -Algorithm SHA256).Hash

& $glslang -V --target-env vulkan1.0 -S comp -e main -o $compiled (Join-Path $root 'tools\vulkan-backend-public-stage2.comp')

if ($LASTEXITCODE -ne 0) {
    throw 'Second public Compute stage failed Vulkan 1.0 GLSL compilation.'
}

& python -B (Join-Path $root 'tools\generate-vulkan-backend-public-kernel.py') --spirv $compiled --header $generated --symbol backend_vulkan_public_stage2_spirv --guard HC_BACKEND_VULKAN_PUBLIC_STAGE2_SPIRV_H --source tools/vulkan-backend-public-stage2.comp

if ($LASTEXITCODE -ne 0 -or (Get-FileHash -LiteralPath $generated -Algorithm SHA256).Hash -ne $before) {
    throw 'Embedded second-stage SPIR-V bytes are not reproducible.'
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
    '-std=gnu99', '-O2', '-Wall', '-Wextra', '-Werror',
    '-Wno-typedef-redefinition', '-DWITH_VULKAN_DISCOVERY'
) + $includes

$diagnosticSources = @(
    (Join-Path $root 'tools\vulkan-backend-public-fault-diagnostic.c'),
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
    $binaryName = "vulkan-backend-public-fault-$($item.Abi)"
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
        throw "Vulkan backend controlled-resource fault diagnostic failed to link for $($item.Abi)."
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

    Write-Output "BACKEND_FAULT_ARTIFACT $(Split-Path $artifact -Leaf) bytes=$size sha256=$digest"
}

Write-Output "PASS $($artifacts.Count) Android backend and controlled Vulkan four-stage cleanup/recovery build products"
