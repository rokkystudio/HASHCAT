<#
.SYNOPSIS
Execute staged Vulkan 1.0 multi-set Compute graphs on an Android GPU.

.DESCRIPTION
Selects the executable from ro.product.cpu.abilist and executes explicit
one-, two- and three-stage Compute graphs with one or two descriptor sets.
Tests optimized/unoptimized SPIR-V 1.0 modules, repeated fence-separated
submissions, shader-to-shader barriers, per-stage readbacks, CPU-updated
std140 uniforms, and real coherent or noncoherent HOST_VISIBLE memory.

Optional acquisition faults validate partial Vulkan resource cleanup.
All raw GPU output is preserved as LF-only text, and staged artifacts are
deleted from the connected device after the test session.

.PARAMETER Serial
Exact adb serial of a connected device.

.PARAMETER Abi
Android ARM ABI advertised by the device, or auto for a supported choice.

.PARAMETER ArtifactDirectory
Directory containing both native binaries and twelve SPIR-V modules.

.PARAMETER ReportPrefix
ASCII-only label used in reproducible log filenames.

.PARAMETER Submissions
Number of repeated GPU submits per configuration, from two to four.

.PARAMETER TestCore11
Also request Vulkan 1.1 without requiring it for the baseline.

.PARAMETER TestNoncoherent
Exercise true noncoherent staging/uniform memory or demand a precise
VK_ERROR_FEATURE_NOT_PRESENT when incompatible.

.PARAMETER TestFaults
Inject every Vulkan post-acquisition error for the smallest and largest
graphs to verify exact partial cleanup and absence of GPU dispatches.
#>
param (
    [Parameter(Mandatory = $true)]
    [string]$Serial,

    [ValidateSet('auto', 'arm64-v8a', 'armeabi-v7a')]
    [string]$Abi = 'auto',

    [string]$ArtifactDirectory = '',

    [string]$ReportPrefix = 'multigraph',

    [ValidateRange(2, 4)]
    [int]$Submissions = 4,

    [switch]$TestCore11,

    [switch]$TestNoncoherent,

    [switch]$TestFaults
)

$ErrorActionPreference = 'Stop'

