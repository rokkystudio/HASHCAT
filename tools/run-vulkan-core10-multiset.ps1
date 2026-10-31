<#
.SYNOPSIS
Verify one- and two-set Vulkan 1.0 Compute descriptors on real Android GPUs.

.DESCRIPTION
Selects the executable from ro.product.cpu.abilist, never a GPU model.
Runs public integer Compute with sparse descriptor bindings, two storage
buffers and one std140 uniform buffer. Each recorded command is submitted
four times with independently checked public CPU reference values.

Both SPIR-V optimization variants run using default Vulkan 1.0 and an
explicit Vulkan 1.0 request. Optional tests require Vulkan 1.1 and
HOST_VISIBLE noncoherent memory, returning explicit incompatibility
rather than substituting a coherent type. Optional fault tests inject
one application failure after each successful Vulkan resource acquisition.

Raw Android diagnostics are archived in LF-only files and all staged
device artifacts are deleted even on failure.

.PARAMETER Serial
Exact ADB serial of one connected Android device.

.PARAMETER ArtifactDirectory
Directory with two Android binaries and four SPIR-V 1.0 shaders.

.PARAMETER Abi
auto chooses an actually reported Android ARM ABI; explicit ABI checks
the device's ro.product.cpu.abilist before staging.

.PARAMETER ReportPrefix
ASCII-only label used in archived local log filenames.

.PARAMETER Submissions
Number of repeated GPU command submissions from two to four.

.PARAMETER TestCore11
Also request Vulkan 1.1 explicitly, in both shader variants.

.PARAMETER TestNoncoherent
Require CPU staging and uniform memory without HOST_COHERENT, verifying
genuine flush/invalidate calls or the exact missing memory type.

.PARAMETER TestFaults
Inject every partial Vulkan object acquisition point for both set counts.
#>
param (
    [Parameter(Mandatory = $true)]
    [string]$Serial,

    [string]$ArtifactDirectory = '',

    [ValidateSet('auto', 'arm64-v8a', 'armeabi-v7a')]
    [string]$Abi = 'auto',

    [string]$ReportPrefix = 'multiset',

    [ValidateRange(2, 4)]
    [int]$Submissions = 4,

    [switch]$TestCore11,

    [switch]$TestNoncoherent,

    [switch]$TestFaults
)

$ErrorActionPreference = 'Stop'

