<#
.SYNOPSIS
Verify two production HASHCAT SPIR-V public Compute stages on Android GPU.

.DESCRIPTION
Uses a physical Android GPU to execute two public SPIR-V 1.0
Compute pipelines in the production backend implementation. Stage
one writes 64 integers and stage two reads the same intermediate
storage buffer after a COMPUTE-to-COMPUTE memory barrier. Every
intermediate and final word must match independent CPU expressions.
Each native Vulkan API mode runs two independent VkInstance/device
lifecycles. No hashing, candidates or password processing occurs.

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
$build = Join-Path $root 'build\diagnostic-multimode\vulkan-backend-public-chain-20261101'
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
    'vulkan-backend-public-chain-arm64'
}
else {
    'vulkan-backend-public-chain-armv7'
}

$local = Join-Path $build $name
$remote = "/data/local/tmp/hc-backend-public-chain-20261101-$ReportPrefix"
$native = "$remote/$name"

if (-not (Test-Path -LiteralPath $local -PathType Leaf)) {
    throw "Missing production Vulkan public-chain test ELF: $local"
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
    Require four real Compute dispatches across two Vulkan lifecycles.

    .DESCRIPTION
    Validates two independent real Vulkan instances, two shader
    pipelines and descriptor sets per trial, four vkCmdDispatch calls,
    256 checked uint32 outputs across intermediate/final buffers,
    exact resource cleanup and unchanged HASHCAT hashing devices.
    #>
    param ([string]$Api, [hashtable]$Result)

    if ($Result.ExitCode -ne 0) {
        throw "Native Vulkan public-kernel process failed: $($Result.Body)"
    }

    $body = $Result.Body
    $lines = @($body -split [char]10)

    $complete = "BACKEND_PUBLIC_CHAIN_DIAGNOSTIC_PASS api=$Api trials=2 dispatches=4 descriptors=4 storage_buffers=3 verified_words=256 shaders=2 password_inputs=0 scheduled=0"

    if ($lines[-1] -cne $complete) {
        throw 'Vulkan GPU compute verification summary is invalid.'
    }

    $counts = @{
        'DEVICE_CATALOG_OPEN ' = 2
        'DEVICE_CATALOG_CLOSE ' = 2
        'BACKEND_VULKAN_CATALOG_READY ' = 2
        'BACKEND_VULKAN_CHAIN_READY ' = 2
        'BACKEND_VULKAN_CHAIN_DISPATCH_PASS ' = 2
        'BACKEND_VULKAN_CHAIN_CLOSE ' = 2
        'BACKEND_PUBLIC_CHAIN_TRIAL_PASS ' = 2
        'DEVICE_CATALOG_SELECT ' = 2
    }

    foreach ($prefix in $counts.Keys) {
        $actual = @($lines | Where-Object { $_.StartsWith($prefix) }).Count

        if ($actual -ne $counts[$prefix]) {
            throw "Missing real Vulkan shader/ownership events $prefix actual=$actual"
        }
    }

    foreach ($trial in @(0, 1)) {
        $event = "BACKEND_PUBLIC_CHAIN_TRIAL_PASS trial=$trial descriptors=4 pipelines=2 dispatches=2 verified_words=128 scheduled=0"

        if (@($lines | Where-Object { $_ -ceq $event }).Count -ne 1) {
            throw "Native Vulkan public kernel trial verification failed: $event"
        }
    }

    $shaderEvents = @(
        $lines | Where-Object {
            $_.StartsWith('BACKEND_VULKAN_CHAIN_DISPATCH_PASS ')
        }
    )

    foreach ($event in $shaderEvents) {
        if ($event -cnotmatch '^BACKEND_VULKAN_CHAIN_DISPATCH_PASS stage1=public_u32_transform stage2=public_u32_rotate words=64 local=32 groups=2 dispatches=2 verified_first=64 verified_second=64 coherent_input=[01] coherent_intermediate=[01] coherent_output=[01] barrier=shader_write_to_shader_read shaders=2 password_inputs=0 scheduled=0$') {
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
            'TEST=vulkan-backend-public-chain',
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

        Write-Output "PASS Android $chosen Vulkan $api two SPIR-V stages/four vkCmdDispatch/256 verified words"
    }

    $invalid = @(
        @{ Name = 'missing-arguments'; Args = @() },
        @{ Name = 'unsupported-api'; Args = @('--minimum','1.2') },
        @{ Name = 'wrong-flag'; Args = @('--api','1.0') }
    )

    $negative = @(
        'TEST=vulkan-backend-public-chain-invalid-inputs',
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

    Write-Output "PASS three native Vulkan $chosen two-stage invalid-input requests rejected"
}
finally {
    & adb -s $Serial shell rm -rf $remote 2>$null | Out-Null
}