if ($ReportPrefix -cnotmatch '^[A-Za-z0-9_-]+$') {
    throw 'ReportPrefix must contain ASCII letters, digits, dash or underscore.'
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

if ([string]::IsNullOrWhiteSpace($ArtifactDirectory)) {
    $ArtifactDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-multiset-graph-20261031'
}
elseif (-not [System.IO.Path]::IsPathRooted($ArtifactDirectory)) {
    $ArtifactDirectory = Join-Path $root $ArtifactDirectory
}

$directory = (Resolve-Path -LiteralPath $ArtifactDirectory).Path
$deviceState = @(& adb -s $Serial get-state 2>&1)

if ($LASTEXITCODE -ne 0 -or ($deviceState -join '').Trim() -ne 'device') {
    throw "Android device unavailable: $Serial"
}

$reported = @(& adb -s $Serial shell getprop ro.product.cpu.abilist 2>&1)

if ($LASTEXITCODE -ne 0 -or $reported.Count -ne 1) {
    throw 'Cannot query supported Android process ABIs.'
}

$available = @($reported[0].Trim() -split ',' | Where-Object {
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
        throw "Android ABI is unsupported: $($available -join ',')"
    }
}
elseif ($available -contains $Abi) {
    $chosen = $Abi
}
else {
    throw "Required ABI is not advertised: $Abi"
}

$binary = if ($chosen -eq 'arm64-v8a') {
    'vulkan-multigraph-arm64'
}
else {
    'vulkan-multigraph-armv7'
}

$files = @($binary)

foreach ($sets in @(1, 2)) {
    foreach ($stage in @(0, 1, 2)) {
        foreach ($optimization in @('O0', 'O')) {
            $files += "multigraph-set$sets-stage$stage-$optimization-vk1.0.spv"
        }
    }
}

foreach ($name in $files) {
    if (-not (Test-Path -LiteralPath (Join-Path $directory $name) -PathType Leaf)) {
        throw "Required Vulkan multi-stage artifact missing: $name"
    }
}

$remote = '/data/local/tmp/hc-vulkan-multigraph-20261031'
$remoteBinary = "$remote/$binary"

Write-Output "MULTIGRAPH_SELECT_ABI=$chosen source=ro.product.cpu.abilist"

function Invoke-Graph {
    <#
    .SYNOPSIS
    Execute one Android graph and extract its exact remote exit code.

    .DESCRIPTION
    Executes literal, caller-supplied synthetic workload arguments and
    captures the complete GPU log. A unique exit marker identifies the
    shell command's exit status independently of adb transport status.
    #>
    param (
        [string[]]$Arguments
    )

    $command = (
        $remoteBinary + ' ' + ($Arguments -join ' ') +
        ' 2>&1; echo MULTIGRAPH_DEVICE_EXIT=$?'
    )
    $lines = @(& adb -s $Serial shell $command 2>&1)

    if ($LASTEXITCODE -ne 0) {
        throw 'ADB failed during Vulkan Compute execution.'
    }

    $output = (($lines -join "`n").Replace("`r", '')).TrimEnd()
    $markers = [regex]::Matches(
        $output, '(?m)^MULTIGRAPH_DEVICE_EXIT=(\d+)$'
    )

    if ($markers.Count -ne 1) {
        throw 'Cannot identify exactly one Android Vulkan exit status.'
    }

    $exitCode = [int]$markers[0].Groups[1].Value
    $body = [regex]::Replace(
        $output, '(?m)^MULTIGRAPH_DEVICE_EXIT=\d+\n?', ''
    ).TrimEnd()

    return @{
        ExitCode = $exitCode
        Body = $body
    }
}

function Get-Shaders {
    <#
    .SYNOPSIS
    Return the three exact SPIR-V 1.0 modules for one declared layout.
    #>
    param (
        [int]$Sets,
        [string]$Optimization
    )

    return @(
        foreach ($stage in @(0, 1, 2)) {
            "$remote/multigraph-set$Sets-stage$stage-$Optimization-vk1.0.spv"
        }
    )
}

function Save-Graph {
    <#
    .SYNOPSIS
    Persist the complete original GPU transcript and public run metadata.

    .DESCRIPTION
    Uses UTF-8 without BOM and LF line endings. Device identifiers and
    private paths are not included in the archived log.
    #>
    param (
        [int]$Sets,
        [int]$Stages,
        [string]$Mode,
        [string]$Optimization,
        [hashtable]$Result
    )

    $rows = @(
        'TEST=vulkan-core10-multiset-graph',
        "SETS=$Sets",
        "STAGES=$Stages",
        "MODE=$Mode",
        "OPTIMIZATION=$Optimization",
        "ABI=$chosen",
        "ADB_EXIT_CODE=$($Result.ExitCode)",
        $Result.Body
    )

    [System.IO.File]::WriteAllText(
        (Join-Path $directory "$ReportPrefix-set$Sets-stages$Stages-$Mode-$Optimization.log"),
        ($rows -join "`n") + "`n",
        [System.Text.UTF8Encoding]::new($false)
    )
}

function Assert-GraphSuccess {
    <#
    .SYNOPSIS
    Validate runtime ownership, ordered stage barriers and all GPU outputs.

    .DESCRIPTION
    Checks the explicit Vulkan API, descriptor counts, expected buffer
    and acquisition counts, all stage dispatches, every fence-separated
    submission and every CPU/GPU checksum. Requires coherent or actual
    noncoherent cache operations consistent with the selected mode.
    #>
    param (
        [int]$Sets,
        [int]$Stages,
        [string]$Mode,
        [hashtable]$Result
    )

    if ($Result.ExitCode -ne 0) {
        throw "Vulkan Compute failed sets=$Sets stages=$Stages mode=$Mode exit=$($Result.ExitCode): $($Result.Body)"
    }

    $api = if ($Mode -eq 'required11') { '1.1' } else { '1.0' }
    $buffers = 2 * $Stages + 3
    $bindings = $Stages + 2
    $mapped = $Stages + 2
    $acquisitions = 14 + 7 * $Stages + 2 * $Sets

    foreach ($line in @(
        "SELECT_INSTANCE_API=$api ",
        "SELECT_DEVICE_API=$api ",
        "DESCRIPTOR_ACCEPT sets=$Sets bindings=$bindings storage=$($Stages + 1) uniform=1",
        "MULTIGRAPH_REQUEST sets=$Sets stages=$Stages buffers=$buffers bindings=$bindings mapped=$mapped ",
        "MULTI_READY acquisitions=$acquisitions buffers=$buffers sets=$Sets bindings=$bindings storage=$($Stages + 1) uniform=1 stages=$Stages mapped=$mapped api=1.0",
        "MULTIGRAPH_ACCEPT stages=$Stages sets=$Sets buffers=$buffers bytes=8192",
        "MULTI_DESTROY acquisitions=$acquisitions buffers=$buffers memories=$buffers maps=$mapped layouts=$Sets sets=$Sets shaders=$Stages pipelines=$Stages submissions=$Submissions idle_result=0"
    )) {
        if (-not $Result.Body.Contains($line)) {
            throw "GPU transcript missing the exact resource contract: $line"
        }
    }

    foreach ($check in @(
        @{ Pattern = '(?m)^MULTIGRAPH_DISPATCH stage=\d+ '; Expected = $Stages },
        @{ Pattern = '(?m)^MULTIGRAPH_BARRIER from=\d+ '; Expected = $Stages - 1 },
        @{ Pattern = '(?m)^MULTIGRAPH_READBACK stage=\d+ '; Expected = $Stages },
        @{ Pattern = '(?m)^MULTI_SUBMIT completed=\d+ '; Expected = $Submissions },
        @{ Pattern = '(?m)^MULTIGRAPH_PASS sets=\d+ '; Expected = $Submissions * $Stages },
        @{ Pattern = '(?m)^MULTI_SYNC index=\d+ '; Expected = $Submissions * ($Stages + 2) },
        @{ Pattern = '(?m)^GRAPH_PLAN_BUFFER index=\d+ '; Expected = $buffers },
        @{ Pattern = '(?m)^MULTI_DESCRIPTOR index=\d+ '; Expected = $bindings },
        @{ Pattern = '(?m)^MULTI_MAPPED index=\d+ '; Expected = $mapped }
    )) {
        $count = @([regex]::Matches($Result.Body, $check.Pattern)).Count

        if ($count -ne $check.Expected) {
            throw "Vulkan stage graph event count $($check.Pattern): expected $($check.Expected), actual $count"
        }
    }

    $known = @(
        @('da420145579e3ab0','2e11a9b4e2906b5d','78fd6584d78a2c98'),
        @('3dc96f338cbbffac','55af4d53d195f1b5','e35d96f4d5d34829'),
        @('fc6cfc42aeaff265','1d55deed24fd7c1f','7c575a1d46ede826'),
        @('00ba58c7ff9a53e2','ebe62698c3a64353','007be5e7346adf5a')
    )

    for ($submission = 0; $submission -lt $Submissions; $submission++) {
        if (-not $Result.Body.Contains("MULTI_SUBMIT completed=$($submission + 1) reused=$([int]($submission -ne 0)) result=0")) {
            throw "Missing completed Vulkan fence at submission=$submission"
        }

        for ($stage = 0; $stage -lt $Stages; $stage++) {
            $line = "MULTIGRAPH_PASS sets=$Sets stages=$Stages submission=$submission stage=$stage words=2048 fnv=$($known[$submission][$stage])"

            if (-not $Result.Body.Contains($line)) {
                throw "GPU/CPU stage output mismatch: $line"
            }
        }
    }

    $syncs = @([regex]::Matches(
        $Result.Body,
        '(?m)^MULTI_SYNC index=\d+ operation=(flush|invalidate) coherent=([01]) driver_call=([01]) atom=\d+ result=0$'
    ))

    if ($syncs.Count -ne $Submissions * ($Stages + 2)) {
        throw 'Vulkan staging/uniform synchronization details are incomplete.'
    }

    foreach ($sync in $syncs) {
        if ([int]$sync.Groups[2].Value + [int]$sync.Groups[3].Value -ne 1) {
            throw 'Host-visible Vulkan synchronization flags are inconsistent.'
        }

        if ($Mode -eq 'noncoherent10' -and $sync.Groups[2].Value -ne '0') {
            throw 'Forced noncoherent memory unexpectedly used HOST_COHERENT.'
        }
    }
}

try {
    & adb -s $Serial shell mkdir -p $remote | Out-Null

    if ($LASTEXITCODE -ne 0) {
        throw 'Android cannot create the private Vulkan diagnostic directory.'
    }

    foreach ($name in $files) {
        & adb -s $Serial push (Join-Path $directory $name) "$remote/$name" | Out-Null

        if ($LASTEXITCODE -ne 0) {
            throw "Android cannot stage the selected executable or SPIR-V: $name"
        }
    }

    & adb -s $Serial shell chmod 700 $remoteBinary | Out-Null

    if ($LASTEXITCODE -ne 0) {
        throw "Android cannot execute the selected ABI: $chosen"
    }

    $modes = @('default10', 'explicit10')

    if ($TestCore11) {
        $modes += 'required11'
    }

    if ($TestNoncoherent) {
        $modes += 'noncoherent10'
    }

    foreach ($sets in @(1, 2)) {
        foreach ($stages in @(1, 2, 3)) {
            foreach ($mode in $modes) {
                foreach ($optimization in @('O0', 'O')) {
                    $arguments = @(Get-Shaders -Sets $sets -Optimization $optimization) + @(
                        '--sets', "$sets", '--stages', "$stages",
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

                    $result = Invoke-Graph -Arguments $arguments

                    if ($mode -eq 'noncoherent10' -and $result.ExitCode -eq 1) {
                        foreach ($fragment in @(
                            'GRAPH_REJECT_MEMORY_TYPE index=',
                            'MULTIGRAPH_CREATE_FAILED result=-8 fail_after=0',
                            'MULTIGRAPH_NO_COMPATIBLE_CONFIGURATION'
                        )) {
                            if (-not $result.Body.Contains($fragment)) {
                                throw "Forced noncoherent memory failed unexpectedly: $fragment"
                            }
                        }

                        if ($result.Body.Contains('MULTIGRAPH_PASS sets=')) {
                            throw 'Incompatible noncoherent memory ran a GPU dispatch.'
                        }

                        Write-Output "UNSUPPORTED $chosen sets=$sets stages=$stages mode=$mode -$optimization"
                    }
                    else {
                        Assert-GraphSuccess -Sets $sets -Stages $stages -Mode $mode -Result $result
                        Write-Output "PASS $chosen sets=$sets stages=$stages mode=$mode -$optimization submits=$Submissions"
                    }

                    Save-Graph -Sets $sets -Stages $stages -Mode $mode -Optimization $optimization -Result $result
                }
            }
        }
    }

    if ($TestFaults) {
        $faultLog = @(
            'TEST=vulkan-core10-multiset-graph-faults',
            "ABI=$chosen",
            'CONFIGURATIONS=set1-stages1,set2-stages3'
        )

        foreach ($config in @(
            @{ Sets = 1; Stages = 1 },
            @{ Sets = 2; Stages = 3 }
        )) {
            $sets = $config.Sets
            $stages = $config.Stages
            $maximum = 14 + 7 * $stages + 2 * $sets
            $shaders = @(Get-Shaders -Sets $sets -Optimization 'O')

            for ($point = 1; $point -le $maximum; $point++) {
                $args = $shaders + @(
                    '--sets', "$sets", '--stages', "$stages",
                    '--fail-after', "$point"
                )
                $result = Invoke-Graph -Arguments $args

                if ($result.ExitCode -ne 42) {
                    throw "Graph fault returned $($result.ExitCode), expected 42: sets=$sets stages=$stages point=$point"
                }

                foreach ($fragment in @(
                    "MULTI_INJECT_FAULT acquisition=$point resource=",
                    "MULTI_DESTROY acquisitions=$point ",
                    "MULTIGRAPH_CREATE_FAILED result=-3 fail_after=$point"
                )) {
                    if (-not $result.Body.Contains($fragment)) {
                        throw "Missing exact Vulkan failure cleanup: $fragment"
                    }
                }

                if ($result.Body.Contains('MULTIGRAPH_PASS sets=') -or
                    $result.Body.Contains('MULTIGRAPH_DISPATCH stage=')) {
                    throw "Fault point $point unexpectedly executed a GPU stage."
                }

                $faultLog += "FAULT_CASE sets=$sets stages=$stages point=$point exit=42"
                $faultLog += @($result.Body -split "`n")
            }

            Write-Output "PASS $chosen sets=$sets stages=$stages all $maximum partial resource cleanups"
        }

        [System.IO.File]::WriteAllText(
            (Join-Path $directory "$ReportPrefix-faults.log"),
            ($faultLog -join "`n") + "`n",
            [System.Text.UTF8Encoding]::new($false)
        )
    }

    $base = @(Get-Shaders -Sets 2 -Optimization 'O')
    $negative = @(
        @{ Name = 'missing-both'; Args = $base },
        @{ Name = 'zero-stages'; Args = $base + @('--sets', '2', '--stages', '0') },
        @{ Name = 'four-stages'; Args = $base + @('--sets', '2', '--stages', '4') },
        @{ Name = 'zero-sets'; Args = $base + @('--sets', '0', '--stages', '3') },
        @{ Name = 'three-sets'; Args = $base + @('--sets', '3', '--stages', '3') },
        @{ Name = 'five-submissions'; Args = $base + @('--sets', '2', '--stages', '3', '--submissions', '5') },
        @{ Name = 'invalid-api'; Args = $base + @('--sets', '2', '--stages', '3', '--minimum', '1.2') },
        @{ Name = 'zero-fault'; Args = $base + @('--sets', '2', '--stages', '3', '--fail-after', '0') },
        @{ Name = 'fault-over-limit'; Args = $base + @('--sets', '2', '--stages', '3', '--fail-after', '40') },
        @{ Name = 'missing-shader'; Args = @("$remote/missing.spv") + @($base[1],$base[2]) + @('--sets', '2', '--stages', '3') }
    )
    $negativeLog = @(
        'TEST=vulkan-core10-multiset-graph-invalid-inputs',
        "ABI=$chosen",
        "INVALID_CASES=$($negative.Count)"
    )

    foreach ($case in $negative) {
        $result = Invoke-Graph -Arguments $case.Args

        if ($result.ExitCode -ne 2) {
            throw "Invalid CLI input $($case.Name) was accepted: exit=$($result.ExitCode)"
        }

        $negativeLog += "INVALID_CASE=$($case.Name) exit=2"
        Write-Output "PASS $chosen malformed CLI $($case.Name)"
    }

    [System.IO.File]::WriteAllText(
        (Join-Path $directory "$ReportPrefix-negative-controls.log"),
        ($negativeLog -join "`n") + "`n",
        [System.Text.UTF8Encoding]::new($false)
    )

    Write-Output "PASS $chosen Vulkan 1.0 stage graphs with explicit descriptor sets"
}
finally {
    & adb -s $Serial shell rm -rf $remote 2>$null | Out-Null
}