if ($ReportPrefix -cnotmatch '^[A-Za-z0-9_-]+$') {
    throw 'ReportPrefix requires ASCII letters, numbers, dash or underscore.'
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

if ([string]::IsNullOrWhiteSpace($ArtifactDirectory)) {
    $ArtifactDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-multiset-20261031'
}
elseif (-not [System.IO.Path]::IsPathRooted($ArtifactDirectory)) {
    $ArtifactDirectory = Join-Path $root $ArtifactDirectory
}

$directory = (Resolve-Path -LiteralPath $ArtifactDirectory).Path
$deviceState = @(& adb -s $Serial get-state 2>&1)

if ($LASTEXITCODE -ne 0 -or ($deviceState -join '').Trim() -ne 'device') {
    throw "Android device is unavailable: $Serial"
}

$reported = @(& adb -s $Serial shell getprop ro.product.cpu.abilist 2>&1)

if ($LASTEXITCODE -ne 0 -or $reported.Count -ne 1) {
    throw 'Unable to read exact Android ABI capabilities.'
}

$available = @(($reported[0].Trim() -split ',') | Where-Object {
    -not [string]::IsNullOrWhiteSpace($_)
})

if ($Abi -eq 'auto') {
    if ($available -contains 'arm64-v8a') {
        $chosen = 'arm64-v8a'
    }
    elseif ($available -contains 'armeabi-v7a') {
        $chosen = 'armeabi-v7a'
    }
    else {
        throw "Android ARM ABI unsupported: $($available -join ',')"
    }
}
else {
    if ($available -notcontains $Abi) {
        throw "Required Android ABI unavailable: $Abi"
    }

    $chosen = $Abi
}

$binary = if ($chosen -eq 'arm64-v8a') {
    'vulkan-multiset-arm64'
}
else {
    'vulkan-multiset-armv7'
}

$files = @($binary)

foreach ($variant in @('one', 'two')) {
    foreach ($optimization in @('O0', 'O')) {
        $files += "multiset-$variant-$optimization-vk1.0.spv"
    }
}

foreach ($file in $files) {
    if (-not (Test-Path -LiteralPath (Join-Path $directory $file) -PathType Leaf)) {
        throw "Required Vulkan 1.0 multi-set artifact not available: $file"
    }
}

$remote = '/data/local/tmp'
$remoteBinary = "$remote/$binary"

$known = @(
    'da420145579e3ab0',
    '3dc96f338cbbffac',
    'fc6cfc42aeaff265',
    '00ba58c7ff9a53e2'
)

Write-Output "MULTI_SELECT_ABI=$chosen source=ro.product.cpu.abilist"

function Invoke-Multiset {
    <#
    .SYNOPSIS
    Execute one Android GPU workload and return its actual remote exit code.

    .DESCRIPTION
    Appends a unique shell exit marker rather than interpreting ADB stderr
    or PowerShell exceptions as the remote program's exit code. All GPU
    diagnostic lines are preserved as original LF-normalized text.
    #>
    param (
        [string[]]$Arguments
    )

    $command = (
        $remoteBinary + ' ' + ($Arguments -join ' ') +
        ' 2>&1; echo MULTI_DEVICE_EXIT=$?'
    )

    $lines = @(& adb -s $Serial shell $command 2>&1)

    if ($LASTEXITCODE -ne 0) {
        throw 'ADB failed while running the actual Vulkan multi-set workload.'
    }

    $output = ($lines | Out-String).Trim()
    $output = $output.Replace("`r`n", "`n").Replace("`r", '')

    $markers = [regex]::Matches(
        $output, '(?m)^MULTI_DEVICE_EXIT=(\d+)$'
    )

    if ($markers.Count -ne 1) {
        throw 'Cannot identify one exact Vulkan multi-set remote exit status.'
    }

    $status = [int]$markers[0].Groups[1].Value
    $body = [regex]::Replace(
        $output, '(?m)^MULTI_DEVICE_EXIT=\d+\n?', ''
    ).TrimEnd()

    return @{
        ExitCode = $status
        Body = $body
    }
}

function Save-Multiset {
    <#
    .SYNOPSIS
    Write one original Vulkan GPU run to a reproducible LF-only log.

    .DESCRIPTION
    Persists the exact GPU output and actual remote exit status with only
    the public descriptor layout, optimization and ABI metadata. Android
    serials and private device identifiers are not included.
    #>
    param (
        [int]$Sets,
        [string]$Mode,
        [string]$Optimization,
        [hashtable]$Result
    )

    $record = @(
        'TEST=vulkan-core10-multiset',
        "SETS=$Sets",
        "MODE=$Mode",
        "OPTIMIZATION=$Optimization",
        "ABI=$chosen",
        "ADB_EXIT_CODE=$($Result.ExitCode)",
        $Result.Body
    )

    [System.IO.File]::WriteAllText(
        (Join-Path $directory "$ReportPrefix-set$Sets-$Mode-$Optimization.log"),
        ($record -join "`n") + "`n",
        [System.Text.UTF8Encoding]::new($false)
    )
}

function Assert-MultisetSuccess {
    <#
    .SYNOPSIS
    Check exact descriptor set ownership, cache events and public CPU oracle.

    .DESCRIPTION
    Requires explicit Vulkan API selection, all 23/25 acquired resources,
    two storage and one uniform descriptor, four reused-command GPU
    completion events and a verified independent FNV-1a checksum for every
    changed uniform parameter generation.
    #>
    param (
        [int]$Sets,
        [string]$Mode,
        [string]$Optimization,
        [hashtable]$Result
    )

    if ($Result.ExitCode -ne 0) {
        throw "Vulkan multiset GPU failed set=$Sets mode=$Mode -$Optimization exit=$($Result.ExitCode): $($Result.Body)"
    }

    $api = if ($Mode -eq 'required11') { '1.1' } else { '1.0' }
    $acquired = 21 + 2 * $Sets

    foreach ($line in @(
        "SELECT_INSTANCE_API=$api ",
        "SELECT_DEVICE_API=$api ",
        "MULTI_SELECTED_DEVICE index=0 requested_api=$api effective_api=$api sets=$Sets ",
        "DESCRIPTOR_ACCEPT sets=$Sets bindings=3 storage=2 uniform=1",
        "MULTI_REQUEST sets=$Sets buffers=5 bindings=3 storage=2 uniform=1 stages=1 mapped=3 ",
        "MULTI_READY acquisitions=$acquired buffers=5 sets=$Sets bindings=3 storage=2 uniform=1 stages=1 mapped=3 api=1.0",
        "MULTI_DESTROY acquisitions=$acquired buffers=5 memories=5 maps=3 layouts=$Sets sets=$Sets shaders=1 pipelines=1 submissions=$Submissions idle_result=0"
    )) {
        if (-not $Result.Body.Contains($line)) {
            throw "Verified Vulkan multiset GPU evidence missing: $line"
        }
    }

    if (@([regex]::Matches(
        $Result.Body, '(?m)^MULTI_REQUIREMENTS index=\d+ '
    )).Count -ne 5) {
        throw 'Expected five queried Vulkan driver memory requirements.'
    }

    if (@([regex]::Matches(
        $Result.Body, '(?m)^GRAPH_PLAN_BUFFER index=\d+ '
    )).Count -ne 5) {
        throw 'Expected five jointly planned Vulkan buffer allocations.'
    }

    if (@([regex]::Matches(
        $Result.Body, '(?m)^MULTI_MAPPED index=\d+ '
    )).Count -ne 3) {
        throw 'Expected mapped input, output and uniform allocations.'
    }

    if (@([regex]::Matches(
        $Result.Body, '(?m)^MULTI_SET_LAYOUT index=\d+ '
    )).Count -ne $Sets -or
        @([regex]::Matches(
            $Result.Body, '(?m)^MULTI_SET_ALLOCATED index=\d+$'
        )).Count -ne $Sets) {
        throw "Vulkan descriptor set allocation/layout count mismatch: $Sets"
    }

    $bindings = @([regex]::Matches(
        $Result.Body,
        '(?m)^MULTI_DESCRIPTOR index=(\d+) set=(\d+) binding=(\d+) type=(storage|uniform) buffer=(\d+) range=(\d+)$'
    ))

    if ($bindings.Count -ne 3) {
        throw 'Expected two STORAGE_BUFFER and one UNIFORM_BUFFER binding.'
    }

    $expectedBindings = if ($Sets -eq 1) {
        @('0:2:storage:0:8192', '0:7:storage:1:8192', '0:4:uniform:4:16')
    }
    else {
        @('0:2:storage:0:8192', '1:1:storage:1:8192', '1:5:uniform:4:16')
    }

    for ($index = 0; $index -lt 3; $index++) {
        $binding = $bindings[$index]
        $current = (
            $binding.Groups[2].Value + ':' +
            $binding.Groups[3].Value + ':' +
            $binding.Groups[4].Value + ':' +
            $binding.Groups[5].Value + ':' +
            $binding.Groups[6].Value
        )

        if ([int]$binding.Groups[1].Value -ne $index -or
            $current -cne $expectedBindings[$index]) {
            throw "Unexpected Vulkan descriptor type/slot mapping at $index."
        }
    }

    $submits = @([regex]::Matches(
        $Result.Body,
        '(?m)^MULTI_SUBMIT completed=(\d+) reused=([01]) result=0$'
    ))
    $passes = @([regex]::Matches(
        $Result.Body,
        '(?m)^MULTI_PASS sets=(\d+) submission=(\d+) verified_words=2048 output=([0-9a-f]{16})$'
    ))
    $syncs = @([regex]::Matches(
        $Result.Body,
        '(?m)^MULTI_SYNC index=(\d+) operation=(flush|invalidate) coherent=([01]) driver_call=([01]) atom=(\d+) result=0$'
    ))

    if ($submits.Count -ne $Submissions -or
        $passes.Count -ne $Submissions -or
        $syncs.Count -ne 3 * $Submissions) {
        throw 'Missing Vulkan GPU fence, uniform update or cache synchronization.'
    }

    for ($index = 0; $index -lt $Submissions; $index++) {
        if ([int]$submits[$index].Groups[1].Value -ne $index + 1 -or
            [int]$submits[$index].Groups[2].Value -ne [int]($index -ne 0)) {
            throw "Recorded Vulkan GPU command reuse incorrect at $index."
        }

        if ([int]$passes[$index].Groups[1].Value -ne $Sets -or
            [int]$passes[$index].Groups[2].Value -ne $index -or
            $passes[$index].Groups[3].Value -cne $known[$index]) {
            throw "Public CPU/GPU uniform parameter output mismatch submission=$index."
        }

        $expectedSync = @('2:flush', '4:flush', '3:invalidate')

        for ($offset = 0; $offset -lt 3; $offset++) {
            $sync = $syncs[3 * $index + $offset]
            $actual = $sync.Groups[1].Value + ':' + $sync.Groups[2].Value

            if ($actual -cne $expectedSync[$offset] -or
                [int]$sync.Groups[4].Value -ne
                    1 - [int]$sync.Groups[3].Value) {
                throw "Vulkan upload/uniform/download cache dependency incorrect."
            }
        }
    }
}

try {
    foreach ($file in $files) {
        & adb -s $Serial push (Join-Path $directory $file) "$remote/$file" | Out-Null

        if ($LASTEXITCODE -ne 0) {
            throw "Cannot stage Vulkan 1.0 multi-set artifact: $file"
        }
    }

    & adb -s $Serial shell chmod 700 $remoteBinary

    if ($LASTEXITCODE -ne 0) {
        throw 'Android cannot execute selected Vulkan multi-set ABI.'
    }

    $modes = @('default10', 'explicit10')

    if ($TestCore11) {
        $modes += 'required11'
    }

    if ($TestNoncoherent) {
        $modes += 'noncoherent10'
    }

    foreach ($sets in @(1, 2)) {
        foreach ($mode in $modes) {
            foreach ($optimization in @('O0', 'O')) {
                $shaders = @(
                    "$remote/multiset-one-$optimization-vk1.0.spv",
                    "$remote/multiset-two-$optimization-vk1.0.spv"
                )
                $arguments = $shaders + @(
                    '--sets', "$sets",
                    '--submissions', "$Submissions"
                )

                if ($mode -eq 'explicit10') {
                    $arguments += @('--minimum', '1.0')
                }
                elseif ($mode -eq 'required11') {
                    $arguments += @('--minimum', '1.1')
                }
                elseif ($mode -eq 'noncoherent10') {
                    $arguments += '--require-noncoherent-staging'
                }

                $result = Invoke-Multiset -Arguments $arguments

                if ($mode -eq 'noncoherent10' -and $result.ExitCode -eq 1) {
                    foreach ($fragment in @(
                        "MULTI_REQUEST sets=$sets ",
                        'GRAPH_REJECT_MEMORY_TYPE index=',
                        'MULTI_CREATE_FAILED result=-8 fail_after=0',
                        'MULTI_NO_COMPATIBLE_CONFIGURATION'
                    )) {
                        if (-not $result.Body.Contains($fragment)) {
                            throw "Expected exact noncoherent incompatibility: $fragment"
                        }
                    }

                    if ($result.Body.Contains('MULTI_PASS sets=')) {
                        throw 'Incompatible noncoherent staging silently executed Compute.'
                    }

                    Write-Output "UNSUPPORTED $chosen sets=$sets -$optimization noncoherent memory: explicit device requirements"
                }
                else {
                    Assert-MultisetSuccess -Sets $sets -Mode $mode -Optimization $optimization -Result $result

                    if ($mode -eq 'noncoherent10') {
                        if (-not $result.Body.Contains('noncoherent=1') -or
                            @([regex]::Matches(
                                $result.Body,
                                '(?m)^MULTI_SYNC index=(2|3|4) operation=(flush|invalidate) coherent=0 driver_call=1 atom=\d+ result=0$'
                            )).Count -ne 3 * $Submissions) {
                            throw 'Actual Vulkan noncoherent uniform and staging cache calls were not executed.'
                        }
                    }

                    Write-Output "PASS $chosen sets=$sets -$optimization mode=$mode $Submissions real GPU submissions with uniform updates"
                }

                Save-Multiset -Sets $sets -Mode $mode -Optimization $optimization -Result $result
            }
        }
    }

    if ($TestFaults) {
        $faultLog = @(
            'TEST=vulkan-core10-multiset-faults',
            "ABI=$chosen",
            'SET_COUNTS=1,2'
        )

        foreach ($sets in @(1, 2)) {
            $maximum = 21 + 2 * $sets
            $shaders = @(
                "$remote/multiset-one-O-vk1.0.spv",
                "$remote/multiset-two-O-vk1.0.spv"
            )

            for ($point = 1; $point -le $maximum; $point++) {
                $arguments = $shaders + @(
                    '--sets', "$sets",
                    '--submissions', "$Submissions",
                    '--fail-after', "$point"
                )
                $result = Invoke-Multiset -Arguments $arguments

                if ($result.ExitCode -ne 42) {
                    throw "Multi-set Vulkan fault point $point sets=$sets returned $($result.ExitCode), expected 42."
                }

                foreach ($fragment in @(
                    "MULTI_INJECT_FAULT acquisition=$point resource=",
                    "MULTI_DESTROY acquisitions=$point ",
                    "MULTI_CREATE_FAILED result=-3 fail_after=$point",
                    'MULTI_NO_COMPATIBLE_CONFIGURATION'
                )) {
                    if (-not $result.Body.Contains($fragment)) {
                        throw "Multi-set resource fault missing expected event: $fragment"
                    }
                }

                if ($result.Body.Contains('MULTI_PASS sets=') -or
                    $result.Body.Contains('MULTI_SUBMIT completed=')) {
                    throw "Vulkan resource fault point $point unexpectedly executed GPU Compute."
                }

                $faultLog += "FAULT_CASE sets=$sets point=$point exit=42"
                $faultLog += @($result.Body -split "`n")
            }

            Write-Output "PASS $chosen sets=$sets all $maximum post-acquisition cleanup fault points"
        }

        [System.IO.File]::WriteAllText(
            (Join-Path $directory "$ReportPrefix-faults.log"),
            ($faultLog -join "`n") + "`n",
            [System.Text.UTF8Encoding]::new($false)
        )
    }

    $shaders = @(
        "$remote/multiset-one-O-vk1.0.spv",
        "$remote/multiset-two-O-vk1.0.spv"
    )
    $negatives = @(
        @{ Name = 'missing-sets'; Args = $shaders },
        @{ Name = 'zero-sets'; Args = $shaders + @('--sets', '0') },
        @{ Name = 'three-sets'; Args = $shaders + @('--sets', '3') },
        @{ Name = 'zero-submissions'; Args = $shaders + @('--sets', '1', '--submissions', '0') },
        @{ Name = 'five-submissions'; Args = $shaders + @('--sets', '1', '--submissions', '5') },
        @{ Name = 'invalid-api'; Args = $shaders + @('--sets', '1', '--minimum', '1.2') },
        @{ Name = 'zero-fault'; Args = $shaders + @('--sets', '1', '--fail-after', '0') },
        @{ Name = 'fault-over-limit'; Args = $shaders + @('--sets', '1', '--fail-after', '24') },
        @{ Name = 'missing-shader'; Args = @("$remote/missing-one.spv") + @($shaders[1]) + @('--sets', '1') }
    )

    $negativeLog = @(
        'TEST=vulkan-core10-multiset-invalid-inputs',
        "ABI=$chosen",
        'INVALID_CASES=9'
    )

    foreach ($case in $negatives) {
        $result = Invoke-Multiset -Arguments $case.Args

        if ($result.ExitCode -ne 2) {
            throw "Malformed Vulkan multi-set input $($case.Name) returned exit=$($result.ExitCode)."
        }

        $negativeLog += "INVALID_CASE=$($case.Name) exit=2"
        Write-Output "PASS $chosen invalid multi-set CLI $($case.Name) exit=2"
    }

    [System.IO.File]::WriteAllText(
        (Join-Path $directory "$ReportPrefix-negative-controls.log"),
        ($negativeLog -join "`n") + "`n",
        [System.Text.UTF8Encoding]::new($false)
    )

    Write-Output "PASS Vulkan 1.0 device-capability-driven storage+uniform multi-set GPU runtime $chosen"
}
finally {
    foreach ($file in $files) {
        & adb -s $Serial shell rm -f "$remote/$file" 2>$null | Out-Null
    }
}
