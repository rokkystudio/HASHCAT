<#
.SYNOPSIS
Builds Vulkan 1.0 variable-size synthetic Compute graph diagnostics.

.DESCRIPTION
Compiles a dynamically allocated Vulkan memory planner and graph owner for
Android API 24 ARM64 and ARMv7, with three existing public integer shaders.
Builds all shaders as validated SPIR-V 1.0 with and without optimization.

Also runs the independent host-native C graph-memory planner for 4, 5, 6
and 7 buffers in both -O0 and -O2, checking candidate backtracking, explicit
noncoherent requirements and unsupported memory properties without a GPU.

.PARAMETER AndroidNdk
Android NDK root with Clang and shader-tools.

.PARAMETER HostGcc
Native Windows GCC used for the CPU-only memory policy tests.

.PARAMETER AndroidApi
Android native API level; minimum permitted is API 24.

.PARAMETER OutputDirectory
Build directory for two Android native programs, six SPIR-V binaries,
two native CPU programs and two host test logs.
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
    throw 'Android Vulkan starts at API 24; lower levels cannot be targeted.'
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$ndk = (Resolve-Path -LiteralPath $AndroidNdk).Path
$llvm = Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin'
$shaderTools = Join-Path $ndk 'shader-tools\windows-x86_64'
$include = Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64\sysroot\usr\include'
$clang = Join-Path $llvm 'clang.exe'
$readelf = Join-Path $llvm 'llvm-readelf.exe'
$glslc = Join-Path $shaderTools 'glslc.exe'
$spirvVal = Join-Path $shaderTools 'spirv-val.exe'

if (Test-Path -LiteralPath $HostGcc -PathType Leaf) {
    $gcc = (Resolve-Path -LiteralPath $HostGcc).Path
}
else {
    $gcc = (Get-Command $HostGcc -ErrorAction Stop).Source
}

$objdump = Join-Path (Split-Path -Parent $gcc) 'objdump.exe'

