<#
.SYNOPSIS
Executes a variable-size Vulkan 1.0 synthetic Compute graph on Android.

.DESCRIPTION
Discovers a supported ARM ABI from ro.product.cpu.abilist rather than
GPU manufacturer or phone model. Executes explicit one-, two- and
three-stage public SPIR-V 1.0 graphs (four, five, six buffer allocations)
in both shader optimization modes and default core Vulkan 1.0.

Optional runs test explicit Vulkan 1.1 and genuine noncoherent CPU staging,
recording exact rejection if the requested host memory type is unavailable.
Optional fault trials stop immediately after each actual Vulkan acquisition
point, checking cleanup for graph sizes with 20, 24 and 28 resources.
Malformatted CLI controls must exit 2 without executing GPU work.

All original observed data are preserved as LF-only local logs. Uploaded
executable and shader files are deleted from Android using finally.

.PARAMETER Serial
Exact ADB device serial.

.PARAMETER ArtifactDirectory
Directory containing both Android API24 executables and six SPIR-V 1.0 files.

.PARAMETER Abi
auto chooses a reported compatible ABI, otherwise requires the exact
specified arm64-v8a or armeabi-v7a ABI.

.PARAMETER ReportPrefix
ASCII-only prefix for original Vulkan GPU and negative-control logs.

.PARAMETER Submissions
Number of sequential public GPU submissions, 2 to 4. All known public
reference digests are verified for each selected submission.

.PARAMETER TestCore11
Also run each graph under an explicit Vulkan 1.1 core requirement.

.PARAMETER TestNoncoherent
Require noncoherent HOST_VISIBLE staging or log the precise incompatibility.

.PARAMETER TestFaults
Test every real resource acquisition point on all three stage counts.
#>
param (
    [Parameter(Mandatory = $true)]
    [string]$Serial,

    [string]$ArtifactDirectory = '',

    [ValidateSet('auto', 'arm64-v8a', 'armeabi-v7a')]
    [string]$Abi = 'auto',

    [string]$ReportPrefix = 'graph',

    [ValidateRange(2, 4)]
    [int]$Submissions = 4,

    [switch]$TestCore11,

    [switch]$TestNoncoherent,

    [switch]$TestFaults
)

$ErrorActionPreference = 'Stop'

if ($ReportPrefix -cnotmatch '^[A-Za-z0-9_-]+$') {
    throw 'ReportPrefix accepts only ASCII letters, digits, underscore or dash.'
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

if ([string]::IsNullOrWhiteSpace($ArtifactDirectory)) {
    $ArtifactDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-dynamic-graph-20261031'
}
elseif (-not [System.IO.Path]::IsPathRooted($ArtifactDirectory)) {
    $ArtifactDirectory = Join-Path $root $ArtifactDirectory
}

$directory = (Resolve-Path -LiteralPath $ArtifactDirectory).Path
$state = @(& adb -s $Serial get-state 2>&1)

if ($LASTEXITCODE -ne 0 -or ($state -join '').Trim() -ne 'device') {
    throw "Requested Android device unavailable: $Serial"
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
        throw "No compatible ARM ABI reported: $($abis -join ',')"
    }
}
else {
    if ($abis -notcontains $Abi) {
        throw "Required ABI $Abi unavailable: $($abis -join ',')"
    }

    $selectedAbi = $Abi
}

$executable = if ($selectedAbi -eq 'arm64-v8a') {
    'vulkan-dynamic-graph-arm64'
}
else {
    'vulkan-dynamic-graph-armv7'
}

$files = @($executable)

foreach ($opt in @('O0', 'O')) {
    foreach ($stage in @('a', 'b', 'c')) {
        $files += "stage-$stage-$opt-vk1.0.spv"
    }
}

foreach ($file in $files) {
    if (-not (Test-Path -LiteralPath (Join-Path $directory $file) -PathType Leaf)) {
        throw "Missing compiled Vulkan graph artifact: $file"
    }
}

