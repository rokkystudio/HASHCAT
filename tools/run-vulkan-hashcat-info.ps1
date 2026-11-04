<#
.SYNOPSIS
Validate an Android Vulkan Compute information session against GPU output.

.DESCRIPTION
Stages one diagnostic frontend and its required assets in an isolated
ADB directory. Runs a Vulkan-only or mixed-runtime information session.
Checks Compute known-answer results, device and storage reuse, pipeline
cache, descriptor rebinding and exclusive command leases. The three
public arithmetic programs submit through their borrowed session command
pair and perform a real zero-timeout fence poll before completion. Either
immediate success or VK_TIMEOUT is valid; a timeout preserves session
ownership until an unlimited fence wait. All five diagnostics retain
real fence-based completion. Confirms healthy runs do not poison command pairs
or recovery. ForceStaging validates independent transfer buffers. CpuReferenceTest
adds an explicit information-only CPU reference mismatch after a real
four-stage GPU fence and a healthy retry on retained session resources.
PostSubmitAbortTest aborts one submitted public Compute command before its
fence wait and verifies exact fence retirement and resource reuse.
PreSubmitAbortTest aborts after command recording and before queue submission,
then verifies lease reuse without introducing a GPU fence or recovery.
Saves the full GPU log and removes only the isolated staged device files.

.PARAMETER Serial
Connected physical Android device identifier for each ADB command.

.PARAMETER Abi
Native build ABI supported by the connected device.

.PARAMETER ReportPrefix
Lowercase label for process isolation and saved evidence.

.PARAMETER VulkanOutputName
Diagnostic native build name within the build directory.

.PARAMETER Mode
Vulkan-only excludes traditional GPU loaders; mixed retains them.

.PARAMETER ForceStaging
Enable separate CPU-visible transfer buffers for Compute storage.

.PARAMETER CpuReferenceTest
Inject one host-only reference mismatch after a confirmed arithmetic GPU
fence and require a successful second execution without session recovery.
The default information run has no injected CPU validation error.

.PARAMETER PostSubmitAbortTest
Abort one public Compute invocation immediately after a successful real
queue submission. Require its fence to be retired during resource cleanup,
then verify a healthy Compute retry without full-device idle recovery.
This test is independent of CpuReferenceTest.

.PARAMETER PreSubmitAbortTest
Inject one host error after a public four-stage command is recorded but
before vkQueueSubmit. Require zero submissions for the failed job,
safe release of its unsubmitted command/descriptor leases and one
healthy GPU retry without command recovery.
#>
param (
    [Parameter(Mandatory = $true)]
    [string]$Serial,

    [Parameter(Mandatory = $true)]
    [ValidateSet('arm64-v8a', 'armeabi-v7a')]
    [string]$Abi,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[a-z0-9-]+$')]
    [string]$ReportPrefix,

    [ValidatePattern('^vulkan-[a-z0-9-]+$')]
    [string]$VulkanOutputName = 'vulkan-hashcat-preview',

    [ValidateSet('vulkan-only', 'mixed')]
    [string]$Mode = 'vulkan-only',

    [switch]$ForceStaging,

    [switch]$CpuReferenceTest,

    [switch]$PostSubmitAbortTest,

    [switch]$PreSubmitAbortTest
)

$ErrorActionPreference = 'Stop'

if (([int]$CpuReferenceTest.IsPresent +
     [int]$PostSubmitAbortTest.IsPresent +
     [int]$PreSubmitAbortTest.IsPresent) -gt 1) {
    throw 'Select only one CPU diagnostic fault mode per GPU information run.'
}

$hostFault = $CpuReferenceTest -or $PostSubmitAbortTest -or $PreSubmitAbortTest

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$source = Join-Path $root "build\diagnostic-multimode\$VulkanOutputName\$Abi"
$frontend = Join-Path $source 'hashcat'
$module = Join-Path $source 'modules\module_00000.so'
$opencl = Join-Path $source 'OpenCL'
$logDirectory = Join-Path $root 'build\diagnostic-multimode\vulkan-integration-runs'
$suffix = if ($ForceStaging) { '-staging' } else { '' }
$remote = "/data/local/tmp/vk-info-$ReportPrefix-$Mode$suffix"

foreach ($asset in @($frontend, $module, $opencl)) {
    if (-not (Test-Path -LiteralPath $asset)) {
        throw "Diagnostic runtime asset is missing: $asset"
    }
}

$abis = @(& adb -s $Serial shell getprop ro.product.cpu.abilist)

