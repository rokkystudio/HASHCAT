<#
.SYNOPSIS
Builds Vulkan 1.0 baseline compute and optional core-1.1 diagnostics.

.DESCRIPTION
Compiles a noncryptographic Vulkan 1.0 Compute shader for Vulkan 1.0 and
validates it for both Vulkan 1.0 and Vulkan 1.1. Builds Android ARM64 and
ARMv7 device diagnostics for Android API 24 using only core Vulkan 1.0 calls
for memory, dispatch, copy and synchronization. Both binaries request core
Vulkan 1.0 by default, with optional explicit core Vulkan 1.1 tests.

Builds the real C loader/device API negotiation policy at native Windows
GCC -O0 and -O2, then executes identical deterministic host policy tests.
No Android GPU is accessed by this build script and no hashcat module or
password-processing code is compiled.

.PARAMETER AndroidNdk
Root directory of Android NDK r29 with Clang and shader-tools.

.PARAMETER HostGcc
Absolute path or command name of the Windows native GCC compiler.

.PARAMETER AndroidApi
Minimum Android API level for both standalone binaries. Vulkan and the
Android libvulkan.so library are available beginning with Android API 24.
Values below 24 are rejected rather than implicitly upgraded.

.PARAMETER OutputDirectory
Directory for ARM64/ARMv7 Compute binaries, an ARMv7 Vulkan memory survey,
two SPIR-V modules and native Windows CPU-only test logs.
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
    throw 'Vulkan on Android requires API level 24 or newer.'
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$ndk = (Resolve-Path -LiteralPath $AndroidNdk).Path
$llvm = Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin'
$include = Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64\sysroot\usr\include'
$shaderTools = Join-Path $ndk 'shader-tools\windows-x86_64'
$clang = Join-Path $llvm 'clang.exe'
$glslc = Join-Path $shaderTools 'glslc.exe'
$spirvVal = Join-Path $shaderTools 'spirv-val.exe'
$readelf = Join-Path $llvm 'llvm-readelf.exe'

if (Test-Path -LiteralPath $HostGcc -PathType Leaf) {
    $gcc = (Resolve-Path -LiteralPath $HostGcc).Path
}
else {
    $gcc = (Get-Command $HostGcc -ErrorAction Stop).Source
}

$objdump = Join-Path (Split-Path -Parent $gcc) 'objdump.exe'

