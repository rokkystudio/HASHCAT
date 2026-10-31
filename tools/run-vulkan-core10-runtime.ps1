<#
.SYNOPSIS
Tests a reusable Vulkan 1.0 Compute resource runtime on one Android device.

.DESCRIPTION
Chooses an Android API 24 native executable by ro.product.cpu.abilist,
never by phone model or GPU vendor. Uploads the runtime and three public
SPIR-V 1.0 shader stages in both compiler optimization modes.

Exercises repeated Vulkan 1.0 command-buffer submissions, CPU/GPU output
verification, coherent or noncoherent staging, and optional core 1.1.
-TestFaults deliberately fails after every one of 28 Vulkan acquisitions,
verifying that the runtime destroys all partially acquired resources.
Malformed command-line arguments are checked independently.

The original driver output is preserved as LF-only logs without Android
serials. Temporary executable/shader files are removed from the device.

.PARAMETER Serial
Exact ADB serial of an Android device connected to this computer.

.PARAMETER ArtifactDirectory
Local directory containing both ABI executables and six SPIR-V modules.

.PARAMETER Abi
auto uses arm64-v8a if reported, otherwise armeabi-v7a if reported.
An explicit unsupported ABI is rejected before any ADB upload.

.PARAMETER ReportPrefix
Safe ASCII prefix used for each archived log.

.PARAMETER Submissions
Number of sequential GPU submissions, from 2 to 16.

.PARAMETER TestCore11
Also require core Vulkan 1.1 for two additional synthetic GPU runs.

.PARAMETER TestNoncoherent
Require HOST_VISIBLE staging memory without HOST_COHERENT; record a real
cache-management GPU success or precise incompatibility without fallback.

.PARAMETER TestFaults
Inject and verify all 28 deterministic Vulkan resource-acquisition errors.
#>
param (
    [Parameter(Mandatory = $true)]
    [string]$Serial,

    [string]$ArtifactDirectory = '',

    [ValidateSet('auto', 'arm64-v8a', 'armeabi-v7a')]
    [string]$Abi = 'auto',

    [string]$ReportPrefix = 'runtime',

    [ValidateRange(2, 16)]
    [int]$Submissions = 4,

    [switch]$TestCore11,

    [switch]$TestNoncoherent,

    [switch]$TestFaults
)

$ErrorActionPreference = 'Stop'