Write-Output "GRAPH_SELECT_ABI=$selectedAbi source=ro.product.cpu.abilist"

$remote = '/data/local/tmp'
$remoteExecutable = "$remote/$executable"

$known = @(
    @('de56d6d0aa66c6ec', 'c1259ad4f024499a', '05de70c62f8d7cfd'),
    @('66ee1b4b1385e0e3', '27fb4f34e64d89a7', '7509bfa6ed0455d8'),
    @('69b8f65ae2e8be72', 'd8038be5522a0b58', 'e50a539d4459e45b'),
    @('8fb63465a93a5631', 'ac37a5c3b35fc366', '07d63cfd9773324d')
)

function Invoke-Graph {
    <#
    .SYNOPSIS
    Execute one remote public Vulkan graph test and capture its actual status.

    .DESCRIPTION
    Retrieves the remote program's exit status through one explicit shell
    marker so expected nonzero Vulkan errors do not depend on PowerShell's
    native stderr behavior. Preserves all original GPU diagnostic lines.
    #>
    param (
        [string[]]$Arguments
    )

    $command = (
        $remoteExecutable + ' ' + ($Arguments -join ' ') +
        ' 2>&1; echo GRAPH_DEVICE_EXIT=$?'
    )

    $lines = @(& adb -s $Serial shell $command 2>&1)

    if ($LASTEXITCODE -ne 0) {
        throw 'ADB failed while executing Vulkan graph on Android.'
    }

    $body = ($lines | Out-String).Trim()
    $body = $body.Replace("`r`n", "`n").Replace("`r", '')

    $markers = [regex]::Matches(
        $body, '(?m)^GRAPH_DEVICE_EXIT=(\d+)$'
    )

    if ($markers.Count -ne 1) {
        throw 'Remote Vulkan graph returned no unique exit status.'
    }

    $status = [int]$markers[0].Groups[1].Value
    $body = [regex]::Replace(
        $body, '(?m)^GRAPH_DEVICE_EXIT=\d+\n?', ''
    ).TrimEnd()

    return @{
        ExitCode = $status
        Body = $body
    }
}

function Save-Graph {
    <#
    .SYNOPSIS
    Save an exact local Vulkan graph GPU observation with LF line endings.

    .DESCRIPTION
    Stores the declared number of stages, shader optimization, device ABI,
    actual remote exit code and unmodified GPU output. Does not write
    Android serial numbers into the reproducible source fixture.
    #>
    param (
        [string]$Mode,
        [string]$Optimization,
        [int]$Stages,
        [hashtable]$Result
    )

    $lines = @(
        'TEST=vulkan-core10-dynamic-graph',
        "STAGES=$Stages",
        "MODE=$Mode",
        "OPTIMIZATION=$Optimization",
        "ABI=$selectedAbi",
        "ADB_EXIT_CODE=$($Result.ExitCode)",
        $Result.Body
    )

    [System.IO.File]::WriteAllText(
        (Join-Path $directory "$ReportPrefix-stage$Stages-$Mode-$Optimization.log"),
        ($lines -join "`n") + "`n",
        [System.Text.UTF8Encoding]::new($false)
    )
}