if ($LASTEXITCODE -ne 0 -or $abis.Count -ne 1) {
    throw 'Cannot read physical device ABI information.'
}

if (@($abis[0].Trim() -split ',') -notcontains $Abi) {
    throw "Device does not support ABI: $Abi"
}

New-Item -ItemType Directory -Force -Path $logDirectory | Out-Null

try {
    & adb -s $Serial shell mkdir -p "$remote/modules" "$remote/rules" "$remote/tunings" | Out-Null

    if ($LASTEXITCODE -ne 0) {
        throw 'Cannot create isolated device directory.'
    }

    foreach ($pair in @(
        @($frontend, "$remote/hashcat"),
        @($module, "$remote/modules/module_00000.so"),
        @($opencl, $remote)
    )) {
        & adb -s $Serial push $pair[0] $pair[1] | Out-Null

        if ($LASTEXITCODE -ne 0) {
            throw "Cannot transfer runtime asset: $($pair[0])"
        }
    }

    & adb -s $Serial shell chmod 700 "$remote/hashcat" | Out-Null

    if ($LASTEXITCODE -ne 0) {
        throw 'Native diagnostic executable is not executable.'
    }

    $arguments = if ($Mode -eq 'vulkan-only') {
        '-I --backend-ignore-opencl --backend-ignore-cuda --backend-ignore-hip'
    } else {
        '-I'
    }

    $environment = if ($ForceStaging) { 'HASHCAT_VULKAN_FORCE_STAGING=1 ' } else { '' }

    if ($CpuReferenceTest) {
        $environment += 'HASHCAT_VULKAN_CPU_REFERENCE_TEST=1 '
    }

    if ($PostSubmitAbortTest) {
        $environment += 'HASHCAT_VULKAN_POST_SUBMIT_ABORT_TEST=1 '
    }

    if ($PreSubmitAbortTest) {
        $environment += 'HASHCAT_VULKAN_PRE_SUBMIT_ABORT_TEST=1 '
    }

    $command = $environment + "LD_LIBRARY_PATH=$remote $remote/hashcat $arguments 2>&1; echo VK_NATIVE_EXIT=" + '$?'
    $captured = @(& adb -s $Serial shell $command)

    if ($LASTEXITCODE -ne 0 -or $captured.Count -lt 2) {
        throw 'ADB did not return complete process output.'
    }

    $lines = @($captured | ForEach-Object { $_.TrimEnd([char]13) })
    $marker = $lines[-1].Trim()

    if ($marker -cnotmatch '^VK_NATIVE_EXIT=([0-9]+)$') {
        throw "Invalid native exit marker: $marker"
    }

    $nativeExit = [int]$Matches[1]
    $body = [string]::Join([char]10, $lines)

    if ($nativeExit -ne 0) {
        throw "Native Compute process returned $nativeExit : $body"
    }

    foreach ($pattern in @(
        'BACKEND_VULKAN_CATALOG_READY devices=',
        'Vulkan 1\.0 Compute device session verified with an empty queue submission',
        'Vulkan 1\.0 public four-stage SPIR-V Compute chain dispatched and verified',
        'Vulkan 1\.0 MD5 SPIR-V known-answer vectors dispatched and verified',
        'Vulkan 1\.0 SHA-256 SPIR-V variable-length batch dispatched and verified',
        'hashing remains disabled'
    )) {
        if ($body -notmatch $pattern) {
            throw "Missing public Compute verification record: $pattern"
        }
    }

    $expectedJobs = if ($hostFault) { 7 } else { 5 }
    $expectedManaged = if ($PreSubmitAbortTest) { 4 } elseif ($hostFault) { 5 } else { 3 }
    $expectedPolls = if ($PostSubmitAbortTest) { 4 } else { $expectedManaged }
    $expectedFencedJobs = if ($PreSubmitAbortTest) { 6 } else { $expectedJobs }
    $expectedReuse = if ($hostFault) { 6 } else { 4 }
    $expectedPipelineHits = if ($hostFault) { 13 } else { 5 }
    $expectedDescriptorReuses = if ($hostFault) { 4 } else { 2 }
    $expectedDescriptorWrites = if ($hostFault) { 34 } else { 18 }
    $expectedStorageReuses = if ($hostFault) { 19 } else { 9 }

    if ($CpuReferenceTest) {
        foreach ($pattern in @(
            '(?m)^BACKEND_VULKAN_PUBLIC_CPU_REFERENCE_FAULT stages=4 index=0 gpu_fence_completed=1 simulated=1 scheduled=0$',
            '(?m)^BACKEND_VULKAN_PROGRAM_MISMATCH stage=0 name=public_u32_transform index=0 actual=0x[0-9a-f]{8} expected=0x[0-9a-f]{8}$',
            '(?m)^BACKEND_VULKAN_PUBLIC_POST_FENCE_CPU_TEST_PASS index=0 injected_result=-3 gpu_fence_completions=2 command_recovery=0 retry=1 scheduled=0$',
            'Vulkan post-fence CPU-reference diagnostic passed with a retained healthy Compute session'
        )) {
            if ([regex]::Matches($body, $pattern).Count -ne 1) {
                throw "Post-fence CPU reference error or GPU reuse evidence is missing: $pattern"
            }
        }
    }
    elseif ($PostSubmitAbortTest) {
        foreach ($pattern in @(
            '(?m)^BACKEND_VULKAN_PUBLIC_POST_SUBMIT_ABORT index=0 submitted=1 fence_completed=0 simulated=1 scheduled=0$',
            '(?m)^BACKEND_VULKAN_MANAGED_ABORT_RETIRED index=0 submitted=1 fence_completed=1 wait_idle=0 result=0$',
            '(?m)^BACKEND_VULKAN_PUBLIC_POST_SUBMIT_TEST_PASS index=0 injected_result=-3 retired_fences=2 command_recovery=0 retry=1 scheduled=0$',
            'Vulkan post-submit host-abort diagnostic passed after fence-verified resource reuse'
        )) {
            if ([regex]::Matches($body, $pattern).Count -ne 1) {
                throw "Post-submit CPU abort or GPU resource reuse evidence is missing: $pattern"
            }
        }
    }
    elseif ($PreSubmitAbortTest) {
        foreach ($pattern in @(
            '(?m)^BACKEND_VULKAN_PUBLIC_PRE_SUBMIT_ABORT index=0 recorded=1 submitted=0 simulated=1 scheduled=0$',
            '(?m)^BACKEND_VULKAN_JOB_COMPLETION index=0 submitted=0 fence_completed=0 wait_idle=0 result=0$',
            '(?m)^BACKEND_VULKAN_PUBLIC_PRE_SUBMIT_TEST_PASS index=0 injected_result=-3 queued_before_retry=0 retry=1 command_recovery=0 scheduled=0$',
            'Vulkan pre-submit host-abort diagnostic passed with a retained healthy Compute session'
        )) {
            if ([regex]::Matches($body, $pattern).Count -ne 1) {
                throw "Pre-submit CPU abort or GPU resource reuse evidence is missing: $pattern"
            }
        }
    }
    elseif ($body -match 'BACKEND_VULKAN_PUBLIC_CPU_REFERENCE_FAULT|BACKEND_VULKAN_PUBLIC_POST_FENCE_CPU_TEST_PASS|BACKEND_VULKAN_PUBLIC_POST_SUBMIT_ABORT|BACKEND_VULKAN_PUBLIC_POST_SUBMIT_TEST_PASS|BACKEND_VULKAN_PUBLIC_PRE_SUBMIT_ABORT|BACKEND_VULKAN_PUBLIC_PRE_SUBMIT_TEST_PASS') {
        throw 'Default information mode unexpectedly injected a CPU validation error.'
    }

    $requiredCounts = @(
        @{ Tag='device'; Count=$expectedJobs; Pattern='(?m)^BACKEND_VULKAN_EXECUTOR_SESSION_REUSE index=0 queue_family=[0-9]+ api=1\.0 owns_device=0 scheduled=0$' },
        @{ Tag='pipeline-build'; Count=4; Pattern='(?m)^BACKEND_VULKAN_PIPELINE_CACHE_BUILD index=0 words=[0-9]+ entries=[0-9]+$' },
        @{ Tag='pipeline-hit'; Count=$expectedPipelineHits; Pattern='(?m)^BACKEND_VULKAN_PIPELINE_CACHE_HIT index=0 words=[0-9]+ entries=[0-9]+ hits=[0-9]+$' },
        @{ Tag='command'; Count=$expectedJobs; Pattern='(?m)^BACKEND_VULKAN_COMMAND_REUSE index=0 acquisition=[0-9]+ reused=[01] reuses=[0-9]+$' },
        @{ Tag='command-reuse'; Count=$expectedReuse; Pattern='(?m)^BACKEND_VULKAN_COMMAND_REUSE index=0 acquisition=[0-9]+ reused=1 reuses=[0-9]+$' },
        @{ Tag='fence-complete'; Count=$expectedFencedJobs; Pattern='(?m)^BACKEND_VULKAN_JOB_COMPLETION index=0 submitted=1 fence_completed=1 wait_idle=0 result=0$' },
        @{ Tag='managed-submit'; Count=$expectedManaged; Pattern='(?m)^BACKEND_VULKAN_MANAGED_COMMAND_SUBMIT index=0 pending=1 submitted=[1-5]$' },
        @{ Tag='managed-complete'; Count=$expectedManaged; Pattern='(?m)^BACKEND_VULKAN_MANAGED_COMMAND_COMPLETE index=0 completed=[1-5] pending=0$' },
        @{ Tag='managed-poll'; Count=$expectedPolls; Pattern='(?m)^BACKEND_VULKAN_PUBLIC_MANAGED_FENCE_POLL index=0 stages=[124] timeout_ns=0 result=[02] pending=[01] scheduled=0$' },
        @{ Tag='descriptor-build'; Count=3; Pattern='(?m)^BACKEND_VULKAN_DESCRIPTOR_POOL_BUILD index=0 stages=[124] pools=[123]$' },
        @{ Tag='descriptor-reuse'; Count=$expectedDescriptorReuses; Pattern='(?m)^BACKEND_VULKAN_DESCRIPTOR_POOL_REUSE index=0 stages=[14] reuses=[1-4]$' },
        @{ Tag='descriptor-bind'; Count=$expectedJobs; Pattern='(?m)^BACKEND_VULKAN_DESCRIPTOR_BIND index=0 stages=[124] writes=[248] acquisitions=[1-7]$' },
        @{ Tag='storage-build'; Count=5; Pattern='(?m)^BACKEND_VULKAN_STORAGE_ALLOC index=0 slot=[0-9]+ ' },
        @{ Tag='storage-reuse'; Count=$expectedStorageReuses; Pattern='(?m)^BACKEND_VULKAN_STORAGE_REUSE index=0 slot=[0-9]+ ' }
    )

    foreach ($check in $requiredCounts) {
        $actual = [regex]::Matches($body, $check.Pattern).Count

        if ($actual -ne $check.Count) {
            throw "GPU lifecycle check '$($check.Tag)' failed: expected $($check.Count), received $actual."
        }
    }

    $polls = [regex]::Matches(
        $body,
        '(?m)^BACKEND_VULKAN_PUBLIC_MANAGED_FENCE_POLL index=0 stages=(1|2|4) timeout_ns=0 result=(0|2) pending=([01]) scheduled=0$'
    )
    $timeouts = 0

    foreach ($stage in @('1', '2', '4')) {
        $expectedStagePolls = if ($stage -eq '4' -and $CpuReferenceTest) { 3 }
                              elseif ($stage -eq '4' -and ($PostSubmitAbortTest -or $PreSubmitAbortTest)) { 2 }
                              else { 1 }

        if (@($polls | Where-Object { $_.Groups[1].Value -eq $stage }).Count -ne $expectedStagePolls) {
            throw "Expected $expectedStagePolls arithmetic fence poll(s) for stage count $stage."
        }
    }

    foreach ($poll in $polls) {
        if (($poll.Groups[2].Value -eq '2') -ne ($poll.Groups[3].Value -eq '1')) {
            throw 'Observed GPU timeout and retained pending state disagree.'
        }
        if ($poll.Groups[2].Value -eq '2') {
            $timeouts++
        }
    }

    $driverPending = [regex]::Matches(
        $body,
        '(?m)^BACKEND_VULKAN_MANAGED_COMMAND_WAIT_PENDING index=0 timeout_ns=0 pending=1 result=2$'
    ).Count

    if ($driverPending -ne $timeouts) {
        throw "GPU fence timeout accounting failed: polls=$timeouts driver_pending=$driverPending."
    }

    foreach ($pattern in @(
        '(?m)^BACKEND_VULKAN_STAGING_RECOVERY_CLOSE index=0 recoveries=0 abandoned=0 released=1 wait_result=0$',
        '(?m)^BACKEND_VULKAN_COMMAND_RECOVERY_CLOSE index=0 recoveries=0 released=1 wait_result=0$',
        ("(?m)^BACKEND_VULKAN_MANAGED_COMMAND_CLOSE index=0 submitted={0} completed={0} cancelled=0 released=1 wait_result=0$" -f $expectedManaged),
        ("(?m)^BACKEND_VULKAN_RESOURCES_CLOSE index=0 pipelines=4 pipeline_hits={0} command_acquisitions={1} command_reuses={2} released=1 wait_result=0$" -f $expectedPipelineHits, $expectedJobs, $expectedReuse),
        ("(?m)^BACKEND_VULKAN_DESCRIPTORS_CLOSE index=0 pools=3 pool_reuses={0} acquisitions={1} writes={2} released=1 wait_result=0$" -f $expectedDescriptorReuses, $expectedJobs, $expectedDescriptorWrites),
        ("(?m)^BACKEND_VULKAN_SESSION_CLOSE index=0 queue_family=[0-9]+ empty_submissions=1 storage_buffers=5 storage_allocations=5 storage_reuses={0} completed_transfers=0 wait_result=0$" -f $expectedStorageReuses)
    )) {
        if ($body -notmatch $pattern) {
            throw "GPU session ownership closure record is missing: $pattern"
        }
    }

    if ($body -match '(?m)^BACKEND_VULKAN_COMMAND_(?:POISON|RECOVER) index=' -or
        $body -match '(?m)^BACKEND_VULKAN_STAGING_(?:RECOVER|ERROR) index=') {
        throw 'A successful GPU information session unexpectedly entered recovery.'
    }

    if ($ForceStaging) {
        $stagingBuilds = [regex]::Matches(
            $body, '(?m)^BACKEND_VULKAN_STORAGE_ALLOC .* staging=1 coherent=[01] allocations=[0-9]+$'
        ).Count
        $stagingReuses = [regex]::Matches(
            $body, '(?m)^BACKEND_VULKAN_STORAGE_REUSE .* staging=1 coherent=[01] reuses=[0-9]+$'
        ).Count
        $records = [regex]::Matches(
            $body, '(?m)^BACKEND_VULKAN_STAGING_PASS stages=([0-9]+) buffers=([0-9]+) uploads=([0-9]+) downloads=([0-9]+) forced=1 scheduled=0$'
        )

        $uploads = 0
        $downloads = 0

        foreach ($record in $records) {
            $uploads += [int]$record.Groups[3].Value
            $downloads += [int]$record.Groups[4].Value
        }

        $expectedStagingPrograms = if ($hostFault) { 6 } else { 5 }
        $expectedUploads = if ($hostFault) { 7 } else { 6 }
        $expectedDownloads = if ($hostFault) { 13 } else { 9 }

        if ($stagingBuilds -ne 5 -or $stagingReuses -ne $expectedStorageReuses -or
            $records.Count -ne $expectedStagingPrograms -or
            $uploads -ne $expectedUploads -or $downloads -ne $expectedDownloads) {
            throw "Staging data transfer failed: allocations=$stagingBuilds reuses=$stagingReuses programs=$($records.Count) uploads=$uploads downloads=$downloads."
        }
    }

    if ($Mode -eq 'vulkan-only' -and
        $body -match 'OpenCL Info:|CUDA Info:|HIP Info:|No devices found/left') {
        throw 'Vulkan-only diagnostics unexpectedly required another GPU runtime.'
    }

    $report = @(
        'TEST=vulkan-compute-fence-session',
        "ABI=$Abi",
        "MODE=$Mode",
        "FORCE_STAGING=$([int]$ForceStaging.IsPresent)",
        "CPU_REFERENCE_TEST=$([int]$CpuReferenceTest.IsPresent)",
        "POST_SUBMIT_ABORT_TEST=$([int]$PostSubmitAbortTest.IsPresent)",
        "PRE_SUBMIT_ABORT_TEST=$([int]$PreSubmitAbortTest.IsPresent)",
        "COMMAND=$arguments",
        "NATIVE_EXIT_CODE=$nativeExit",
        $body
    )
    $output = Join-Path $logDirectory "$ReportPrefix-$Abi-$Mode$suffix.log"

    [System.IO.File]::WriteAllText(
        $output,
        ([string]::Join([char]10, $report)) + [char]10,
        [System.Text.UTF8Encoding]::new($false)
    )

    Write-Output "PASS GPU session mode=$Mode abi=$Abi staging=$($ForceStaging.IsPresent) cpu_reference_test=$($CpuReferenceTest.IsPresent) post_submit_abort_test=$($PostSubmitAbortTest.IsPresent) pre_submit_abort_test=$($PreSubmitAbortTest.IsPresent) report=$ReportPrefix"
    Write-Output "VULKAN_INFO_LOG=$output"
}
finally {
    & adb -s $Serial shell rm -rf $remote 2>$null | Out-Null
}