foreach ($component in @($clang, $readelf, $glslc, $spirvVal, $gcc, $objdump)) {
    if (-not (Test-Path -LiteralPath $component -PathType Leaf)) {
        throw "Required Vulkan graph tool unavailable: $component"
    }
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-dynamic-graph-20261031'
}
elseif (-not [System.IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory = Join-Path $root $OutputDirectory
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$directory = (Resolve-Path -LiteralPath $OutputDirectory).Path

$planner = Join-Path $PSScriptRoot 'vulkan-core10-graph-memory.c'
$plannerTest = Join-Path $PSScriptRoot 'test-vulkan-core10-graph-memory-host.c'
$expected = @(
    'PASS_GRAPH_VARIABLE count=4 gpu=2 mapped=2',
    'PASS_GRAPH_VARIABLE count=5 gpu=3 mapped=2',
    'PASS_GRAPH_VARIABLE count=6 gpu=4 mapped=2',
    'PASS_GRAPH_VARIABLE count=7 gpu=5 mapped=2',
    'PASS_GRAPH_BACKTRACK six_buffers_exact_heap_capacity',
    'PASS_GRAPH_NONCOHERENT valid_and_explicit_type_rejection',
    'PASS_GRAPH_NEGATIVE allocation_limit_alignment_capacity_heap_zero_count',
    'PASS_GRAPH_MEMORY_POLICY dynamic_4_5_6_7 no_vulkan_driver'
)

$previous = $null

foreach ($mode in @('O0', 'O2')) {
    $exe = Join-Path $directory "vulkan-graph-host-$mode.exe"

    & $gcc -std=c11 "-$mode" -Wall -Wextra -Werror -Wconversion -Wshadow -I $PSScriptRoot -idirafter $include $planner $plannerTest -o $exe

    if ($LASTEXITCODE -ne 0) {
        throw "Dynamic Vulkan heap planner strict build failed: $mode"
    }

    $libraries = @(
        (& $objdump -p $exe) |
            Select-String '^\s*DLL Name:\s*(\S+)\s*$' |
            ForEach-Object { $_.Matches[0].Groups[1].Value }
    )

    if ($LASTEXITCODE -ne 0 -or $libraries.Count -eq 0) {
        throw "Cannot audit independent host planner dependencies: $mode"
    }

    foreach ($library in $libraries) {
        if ($library -match '(?i)vulkan|opencl') {
            throw "Host allocation-policy test linked a GPU driver: $library"
        }
    }

    $lines = @(& $exe 2>&1)

    if ($LASTEXITCODE -ne 0) {
        $lines | ForEach-Object { Write-Output $_ }

        throw "Independent Vulkan dynamic memory C unit tests failed: $mode"
    }

    $passes = @($lines | Where-Object { $_ -match '^PASS_GRAPH_' })

    if ($passes.Count -ne $expected.Count) {
        throw "Dynamic memory C policy pass count incorrect: $mode"
    }

    for ($i = 0; $i -lt $expected.Count; $i++) {
        if ($passes[$i] -cne $expected[$i]) {
            throw "Host dynamic memory exact pass mismatch: $($expected[$i])"
        }
    }

    if ($null -ne $previous -and
        $previous -cne ($lines -join "`n")) {
        throw 'Dynamic Vulkan memory planning differs between compiler optimization modes.'
    }

    $previous = $lines -join "`n"

    [System.IO.File]::WriteAllText(
        (Join-Path $directory "vulkan-graph-host-$mode.log"),
        ((@(
            'TEST=vulkan-core10-dynamic-graph-host',
            "OPTIMIZATION=$mode",
            'GPU_DRIVER_LINKED=0',
            'EXIT_CODE=0'
        ) + $lines) -join "`n") + "`n",
        [System.Text.UTF8Encoding]::new($false)
    )

    Write-Output "PASS variable-size Vulkan native memory planner -$mode"
}

foreach ($mode in @('O0', 'O')) {
    foreach ($stage in @('a', 'b', 'c')) {
        $source = Join-Path $PSScriptRoot "vulkan-pipeline-stage-$stage.comp"
        $output = Join-Path $directory "stage-$stage-$mode-vk1.0.spv"

        & $glslc --target-env=vulkan1.0 "-$mode" -Werror -o $output $source

        if ($LASTEXITCODE -ne 0) {
            throw "Synthetic graph stage $stage -$mode SPIR-V 1.0 compilation failed"
        }

        foreach ($target in @('vulkan1.0', 'vulkan1.1')) {
            & $spirvVal --target-env $target $output

            if ($LASTEXITCODE -ne 0) {
                throw "SPIR-V 1.0 module incompatible with ${target}: stage=$stage mode=$mode"
            }
        }

        Write-Output "PASS synthetic stage $stage -$mode compatible with Vulkan1.0 and 1.1"
    }
}

$sources = @(
    (Join-Path $PSScriptRoot 'vulkan-auto-config.c'),
    (Join-Path $PSScriptRoot 'vulkan-core11-policy.c'),
    (Join-Path $PSScriptRoot 'vulkan-core10-graph-memory.c'),
    (Join-Path $PSScriptRoot 'vulkan-core10-graph.c'),
    (Join-Path $PSScriptRoot 'vulkan-core10-graph-diagnostic.c')
)

$targets = @(
    @{ Target = "aarch64-linux-android$AndroidApi"; Abi = 'arm64' },
    @{ Target = "armv7a-linux-androideabi$AndroidApi"; Abi = 'armv7' }
)

foreach ($item in $targets) {
    $exe = Join-Path $directory "vulkan-dynamic-graph-$($item.Abi)"

    & $clang ("--target=" + $item.Target) -std=gnu11 -O2 -Wall -Wextra -Werror -I $PSScriptRoot $sources -o $exe -lvulkan

    if ($LASTEXITCODE -ne 0) {
        throw "Dynamic Vulkan 1.0 graph Android API $AndroidApi build failed: $($item.Target)"
    }

    $symbols = @(& $readelf -W --dyn-syms $exe)

    if ($LASTEXITCODE -ne 0 -or
        $symbols -match 'UND.*vkEnumerateInstanceVersion' -or
        -not ($symbols -match 'UND.*vkGetInstanceProcAddr')) {
        throw "Android graph executable requires unsupported Vulkan loader symbols: $($item.Target)"
    }

    Write-Output "PASS dynamic Vulkan 1.0 graph Android API $AndroidApi $($item.Abi)"
}

$artifacts = @(
    'vulkan-dynamic-graph-arm64',
    'vulkan-dynamic-graph-armv7',
    'vulkan-graph-host-O0.exe',
    'vulkan-graph-host-O2.exe'
)

foreach ($mode in @('O0', 'O')) {
    foreach ($stage in @('a', 'b', 'c')) {
        $artifacts += "stage-$stage-$mode-vk1.0.spv"
    }
}

foreach ($name in $artifacts) {
    $file = Join-Path $directory $name
    $size = (Get-Item -LiteralPath $file).Length
    $digest = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()

    Write-Output "GRAPH_BUILD_ARTIFACT $name bytes=$size sha256=$digest"
}

Write-Output 'PASS Vulkan 1.0 dynamic buffer/descriptor/shader graph complete portable build'