function Assert-GraphSuccess {
    <#
    .SYNOPSIS
    Validate the real Vulkan graph topology, resource lifetime and CPU oracle.

    .DESCRIPTION
    Checks one exact allocation/descriptor/pipeline count per stage count,
    the complete per-submission GPU fence and cache event sequence, and
    independently established FNV-1a digests of all public output stages.
    #>
    param (
        [int]$Stages,
        [string]$Mode,
        [string]$Optimization,
        [hashtable]$Result
    )

    if ($Result.ExitCode -ne 0) {
        throw "Vulkan graph stage $Stages $Mode -$Optimization returned exit=$($Result.ExitCode): $($Result.Body)"
    }

    $api = if ($Mode -eq 'required11') { '1.1' } else { '1.0' }
    $buffers = $Stages + 3
    $descriptors = $Stages + 1
    $resources = 16 + 4 * $Stages
    $outputBytes = if ($Stages -eq 3) { 32768 } else { 8192 }
    $words = if ($Stages -eq 3) { 12288 } else { 2048 }
    $hostWrite = $buffers - 2
    $hostRead = $buffers - 1

    foreach ($fragment in @(
        "SELECT_INSTANCE_API=$api ",
        "SELECT_DEVICE_API=$api ",
        "GRAPH_SELECTED_DEVICE index=0 requested_api=$api effective_api=$api stages=$Stages descriptors=$descriptors ",
        "GRAPH_REQUEST stages=$Stages buffers=$buffers descriptors=$descriptors mapped=2 output_bytes=$outputBytes ",
        "GRAPH_READY acquisitions=$resources buffers=$buffers descriptors=$descriptors stages=$Stages maps=2 core=1.0",
        "GRAPH_DESTROY acquisitions=$resources buffers=$buffers memories=$buffers maps=2 shaders=$Stages pipelines=$Stages submissions=$Submissions idle_result=0"
    )) {
        if (-not $Result.Body.Contains($fragment)) {
            throw "Vulkan graph successful result missing: $fragment"
        }
    }

    $planned = @([regex]::Matches(
        $Result.Body, '(?m)^GRAPH_PLAN_BUFFER index=(\d+) '
    ))

    if ($planned.Count -ne $buffers) {
        throw "Expected $buffers independently planned Vulkan buffers."
    }

    for ($index = 0; $index -lt $buffers; $index++) {
        if ([int]$planned[$index].Groups[1].Value -ne $index) {
            throw "Vulkan graph buffer index mismatch at $index."
        }
    }

    $submits = @([regex]::Matches(
        $Result.Body,
        '(?m)^GRAPH_SUBMIT completed=(\d+) reused=([01]) result=0$'
    ))
    $passes = @([regex]::Matches(
        $Result.Body,
        '(?m)^GRAPH_PASS stages=(\d+) submission=(\d+) verified_words=(\d+) stageA=([0-9a-f]{16}) stageB=([0-9a-f]{16}) output=([0-9a-f]{16})$'
    ))
    $syncs = @([regex]::Matches(
        $Result.Body,
        '(?m)^GRAPH_SYNC index=(\d+) operation=(flush|invalidate) coherent=([01]) driver_call=([01]) atom=(\d+) result=0$'
    ))

    if ($submits.Count -ne $Submissions -or
        $passes.Count -ne $Submissions -or
        $syncs.Count -ne 2 * $Submissions) {
        throw "Expected exactly $Submissions complete Vulkan graph GPU submissions."
    }

    for ($index = 0; $index -lt $Submissions; $index++) {
        if ([int]$submits[$index].Groups[1].Value -ne $index + 1 -or
            [int]$submits[$index].Groups[2].Value -ne [int]($index -gt 0)) {
            throw "Vulkan graph recorded command reuse mismatch at $index."
        }

        $pass = $passes[$index]

        if ([int]$pass.Groups[1].Value -ne $Stages -or
            [int]$pass.Groups[2].Value -ne $index -or
            [int]$pass.Groups[3].Value -ne $words) {
            throw "Wrong stage count or verified GPU words at submission=$index."
        }

        $a = $known[$index][0]
        $b = $known[$index][1]
        $c = $known[$index][2]
        $empty = 'cbf29ce484222325'

        $expectedA = if ($Stages -eq 2) { $empty } else { $a }
        $expectedB = if ($Stages -eq 1) { $empty } else { $b }
        $expectedOutput = if ($Stages -eq 1) {
            $a
        }
        elseif ($Stages -eq 2) {
            $b
        }
        else {
            $c
        }

        if ($pass.Groups[4].Value -cne $expectedA -or
            $pass.Groups[5].Value -cne $expectedB -or
            $pass.Groups[6].Value -cne $expectedOutput) {
            throw "Independent CPU/GPU digest mismatch at stage=$Stages submission=$index."
        }

        foreach ($pair in @(
            @{ Point = 2 * $index; Buffer = $hostWrite; Operation = 'flush' },
            @{ Point = 2 * $index + 1; Buffer = $hostRead; Operation = 'invalidate' }
        )) {
            $sync = $syncs[$pair.Point]

            if ([int]$sync.Groups[1].Value -ne $pair.Buffer -or
                $sync.Groups[2].Value -cne $pair.Operation -or
                [int]$sync.Groups[4].Value -ne 1 - [int]$sync.Groups[3].Value) {
                throw "Vulkan mapped memory coherence synchronization mismatch."
            }
        }
    }
}

