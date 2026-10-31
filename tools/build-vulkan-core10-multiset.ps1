<#
.SYNOPSIS
Builds synthetic Vulkan 1.0 multi-set uniform/storage Compute diagnostics.

.DESCRIPTION
Builds Android API 24 ARM64 and ARMv7 resource owners using only Vulkan
1.0 symbols. Compiles two public GLSL variants with one/two descriptor
sets and two descriptor types to SPIR-V 1.0 with and without optimization.
Validates every shader with both Vulkan 1.0 and Vulkan 1.1 validators.

Builds native host C descriptor-policy tests with GCC -O0 and -O2 and
records original pass/negative-control logs without linking a GPU driver.
The host linker writes a zero PE timestamp for reproducible SHA-256 binaries.

.PARAMETER AndroidNdk
Android NDK installation with Clang, glslc, spirv-val and LLVM readelf.

.PARAMETER HostGcc
Native Windows GCC used only for independent CPU descriptor policy tests.

.PARAMETER AndroidApi
Minimum Android target API; Vulkan requires API 24 or newer.

.PARAMETER OutputDirectory
Root of independently archived local executable, SPIR-V and test outputs.
#>
param (
    [Parameter(Mandatory = $true)]
    [string]$AndroidNdk,

    [string]$HostGcc = 'gcc.exe',

    [int]$AndroidApi = 24,

    [string]$OutputDirectory = ''
)

$ErrorActionPreference = 'Stop'

