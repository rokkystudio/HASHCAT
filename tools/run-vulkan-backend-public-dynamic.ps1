<#
.SYNOPSIS
Verify four public Vulkan Compute stages with dynamic resource ownership.

.DESCRIPTION
Executes the real backend_vulkan_kernel_verify_chain4 method on a
physical Android GPU. Every trial validates four declared SPIR-V 1.0
Compute stages, allocates five storage buffers plus four descriptor
sets/shader modules/pipelines, and dispatches four stages separated by
three GPU barriers. Every one of the 256 public results is CPU-verified.
Two independent VkInstances and VkDevices run per requested Vulkan API.
No hashing, password candidates or attack scheduler are executed.

.PARAMETER Serial
Actual ADB serial of one connected Android GPU device.

.PARAMETER Abi
Explicit ARM ABI or auto discovery via ro.product.cpu.abilist.

.PARAMETER ReportPrefix
Lowercase public test identifier such as redmagic-armv7.

.PARAMETER TestCore11
Also validate the explicitly requested Vulkan 1.1 instance API.
#>
param (
    [Parameter(Mandatory = $true)]
    [string]$Serial,

    [ValidateSet('auto', 'arm64-v8a', 'armeabi-v7a')]
    [string]$Abi = 'auto',

    [Parameter(Mandatory = $true)]
    [string]$ReportPrefix,

    [switch]$TestCore11
)

$ErrorActionPreference = 'Stop'

