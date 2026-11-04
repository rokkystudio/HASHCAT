<#
.SYNOPSIS
Validate Vulkan fence polling, cancellation and progressive session shutdown.

.DESCRIPTION
Executes two standalone native Vulkan Compute-session lifetimes using the
selected ABI and core API. Each lifetime submits real staging uploads and
downloads, polls its fence with a zero-nanosecond timeout, and cancels one upload and one download without revoking in-flight GPU
commands. It also submits an empty Compute command for cancellation and a
second normal Compute command using the same session-owned command pair.
Three additional sessions verify cancellation-aware shutdown for pending
Compute and staging work and an already fence-confirmed Compute command
with outstanding descriptor and command leases.
The runner accepts only actual VK_SUCCESS or VK_TIMEOUT, verifies pending
ownership, canceled-result retirement, pinned descriptor leases, readback
and idle-verified recovery, and saves all native output. The test also opens two additional Vulkan
sessions to verify cancellation-aware graceful shutdown of a pending
Compute submission and a pending staging transfer. Borrowed but
unsubmitted work is never silently discarded.

.PARAMETER Serial
Exact ADB device serial to use for all commands.

.PARAMETER Abi
Exact supported ARM executable architecture: arm64 or armv7.

.PARAMETER MinimumApi
Requested Vulkan core API version: 1.0 or 1.1.

.PARAMETER ReportLabel
Lowercase, hyphenated evidence label identifying this device/run.

.PARAMETER BinaryDirectory
Directory containing the two standalone diagnostic executables.

.PARAMETER ReportDirectory
Directory in which the complete native GPU log will be saved.
#>
[CmdletBinding()]
param (
    [Parameter(Mandatory = $true)]
    [string]$Serial,

    [Parameter(Mandatory = $true)]
    [ValidateSet('arm64', 'armv7')]
    [string]$Abi,

    [Parameter(Mandatory = $true)]
    [ValidateSet('1.0', '1.1')]
    [string]$MinimumApi,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[a-z0-9-]+$')]
    [string]$ReportLabel,

    [string]$BinaryDirectory = '',

    [string]$ReportDirectory = ''
)

$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

if ([string]::IsNullOrWhiteSpace($BinaryDirectory)) {
    $BinaryDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-backend-session-20261101'
}

if ([string]::IsNullOrWhiteSpace($ReportDirectory)) {
    $ReportDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-timed-staging-20261104'
}

$binary = Join-Path $BinaryDirectory "vulkan-backend-session-$Abi"
$nativeAbi = if ($Abi -eq 'arm64') { 'arm64-v8a' } else { 'armeabi-v7a' }
$remote = "/data/local/tmp/vk-timed-$ReportLabel-$Abi-$MinimumApi"
$report = Join-Path $ReportDirectory "$ReportLabel-$Abi-$MinimumApi.log"

if (-not (Test-Path -LiteralPath $binary -PathType Leaf)) {
    throw "Standalone native executable not found: $binary"
}

$deviceAbis = (& adb -s $Serial shell getprop ro.product.cpu.abilist) -join ''

if ($LASTEXITCODE -ne 0) {
    throw "Cannot read device ABI list: $Serial"
}

if (@($deviceAbis.Trim() -split ',') -notcontains $nativeAbi) {
    throw "Physical device $Serial does not support the requested $nativeAbi ABI."
}

New-Item -Path $ReportDirectory -ItemType Directory -Force | Out-Null