try {
    foreach ($file in $files) {
        & adb -s $Serial push (Join-Path $directory $file) "$remote/$file" | Out-Null

        if ($LASTEXITCODE -ne 0) {
            throw "Failed to upload portable graph artifact: $file"
        }
    }

    & adb -s $Serial shell chmod 700 $remoteExecutable

    if ($LASTEXITCODE -ne 0) {
        throw 'Selected Android ABI cannot execute Vulkan graph binary.'
    }

    $modes = @('default10', 'explicit10')

    if ($TestCore11) {
        $modes += 'required11'
    }

    if ($TestNoncoherent) {
        $modes += 'noncoherent10'
    }

    foreach ($stages in @(1, 2, 3)) {
        foreach ($mode in $modes) {
            foreach ($optimization in @('O0', 'O')) {
                $shaderArgs = @(
                    "$remote/stage-a-$optimization-vk1.0.spv",
                    "$remote/stage-b-$optimization-vk1.0.spv",
                    "$remote/stage-c-$optimization-vk1.0.spv"
                )
                $arguments = $shaderArgs + @(
                    '--stages', "$stages",
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
                    foreach ($required in @(
                        'GRAPH_REQUEST stages=',
                        'GRAPH_REJECT_MEMORY_TYPE index=',
                        'GRAPH_CREATE_FAILED result=-8 fail_after=0',
                        'GRAPH_NO_COMPATIBLE_CONFIGURATION'
                    )) {
                        if (-not $result.Body.Contains($required)) {
                            throw "No exact noncoherent Vulkan incompatibility: $required"
                        }
                    }

                    if ($result.Body.Contains('GRAPH_PASS stages=')) {
                        throw 'Incompatible noncoherent request must not trigger GPU dispatch.'
                    }

                    Write-Output "UNSUPPORTED $selectedAbi stages=$stages -$optimization noncoherent staging: explicit memory incompatibility"
                }
                else {
                    Assert-GraphSuccess -Stages $stages -Mode $mode -Optimization $optimization -Result $result

                    if ($mode -eq 'noncoherent10') {
                        if (-not $result.Body.Contains('requested_noncoherent=1') -or
                            @([regex]::Matches(
                                $result.Body, '(?m)^GRAPH_SYNC index=\d+ operation=(flush|invalidate) coherent=0 driver_call=1 atom=\d+ result=0$'
                            )).Count -ne $Submissions * 2) {
                            throw 'Vulkan device did not perform real noncoherent cache operations.'
                        }
                    }

                    Write-Output "PASS $selectedAbi stages=$stages -$optimization mode=${mode}: $Submissions real GPU submissions"
                }

                Save-Graph -Mode $mode -Optimization $optimization -Stages $stages -Result $result
            }
        }
    }

    if ($TestFaults) {
        $faultLogs = @(
            'TEST=vulkan-core10-dynamic-graph-faults',
            "ABI=$selectedAbi",
            'STAGE_COUNTS=1,2,3'
        )

        for ($stages = 1; $stages -le 3; $stages++) {
            $total = 16 + 4 * $stages
            $shaders = @(
                "$remote/stage-a-O-vk1.0.spv",
                "$remote/stage-b-O-vk1.0.spv",
                "$remote/stage-c-O-vk1.0.spv"
            )

            for ($point = 1; $point -le $total; $point++) {
                $arguments = $shaders + @(
                    '--stages', "$stages",
                    '--submissions', "$Submissions",
                    '--fail-after', "$point"
                )
                $result = Invoke-Graph -Arguments $arguments

                if ($result.ExitCode -ne 42) {
                    throw "Graph fault stages=$stages point=$point exit=$($result.ExitCode) != 42"
                }

                foreach ($expected in @(
                    "GRAPH_FAULT acquisition=$point resource=",
                    "GRAPH_DESTROY acquisitions=$point ",
                    "GRAPH_CREATE_FAILED result=-3 fail_after=$point",
                    'GRAPH_NO_COMPATIBLE_CONFIGURATION'
                )) {
                    if (-not $result.Body.Contains($expected)) {
                        throw "Dynamic graph fault missing cleanup evidence: $expected"
                    }
                }

                if ($result.Body.Contains('GRAPH_PASS stages=') -or
                    $result.Body.Contains('GRAPH_SUBMIT completed=')) {
                    throw "Dynamic graph fault point $point executed GPU Compute."
                }

                $faultLogs += "FAULT_CASE stages=$stages point=$point exit=42"
                $faultLogs += @($result.Body -split "`n")
            }

            Write-Output "PASS $selectedAbi stages=$stages all $total Vulkan acquired-resource cleanup fault points"
        }

        [System.IO.File]::WriteAllText(
            (Join-Path $directory "$ReportPrefix-faults.log"),
            ($faultLogs -join "`n") + "`n",
            [System.Text.UTF8Encoding]::new($false)
        )
    }

    $shaders = @(
        "$remote/stage-a-O-vk1.0.spv",
        "$remote/stage-b-O-vk1.0.spv",
        "$remote/stage-c-O-vk1.0.spv"
    )
    $negative = @(
        @{ Name = 'missing-stage-count'; Arguments = $shaders },
        @{ Name = 'zero-stages'; Arguments = $shaders + @('--stages', '0') },
        @{ Name = 'four-stages'; Arguments = $shaders + @('--stages', '4') },
        @{ Name = 'zero-submissions'; Arguments = $shaders + @('--stages', '1', '--submissions', '0') },
        @{ Name = 'seventeen-submissions'; Arguments = $shaders + @('--stages', '1', '--submissions', '17') },
        @{ Name = 'invalid-version'; Arguments = $shaders + @('--stages', '1', '--minimum', '1.2') },
        @{ Name = 'zero-fault'; Arguments = $shaders + @('--stages', '1', '--fail-after', '0') },
        @{ Name = 'fault-over-limit'; Arguments = $shaders + @('--stages', '1', '--fail-after', '21') },
        @{ Name = 'missing-shader'; Arguments = @("$remote/unknown-stage-a.spv") + $shaders[1..2] + @('--stages', '1') }
    )

    $negativeLog = @(
        'TEST=vulkan-core10-dynamic-graph-invalid-inputs',
        "ABI=$selectedAbi",
        'INVALID_CASES=9'
    )

    foreach ($case in $negative) {
        $result = Invoke-Graph -Arguments $case.Arguments

        if ($result.ExitCode -ne 2) {
            throw "Invalid Vulkan graph command $($case.Name) returned exit=$($result.ExitCode)"
        }

        $negativeLog += "INVALID_CASE=$($case.Name) exit=2"
        Write-Output "PASS $selectedAbi dynamic graph invalid $($case.Name) exit=2"
    }

    [System.IO.File]::WriteAllText(
        (Join-Path $directory "$ReportPrefix-negative-controls.log"),
        ($negativeLog -join "`n") + "`n",
        [System.Text.UTF8Encoding]::new($false)
    )

    Write-Output 'PASS portable dynamic Vulkan 1.0 GPU resource graph with exact configuration'
}
finally {
    foreach ($file in $files) {
        & adb -s $Serial shell rm -f "$remote/$file" 2>$null | Out-Null
    }
}
