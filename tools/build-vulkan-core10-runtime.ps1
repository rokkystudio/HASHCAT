<#
.SYNOPSIS
Builds a reusable Vulkan 1.0 synthetic Compute runtime for Android.

.DESCRIPTION
Builds two native Android API 24 executables for arm64-v8a and armeabi-v7a.
Compiles three public integer shaders as SPIR-V 1.0 with and without
optimization, validating each for both Vulkan 1.0 and 1.1.

Independently compiles and executes the six-buffer memory allocation C
policy tests on Windows at -O0 and -O2. Vulkan resource ownership and
sequential command buffer reuse are implemented in a separate C module,
without GPU model selection or hashcat-specific code.

.PARAMETER AndroidNdk
Root of the Android NDK containing Clang and Vulkan shader tools.

.PARAMETER HostGcc
Path to Windows GCC used only for the CPU-only memory planner tests.

.PARAMETER AndroidApi
Minimum Android version used when targeting both native ABIs.
Vulkan starts at Android API 24, so earlier versions are rejected.

.PARAMETER OutputDirectory
Directory for native executables, SPIR-V shaders and host-test evidence.
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
    throw 'Android Vulkan requires API 24 or newer.'
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
        throw "Required Vulkan 1.0 runtime build tool missing: $tool"
    }
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-runtime-20261031'
}
elseif (-not [System.IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory = Join-Path $root $OutputDirectory
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$directory = (Resolve-Path -LiteralPath $OutputDirectory).Path

$planner = Join-Path $PSScriptRoot 'vulkan-multistage-memory-plan.c'
$plannerTest = Join-Path $PSScriptRoot 'test-vulkan-multistage-memory-plan-host.c'

$expectedCpuPasses = @(
    'PASS_MULTISTAGE_SIX all_resources capacity_and_roles',
    'PASS_MULTISTAGE_BACKTRACK six_requests_exact_placement',
    'PASS_MULTISTAGE_NONCOHERENT compatible_and_explicit_rejection',
    'PASS_MULTISTAGE_ERRORS count alignment heap capacity untouched',
    'PASS_MULTISTAGE_MEMORY_POLICY no_vulkan_driver'
)

$previous = $null

foreach ($mode in @('O0', 'O2')) {
    $exe = Join-Path $directory "vulkan-runtime-heap-$mode.exe"

    & $gcc -std=c11 "-$mode" -Wall -Wextra -Werror -Wconversion -Wshadow -I $PSScriptRoot -idirafter $include $planner $plannerTest -o $exe

    if ($LASTEXITCODE -ne 0) {
        throw "Vulkan runtime six-buffer native planner build failed: $mode"
    }

    $imports = @(
        (& $objdump -p $exe) |
            Select-String '^\s*DLL Name:\s*(\S+)\s*$' |
            ForEach-Object { $_.Matches[0].Groups[1].Value }
    )

    if ($LASTEXITCODE -ne 0 -or $imports.Count -eq 0) {
        throw "Native runtime test linked libraries cannot be examined: $mode"
    }

    foreach ($library in $imports) {
        if ($library -match '(?i)vulkan|opencl') {
            throw "CPU-only Vulkan memory planner depends on GPU driver: $library"
        }
    }

    $lines = @(& $exe 2>&1)

    if ($LASTEXITCODE -ne 0) {
        $lines | ForEach-Object { Write-Output $_ }

        throw "Native Vulkan runtime planner test failed: $mode"
    }

    $passes = @($lines | Where-Object { $_ -match '^PASS_MULTISTAGE_' })

    if ($passes.Count -ne $expectedCpuPasses.Count) {
        throw "Native memory planner pass count unexpected: $mode"
    }

    foreach ($expected in $expectedCpuPasses) {
        if (@($passes | Where-Object { $_ -ceq $expected }).Count -ne 1) {
            throw "Native runtime policy test missing: $expected"
        }
    }

    if ($null -ne $previous -and
        $previous -cne ($lines -join "`n")) {
        throw 'Native CPU planner results differ between -O0 and -O2.'
    }

    $previous = $lines -join "`n"

    [System.IO.File]::WriteAllText(
        (Join-Path $directory "vulkan-runtime-heap-$mode.log"),
        ((@(
            'TEST=vulkan-core10-runtime-heap-policy',
            "OPTIMIZATION=$mode",
            'GPU_DRIVER_LINKED=0',
            'EXIT_CODE=0'
        ) + $lines) -join "`n") + "`n",
        [System.Text.UTF8Encoding]::new($false)
    )

    Write-Output "PASS native Vulkan runtime six-buffer memory planner -$mode"
}

foreach ($mode in @('O0', 'O')) {
    foreach ($stage in @('a', 'b', 'c')) {
        $shader = Join-Path $PSScriptRoot "vulkan-pipeline-stage-$stage.comp"
        $binary = Join-Path $directory "stage-$stage-$mode-vk1.0.spv"

        & $glslc --target-env=vulkan1.0 "-$mode" -Werror -o $binary $shader

        if ($LASTEXITCODE -ne 0) {
            throw "Runtime SPIR-V 1.0 compile failed: stage=$stage mode=$mode"
        }

        foreach ($target in @('vulkan1.0', 'vulkan1.1')) {
            & $spirvVal --target-env $target $binary

            if ($LASTEXITCODE -ne 0) {
                throw "Runtime SPIR-V stage $stage -$mode invalid for $target"
            }
        }

        Write-Output "PASS public shader stage $stage -$mode Vulkan core 1.0 and 1.1"
    }
}

$sourceFiles = @(
    (Join-Path $PSScriptRoot 'vulkan-auto-config.c'),
    (Join-Path $PSScriptRoot 'vulkan-core11-policy.c'),
    (Join-Path $PSScriptRoot 'vulkan-multistage-memory-plan.c'),
    (Join-Path $PSScriptRoot 'vulkan-core10-runtime.c'),
    (Join-Path $PSScriptRoot 'vulkan-core10-runtime-diagnostic.c')
)

$targets = @(
    @{ Target = "aarch64-linux-android$AndroidApi"; Abi = 'arm64' },
    @{ Target = "armv7a-linux-androideabi$AndroidApi"; Abi = 'armv7' }
)

foreach ($architecture in $targets) {
    $exe = Join-Path $directory "vulkan-runtime-$($architecture.Abi)"

    & $clang ("--target=" + $architecture.Target) -std=gnu11 -O2 -Wall -Wextra -Werror -I $PSScriptRoot $sourceFiles -o $exe -lvulkan

    if ($LASTEXITCODE -ne 0) {
        throw "Vulkan core 1.0 native runtime failed: $($architecture.Target)"
    }

    $symbols = @(& $readelf -W --dyn-syms $exe)

    if ($LASTEXITCODE -ne 0) {
        throw "Unable to examine Vulkan runtime ABI symbols: $($architecture.Target)"
    }

    if ($symbols -match 'UND.*vkEnumerateInstanceVersion') {
        throw "Runtime executable imports a mandatory Vulkan 1.1 loader entry: $($architecture.Target)"
    }

    if (-not ($symbols -match 'UND.*vkGetInstanceProcAddr')) {
        throw "Runtime executable does not import Vulkan 1.0 loader query: $($architecture.Target)"
    }

    Write-Output "PASS Vulkan 1.0 Android API $AndroidApi $($architecture.Abi) runtime"
}

$artifacts = @(
    'vulkan-runtime-arm64',
    'vulkan-runtime-armv7',
    'vulkan-runtime-heap-O0.exe',
    'vulkan-runtime-heap-O2.exe'
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

    Write-Output "RUNTIME_BUILD_ARTIFACT $name bytes=$size sha256=$digest"
}

Write-Output 'PASS Vulkan 1.0 reusable resource runtime build for Android 24 ARM64 and ARMv7'