if ($ReportPrefix -cnotmatch '^[A-Za-z0-9_-]+$') {
    throw 'ReportPrefix must contain only ASCII letters, numbers, underscore or hyphen.'
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

if ([string]::IsNullOrWhiteSpace($ArtifactDirectory)) {
    $ArtifactDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-runtime-20261031'
}
elseif (-not [System.IO.Path]::IsPathRooted($ArtifactDirectory)) {
    $ArtifactDirectory = Join-Path $root $ArtifactDirectory
}

$directory = (Resolve-Path -LiteralPath $ArtifactDirectory).Path
$state = @(& adb -s $Serial get-state 2>&1)

if ($LASTEXITCODE -ne 0 -or ($state -join '').Trim() -ne 'device') {
    throw "Requested Android device is unavailable: $Serial"
}

$reported = @(& adb -s $Serial shell getprop ro.product.cpu.abilist 2>&1)

if ($LASTEXITCODE -ne 0 -or $reported.Count -ne 1) {
    throw 'Android ABI capability list is unavailable.'
}

$abis = @(($reported[0].Trim() -split ',') | Where-Object {
    -not [string]::IsNullOrWhiteSpace($_)
})

if ($Abi -eq 'auto') {
    if ($abis -contains 'arm64-v8a') {
        $selectedAbi = 'arm64-v8a'
    }
    elseif ($abis -contains 'armeabi-v7a') {
        $selectedAbi = 'armeabi-v7a'
    }
    else {
        throw "No supported Android ARM ABI: $($abis -join ',')"
    }
}
else {
    if ($abis -notcontains $Abi) {
        throw "Required Android ABI $Abi unavailable: $($abis -join ',')"
    }

    $selectedAbi = $Abi
}

$executable = if ($selectedAbi -eq 'arm64-v8a') {
    'vulkan-runtime-arm64'
}
else {
    'vulkan-runtime-armv7'
}

$files = @($executable)

foreach ($mode in @('O0', 'O')) {
    foreach ($stage in @('a', 'b', 'c')) {
        $files += "stage-$stage-$mode-vk1.0.spv"
    }
}

foreach ($file in $files) {
    if (-not (Test-Path -LiteralPath (Join-Path $directory $file) -PathType Leaf)) {
        throw "Vulkan 1.0 runtime artifact missing: $file"
    }
}

Write-Output "SELECT_ANDROID_ABI=$selectedAbi source=ro.product.cpu.abilist"

$remote = '/data/local/tmp'
$remoteExecutable = "$remote/$executable"

$known = @(
    'de56d6d0aa66c6ec c1259ad4f024499a 05de70c62f8d7cfd',
    '66ee1b4b1385e0e3 27fb4f34e64d89a7 7509bfa6ed0455d8',
    '69b8f65ae2e8be72 d8038be5522a0b58 e50a539d4459e45b',
    '8fb63465a93a5631 ac37a5c3b35fc366 07d63cfd9773324d'
)

function Invoke-DeviceRuntime {
    <#
    .SYNOPSIS
    Execute one remote Vulkan command and preserve its actual remote exit code.

    .DESCRIPTION
    Uses an explicit shell exit-code line because expected nonzero Vulkan
    error tests can produce ADB stderr and the host's stderr preference is
    not an indication of the remote program's return status. The output
    remains unmodified except for removing the appended exit marker and
    normalizing line endings to LF.
    #>
    param (
        [string[]]$Arguments,

        [string]$Mode,

        [string]$Optimization
    )

    $remoteCommand = (
        $remoteExecutable + ' ' + ($Arguments -join ' ') +
        ' 2>&1; echo RUNTIME_DEVICE_EXIT=$?'
    )

    $lines = @(& adb -s $Serial shell $remoteCommand 2>&1)

    if ($LASTEXITCODE -ne 0) {
        throw "ADB failed while running $Mode -$Optimization."
    }

    $output = ($lines | Out-String).Trim()
    $output = $output.Replace("`r`n", "`n").Replace("`r", '')

    $markers = [regex]::Matches(
        $output, '(?m)^RUNTIME_DEVICE_EXIT=(\d+)$'
    )

    if ($markers.Count -ne 1) {
        throw "Remote Vulkan exit status not found for $Mode -$Optimization."
    }

    $code = [int]$markers[0].Groups[1].Value
    $body = [regex]::Replace(
        $output, '(?m)^RUNTIME_DEVICE_EXIT=\d+\n?', ''
    ).TrimEnd()

    return @{
        ExitCode = $code
        Body = $body
    }
}

function Save-DeviceRuntime {
    <#
    .SYNOPSIS
    Save one original Vulkan device-test record with exact normalized LF bytes.

    .DESCRIPTION
    Includes mode, optimization, detected ABI and the actual remote exit
    status. Does not include the ADB serial, which is private device metadata.
    #>
    param (
        [string]$Mode,

        [string]$Optimization,

        [hashtable]$Result,

        [string]$Name
    )

    $record = @(
        'TEST=vulkan-core10-reusable-runtime',
        "MODE=$Mode",
        "OPTIMIZATION=$Optimization",
        "ABI=$selectedAbi",
        "ADB_EXIT_CODE=$($Result.ExitCode)",
        $Result.Body
    )

    [System.IO.File]::WriteAllText(
        (Join-Path $directory "$ReportPrefix-$Name.log"),
        ($record -join "`n") + "`n",
        [System.Text.UTF8Encoding]::new($false)
    )
}

function Assert-DeviceRuntimeSuccess {
    <#
    .SYNOPSIS
    Check all expected GPU submissions, barriers and resource teardown.

    .DESCRIPTION
    Requires one result and one fence completion per submission, two cache
    synchronization events per submission, the known public CPU checksums
    for the first four input generations, and exactly 28 acquisitions
    released as six buffers, six allocations and three Compute pipelines.
    #>
    param (
        [hashtable]$Result,

        [string]$Mode,

        [string]$Optimization
    )

    if ($Result.ExitCode -ne 0) {
        throw "Vulkan runtime GPU failed for $Mode -$Optimization (exit=$($Result.ExitCode)): $($Result.Body)"
    }

    $expectedVersion = if ($Mode -eq 'required11') { '1.1' } else { '1.0' }

    foreach ($marker in @(
        "SELECT_INSTANCE_API=$expectedVersion ",
        "SELECT_DEVICE_API=$expectedVersion ",
        "RUNTIME_SELECTED_DEVICE index=0 effective_api=$expectedVersion requested_api=$expectedVersion ",
        'RUNTIME_READY acquisitions=28 buffers=6 pipelines=3 staging=2',
        "RUNTIME_DESTROY acquired=28 buffers=6 allocations=6 mapped=2 shaders=3 pipelines=3 submissions=$Submissions idle_result=0"
    )) {
        if (-not $Result.Body.Contains($marker)) {
            throw "Runtime successful GPU evidence missing: $marker"
        }
    }

    if (@([regex]::Matches(
        $Result.Body, '(?m)^MULTISTAGE_PLAN_BUFFER index=\d+'
    )).Count -ne 6) {
        throw 'Exact six-buffer Vulkan memory planning evidence missing.'
    }

    $completed = @([regex]::Matches(
        $Result.Body,
        '(?m)^RUNTIME_SUBMIT complete=(\d+) command_reused=([01]) result=0$'
    ))

    if ($completed.Count -ne $Submissions) {
        throw 'The runtime did not submit the recorded command the requested number of times.'
    }

    for ($index = 0; $index -lt $Submissions; $index++) {
        if ([int]$completed[$index].Groups[1].Value -ne ($index + 1) -or
            [int]$completed[$index].Groups[2].Value -ne [int]($index -ne 0)) {
            throw "Runtime fence sequence mismatch at submission=$index."
        }
    }

    $syncs = @([regex]::Matches(
        $Result.Body,
        '(?m)^RUNTIME_SYNC index=(\d+) operation=(flush|invalidate) coherent=([01]) driver_call=([01]) atom=(\d+) result=0$'
    ))

    if ($syncs.Count -ne $Submissions * 2) {
        throw 'Incorrect number of Vulkan flush/invalidate calls.'
    }

    for ($index = 0; $index -lt $syncs.Count; $index++) {
        $sync = $syncs[$index]

        if ($index % 2 -eq 0) {
            if ($sync.Groups[1].Value -ne '4' -or
                $sync.Groups[2].Value -ne 'flush') {
                throw "Runtime GPU upload cache ordering mismatch at index=$index."
            }
        }
        else {
            if ($sync.Groups[1].Value -ne '5' -or
                $sync.Groups[2].Value -ne 'invalidate') {
                throw "Runtime GPU download cache ordering mismatch at index=$index."
            }
        }

        if ($sync.Groups[4].Value -ne [string](1 - [int]$sync.Groups[3].Value)) {
            throw 'Vulkan coherent cache synchronization rule violated.'
        }
    }

    $checksums = @([regex]::Matches(
        $Result.Body,
        '(?m)^RUNTIME_PASS submission=(\d+) verified_words=12288 stageA=([0-9a-f]{16}) stageB=([0-9a-f]{16}) output=([0-9a-f]{16})$'
    ))

    if ($checksums.Count -ne $Submissions) {
        throw 'Missing independent CPU verification for a Vulkan command replay.'
    }

    for ($index = 0; $index -lt $Submissions; $index++) {
        if ([int]$checksums[$index].Groups[1].Value -ne $index) {
            throw "GPU output from an unexpected input generation at submission=$index."
        }

        if ($index -lt $known.Count) {
            $actual = (
                $checksums[$index].Groups[2].Value + ' ' +
                $checksums[$index].Groups[3].Value + ' ' +
                $checksums[$index].Groups[4].Value
            )

            if ($actual -cne $known[$index]) {
                throw "Independent CPU checksum mismatch at submission=$index."
            }
        }
    }
}

try {
    foreach ($file in $files) {
        & adb -s $Serial push (Join-Path $directory $file) "$remote/$file" | Out-Null

        if ($LASTEXITCODE -ne 0) {
            throw "Could not upload public Vulkan 1.0 artifact: $file"
        }
    }

    & adb -s $Serial shell chmod 700 $remoteExecutable

    if ($LASTEXITCODE -ne 0) {
        throw 'Android executable permission failed.'
    }

    $modes = @('default10', 'explicit10')

    if ($TestCore11) {
        $modes += 'required11'
    }

    foreach ($mode in $modes) {
        foreach ($optimization in @('O0', 'O')) {
            $shaders = @(
                "$remote/stage-a-$optimization-vk1.0.spv",
                "$remote/stage-b-$optimization-vk1.0.spv",
                "$remote/stage-c-$optimization-vk1.0.spv"
            )
            $arguments = $shaders + @('--submissions', "$Submissions")

            if ($mode -eq 'explicit10') {
                $arguments += @('--minimum', '1.0')
            }
            elseif ($mode -eq 'required11') {
                $arguments += @('--minimum', '1.1')
            }

            $result = Invoke-DeviceRuntime -Arguments $arguments -Mode $mode -Optimization $optimization
            Assert-DeviceRuntimeSuccess -Result $result -Mode $mode -Optimization $optimization
            Save-DeviceRuntime -Result $result -Mode $mode -Optimization $optimization -Name "$mode-$optimization"

            Write-Output "PASS $selectedAbi $mode -$optimization Vulkan 1.0 reusable command: $Submissions submits, 12,288 words per submit"
        }
    }

    if ($TestNoncoherent) {
        foreach ($optimization in @('O0', 'O')) {
            $shaders = @(
                "$remote/stage-a-$optimization-vk1.0.spv",
                "$remote/stage-b-$optimization-vk1.0.spv",
                "$remote/stage-c-$optimization-vk1.0.spv"
            )
            $arguments = $shaders + @(
                '--submissions', "$Submissions",
                '--require-noncoherent-staging'
            )
            $mode = 'noncoherent10'
            $result = Invoke-DeviceRuntime -Arguments $arguments -Mode $mode -Optimization $optimization

            if ($result.ExitCode -eq 0) {
                Assert-DeviceRuntimeSuccess -Result $result -Mode $mode -Optimization $optimization

                if (-not $result.Body.Contains(
                    'RUNTIME_STAGING_REQUEST mode=noncoherent'
                ) -or -not $result.Body.Contains(
                    'upload_coherent=0 download_coherent=0'
                )) {
                    throw 'Noncoherent runtime execution did not select noncoherent memory.'
                }

                foreach ($sync in @([regex]::Matches(
                    $result.Body, '(?m)^RUNTIME_SYNC index='
                ))) {
                    $line = ($result.Body.Substring($sync.Index) -split "`n")[0]

                    if ($line -notmatch 'coherent=0 driver_call=1 atom=\d+ result=0$') {
                        throw 'Required noncoherent Vulkan cache driver operation was not invoked.'
                    }
                }

                Write-Output "PASS $selectedAbi real noncoherent -$optimization Vulkan1.0 reusable command and driver flush/invalidate"
            }
            elseif ($result.ExitCode -eq 1) {
                foreach ($required in @(
                    'REJECT_MULTISTAGE_MEMORY_TYPE index=',
                    'RUNTIME_CREATE_FAILED result=-8 fault_after=0',
                    'RUNTIME_NO_COMPATIBLE_EXECUTION'
                )) {
                    if (-not $result.Body.Contains($required)) {
                        throw "Missing exact noncoherent incompatibility explanation: $required"
                    }
                }

                if ($result.Body.Contains('RUNTIME_PASS submission=')) {
                    throw 'Incompatible GPU staging memory was silently replaced.'
                }

                Write-Output "UNSUPPORTED $selectedAbi noncoherent -${optimization}: precise memory type rejection, no fallback"
            }
            else {
                throw "Unexpected noncoherent runtime exit=$($result.ExitCode)."
            }

            Save-DeviceRuntime -Result $result -Mode $mode -Optimization $optimization -Name "$mode-$optimization"
        }
    }

    if ($TestFaults) {
        $optimization = 'O'
        $shaders = @(
            "$remote/stage-a-$optimization-vk1.0.spv",
            "$remote/stage-b-$optimization-vk1.0.spv",
            "$remote/stage-c-$optimization-vk1.0.spv"
        )
        $faultLines = @(
            'TEST=vulkan-core10-runtime-faults',
            "ABI=$selectedAbi",
            'FAULT_POINTS=28'
        )

        for ($point = 1; $point -le 28; $point++) {
            $arguments = $shaders + @(
                '--submissions', "$Submissions",
                '--fail-after', "$point"
            )
            $result = Invoke-DeviceRuntime -Arguments $arguments -Mode 'fault' -Optimization $optimization

            if ($result.ExitCode -ne 1) {
                throw "Fault point $point returned unexpected exit=$($result.ExitCode)."
            }

            foreach ($required in @(
                "RUNTIME_INJECT_FAULT acquisition=$point resource=",
                "RUNTIME_DESTROY acquired=$point ",
                "RUNTIME_CREATE_FAILED result=-3 fault_after=$point",
                'RUNTIME_NO_COMPATIBLE_EXECUTION'
            )) {
                if (-not $result.Body.Contains($required)) {
                    throw "Fault point $point did not produce the required cleanup event: $required"
                }
            }

            if ($result.Body.Contains('RUNTIME_PASS submission=')) {
                throw "Fault injection point $point unexpectedly dispatched GPU work."
            }

            $lines = @($result.Body -split "`n")
            $faultLines += "FAULT_POINT=$point exit=1"
            $faultLines += $lines
        }

        [System.IO.File]::WriteAllText(
            (Join-Path $directory "$ReportPrefix-faults.log"),
            ($faultLines -join "`n") + "`n",
            [System.Text.UTF8Encoding]::new($false)
        )

        Write-Output "PASS $selectedAbi all 28 genuine Vulkan resource acquisition/partial cleanup fault points"
    }

    $shaders = @(
        "$remote/stage-a-O-vk1.0.spv",
        "$remote/stage-b-O-vk1.0.spv",
        "$remote/stage-c-O-vk1.0.spv"
    )

    $invalids = @(
        @{ Label = 'zero-submissions'; Args = $shaders + @('--submissions', '0') },
        @{ Label = 'seventeen-submissions'; Args = $shaders + @('--submissions', '17') },
        @{ Label = 'invalid-version'; Args = $shaders + @('--minimum', '1.2') },
        @{ Label = 'invalid-fault'; Args = $shaders + @('--fail-after', '29') },
        @{ Label = 'missing-shader'; Args = @("$remote/no-stage-a.spv") + $shaders[1..2] }
    )

    $negativeLines = @(
        'TEST=vulkan-core10-runtime-invalid-inputs',
        "ABI=$selectedAbi",
        'INVALID_CASES=5'
    )

    foreach ($case in $invalids) {
        $result = Invoke-DeviceRuntime -Arguments $case.Args -Mode 'invalid' -Optimization $case.Label

        if ($result.ExitCode -ne 2) {
            throw "Invalid runtime command '$($case.Label)' returned $($result.ExitCode) instead of 2."
        }

        $negativeLines += "INVALID_CASE=$($case.Label) exit=2"

        Write-Output "PASS $selectedAbi runtime invalid input $($case.Label): exit=2"
    }

    [System.IO.File]::WriteAllText(
        (Join-Path $directory "$ReportPrefix-negative-controls.log"),
        ($negativeLines -join "`n") + "`n",
        [System.Text.UTF8Encoding]::new($false)
    )

    Write-Output "PASS Vulkan 1.0 reusable runtime for $selectedAbi; no vendor-specific defaults"
}
finally {
    foreach ($file in $files) {
        & adb -s $Serial shell rm -f "$remote/$file" 2>$null | Out-Null
    }
}