if ($ReportPrefix -cnotmatch '^[a-z0-9-]+$') {
    throw 'ReportPrefix must contain lowercase letters, digits and dashes only.'
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$build = Join-Path $root 'build\diagnostic-multimode\vulkan-backend-public-dynamic-20261102'
$availableAbi = @(& adb -s $Serial shell getprop ro.product.cpu.abilist)

if ($LASTEXITCODE -ne 0 -or $availableAbi.Count -ne 1) {
    throw 'ADB did not return one device ABI capability list.'
}

$available = @($availableAbi[0].Trim() -split ',')

if ($Abi -eq 'auto') {
    if ($available -contains 'arm64-v8a') {
        $chosen = 'arm64-v8a'
    }
    elseif ($available -contains 'armeabi-v7a') {
        $chosen = 'armeabi-v7a'
    }
    else {
        throw 'Connected Android hardware has no supported ARM ABI.'
    }
}
else {
    $chosen = $Abi
}

if ($available -notcontains $chosen) {
    throw "Android hardware does not support the requested ABI $chosen."
}

$name = if ($chosen -eq 'arm64-v8a') {
    'vulkan-backend-public-dynamic-arm64'
}
else {
    'vulkan-backend-public-dynamic-armv7'
}

$local = Join-Path $build $name
$remote = "/data/local/tmp/hc-backend-public-dynamic-20261102-$ReportPrefix"
$native = "$remote/$name"

if (-not (Test-Path -LiteralPath $local -PathType Leaf)) {
    throw "Missing production Vulkan program-executor test ELF: $local"
}

function Invoke-NativeKernel {
    <#
    .SYNOPSIS
    Capture one real Android Vulkan process and its exact exit status.

    .DESCRIPTION
    Executes the specified literal native CLI arguments over ADB.
    Native stdout/stderr is retained with LF-only formatting and an
    exact process exit marker. No GPU result is inferred from ADB exit.
    #>
    param ([string[]]$Arguments)

    $command = "$native $($Arguments -join ' ')" + ' 2>&1; echo KERNEL_NATIVE_EXIT=$?'
    $response = @(& adb -s $Serial shell $command)

    if ($LASTEXITCODE -ne 0 -or $response.Count -lt 2) {
        throw 'ADB did not capture a complete Vulkan public Compute process.'
    }

    $lines = @($response | ForEach-Object { $_.TrimEnd([char]13) })
    $marker = $lines[-1].Trim()

    if ($marker -cnotmatch '^KERNEL_NATIVE_EXIT=([0-9]+)$') {
        throw "Missing native Android public-kernel exit status: $marker"
    }

    return @{
        ExitCode = [int]$Matches[1]
        Body = ([string]::Join([char]10, $lines[0..($lines.Count - 2)])).TrimEnd()
    }
}

function Assert-NativeKernel {
    <#
    .SYNOPSIS
    Require eight real Compute dispatches across two Vulkan lifecycles.

    .DESCRIPTION
    Validates two independent real Vulkan instances, four dynamic
    shader pipelines, five buffers and four descriptor sets per trial.
    All eight vkCmdDispatch calls and 512 public uint32 comparisons
    are required, plus complete resource cleanup and unchanged
    HASHCAT hashing-device state.
    #>
    param ([string]$Api, [hashtable]$Result)

    if ($Result.ExitCode -ne 0) {
        throw "Native Vulkan public-kernel process failed: $($Result.Body)"
    }

    $body = $Result.Body
    $lines = @($body -split [char]10)

    $complete = "BACKEND_PUBLIC_DYNAMIC_DIAGNOSTIC_PASS api=$Api trials=2 dispatches=8 buffers=5 descriptor_sets=4 pipelines=4 verified_words=512 dynamic_resources=1 password_inputs=0 scheduled=0"

    if ($lines[-1] -cne $complete) {
        throw 'Vulkan GPU compute verification summary is invalid.'
    }

    $counts = @{
        'DEVICE_CATALOG_OPEN ' = 2
        'DEVICE_CATALOG_CLOSE ' = 2
        'BACKEND_VULKAN_CATALOG_READY ' = 2
        'BACKEND_VULKAN_PROGRAM_ACCEPT ' = 2
        'BACKEND_VULKAN_PROGRAM_READY ' = 2
        'BACKEND_VULKAN_PROGRAM_DISPATCH_PASS ' = 2
        'BACKEND_VULKAN_PROGRAM_CLOSE ' = 2
        'BACKEND_PUBLIC_DYNAMIC_TRIAL_PASS ' = 2
        'DEVICE_CATALOG_SELECT ' = 2
    }

    foreach ($prefix in $counts.Keys) {
        $actual = @($lines | Where-Object { $_.StartsWith($prefix) }).Count

        if ($actual -ne $counts[$prefix]) {
            throw "Missing real Vulkan shader/ownership events $prefix actual=$actual"
        }
    }

    $programEvents = @(
        $lines | Where-Object {
            $_.StartsWith('BACKEND_VULKAN_PROGRAM_ACCEPT ')
        }
    )

    foreach ($event in $programEvents) {
        if ($event -cne 'BACKEND_VULKAN_PROGRAM_ACCEPT stages=4 buffers=5 bytes=256 descriptors_per_stage=2') {
            throw "Declared Vulkan program validation is incomplete: $event"
        }
    }

    $readyEvents = @(
        $lines | Where-Object {
            $_.StartsWith('BACKEND_VULKAN_PROGRAM_READY ')
        }
    )

    foreach ($event in $readyEvents) {
        if ($event -cnotmatch '^BACKEND_VULKAN_PROGRAM_READY index=0 queue_family=[0-9]+ api=1\.[01] stages=4 buffers=5 descriptor_sets=4 descriptors=8 shader_modules=4 pipelines=4 local=32 groups=2 program_validated=1 dynamic_resources=1 features=0 extensions=0 scheduled=0$') {
            throw "Program-driven Vulkan chain readiness is incomplete: $event"
        }
    }

    foreach ($trial in @(0, 1)) {
        $event = "BACKEND_PUBLIC_DYNAMIC_TRIAL_PASS trial=$trial buffers=5 descriptor_sets=4 pipelines=4 dispatches=4 verified_words=256 dynamic_resources=1 scheduled=0"

        if (@($lines | Where-Object { $_ -ceq $event }).Count -ne 1) {
            throw "Native Vulkan public kernel trial verification failed: $event"
        }
    }

    $shaderEvents = @(
        $lines | Where-Object {
            $_.StartsWith('BACKEND_VULKAN_PROGRAM_DISPATCH_PASS ')
        }
    )

    foreach ($event in $shaderEvents) {
        if ($event -cnotmatch '^BACKEND_VULKAN_PROGRAM_DISPATCH_PASS stages=4 buffers=5 words=64 dispatches=4 verified_words=256 dependency_barriers=3 descriptors=8 coherent_input=[01] noncoherent_outputs=[0-4] password_inputs=0 scheduled=0$') {
            throw "Actual GPU shader result is incomplete: $event"
        }
    }
}

New-Item -Path $build -ItemType Directory -Force | Out-Null

try {
    & adb -s $Serial shell mkdir -p $remote | Out-Null

    if ($LASTEXITCODE -ne 0) { throw 'Cannot create Android GPU diagnostic path.' }

    & adb -s $Serial push $local $native | Out-Null

    if ($LASTEXITCODE -ne 0) { throw 'Cannot stage the native Vulkan kernel ELF.' }

    & adb -s $Serial shell chmod 700 $native | Out-Null

    if ($LASTEXITCODE -ne 0) { throw 'Cannot run the native Android kernel ELF.' }

    $apis = @('1.0')
    if ($TestCore11) { $apis += '1.1' }

    foreach ($api in $apis) {
        $result = Invoke-NativeKernel -Arguments @('--minimum', $api)

        Assert-NativeKernel -Api $api -Result $result

        $lines = @(
            'TEST=vulkan-backend-public-dynamic',
            "ABI=$chosen",
            "API=$api",
            "ADB_EXIT_CODE=$($result.ExitCode)",
            $result.Body
        )

        [System.IO.File]::WriteAllText(
            (Join-Path $build "$ReportPrefix-$api.log"),
            ([string]::Join([char]10, $lines)) + [char]10,
            [System.Text.UTF8Encoding]::new($false)
        )

        Write-Output "PASS Android $chosen Vulkan $api four declared stages/eight vkCmdDispatch/512 verified words"
    }

    $invalid = @(
        @{ Name = 'missing-arguments'; Args = @() },
        @{ Name = 'unsupported-api'; Args = @('--minimum','1.2') },
        @{ Name = 'wrong-flag'; Args = @('--api','1.0') }
    )

    $negative = @(
        'TEST=vulkan-backend-public-dynamic-invalid-inputs',
        "ABI=$chosen",
        "INVALID_CASES=$($invalid.Count)"
    )

    foreach ($case in $invalid) {
        $result = Invoke-NativeKernel -Arguments $case.Args

        if ($result.ExitCode -ne 2) {
            throw "Native Vulkan shader invalid CLI unexpectedly succeeded: $($case.Name)"
        }

        $negative += "INVALID_CASE=$($case.Name) exit=2"
    }

    [System.IO.File]::WriteAllText(
        (Join-Path $build "$ReportPrefix-negative-controls.log"),
        ([string]::Join([char]10, $negative)) + [char]10,
        [System.Text.UTF8Encoding]::new($false)
    )

    Write-Output "PASS three native Vulkan $chosen four-stage invalid-input requests rejected"
}
finally {
    & adb -s $Serial shell rm -rf $remote 2>$null | Out-Null
}