if ($AndroidApi -lt 24) {
    throw 'Vulkan requires Android API level 24 or newer.'
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$ndk = (Resolve-Path -LiteralPath $AndroidNdk).Path
$llvm = Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin'
$shaderTools = Join-Path $ndk 'shader-tools\windows-x86_64'
$include = Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64\sysroot\usr\include'
$clang = Join-Path $llvm 'clang.exe'
$readelf = Join-Path $llvm 'llvm-readelf.exe'
$glslc = Join-Path $shaderTools 'glslc.exe'
$validator = Join-Path $shaderTools 'spirv-val.exe'

if (Test-Path -LiteralPath $HostGcc -PathType Leaf) {
    $gcc = (Resolve-Path -LiteralPath $HostGcc).Path
}
else {
    $gcc = (Get-Command $HostGcc -ErrorAction Stop).Source
}

$objdump = Join-Path (Split-Path -Parent $gcc) 'objdump.exe'

foreach ($tool in @($clang, $readelf, $glslc, $validator, $gcc, $objdump)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "Vulkan 1.0 multi-set build tool not found: $tool"
    }
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-multiset-20261031'
}
elseif (-not [System.IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory = Join-Path $root $OutputDirectory
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$directory = (Resolve-Path -LiteralPath $OutputDirectory).Path

$policy = Join-Path $PSScriptRoot 'vulkan-core10-descriptor-policy.c'
$policyTest = Join-Path $PSScriptRoot 'test-vulkan-core10-descriptor-policy-host.c'
$expected = @(
    'PASS_DESCRIPTOR_ONE_SET sparse_storage_and_uniform',
    'PASS_DESCRIPTOR_TWO_SETS independent_layouts_and_types',
    'PASS_DESCRIPTOR_LIMITS bound_sets_stage_resources_storage_uniform',
    'PASS_DESCRIPTOR_RANGES unsupported_type_wrong_usage_alignment_length',
    'PASS_DESCRIPTOR_STRUCTURE duplicate_empty_set_invalid_set_empty_input',
    'PASS_DESCRIPTOR_POLICY no_vulkan_driver no_model_profiles'
)

$reference = $null

foreach ($optimization in @('O0', 'O2')) {
    $exe = Join-Path $directory "multiset-descriptor-host-$optimization.exe"

    & $gcc -std=c11 "-$optimization" -Wall -Wextra -Werror -Wconversion -Wshadow '-Wl,--no-insert-timestamp' -I $PSScriptRoot -idirafter $include $policy $policyTest -o $exe

    if ($LASTEXITCODE -ne 0) {
        throw "Native descriptor policy compiler rejected -$optimization."
    }

    $libraries = @(
        (& $objdump -p $exe) |
            Select-String '^\s*DLL Name:\s*(\S+)\s*$' |
            ForEach-Object { $_.Matches[0].Groups[1].Value }
    )

    if ($LASTEXITCODE -ne 0 -or $libraries.Count -eq 0) {
        throw "Cannot verify driver-free CPU policy dependencies: -$optimization"
    }

    foreach ($library in $libraries) {
        if ($library -match '(?i)vulkan|opencl') {
            throw "Standalone descriptor test unexpectedly links GPU library: $library"
        }
    }

    $lines = @(& $exe 2>&1)

    if ($LASTEXITCODE -ne 0) {
        $lines | ForEach-Object { Write-Output $_ }

        throw "Native Vulkan descriptor policy test failed: -$optimization"
    }

    $passes = @($lines | Where-Object { $_ -match '^PASS_DESCRIPTOR_' })

    if ($passes.Count -ne $expected.Count) {
        throw "Native Vulkan descriptor test count mismatch: -$optimization"
    }

    for ($index = 0; $index -lt $expected.Count; $index++) {
        if ($passes[$index] -cne $expected[$index]) {
            throw "Native Vulkan descriptor policy failed requirement: $($expected[$index])"
        }
    }

    if ($null -ne $reference -and
        $reference -cne ($lines -join "`n")) {
        throw 'Native descriptor policy results differ between -O0 and -O2.'
    }

    $reference = $lines -join "`n"

    [System.IO.File]::WriteAllText(
        (Join-Path $directory "multiset-descriptor-host-$optimization.log"),
        ((@(
            'TEST=vulkan-core10-multiset-host-policy',
            "OPTIMIZATION=$optimization",
            'GPU_DRIVER_LINKED=0',
            'EXIT_CODE=0'
        ) + $lines) -join "`n") + "`n",
        [System.Text.UTF8Encoding]::new($false)
    )

    Write-Output "PASS native Vulkan one/two-set storage/uniform descriptor policy -$optimization"
}

foreach ($variant in @('one', 'two')) {
    foreach ($optimization in @('O0', 'O')) {
        $shader = Join-Path $PSScriptRoot "vulkan-multiset-$variant.comp"
        $output = Join-Path $directory "multiset-$variant-$optimization-vk1.0.spv"

        & $glslc --target-env=vulkan1.0 "-$optimization" -Werror -o $output $shader

        if ($LASTEXITCODE -ne 0) {
            throw "Vulkan synthetic multi-set shader compile failed: $variant -$optimization"
        }

        foreach ($target in @('vulkan1.0', 'vulkan1.1')) {
            & $validator --target-env $target $output

            if ($LASTEXITCODE -ne 0) {
                throw "SPIR-V 1.0 multi-set shader $variant -$optimization invalid for $target"
            }
        }

        Write-Output "PASS $variant descriptor layout -$optimization SPIR-V valid under Vulkan 1.0 and 1.1"
    }
}

$sources = @(
    (Join-Path $PSScriptRoot 'vulkan-auto-config.c'),
    (Join-Path $PSScriptRoot 'vulkan-core11-policy.c'),
    (Join-Path $PSScriptRoot 'vulkan-core10-graph-memory.c'),
    (Join-Path $PSScriptRoot 'vulkan-core10-descriptor-policy.c'),
    (Join-Path $PSScriptRoot 'vulkan-core10-multiset.c'),
    (Join-Path $PSScriptRoot 'vulkan-core10-multiset-diagnostic.c')
)

foreach ($item in @(
    @{ Abi = 'arm64'; Target = "aarch64-linux-android$AndroidApi" },
    @{ Abi = 'armv7'; Target = "armv7a-linux-androideabi$AndroidApi" }
)) {
    $exe = Join-Path $directory "vulkan-multiset-$($item.Abi)"

    & $clang ("--target=" + $item.Target) -std=gnu11 -O2 -Wall -Wextra -Werror -I $PSScriptRoot $sources -o $exe -lvulkan

    if ($LASTEXITCODE -ne 0) {
        throw "Vulkan 1.0 multi-set Android $AndroidApi executable failed: $($item.Target)"
    }

    $symbols = @(& $readelf -W --dyn-syms $exe)

    if ($LASTEXITCODE -ne 0 -or
        $symbols -match 'UND.*vkEnumerateInstanceVersion' -or
        -not ($symbols -match 'UND.*vkGetInstanceProcAddr')) {
        throw "Multi-set executable imports unsupported Vulkan loader symbols: $($item.Abi)"
    }

    Write-Output "PASS Vulkan 1.0 multi-set storage/uniform Android $AndroidApi $($item.Abi) ABI"
}

$artifacts = @(
    'vulkan-multiset-arm64',
    'vulkan-multiset-armv7',
    'multiset-descriptor-host-O0.exe',
    'multiset-descriptor-host-O2.exe'
)

foreach ($variant in @('one', 'two')) {
    foreach ($optimization in @('O0', 'O')) {
        $artifacts += "multiset-$variant-$optimization-vk1.0.spv"
    }
}

foreach ($name in $artifacts) {
    $file = Join-Path $directory $name
    $size = (Get-Item -LiteralPath $file).Length
    $digest = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()

    Write-Output "MULTI_BUILD_ARTIFACT $name bytes=$size sha256=$digest"
}

Write-Output 'PASS exact one/two descriptor set Vulkan 1.0 Android API24 and host policy build'