try {
    & adb -s $Serial push $binary $remote | Out-Null

    if ($LASTEXITCODE -ne 0) {
        throw 'Unable to stage the standalone native executable.'
    }

    & adb -s $Serial shell chmod 700 $remote | Out-Null

    if ($LASTEXITCODE -ne 0) {
        throw 'Unable to mark the native executable runnable.'
    }

    $command = "$remote --minimum $MinimumApi" + '; echo VK_TIMED_NATIVE_EXIT=' + '$?'
    $captured = @(& adb -s $Serial shell $command)

    if ($LASTEXITCODE -ne 0 -or $captured.Count -lt 2) {
        throw 'ADB did not return the complete native diagnostic.'
    }

    $lines = @($captured | ForEach-Object { $_.TrimEnd([char]13) })
    $body = [string]::Join([char]10, $lines)
    [System.IO.File]::WriteAllText(
        $report,
        $body + [char]10,
        [System.Text.UTF8Encoding]::new($false)
    )

    $exitMarker = $lines[-1].Trim()

    if ($exitMarker -cnotmatch '^VK_TIMED_NATIVE_EXIT=([0-9]+)$') {
        throw "Unexpected native process exit marker: $exitMarker. Report: $report"
    }

    $nativeExit = [int]$Matches[1]

    if ($nativeExit -ne 0) {
        throw "Native GPU test exited with $nativeExit. Report: $report"
    }

    $polls = [regex]::Matches(
        $body,
        '(?m)^BACKEND_SESSION_STAGING_POLL_PASS trial=[01] timeout_ns=0 upload_result=(0|2) download_result=(0|2) completed=2 verified_bytes=4$'
    )
    $timeoutEvents = [regex]::Matches(
        $body,
        '(?m)^BACKEND_VULKAN_STAGING_WAIT_PENDING index=0 timeout_ns=0 pending=1 result=2$'
    ).Count
    $recovery = [regex]::Matches(
        $body,
        '(?m)^BACKEND_SESSION_STAGING_RECOVERY_PASS trial=[01] abandoned=1 recovered=1 completed=3 verified_bytes=8 rejected=10$'
    ).Count
    $lifetimes = [regex]::Matches(
        $body,
        '(?m)^BACKEND_SESSION_TRIAL_PASS trial=[01] '
    ).Count
    $close = [regex]::Matches(
        $body,
        '(?m)^BACKEND_VULKAN_STAGING_RECOVERY_CLOSE index=0 recoveries=1 abandoned=1 released=1 wait_result=0$'
    ).Count
    $complete = [regex]::Matches(
        $body,
        '(?m)^BACKEND_SESSION_DIAGNOSTIC_PASS .*staging_transfers=34 staging_rejected=10 staging_recoveries=2 staging_abandoned=2 staging_cancellations=4 command_submissions=8 command_recoveries=2 managed_submissions=4 managed_completed=2 managed_cancelled=2 shutdown_trials=3 scheduled=0$'
    ).Count
    $cancelTrials = [regex]::Matches(
        $body,
        '(?m)^BACKEND_SESSION_STAGING_CANCEL_PASS trial=[01] cancelled=2 verified_downloads=2 upload_poll=(0|2) download_poll=(0|2) preserved_storage=1$'
    )
    $cancelRequests = [regex]::Matches(
        $body,
        '(?m)^BACKEND_VULKAN_STAGING_CANCEL_REQUEST index=0 slot=0 direction=(upload|download) bytes=4 pending=1$'
    ).Count
    $cancelRetired = [regex]::Matches(
        $body,
        '(?m)^BACKEND_VULKAN_STAGING_CANCEL_COMPLETE index=0 slot=0 direction=(upload|download) bytes=4 cancelled=[12] pending=0$'
    ).Count
    $cancelClose = [regex]::Matches(
        $body,
        '(?m)^BACKEND_VULKAN_STAGING_CANCEL_CLOSE index=0 cancelled=2 released=1 wait_result=0$'
    ).Count
    $managedTrials = [regex]::Matches(
        $body,
        '(?m)^BACKEND_SESSION_MANAGED_COMPUTE_PASS trial=[01] submissions=2 cancelled=1 completed=1 command_reuses=2 cancelled_poll=(0|2) normal_poll=(0|2) verified_bytes=4$'
    )
    $managedRequests = [regex]::Matches(
        $body,
        '(?m)^BACKEND_VULKAN_MANAGED_COMMAND_CANCEL_REQUEST index=0 pending=1$'
    ).Count
    $managedRetired = [regex]::Matches(
        $body,
        '(?m)^BACKEND_VULKAN_MANAGED_COMMAND_CANCEL_COMPLETE index=0 cancelled=1 pending=0$'
    ).Count
    $managedCompleted = [regex]::Matches(
        $body,
        '(?m)^BACKEND_VULKAN_MANAGED_COMMAND_COMPLETE index=0 completed=1 pending=0$'
    ).Count
    $managedClosed = [regex]::Matches(
        $body,
        '(?m)^BACKEND_VULKAN_MANAGED_COMMAND_CLOSE index=0 submitted=2 completed=1 cancelled=1 released=1 wait_result=0$'
    ).Count
    $managedTimeoutEvents = [regex]::Matches(
        $body,
        '(?m)^BACKEND_VULKAN_MANAGED_COMMAND_WAIT_PENDING index=0 timeout_ns=0 pending=1 result=2$'
    ).Count
    $descriptorTrials = [regex]::Matches(
        $body,
        '(?m)^BACKEND_SESSION_TRIAL_PASS trial=[01] .*descriptor_pools=2 descriptor_reuses=4 descriptor_writes=14 command_acquisitions=6 command_reuses=4'
    ).Count
    $descriptorBindings = [regex]::Matches(
        $body,
        '(?m)^BACKEND_VULKAN_DESCRIPTOR_BIND index=0 '
    ).Count
    $statusTrials = [regex]::Matches(
        $body,
        '(?m)^BACKEND_SESSION_STATUS_PASS trial=[01] pending=0 leased=0 poisoned=0 compute_submitted=2 compute_completed=1 compute_cancelled=1 transfer_completed=17 transfer_cancelled=2$'
    ).Count
    $closeGuardTrials = [regex]::Matches(
        $body,
        '(?m)^BACKEND_SESSION_CLOSE_GUARD_PASS trial=[01] deferred=5 ready=1 idempotent=1$'
    ).Count
    $closeDeferred = [regex]::Matches(
        $body,
        '(?m)^BACKEND_VULKAN_CLOSE_DEFERRED index=0 .*result=1$'
    ).Count
    $shutdownCompute = [regex]::Matches(
        $body,
        '(?m)^BACKEND_SESSION_SHUTDOWN_PASS trial=0 kind=compute first_poll=(0|1) cancelled=1 completed=0 closed=1 idempotent=1$'
    )
    $shutdownTransfer = [regex]::Matches(
        $body,
        '(?m)^BACKEND_SESSION_SHUTDOWN_PASS trial=1 kind=transfer first_poll=(0|1) cancelled=1 completed=0 closed=1 idempotent=1$'
    )
    $shutdownPrecompleted = [regex]::Matches(
        $body,
        '(?m)^BACKEND_SESSION_SHUTDOWN_PASS trial=2 kind=compute first_poll=0 cancelled=0 completed=1 closed=1 idempotent=1$'
    ).Count
    $shutdownPrecompletedClosed = [regex]::Matches(
        $body,
        '(?m)^BACKEND_VULKAN_SHUTDOWN_COMPLETE index=0 compute_cancelled=0 transfer_cancelled=0$'
    ).Count
    $shutdownPrecompletedCommand = [regex]::Matches(
        $body,
        '(?m)^BACKEND_VULKAN_MANAGED_COMMAND_CLOSE index=0 submitted=1 completed=1 cancelled=0 released=1 wait_result=0$'
    ).Count
    $shutdownComputePending = [regex]::Matches(
        $body,
        '(?m)^BACKEND_VULKAN_SHUTDOWN_PENDING index=0 kind=compute timeout_ns=0 result=1$'
    ).Count
    $shutdownTransferPending = [regex]::Matches(
        $body,
        '(?m)^BACKEND_VULKAN_SHUTDOWN_PENDING index=0 kind=transfer timeout_ns=0 result=1$'
    ).Count
    $shutdownComputeClosed = [regex]::Matches(
        $body,
        '(?m)^BACKEND_VULKAN_SHUTDOWN_COMPLETE index=0 compute_cancelled=1 transfer_cancelled=0$'
    ).Count
    $shutdownTransferClosed = [regex]::Matches(
        $body,
        '(?m)^BACKEND_VULKAN_SHUTDOWN_COMPLETE index=0 compute_cancelled=0 transfer_cancelled=1$'
    ).Count
    $shutdownManagedClose = [regex]::Matches(
        $body,
        '(?m)^BACKEND_VULKAN_MANAGED_COMMAND_CLOSE index=0 submitted=1 completed=0 cancelled=1 released=1 wait_result=0$'
    ).Count
    $shutdownStagingClose = [regex]::Matches(
        $body,
        '(?m)^BACKEND_VULKAN_STAGING_CANCEL_CLOSE index=0 cancelled=1 released=1 wait_result=0$'
    ).Count

    $actualTimeouts = 0
    $managedTimeouts = 0

    foreach ($poll in $polls) {
        if ($poll.Groups[1].Value -eq '2') {
            $actualTimeouts++
        }

        if ($poll.Groups[2].Value -eq '2') {
            $actualTimeouts++
        }
    }

    foreach ($poll in $cancelTrials) {
        if ($poll.Groups[1].Value -eq '2') {
            $actualTimeouts++
        }

        if ($poll.Groups[2].Value -eq '2') {
            $actualTimeouts++
        }
    }

    foreach ($poll in $managedTrials) {
        if ($poll.Groups[1].Value -eq '2') {
            $managedTimeouts++
        }

        if ($poll.Groups[2].Value -eq '2') {
            $managedTimeouts++
        }
    }

    if ($shutdownCompute.Count -ne 1 -or $shutdownTransfer.Count -ne 1) {
        throw "Missing exact Compute or transfer shutdown trial. Report: $report"
    }

    $expectedComputePending = if ($shutdownCompute[0].Groups[1].Value -eq '1') { 1 } else { 0 }
    $expectedTransferPending = if ($shutdownTransfer[0].Groups[1].Value -eq '1') { 1 } else { 0 }

    if ($polls.Count -ne 2 -or
        $timeoutEvents -ne ($actualTimeouts + $expectedTransferPending) -or
        $recovery -ne 2 -or $lifetimes -ne 2 -or $close -ne 2 -or
        $cancelTrials.Count -ne 2 -or $cancelRequests -ne 5 -or
        $cancelRetired -ne 5 -or $cancelClose -ne 2 -or
        $managedTrials.Count -ne 2 -or $managedRequests -ne 3 -or
        $managedRetired -ne 3 -or $managedCompleted -ne 3 -or
        $managedClosed -ne 2 -or
        $managedTimeoutEvents -ne ($managedTimeouts + $expectedComputePending) -or
        $descriptorTrials -ne 2 -or $descriptorBindings -ne 14 -or
        $statusTrials -ne 2 -or $closeGuardTrials -ne 2 -or
        $closeDeferred -ne 11 -or
        $shutdownPrecompleted -ne 1 -or
        $shutdownPrecompletedClosed -ne 1 -or
        $shutdownPrecompletedCommand -ne 1 -or
        $shutdownComputePending -ne $expectedComputePending -or
        $shutdownTransferPending -ne $expectedTransferPending -or
        $shutdownComputeClosed -ne 1 -or $shutdownTransferClosed -ne 1 -or
        $shutdownManagedClose -ne 1 -or $shutdownStagingClose -ne 1 -or
        $complete -ne 1) {
        throw "GPU evidence mismatch: staging_polls=$($polls.Count) staging_timeouts=$actualTimeouts pending_events=$timeoutEvents recoveries=$recovery lifetimes=$lifetimes closes=$close transfer_cancels=$cancelRetired managed_trials=$($managedTrials.Count) managed_cancels=$managedRetired managed_completed=$managedCompleted managed_closed=$managedClosed managed_timeouts=$managedTimeouts managed_pending=$managedTimeoutEvents descriptor_trials=$descriptorTrials descriptor_bindings=$descriptorBindings status_trials=$statusTrials close_guard_trials=$closeGuardTrials close_deferred=$closeDeferred shutdown_compute=$shutdownComputeClosed shutdown_transfer=$shutdownTransferClosed shutdown_compute_pending=$shutdownComputePending shutdown_transfer_pending=$shutdownTransferPending shutdown_precompleted=$shutdownPrecompleted final=$complete. Report: $report"
    }

    Write-Output "GPU_TIMED_STAGING_PASS label=$ReportLabel abi=$Abi api=$MinimumApi lifetimes=2 fence_polls=8 real_timeouts=$actualTimeouts cancelled=4 managed_submissions=4 managed_cancelled=2 managed_timeouts=$managedTimeouts descriptor_bindings=$descriptorBindings status_trials=$statusTrials close_guard_trials=$closeGuardTrials close_deferred=$closeDeferred shutdown_compute=1 shutdown_transfer=1 shutdown_precompleted=1"
    Write-Output "GPU_TIMED_STAGING_REPORT=$report"
}
finally {
    & adb -s $Serial shell rm -f $remote 2>$null | Out-Null
}
