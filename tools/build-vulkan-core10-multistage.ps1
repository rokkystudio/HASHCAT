<#
.SYNOPSIS
Builds a portable three-stage synthetic Vulkan 1.0 Compute diagnostic.

.DESCRIPTION
Compiles three independent public integer shaders as SPIR-V 1.0 with and
without optimization, validates each module for Vulkan 1.0 and 1.1,
and builds strict Android API 24 ARM64/ARMv7 executables.

Compiles and executes the six-buffer heap-planning C unit test natively
at -O0 and -O2, without linking Vulkan or any GPU driver. The standalone
device executable uses four GPU storage buffers and two host staging
buffers, six Vulkan allocations, no optional features and core API 1.0
unless explicitly requested otherwise.

.PARAMETER AndroidNdk
Android NDK installation root containing Clang and shader-tools.

.PARAMETER HostGcc
Native Windows GCC compiler path or command name.

.PARAMETER AndroidApi
Android API level for both native device ABIs; minimum accepted is 24.

.PARAMETER OutputDirectory
Destination for two device programs, six SPIR-V binaries and two CPU tests.
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
    throw 'Android Vulkan support begins at API 24.'
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

foreach ($tool in @($clang, $readelf, $glslc, $spirvVal, $gcc, $objdump)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "Required Vulkan 1.0 toolchain component unavailable: $tool"
    }
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-core-multistage-20261031'
}
elseif (-not [System.IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory = Join-Path $root $OutputDirectory
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$directory = (Resolve-Path -LiteralPath $OutputDirectory).Path

$planner = Join-Path $PSScriptRoot 'vulkan-multistage-memory-plan.c'
$plannerTest = Join-Path $PSScriptRoot 'test-vulkan-multistage-memory-plan-host.c'
$expectedCpu = @(
    'PASS_MULTISTAGE_SIX all_resources capacity_and_roles',
    'PASS_MULTISTAGE_BACKTRACK six_requests_exact_placement',
    'PASS_MULTISTAGE_NONCOHERENT compatible_and_explicit_rejection',
    'PASS_MULTISTAGE_ERRORS count alignment heap capacity untouched',
    'PASS_MULTISTAGE_MEMORY_POLICY no_vulkan_driver'
)

$previous = $null

foreach ($mode in @('O0', 'O2')) {
    $exe = Join-Path $directory "vulkan-multistage-heap-$mode.exe"

    & $gcc -std=c11 "-$mode" -Wall -Wextra -Werror -Wconversion -Wshadow -I $PSScriptRoot -idirafter $include $planner $plannerTest -o $exe

    if ($LASTEXITCODE -ne 0) {
        throw "Six-buffer host memory planner build failed: $mode"
    }

    $imports = @(
        (& $objdump -p $exe) |
            Select-String '^\s*DLL Name:\s*(\S+)\s*$' |
            ForEach-Object { $_.Matches[0].Groups[1].Value }
    )

    if ($LASTEXITCODE -ne 0 -or $imports.Count -eq 0) {
        throw "Native host planner library dependencies unavailable: $mode"
    }

    foreach ($library in $imports) {
        if ($library -match '(?i)vulkan|opencl') {
            throw "Native heap test linked a GPU driver: $library"
        }
    }

    $result = @(& $exe 2>&1)

    if ($LASTEXITCODE -ne 0) {
        $result | ForEach-Object { Write-Output $_ }

        throw "Six-buffer CPU memory planning failed: $mode"
    }

    $observed = @($result | Where-Object { $_ -match '^PASS_MULTISTAGE_' })

    if ($observed.Count -ne $expectedCpu.Count) {
        throw "Unexpected number of Vulkan heap CPU test checks: $mode"
    }

    foreach ($required in $expectedCpu) {
        if (@($observed | Where-Object { $_ -ceq $required }).Count -ne 1) {
            throw "Missing exact six-buffer host policy check: $required"
        }
    }

    if ($null -ne $previous -and
        $previous -cne ($result -join "`n")) {
        throw 'Native memory policy results differ between -O0 and -O2.'
    }

    $previous = $result -join "`n"

    [System.IO.File]::WriteAllText(
        (Join-Path $directory "vulkan-multistage-heap-$mode.log"),
        (@(
            'TEST=vulkan-multistage-memory-plan-host',
            "OPTIMIZATION=$mode",
            'VULKAN_DRIVER_LINKED=0',
            'EXIT_CODE=0'
        ) + $result -join "`n") + "`n",
        [System.Text.UTF8Encoding]::new($false)
    )

    Write-Output "PASS native six-buffer Vulkan 1.0 heap allocator -$mode"
}

foreach ($mode in @('O0', 'O')) {
    foreach ($stage in @('a', 'b', 'c')) {
        $source = Join-Path $PSScriptRoot "vulkan-pipeline-stage-$stage.comp"
        $output = Join-Path $directory "stage-$stage-$mode-vk1.0.spv"

        & $glslc --target-env=vulkan1.0 "-$mode" -Werror -o $output $source

        if ($LASTEXITCODE -ne 0) {
            throw "Vulkan 1.0 Compute shader failed: stage=$stage mode=$mode"
        }

        foreach ($target in @('vulkan1.0', 'vulkan1.1')) {
            & $spirvVal --target-env $target $output

            if ($LASTEXITCODE -ne 0) {
                throw "SPIR-V incompatible with $target stage=$stage mode=$mode"
            }
        }

        Write-Output "PASS stage $stage -$mode targets Vulkan 1.0 and 1.1"
    }
}

$auto = Join-Path $PSScriptRoot 'vulkan-auto-config.c'
$api = Join-Path $PSScriptRoot 'vulkan-core11-policy.c'
$gpu = Join-Path $PSScriptRoot 'vulkan-core10-multistage-diagnostic.c'

$targets = @(
    @{ Abi = 'arm64'; Target = "aarch64-linux-android$AndroidApi" },
    @{ Abi = 'armv7'; Target = "armv7a-linux-androideabi$AndroidApi" }
)

foreach ($item in $targets) {
    $exe = Join-Path $directory ("vulkan-core10-multistage-" + $item.Abi)

    & $clang ("--target=" + $item.Target) -std=gnu11 -O2 -Wall -Wextra -Werror -I $PSScriptRoot $auto $api $planner $gpu -o $exe -lvulkan

    if ($LASTEXITCODE -ne 0) {
        throw "Portable Vulkan 1.0 three-stage ABI failed: $($item.Target)"
    }

    $symbols = @(& $readelf -W --dyn-syms $exe)

    if ($LASTEXITCODE -ne 0 -or
        $symbols -match 'UND.*vkEnumerateInstanceVersion') {
        throw "ABI unexpectedly requires Vulkan 1.1 loader: $($item.Target)"
    }

    if (-not ($symbols -match 'UND.*vkGetInstanceProcAddr')) {
        throw "Missing Vulkan 1.0 core loader query: $($item.Target)"
    }

    Write-Output "PASS Android API $AndroidApi Vulkan 1.0 three-stage $($item.Abi)"
}

$artifacts = @(
    'vulkan-core10-multistage-arm64',
    'vulkan-core10-multistage-armv7',
    'vulkan-multistage-heap-O0.exe',
    'vulkan-multistage-heap-O2.exe'
)

foreach ($stage in @('a', 'b', 'c')) {
    foreach ($mode in @('O0', 'O')) {
        $artifacts += "stage-$stage-$mode-vk1.0.spv"
    }
}

foreach ($name in $artifacts) {
    $file = Join-Path $directory $name
    $digest = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
    $bytes = (Get-Item -LiteralPath $file).Length

    Write-Output "BUILD_ARTIFACT $name bytes=$bytes sha256=$digest"
}

Write-Output 'PASS reproducible Vulkan 1.0 six-buffer triple Compute build'