foreach ($tool in @($clang, $glslc, $spirvVal, $readelf, $gcc, $objdump)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "Required native Vulkan 1.0 test tool unavailable: $tool"
    }
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-core-baseline-20261031'
}
elseif (-not [System.IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory = Join-Path $root $OutputDirectory
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$directory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$policy = Join-Path $PSScriptRoot 'vulkan-core11-policy.c'
$policyTest = Join-Path $PSScriptRoot 'test-vulkan-core11-policy-host.c'

$required = @(
    'PASS_CORE11_LOADER dynamic_query core10_missing version_errors',
    'PASS_CORE11_INSTANCE default10 newer_loader_never_raises_api explicit11',
    'PASS_CORE11_DEVICE cap11 physical_version_independent lower_minimum',
    'PASS_CORE11_POLICY all three groups without Vulkan driver'
)
$previousBody = $null

foreach ($mode in @('O0', 'O2')) {
    $binary = Join-Path $directory "vulkan-core11-policy-$mode.exe"

    & $gcc -std=c11 "-$mode" -Wall -Wextra -Werror -Wconversion -Wshadow -I $PSScriptRoot -idirafter $include $policy $policyTest -o $binary

    if ($LASTEXITCODE -ne 0) {
        throw "Native Vulkan core API policy compilation failed: $mode"
    }

    $importsOutput = @(& $objdump -p $binary)

    if ($LASTEXITCODE -ne 0) {
        throw "Cannot inspect native Windows test imports: $mode"
    }

    $imports = @(
        $importsOutput |
            Select-String '^\s*DLL Name:\s*(\S+)\s*$' |
            ForEach-Object { $_.Matches[0].Groups[1].Value }
    )

    if ($imports.Count -eq 0) {
        throw "No Windows executable imports available: $mode"
    }

    foreach ($import in $imports) {
        if ($import -match '(?i)vulkan|opencl') {
            throw "A Vulkan/OpenCL driver is linked to the native policy test: $import"
        }
    }

    $output = @(& $binary 2>&1)
    $status = $LASTEXITCODE

    if ($status -ne 0) {
        $output | ForEach-Object { Write-Output $_ }

        throw "Core 1.0/1.1 native C selection tests failed: $mode"
    }

    if (@($output | Where-Object { $_ -match '^PASS_CORE11_' }).Count -ne $required.Count) {
        throw "Incorrect number of native policy test records: $mode"
    }

    foreach ($entry in $required) {
        if (@($output | Where-Object { $_ -ceq $entry }).Count -ne 1) {
            throw "Missing exact core API negotiation result: $entry"
        }
    }

    $body = $output -join "`n"

    if ($null -ne $previousBody -and $previousBody -cne $body) {
        throw 'Native policy decisions differ between -O0 and -O2.'
    }

    $previousBody = $body

    $record = @(
        'TEST=vulkan-core-api-1.0-baseline',
        "OPTIMIZATION=$mode",
        'VULKAN_DRIVER_LINKED=0',
        'EXIT_CODE=0'
    ) + $output

    [System.IO.File]::WriteAllText(
        (Join-Path $directory "vulkan-core11-policy-$mode.log"),
        ($record -join "`n") + "`n",
        [System.Text.UTF8Encoding]::new($false)
    )

    Write-Output "PASS native API negotiation -$mode (Vulkan 1.0 is default)"
}

$auto = Join-Path $PSScriptRoot 'vulkan-auto-config.c'
$heap = Join-Path $PSScriptRoot 'vulkan-memory-heap-policy.c'
$gpu = Join-Path $PSScriptRoot 'vulkan-core11-compute-diagnostic.c'
$archTargets = @(
    @{
        Target = "aarch64-linux-android$AndroidApi"
        Binary = 'vulkan-core-baseline-arm64'
    },
    @{
        Target = "armv7a-linux-androideabi$AndroidApi"
        Binary = 'vulkan-core-baseline-armv7'
    }
)

foreach ($architecture in $archTargets) {
    $androidBinary = Join-Path $directory $architecture.Binary

    & $clang ("--target=" + $architecture.Target) -std=gnu11 -O2 -Wall -Wextra -Werror -I $PSScriptRoot $auto $heap $policy $gpu -o $androidBinary -lvulkan

    if ($LASTEXITCODE -ne 0) {
        throw "Vulkan 1.0 Android build failed for $($architecture.Target)"
    }

    $dynamic = @(& $readelf -W --dyn-syms $androidBinary)

    if ($LASTEXITCODE -ne 0) {
        throw "Cannot inspect Vulkan symbols: $($architecture.Target)"
    }

    if ($dynamic -match 'UND.*vkEnumerateInstanceVersion') {
        throw "A Vulkan 1.1 loader function was linked as mandatory: $($architecture.Target)"
    }

    if (-not ($dynamic -match 'UND.*vkGetInstanceProcAddr')) {
        throw "Vulkan 1.0 loader query missing from binary: $($architecture.Target)"
    }

    Write-Output "PASS Android $($architecture.Target) Vulkan 1.0 core-only executable"
}

$heapProbe = Join-Path $PSScriptRoot 'vulkan-memory-heap-probe.c'
$armv7Survey = Join-Path $directory 'vulkan-heap-probe-armv7'

& $clang ("--target=armv7a-linux-androideabi" + $AndroidApi) -std=gnu11 -O2 -Wall -Wextra -Werror -I $PSScriptRoot $auto $heap $heapProbe -o $armv7Survey -lvulkan

if ($LASTEXITCODE -ne 0) {
    throw 'Vulkan 1.0 Android API24 ARMv7 heap survey compilation failed.'
}

$probeSymbols = @(& $readelf -W --dyn-syms $armv7Survey)

if ($LASTEXITCODE -ne 0 -or
    $probeSymbols -match 'UND.*vkEnumerateInstanceVersion') {
    throw 'ARMv7 Vulkan 1.0 heap survey imports a missing loader entry point.'
}

Write-Output 'PASS Android ARMv7 Vulkan 1.0 real memory heap survey executable'

foreach ($mode in @('O0', 'O')) {
    $name = "stage-a-$mode-vk1.0.spv"
    $output = Join-Path $directory $name
    $source = Join-Path $PSScriptRoot 'vulkan-pipeline-stage-a.comp'

    & $glslc --target-env=vulkan1.0 "-$mode" -Werror -o $output $source

    if ($LASTEXITCODE -ne 0) {
        throw "SPIR-V 1.0 shader compilation failed: $mode"
    }

    foreach ($target in @('vulkan1.0', 'vulkan1.1')) {
        & $spirvVal --target-env $target $output

        if ($LASTEXITCODE -ne 0) {
            throw "SPIR-V module incompatible with ${target}: $mode"
        }
    }

    Write-Output "PASS unchanged SPIR-V 1.0 shader -$mode on Vulkan 1.0 and 1.1"
}

foreach ($file in @(
    'vulkan-core-baseline-arm64',
    'vulkan-core-baseline-armv7',
    'vulkan-heap-probe-armv7',
    'vulkan-core11-policy-O0.exe',
    'vulkan-core11-policy-O2.exe',
    'stage-a-O0-vk1.0.spv',
    'stage-a-O-vk1.0.spv'
)) {
    $path = Join-Path $directory $file
    $sha = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    $bytes = (Get-Item -LiteralPath $path).Length

    Write-Output "BUILD_ARTIFACT $file bytes=$bytes sha256=$sha"
}

Write-Output 'BUILD COMPLETE: portable Vulkan 1.0 GPU baseline and optional 1.1 diagnostic'
