<#
.SYNOPSIS
Builds and tests a vendor-independent Vulkan 1.0 memory heap planner.

.DESCRIPTION
Compiles the exact Vulkan heap placement policy with native Windows GCC at
-O0 and -O2. The CPU-only test uses deterministic property sets and an
independent exhaustive allocation-feasibility oracle. It also compiles a
standalone Android ARM64 Vulkan 1.0 buffer allocation probe with NDK Clang.

The native host tests do not load Vulkan drivers. The Android diagnostic
is compiled but not executed by this build script. No GPU shader, hashcat
backend or cryptographic workload is involved.

.PARAMETER AndroidNdk
Exact Android NDK root providing Clang and Vulkan C headers.

.PARAMETER HostGcc
Windows GCC path or command name for native mock-free policy testing.

.PARAMETER AndroidApi
Minimum Android API level for the standalone ARM64 heap probe.

.PARAMETER OutputDirectory
Directory containing two native CPU test binaries, their deterministic logs,
and one Android ARM64 Vulkan heap inspection binary.
#>
param (
    [Parameter(Mandatory = $true)]
    [string]$AndroidNdk,

    [string]$HostGcc = 'gcc.exe',

    [int]$AndroidApi = 26,

    [string]$OutputDirectory = ''
)

$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$ndk = (Resolve-Path -LiteralPath $AndroidNdk).Path
$llvmBin = Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64\bin'
$include = Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64\sysroot\usr\include'
$clang = Join-Path $llvmBin 'clang.exe'
$head = Join-Path $include 'vulkan\vulkan.h'

foreach ($file in @($clang, $head)) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "NDK compiler or Vulkan header unavailable: $file"
    }
}

if (Test-Path -LiteralPath $HostGcc -PathType Leaf) {
    $gcc = (Resolve-Path -LiteralPath $HostGcc).Path
}
else {
    $gcc = (Get-Command $HostGcc -ErrorAction Stop).Source
}

$objdump = Join-Path (Split-Path -Parent $gcc) 'objdump.exe'

if (-not (Test-Path -LiteralPath $objdump -PathType Leaf)) {
    throw "Host PE import inspector unavailable: $objdump"
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-heap-policy-20261031'
}
elseif (-not [System.IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory = Join-Path $root $OutputDirectory
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$directory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$policy = Join-Path $PSScriptRoot 'vulkan-memory-heap-policy.c'
$test = Join-Path $PSScriptRoot 'test-vulkan-memory-heap-policy-host.c'

$requiredPasses = @(
    'PASS_HEAP_BACKTRACK alternate_type_prevents_false_incompatibility',
    'PASS_HEAP_FOUR_BUFFERS two_gpu_two_staging_exact_heap_sizes',
    'PASS_HEAP_NONCOHERENT_FALLBACK explicit_capacity_and_cache_flags',
    'PASS_HEAP_FAILURES capacity masks counts invalid_input untouched',
    'PASS_HEAP_BOUNDARIES bit31 unsigned64_capacity',
    'PASS_HEAP_EXHAUSTIVE layouts=64 heap_maps=8 masks=7 roles=4 sizes=2 cases=200704 accepted=78138 unsupported=24816 capacity=97750',
    'PASS_HEAP_POLICY all six groups without Vulkan driver'
)

$previous = $null

foreach ($optimization in @('O0', 'O2')) {
    $name = "vulkan-heap-policy-$optimization.exe"
    $binary = Join-Path $directory $name

    & $gcc -std=c11 "-$optimization" -Wall -Wextra -Werror -Wconversion -Wshadow -I $PSScriptRoot -idirafter $include $policy $test -o $binary

    if ($LASTEXITCODE -ne 0) {
        throw "Strict native C Vulkan heap test build failed: $optimization"
    }

    $importLines = @(& $objdump -p $binary)

    if ($LASTEXITCODE -ne 0) {
        throw "Cannot inspect native Vulkan heap test PE imports: $optimization"
    }

    $imports = @(
        $importLines |
            Select-String '^\s*DLL Name:\s*(\S+)\s*$' |
            ForEach-Object { $_.Matches[0].Groups[1].Value }
    )

    if ($imports.Count -eq 0) {
        throw "Missing valid native Windows PE import list: $optimization"
    }

    foreach ($import in $imports) {
        if ($import -match '(?i)vulkan|opencl') {
            throw "A GPU runtime was unexpectedly linked: $import"
        }
    }

    $observed = @(& $binary 2>&1)
    $exitCode = $LASTEXITCODE

    if ($exitCode -ne 0) {
        $observed | Select-Object -Last 30 | ForEach-Object { Write-Output $_ }

        throw "Vulkan heap feasibility oracle failed: $optimization"
    }

    $passes = @($observed | Where-Object { $_ -match '^PASS_HEAP_' })

    if ($passes.Count -ne $requiredPasses.Count) {
        throw "Missing Vulkan heap test results: $optimization"
    }

    foreach ($required in $requiredPasses) {
        if (@($passes | Where-Object { $_ -ceq $required }).Count -ne 1) {
            throw "Required Vulkan heap C assertion missing: $required"
        }
    }

    $body = $observed -join "`n"

    if ($null -ne $previous -and $previous -cne $body) {
        throw 'Different optimization modes produced different memory heap decisions.'
    }

    $previous = $body

    $logLines = @(
        'TEST=synthetic-vulkan-heap-allocation-policy',
        "OPTIMIZATION=$optimization",
        'VULKAN_DRIVER_LINKED=0',
        "EXIT_CODE=$exitCode"
    ) + $observed

    $log = Join-Path $directory "vulkan-heap-policy-$optimization.log"

    [System.IO.File]::WriteAllText(
        $log,
        ($logLines -join "`n") + "`n",
        [System.Text.UTF8Encoding]::new($false)
    )

    $digest = (Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash.ToLowerInvariant()

    Write-Output "PASS native Vulkan heap policy -${optimization}: 200704 cases, SHA-256=$digest"

    $passes | ForEach-Object { Write-Output $_ }
}

$android = Join-Path $directory 'vulkan-heap-probe-arm64'
$autoSource = Join-Path $PSScriptRoot 'vulkan-auto-config.c'
$probeSource = Join-Path $PSScriptRoot 'vulkan-memory-heap-probe.c'

& $clang "--target=aarch64-linux-android$AndroidApi" -std=gnu11 -O2 -Wall -Wextra -Werror -I $PSScriptRoot $autoSource $policy $probeSource -o $android -lvulkan

if ($LASTEXITCODE -ne 0) {
    throw 'Strict Android ARM64 Vulkan memory heap probe compilation failed.'
}

$androidHash = (Get-FileHash -LiteralPath $android -Algorithm SHA256).Hash.ToLowerInvariant()
$androidBytes = (Get-Item -LiteralPath $android).Length

Write-Output "PASS Android Vulkan 1.0 buffer-only memory probe: bytes=$androidBytes SHA-256=$androidHash"
Write-Output 'BUILD COMPLETE: two native C CPU oracle tests and one Android memory-only Vulkan probe'
