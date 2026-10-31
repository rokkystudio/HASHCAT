<#
.SYNOPSIS
Build explicitly staged Vulkan 1.0 Compute graphs with multiple descriptor sets.

.DESCRIPTION
Compiles a shared C Vulkan 1.0 graph recorder and a public integer
diagnostic for Android API 24 ARM64 and ARMv7. Compiles twelve SPIR-V 1.0
modules covering one/two descriptor sets, three stages and both shader
optimization modes. Validates each module for Vulkan 1.0 and Vulkan 1.1.

The build uses only Vulkan 1.0 required symbols. No GPU vendor preset,
Vulkan 1.1+ extension, password shader or cryptographic kernel is used.

.PARAMETER AndroidNdk
Android NDK with Clang, glslc, spirv-val and llvm-readelf.

.PARAMETER AndroidApi
Android target API level, at least API 24.

.PARAMETER OutputDirectory
Directory containing the exact new executables and SPIR-V variants.
#>
param (
    [Parameter(Mandatory = $true)]
    [string]$AndroidNdk,

    [int]$AndroidApi = 24,

    [string]$OutputDirectory = ''
)

$ErrorActionPreference = 'Stop'

if ($AndroidApi -lt 24) {
    throw 'Android Vulkan requires API 24 or newer.'
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$ndk = (Resolve-Path -LiteralPath $AndroidNdk).Path
$llvm = Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin'
$shaderTools = Join-Path $ndk 'shader-tools\windows-x86_64'
$clang = Join-Path $llvm 'clang.exe'
$readelf = Join-Path $llvm 'llvm-readelf.exe'
$glslc = Join-Path $shaderTools 'glslc.exe'
$validator = Join-Path $shaderTools 'spirv-val.exe'

foreach ($tool in @($clang, $readelf, $glslc, $validator)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "Required Vulkan Android tool not found: $tool"
    }
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-multiset-graph-20261031'
}
elseif (-not [System.IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory = Join-Path $root $OutputDirectory
}

New-Item -Path $OutputDirectory -ItemType Directory -Force | Out-Null
$directory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$artifacts = @()

foreach ($sets in @(1, 2)) {
    foreach ($stage in @(0, 1, 2)) {
        foreach ($optimization in @('O0', 'O')) {
            $name = "multigraph-set$sets-stage$stage-$optimization-vk1.0.spv"
            $path = Join-Path $directory $name

            & $glslc --target-env=vulkan1.0 "-$optimization" -Werror "-DGRAPH_SETS=$sets" "-DGRAPH_STAGE=$stage" -o $path (Join-Path $PSScriptRoot 'vulkan-multiset-graph.comp')

            if ($LASTEXITCODE -ne 0) {
                throw "Vulkan 1.0 SPIR-V compilation failed: $name"
            }

            foreach ($api in @('vulkan1.0', 'vulkan1.1')) {
                & $validator --target-env $api $path

                if ($LASTEXITCODE -ne 0) {
                    throw "SPIR-V does not satisfy $api requirements: $name"
                }
            }

            $artifacts += $name
            Write-Output "PASS SPIR-V 1.0 layout=$sets stage=$stage -$optimization"
        }
    }
}

$sources = @(
    (Join-Path $PSScriptRoot 'vulkan-auto-config.c'),
    (Join-Path $PSScriptRoot 'vulkan-core11-policy.c'),
    (Join-Path $PSScriptRoot 'vulkan-core10-device-catalog.c'),
    (Join-Path $PSScriptRoot 'vulkan-core10-graph-memory.c'),
    (Join-Path $PSScriptRoot 'vulkan-core10-descriptor-policy.c'),
    (Join-Path $PSScriptRoot 'vulkan-core10-multiset.c'),
    (Join-Path $PSScriptRoot 'vulkan-core10-multiset-graph.c'),
    (Join-Path $PSScriptRoot 'vulkan-core10-job-port.c'),
    (Join-Path $PSScriptRoot 'vulkan-core10-multiset-graph-diagnostic.c')
)

foreach ($item in @(
    @{ Abi = 'arm64'; Target = "aarch64-linux-android$AndroidApi" },
    @{ Abi = 'armv7'; Target = "armv7a-linux-androideabi$AndroidApi" }
)) {
    $name = "vulkan-multigraph-$($item.Abi)"
    $output = Join-Path $directory $name

    & $clang ("--target=" + $item.Target) -std=gnu11 -O2 -Wall -Wextra -Werror -I $PSScriptRoot $sources -o $output -lvulkan

    if ($LASTEXITCODE -ne 0) {
        throw "Android API $AndroidApi multi-stage build failed: $($item.Abi)"
    }

    $symbols = @(& $readelf -W --dyn-syms $output)

    if ($LASTEXITCODE -ne 0 -or
        $symbols -match 'UND.*vkEnumerateInstanceVersion' -or
        -not ($symbols -match 'UND.*vkGetInstanceProcAddr')) {
        throw "Android $($item.Abi) uses an incompatible Vulkan loader symbol."
    }

    $artifacts += $name

    Write-Output "PASS Android API $AndroidApi $($item.Abi) no mandatory Vulkan 1.1 symbols"
}

foreach ($name in $artifacts) {
    $path = Join-Path $directory $name
    $size = (Get-Item -LiteralPath $path).Length
    $digest = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()

    Write-Output "MULTIGRAPH_BUILD_ARTIFACT $name bytes=$size sha256=$digest"
}

Write-Output "PASS $($artifacts.Count) Vulkan 1.0 multi-stage multi-set build artifacts"
