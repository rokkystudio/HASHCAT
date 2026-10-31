<#
.SYNOPSIS
Checks real Vulkan 1.0 memory heap allocations on an Android device.

.DESCRIPTION
Stages one noncryptographic ARM64 binary, creates four Vulkan buffers using
real vkGetBufferMemoryRequirements, selects compatible physical memory heaps,
allocates/binds them and maps the two host-visible staging buffers.

The script validates every selected memory type against actual compatibility
bits, type heap indices, heap capacity, the allocation count limit and
HOST_VISIBLE requirements, then writes deterministic local logs and removes
the temporary binary. It executes no GPU compute work or hashcat component.

.PARAMETER Serial
Exact ADB serial of the Android test device.

.PARAMETER ArtifactDirectory
Local build output containing vulkan-heap-probe-arm64.

.PARAMETER ReportPrefix
ASCII-only label of local logs. The ADB serial is not stored in the logs.
#>
param (
    [Parameter(Mandatory = $true)]
    [string]$Serial,

    [string]$ArtifactDirectory = '',

    [string]$ReportPrefix = 'vulkan-heap'
)

$ErrorActionPreference = 'Stop'

if ($ReportPrefix -cnotmatch '^[A-Za-z0-9_-]+$') {
    throw 'ReportPrefix requires ASCII letters, digits, hyphens or underscores.'
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

if ([string]::IsNullOrWhiteSpace($ArtifactDirectory)) {
    $ArtifactDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-heap-policy-20261031'
}
elseif (-not [System.IO.Path]::IsPathRooted($ArtifactDirectory)) {
    $ArtifactDirectory = Join-Path $root $ArtifactDirectory
}

$directory = (Resolve-Path -LiteralPath $ArtifactDirectory).Path
$binary = 'vulkan-heap-probe-arm64'
$local = Join-Path $directory $binary
$remote = '/data/local/tmp/' + $binary

if (-not (Test-Path -LiteralPath $local -PathType Leaf)) {
    throw "Vulkan 1.0 heap probe executable unavailable: $local"
}

$device = @(& adb -s $Serial get-state 2>&1)

if ($LASTEXITCODE -ne 0 -or ($device -join '').Trim() -ne 'device') {
    throw "Requested Android device is unavailable: $Serial"
}

try {
    & adb -s $Serial push $local $remote | Out-Null

    if ($LASTEXITCODE -ne 0) {
        throw 'Unable to stage Vulkan heap-only diagnostic.'
    }

    & adb -s $Serial shell chmod 700 $remote

    if ($LASTEXITCODE -ne 0) {
        throw 'Unable to execute Vulkan heap-only diagnostic.'
    }

    $lines = @(& adb -s $Serial shell $remote 2>&1)
    $exitCode = $LASTEXITCODE
    $output = ($lines | Out-String).Trim()
    $output = $output.Replace("`r`n", "`n").Replace("`r", '')

    if ($exitCode -ne 0) {
        throw "Vulkan real heap allocation failed: exit=$exitCode; $output"
    }

    $profile = [regex]::Match(
        $output,
        '(?m)^DEVICE_HEAP_PROFILE memory_types=(\d+) memory_heaps=(\d+) max_allocations=(\d+)\r?$'
    )

    if (-not $profile.Success) {
        throw 'Actual Vulkan driver memory heap profile is unavailable.'
    }

    $memoryTypes = [int]$profile.Groups[1].Value
    $memoryHeaps = [int]$profile.Groups[2].Value
    $maxAllocations = [int]$profile.Groups[3].Value

    if ($memoryTypes -lt 1 -or $memoryTypes -gt 32 -or
        $memoryHeaps -lt 1 -or $memoryHeaps -gt 16 -or
        $maxAllocations -lt 4) {
        throw 'Actual Vulkan memory type/heap counts or allocation limit invalid.'
    }

    $heaps = [regex]::Matches(
        $output,
        '(?m)^DEVICE_HEAP index=(\d+) size=(\d+) flags=0x([0-9a-f]{8})\r?$'
    )
    $types = [regex]::Matches(
        $output,
        '(?m)^DEVICE_MEMORY_TYPE index=(\d+) heap=(\d+) flags=0x([0-9a-f]{8})\r?$'
    )
    $requirements = [regex]::Matches(
        $output,
        '(?m)^ACTUAL_REQUIREMENTS index=(\d+) label=(gpu-input|gpu-output|staging-upload|staging-download) size=(\d+) alignment=(\d+) memory_bits=0x([0-9a-f]{8})\r?$'
    )
    $planned = [regex]::Matches(
        $output,
        '(?m)^PLAN_BUFFER index=(\d+) label=(gpu-input|gpu-output|staging-upload|staging-download) type=(\d+) heap=(\d+) bytes=(\d+) coherent=([01])\r?$'
    )
    $heapPlans = [regex]::Matches(
        $output,
        '(?m)^PLAN_HEAP index=(\d+) reserved=(\d+) declared=(\d+)\r?$'
    )
    $allocated = [regex]::Matches(
        $output,
        '(?m)^ALLOCATED_BUFFER index=(\d+) type=(\d+) heap=(\d+) bytes=(\d+)\r?$'
    )
    $mapped = [regex]::Matches(
        $output,
        '(?m)^MAPPED_STAGING index=(\d+) coherent=([01]) bytes=(\d+)\r?$'
    )

    if ($heaps.Count -ne $memoryHeaps -or
        $types.Count -ne $memoryTypes -or
        $requirements.Count -ne 4 -or
        $planned.Count -ne 4 -or
        $heapPlans.Count -ne $memoryHeaps -or
        $allocated.Count -ne 4 -or
        $mapped.Count -ne 2) {
        throw 'Vulkan heap probe did not report all actual allocations and memory properties.'
    }

    $capacities = @()
    $reserved = @()

    for ($heap = 0; $heap -lt $memoryHeaps; $heap++) {
        $declared = $heaps[$heap].Groups
        $plan = $heapPlans[$heap].Groups

        $capacity = [decimal]::Parse($declared[2].Value)
        $plannedCapacity = [decimal]::Parse($plan[3].Value)
        $plannedReserved = [decimal]::Parse($plan[2].Value)

        if ([int]$declared[1].Value -ne $heap -or
            [int]$plan[1].Value -ne $heap -or
            $capacity -le 0 -or
            $capacity -ne $plannedCapacity -or
            $plannedReserved -gt $capacity) {
            throw "The Vulkan heap size or reserved capacity is inconsistent: $heap"
        }

        $capacities += $capacity
        $reserved += [decimal]0
    }

    $expectedLabels = @(
        'gpu-input', 'gpu-output', 'staging-upload', 'staging-download'
    )

    for ($index = 0; $index -lt 4; $index++) {
        $req = $requirements[$index].Groups
        $selection = $planned[$index].Groups
        $allocation = $allocated[$index].Groups
        $type = [int]$selection[3].Value
        $heap = [int]$selection[4].Value
        $size = [decimal]::Parse($req[3].Value)
        $alignment = [uint64]::Parse($req[4].Value)
        $compatibility = [Convert]::ToUInt32($req[5].Value, 16)

        if ($type -lt 0 -or $type -ge $memoryTypes -or
            $heap -lt 0 -or $heap -ge $memoryHeaps -or
            [int]$req[1].Value -ne $index -or
            [int]$selection[1].Value -ne $index -or
            [int]$allocation[1].Value -ne $index -or
            $req[2].Value -cne $expectedLabels[$index] -or
            $selection[2].Value -cne $expectedLabels[$index] -or
            $size -lt 8192 -or
            $alignment -le 0 -or
            ($alignment -band ($alignment - 1)) -ne 0 -or
            ([uint64]$compatibility -band ([uint64]1 -shl $type)) -eq 0 -or
            [decimal]::Parse($selection[5].Value) -ne $size -or
            [decimal]::Parse($allocation[4].Value) -ne $size -or
            [int]$allocation[2].Value -ne $type -or
            [int]$allocation[3].Value -ne $heap) {
            throw "Incompatible Vulkan buffer allocation selected: $index"
        }

        $memory = $types[$type].Groups
        $memoryFlags = [Convert]::ToUInt32($memory[3].Value, 16)
        $coherent = (($memoryFlags -band 4) -ne 0)

        if ([int]$memory[1].Value -ne $type -or
            [int]$memory[2].Value -ne $heap -or
            [int]$selection[6].Value -ne [int]$coherent -or
            ($index -ge 2 -and ($memoryFlags -band 2) -eq 0)) {
            throw "Selected Vulkan memory type violates its buffer role: $index"
        }

        $reserved[$heap] += $size
    }

    for ($heap = 0; $heap -lt $memoryHeaps; $heap++) {
        $reported = [decimal]::Parse($heapPlans[$heap].Groups[2].Value)

        if ($reported -ne $reserved[$heap] -or $reserved[$heap] -gt $capacities[$heap]) {
            throw "Vulkan heap reservations do not equal actual allocations: $heap"
        }
    }

    foreach ($index in @(2, 3)) {
        $entry = $mapped[$index - 2].Groups

        if ([int]$entry[1].Value -ne $index -or
            [int]$entry[2].Value -ne [int]$planned[$index].Groups[6].Value -or
            [int]$entry[3].Value -ne 8192) {
            throw "Vulkan staging mapping mismatch: $index"
        }
    }

    $success = [regex]::Matches(
        $output,
        '(?m)^PASS_VULKAN_HEAP_PROBE device_index=(\d+) buffers=4 allocations=4 mapped_staging=2 gpu_dispatches=0\r?$'
    )

    if ($success.Count -ne 1) {
        throw 'Vulkan heap allocation final pass record absent or duplicated.'
    }

    $log = Join-Path $directory "$ReportPrefix-allocation.log"
    $report = @(
        'TEST=vulkan-1.0-memory-heap-probe',
        'ADB_EXIT_CODE=0',
        $output
    )

    [System.IO.File]::WriteAllText(
        $log, ($report -join "`n") + "`n",
        [System.Text.UTF8Encoding]::new($false)
    )

    Write-Output "PASS Vulkan memory probe: types=$memoryTypes heaps=$memoryHeaps, four allocated/bound buffers, two staging mappings, no GPU dispatch"

    $saved = $ErrorActionPreference

    try {
        $ErrorActionPreference = 'Continue'
        $invalidLines = @(& adb -s $Serial shell $remote --invalid 2>&1)
        $invalidCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $saved
    }

    if ($invalidCode -ne 2 -or
        -not ($invalidLines -join ' ').Contains('Usage:')) {
        throw 'Vulkan heap-only probe invalid CLI was not rejected as specified.'
    }

    $invalid = Join-Path $directory "$ReportPrefix-negative-controls.log"

    [System.IO.File]::WriteAllText(
        $invalid,
        "TEST=vulkan-1.0-memory-heap-probe`nINVALID unexpected-arguments exit=2`n",
        [System.Text.UTF8Encoding]::new($false)
    )

    Write-Output 'PASS unexpected arguments rejected, exit code 2'
}
finally {
    & adb -s $Serial shell rm -f $remote 2>$null | Out-Null
}
