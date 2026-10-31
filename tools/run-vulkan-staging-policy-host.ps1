<#
.SYNOPSIS
Compiles and tests portable Vulkan memory policy without a GPU driver.

.DESCRIPTION
Uses the native Windows GCC toolchain and Vulkan 1.0-compatible type headers
from an explicit Android NDK installation. Compiles the real C implementation
of Vulkan auto-configuration and staging transfer selection alongside mock
Vulkan queries, and runs deterministic tests at -O0 and -O2.

The test enumerates all 64 valid three-memory-type arrangements, all seven
nonempty three-bit masks per storage buffer and both transfer mode requests,
verifying 6,272 exact transfer decisions. It independently tests allocation
arithmetic, noncoherent mapped-range cache calls and compute-device limits.
No Vulkan loader, GPU driver, Android device, passwords or hashcat runtime
is opened. The binaries and logs are written only to the ignored build tree.

.PARAMETER AndroidNdk
NDK root containing Vulkan headers; no Android compiler is invoked.

.PARAMETER HostGcc
Absolute path or command name for a native Windows GCC compiler.

.PARAMETER OutputDirectory
Output path for two Windows test executables and deterministic UTF-8 logs.
#>
param (
    [Parameter(Mandatory = $true)]
    [string]$AndroidNdk,

    [string]$HostGcc = 'gcc.exe',

    [string]$OutputDirectory = ''
)

$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$ndk = (Resolve-Path -LiteralPath $AndroidNdk).Path
$include = Join-Path $ndk 'toolchains\llvm\prebuilt\windows-x86_64\sysroot\usr\include'
$vulkanHeader = Join-Path $include 'vulkan\vulkan.h'

if (-not (Test-Path -LiteralPath $vulkanHeader -PathType Leaf)) {
    throw "Vulkan 1.0-compatible NDK header unavailable: $vulkanHeader"
}

if (Test-Path -LiteralPath $HostGcc -PathType Leaf) {
    $gcc = (Resolve-Path -LiteralPath $HostGcc).Path
}
else {
    $gcc = (Get-Command -Name $HostGcc -ErrorAction Stop).Source
}

$objdump = Join-Path (Split-Path -Parent $gcc) 'objdump.exe'

if (-not (Test-Path -LiteralPath $objdump -PathType Leaf)) {
    throw "PE import checker unavailable beside host GCC: $objdump"
}

if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-staging-policy-host-20261031'
}
elseif (-not [System.IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory = Join-Path $root $OutputDirectory
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$directory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$sources = @(
    (Join-Path $root 'tools\vulkan-auto-config.c'),
    (Join-Path $root 'tools\vulkan-staging-memory.c'),
    (Join-Path $root 'tools\test-vulkan-staging-policy-host.c')
)
$includeTools = Join-Path $root 'tools'

$expected = @(
    'PASS_HOST_MEMORY_TYPES coherent noncoherent gpu_local bit31 invalid',
    'PASS_HOST_TRANSFER_MODES direct local_only mixed forced invalid',
    'PASS_HOST_EXHAUSTIVE_MATRIX layouts=64 masks=7 selections=448 transfers=6272',
    'PASS_HOST_MEMORY_PLANS atomic_alignment coherent disjoint overflow',
    'PASS_HOST_MAPPED_SYNC coherent flush invalidate full_range errors',
    'PASS_HOST_COMPUTE_CAPABILITIES queue limits requirements errors',
    'PASS_HOST_POLICY all six native-C test groups, no Vulkan driver'
)

$previousBody = $null

foreach ($optimization in @('O0', 'O2')) {
    $filename = "vulkan-staging-policy-$optimization.exe"
    $binary = Join-Path $directory $filename

    & $gcc -std=c11 "-$optimization" -Wall -Wextra -Werror -Wconversion -Wshadow -I $includeTools -idirafter $include $sources[0] $sources[1] $sources[2] -o $binary

    if ($LASTEXITCODE -ne 0) {
        throw "Strict native-C Vulkan policy compilation failed: $optimization"
    }

    $objdumpOutput = @(& $objdump -p $binary)
    $objdumpCode = $LASTEXITCODE

    if ($objdumpCode -ne 0) {
        throw "Cannot inspect native-C Vulkan test imports: $optimization"
    }

    $imports = @(
        $objdumpOutput |
            Select-String -Pattern '^\s*DLL Name:\s*(\S+)\s*$' |
            ForEach-Object { $_.Matches[0].Groups[1].Value }
    )

    if ($imports.Count -eq 0) {
        throw "No normal Windows PE import table found: $optimization"
    }

    foreach ($import in $imports) {
        if ($import -match '(?i)vulkan|opencl') {
            throw "GPU loader or compute driver unexpectedly linked: $import"
        }
    }

    $output = @(& $binary 2>&1)
    $exitCode = $LASTEXITCODE

    if ($exitCode -ne 0) {
        $output | Select-Object -Last 30 | ForEach-Object { Write-Output $_ }

        throw "Compiled native-C Vulkan policy check failed: $optimization"
    }

    $body = ($output -join "`n")

    foreach ($pass in $expected) {
        $count = @($output | Where-Object { $_ -ceq $pass }).Count

        if ($count -ne 1) {
            throw "Missing or duplicate Vulkan host policy result: $optimization $pass"
        }
    }

    $allPass = @($output | Where-Object { $_ -match '^PASS_HOST_' })

    if ($allPass.Count -ne $expected.Count) {
        throw "Additional or missing Vulkan host policy test records: $optimization"
    }

    if ($null -ne $previousBody -and $previousBody -cne $body) {
        throw "The C Vulkan selection decisions differ across -O0 and -O2"
    }

    $previousBody = $body

    $lines = @(
        'TEST=vulkan-host-c-memory-policy',
        "OPTIMIZATION=$optimization",
        'VULKAN_DRIVER_LINKED=0',
        "EXIT_CODE=$exitCode"
    ) + $output

    $log = Join-Path $directory "vulkan-staging-policy-$optimization.log"

    [System.IO.File]::WriteAllText(
        $log,
        ($lines -join "`n") + "`n",
        [System.Text.UTF8Encoding]::new($false)
    )

    $hash = (Get-FileHash -LiteralPath $binary -Algorithm SHA256).Hash.ToLowerInvariant()

    Write-Output "PASS HOST -$optimization binary_sha256=$hash imported_dlls=$($imports -join ',')"

    $expected | ForEach-Object { Write-Output $_ }
}

Write-Output 'PASS deterministic native C Vulkan memory/compute policy at -O0 and -O2, without a GPU loader'
