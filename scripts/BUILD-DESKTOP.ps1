param(
    [ValidateSet('windows-x64', 'linux-x64', 'macos-x64', 'macos-arm64', 'macos-universal', 'all')]
    [string[]]$Target = @('windows-x64'),
    [string]$Msys2Bash = '',
    [string]$Msys2Root = '',
    [string]$WslDistro = '',
    [string]$MakeJobs = '8',
    [string]$VersionTag = 'v7.1.2',
    [switch]$Clean,
    [switch]$Package
)

$ErrorActionPreference = 'Stop'
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
[Console]::InputEncoding = $utf8NoBom
[Console]::OutputEncoding = $utf8NoBom
$OutputEncoding = $utf8NoBom

<#
.SYNOPSIS
Detects whether the current PowerShell process runs on Windows.

.DESCRIPTION
Uses RuntimeInformation so the check works in Windows PowerShell and PowerShell Core.
#>
function Test-WindowsHost
{
    return [System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
        [System.Runtime.InteropServices.OSPlatform]::Windows
    )
}

<#
.SYNOPSIS
Detects whether the current PowerShell process runs on Linux.

.DESCRIPTION
Uses RuntimeInformation so the same build script can run from PowerShell Core on Linux CI hosts.
#>
function Test-LinuxHost
{
    return [System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
        [System.Runtime.InteropServices.OSPlatform]::Linux
    )
}

<#
.SYNOPSIS
Detects whether the current PowerShell process runs on macOS.

.DESCRIPTION
Uses RuntimeInformation so macOS builds can use the same wrapper script as Windows builds.
#>
function Test-MacOsHost
{
    return [System.Runtime.InteropServices.RuntimeInformation]::IsOSPlatform(
        [System.Runtime.InteropServices.OSPlatform]::OSX
    )
}

<#
.SYNOPSIS
Checks whether Windows has a runnable default WSL distribution.

.DESCRIPTION
Runs a no-op bash command through wsl.exe and returns true only when Linux build commands can execute in the default distribution.
#>
function Test-WslBuildHost
{
    if (-not (Test-WindowsHost))
    {
        return $false
    }

    if (-not (Get-Command wsl.exe -ErrorAction SilentlyContinue))
    {
        return $false
    }

    $wslErrorActionPreference = $ErrorActionPreference

    try
    {
        $ErrorActionPreference = 'Continue'
        & wsl.exe -- bash -lc true *> $null

        return $LASTEXITCODE -eq 0
    }
    catch
    {
        return $false
    }
    finally
    {
        $ErrorActionPreference = $wslErrorActionPreference
    }
}

<#
.SYNOPSIS
Returns desktop targets supported by the current host.

.DESCRIPTION
Windows builds windows-x64 and adds linux-x64 when WSL can execute bash. Linux builds linux-x64. macOS builds all macOS variants.
#>
function Get-SupportedDesktopTargets
{
    if (Test-WindowsHost)
    {
        $targets = @('windows-x64')

        if (Test-WslBuildHost)
        {
            $targets += 'linux-x64'
        }
        else
        {
            Write-Warning 'Skipping linux-x64 for -Target all because no runnable default WSL distribution is available.'
        }

        Write-Warning 'Skipping macOS targets for -Target all because the current host is Windows.'

        return $targets
    }

    if (Test-LinuxHost)
    {
        return @('linux-x64')
    }

    if (Test-MacOsHost)
    {
        return @('macos-x64', 'macos-arm64', 'macos-universal')
    }

    return @()
}

<#
.SYNOPSIS
Returns an absolute path for an existing or planned filesystem entry.

.DESCRIPTION
Resolve-Path is used for existing paths, and GetFullPath is used for paths that are created later.
#>
function Resolve-AbsolutePath([string]$Path)
{
    if (Test-Path -LiteralPath $Path)
    {
        return (Resolve-Path -LiteralPath $Path).Path
    }

    return [System.IO.Path]::GetFullPath($Path)
}

<#
.SYNOPSIS
Finds MSYS2 bash for Windows desktop builds.

.DESCRIPTION
Checks explicit parameters, MSYS2_ROOT, PATH and the standard C:\msys64 / C:\msys32 locations.
#>
function Resolve-Msys2Bash([string]$ExplicitPath, [string]$ExplicitRoot)
{
    if (-not [string]::IsNullOrWhiteSpace($ExplicitPath))
    {
        return (Resolve-Path -LiteralPath $ExplicitPath).Path
    }

    if (-not [string]::IsNullOrWhiteSpace($ExplicitRoot))
    {
        $candidate = Join-Path $ExplicitRoot 'usr\bin\bash.exe'
        if (Test-Path -LiteralPath $candidate)
        {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    if (-not [string]::IsNullOrWhiteSpace($env:MSYS2_ROOT))
    {
        $candidate = Join-Path $env:MSYS2_ROOT 'usr\bin\bash.exe'
        if (Test-Path -LiteralPath $candidate)
        {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    $fromPath = Get-Command bash.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($fromPath)
    {
        return $fromPath.Source
    }

    foreach ($candidate in @('C:\msys64\usr\bin\bash.exe', 'C:\msys32\usr\bin\bash.exe'))
    {
        if (Test-Path -LiteralPath $candidate)
        {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    throw 'MSYS2 bash was not found. Pass -Msys2Bash or set MSYS2_ROOT.'
}

<#
.SYNOPSIS
Returns the MSYS2 root directory for a bash executable.

.DESCRIPTION
The bash path points to usr\bin\bash.exe; walking two parent directories yields the MSYS2 root.
#>
function Resolve-Msys2Root([string]$BashPath, [string]$ExplicitRoot)
{
    if (-not [string]::IsNullOrWhiteSpace($ExplicitRoot))
    {
        return (Resolve-Path -LiteralPath $ExplicitRoot).Path
    }

    $binDirectory = Split-Path -Parent $BashPath
    $usrDirectory = Split-Path -Parent $binDirectory

    return Split-Path -Parent $usrDirectory
}

<#
.SYNOPSIS
Quotes a value for a POSIX shell command line.

.DESCRIPTION
Wraps the value in single quotes and represents embedded single quotes with the POSIX-safe sequence.
#>
function Quote-Bash([string]$Value)
{
    return "'" + $Value.Replace("'", "'\''") + "'"
}

<#
.SYNOPSIS
Converts a Windows path into an MSYS2 path.

.DESCRIPTION
Maps drive-letter paths to /C/... style paths and replaces backslashes with forward slashes.
#>
function ConvertTo-MsysPath([string]$Path)
{
    $absolute = Resolve-AbsolutePath $Path

    return $absolute -replace '^([A-Za-z]):', '/$1' -replace '\\', '/'
}

<#
.SYNOPSIS
Converts an MSYS2 path from ldd output into a Windows path.

.DESCRIPTION
Maps /mingw64, /mingw32, /ucrt64 and /usr paths into the selected MSYS2 root, and maps /c/... paths to drive-letter paths.
#>
function ConvertFrom-MsysPath([string]$Path, [string]$Root)
{
    $normalized = $Path -replace '/', '\'

    if ($Path -match '^/([A-Za-z])/(.*)$')
    {
        return ($matches[1].ToUpperInvariant() + ':\' + ($matches[2] -replace '/', '\'))
    }

    foreach ($prefix in @('mingw64', 'mingw32', 'ucrt64', 'clang64', 'clangarm64', 'usr'))
    {
        $msysPrefix = '/' + $prefix + '/'
        if ($Path.StartsWith($msysPrefix))
        {
            $relative = $Path.Substring(1) -replace '/', '\'
            return Join-Path $Root $relative
        }
    }

    return $normalized
}

<#
.SYNOPSIS
Converts a Windows path into a WSL path.

.DESCRIPTION
Validates an absolute Windows drive-letter path and maps D:\dir\file to /mnt/d/dir/file for Linux builds executed through WSL.
#>
function ConvertTo-WslPath([string]$Path)
{
    $absolute = Resolve-AbsolutePath $Path

    if ($absolute.Length -lt 3 -or -not [char]::IsLetter($absolute[0]) -or $absolute[1] -ne ':' -or $absolute[2] -ne [System.IO.Path]::DirectorySeparatorChar)
    {
        throw "WSL path conversion expects a drive-letter path: $absolute"
    }

    $drive = $absolute[0].ToString().ToLowerInvariant()
    $tail = $absolute.Substring(3).Replace('\', '/')

    return "/mnt/$drive/$tail"
}

<#
.SYNOPSIS
Runs a command in MSYS2 bash and stops when the command exits with an error.

.DESCRIPTION
The command is executed through bash -lc so exported variables and shell pipelines work as expected.
#>
function Invoke-Msys2([string]$BashPath, [string]$Command)
{
    & $BashPath -lc $Command

    if ($LASTEXITCODE -ne 0)
    {
        throw "MSYS2 command failed with exit code $LASTEXITCODE"
    }
}

<#
.SYNOPSIS
Runs a command in WSL bash and stops when the command exits with an error.

.DESCRIPTION
Uses an optional distro name when -WslDistro is supplied and otherwise uses the default WSL distribution.
#>
function Invoke-Wsl([string]$Command, [string]$Distro)
{
    $arguments = @()

    if (-not [string]::IsNullOrWhiteSpace($Distro))
    {
        $arguments += @('--distribution', $Distro)
    }

    $arguments += @('--', 'bash', '-lc', $Command)

    & wsl.exe @arguments

    if ($LASTEXITCODE -ne 0)
    {
        throw "WSL command failed with exit code $LASTEXITCODE"
    }
}

<#
.SYNOPSIS
Runs a command in the local POSIX shell and stops when the command exits with an error.

.DESCRIPTION
Uses bash -lc for Linux and macOS hosts where the source Makefile is executed directly.
#>
function Invoke-LocalBash([string]$Command)
{
    & bash -lc $Command

    if ($LASTEXITCODE -ne 0)
    {
        throw "bash command failed with exit code $LASTEXITCODE"
    }
}

<#
.SYNOPSIS
Creates an empty destination directory for a platform artifact set.

.DESCRIPTION
Deletes the previous destination directory and creates a fresh one under build\<platform>.
#>
function New-PlatformBuildDirectory([string]$BuildRoot, [string]$Platform)
{
    $destination = Join-Path $BuildRoot $Platform

    if (Test-Path -LiteralPath $destination)
    {
        Remove-Item -Recurse -Force -LiteralPath $destination
    }

    New-Item -ItemType Directory -Force -Path $destination | Out-Null

    return $destination
}

<#
.SYNOPSIS
Copies a file when it exists and optionally requires it.

.DESCRIPTION
Required files stop the build when missing; optional files are copied only when the source exists.
#>
function Copy-BuildFile([string]$SourceDir, [string]$DestinationDir, [string]$Name, [bool]$Required)
{
    $source = Join-Path $SourceDir $Name

    if (-not (Test-Path -LiteralPath $source))
    {
        if ($Required)
        {
            throw "Required build output is missing: $source"
        }

        return
    }

    Copy-Item -Force -LiteralPath $source -Destination (Join-Path $DestinationDir $Name)
}

<#
.SYNOPSIS
Copies files matching a root-level pattern.

.DESCRIPTION
Copies optional shared libraries such as libhashcat.so.* or *.dylib into the platform directory.
#>
function Copy-BuildFilePattern([string]$SourceDir, [string]$DestinationDir, [string]$Pattern)
{
    Get-ChildItem -LiteralPath $SourceDir -Filter $Pattern -File -ErrorAction SilentlyContinue | ForEach-Object {
        Copy-Item -Force -LiteralPath $_.FullName -Destination (Join-Path $DestinationDir $_.Name)
    }
}

<#
.SYNOPSIS
Copies a runtime directory when it exists.

.DESCRIPTION
Runtime directories carry modules, bridges, feeds, kernels, rules, masks and support data used by the binary.
#>
function Copy-RuntimeDirectory([string]$SourceDir, [string]$DestinationDir, [string]$Name)
{
    $source = Join-Path $SourceDir $Name

    if (-not (Test-Path -LiteralPath $source))
    {
        return
    }

    $destination = Join-Path $DestinationDir $Name
    Copy-Item -Recurse -Force -LiteralPath $source -Destination $destination
}

<#
.SYNOPSIS
Copies the common runtime tree produced by hashcat.

.DESCRIPTION
The copied tree is portable for unpacked local execution and includes plugins, kernels, rules and sample data.
#>
function Copy-CommonRuntime([string]$SourceDir, [string]$DestinationDir)
{
    foreach ($name in @(
        'modules',
        'bridges',
        'feeds',
        'OpenCL',
        'rules',
        'tunings',
        'pcfg',
        'charsets',
        'masks',
        'docs'
    ))
    {
        Copy-RuntimeDirectory $SourceDir $DestinationDir $name
    }

    foreach ($name in @(
        'hashcat.hcstat2',
        'example.dict',
        'example0.hash',
        'example0.cmd',
        'example0.sh',
        'example400.hash',
        'example400.cmd',
        'example400.sh',
        'example500.hash',
        'example500.cmd',
        'example500.sh'
    ))
    {
        Copy-BuildFile $SourceDir $DestinationDir $name $false
    }
}

<#
.SYNOPSIS
Copies MSYS2 DLL dependencies required by hashcat.exe.

.DESCRIPTION
Reads ldd output for hashcat.exe and copies resolved MSYS2 DLLs next to the executable.
#>
function Copy-Msys2RuntimeDependencies([string]$BashPath, [string]$Root, [string]$SourceDir, [string]$DestinationDir)
{
    $sourceForMsys = ConvertTo-MsysPath $SourceDir
    $lddCommand = "cd $(Quote-Bash $sourceForMsys) && ldd ./hashcat.exe"
    $lines = & $BashPath -lc $lddCommand 2>$null

    if ($LASTEXITCODE -ne 0)
    {
        Write-Warning 'ldd did not return dependency data for hashcat.exe.'
        return
    }

    foreach ($line in $lines)
    {
        $paths = [regex]::Matches($line, '(/[^\s]+\.dll)') | ForEach-Object { $_.Groups[1].Value }

        foreach ($path in $paths)
        {
            if ($path -match '^/[A-Za-z]/Windows/')
            {
                continue
            }

            $windowsPath = ConvertFrom-MsysPath $path $Root

            if (Test-Path -LiteralPath $windowsPath)
            {
                Copy-Item -Force -LiteralPath $windowsPath -Destination (Join-Path $DestinationDir (Split-Path -Leaf $windowsPath))
            }
        }
    }
}

<#
.SYNOPSIS
Builds Windows x64 artifacts with MSYS2.

.DESCRIPTION
Runs the upstream native MSYS2 production build with the requested version tag, supplies the four-part numeric Windows resource version and collects hashcat.exe, plugins, assets and required MSYS2 DLLs into build\windows-x64.
#>
function Build-WindowsX64([string]$SourceDir, [string]$BuildRoot)
{
    if (-not (Test-WindowsHost))
    {
        throw 'windows-x64 target requires a Windows host with MSYS2.'
    }

    $bash = Resolve-Msys2Bash $Msys2Bash $Msys2Root
    $root = Resolve-Msys2Root $bash $Msys2Root
    $sourceForMsys = ConvertTo-MsysPath $SourceDir
    $cleanCommand = if ($Clean) { 'make clean' } else { 'true' }
    $patchFile = Join-Path $RootDir 'patches\hashcat-msys2-nvml-filehandling.patch'
    $patchApplied = $false

    if ($VersionTag -notmatch '^v?(\d+)\.(\d+)\.(\d+)$')
    {
        throw "windows-x64 production version must use vMAJOR.MINOR.PATCH format: $VersionTag"
    }

    $windowsVersionNumber = "$($matches[1]),$($matches[2]),$($matches[3]),0"

    try
    {
        if (Test-Path -LiteralPath $patchFile)
        {
            Push-Location $SourceDir
            try
            {
                git apply --check $patchFile *> $null
                if ($LASTEXITCODE -eq 0)
                {
                    Write-Host "Applying temporary Windows/MSYS2 patch: $patchFile"
                    git apply $patchFile
                    if ($LASTEXITCODE -ne 0) { throw "git apply failed for: $patchFile" }
                    $patchApplied = $true
                }
                else
                {
                    git apply --reverse --check $patchFile *> $null
                    if ($LASTEXITCODE -eq 0)
                    {
                        Write-Host 'Temporary Windows/MSYS2 patch is already applied.'
                    }
                    else
                    {
                        throw "Temporary Windows/MSYS2 patch cannot be applied cleanly: $patchFile"
                    }
                }
            }
            finally
            {
                Pop-Location
            }
        }

        $skipNoticeRegex = 'Skipping (freethreaded|regular) plugin (72000|73000|74000)'
        $skipNoticeRegex += '|Skipping generic attack-mode 8 plugin'
        $skipNoticeRegex += '|Python Windows headers not found'
        $skipNoticeRegex += '|cargo not found'
        $skipNoticeRegex += '|rustup not found'
        $skipNoticeRegex += '|To use -m 7[234]000, you must install'
        $skipNoticeRegex += '|To use it, you must install Rust'
        $skipNoticeRegex += '|Otherwise, you can safely ignore this warning'
        $skipNoticeRegex += '|For more information, see .docs/hashcat-(python|rust)-plugin-requirements'
        $skipNoticeRegex += '|See BUILD_WSL.md how to prepare'

        $makeCommand = @(
            'set -e',
            "cd $(Quote-Bash $sourceForMsys)",
            'export PATH=/mingw64/bin:/usr/bin:$PATH',
            $cleanCommand,
            'set +e',
            "make -j$MakeJobs PRODUCTION=1 VERSION_TAG=$(Quote-Bash $VersionTag) VERSION_NUM=$(Quote-Bash $windowsVersionNumber) WIN_PYTHON= 2>&1 | grep -v -E $(Quote-Bash $skipNoticeRegex)",
            'make_status=${PIPESTATUS[0]}',
            'set -e',
            'exit $make_status'
        ) -join '; '

        Invoke-Msys2 $bash $makeCommand | Out-Host

        $destination = New-PlatformBuildDirectory $BuildRoot 'windows-x64'

        Copy-BuildFile $SourceDir $destination 'hashcat.exe' $true
        Copy-BuildFile $SourceDir $destination 'hashcat.dll' $false
        Copy-BuildFilePattern $SourceDir $destination '*.dll'
        Copy-CommonRuntime $SourceDir $destination
        Copy-Msys2RuntimeDependencies $bash $root $SourceDir $destination

        return $destination
    }
    finally
    {
        if ($patchApplied)
        {
            Write-Host 'Reverting temporary Windows/MSYS2 patch.'
            git -C $SourceDir checkout -- src/ext_nvml.c
        }
    }
}
<#
.SYNOPSIS
Builds Linux x64 artifacts through WSL or a Linux host.

.DESCRIPTION
Runs the upstream native Linux production build with the requested version tag and collects the Linux binary, shared library, plugins and runtime assets into build\linux-x64.
#>
function Build-LinuxX64([string]$SourceDir, [string]$BuildRoot)
{
    $cleanCommand = if ($Clean) { 'make clean' } else { 'true' }

    if (Test-WindowsHost)
    {
        $wslPath = ConvertTo-WslPath $SourceDir
        $command = @(
            'set -e',
            "cd $(Quote-Bash $wslPath)",
            $cleanCommand,
            "make -j$MakeJobs PRODUCTION=1 VERSION_TAG=$(Quote-Bash $VersionTag)"
        ) -join '; '

        Invoke-Wsl $command $WslDistro | Out-Host
    }
    elseif (Test-LinuxHost)
    {
        $sourceForShell = Resolve-AbsolutePath $SourceDir
        $command = @(
            'set -e',
            "cd $(Quote-Bash $sourceForShell)",
            $cleanCommand,
            "make -j$MakeJobs PRODUCTION=1 VERSION_TAG=$(Quote-Bash $VersionTag)"
        ) -join '; '

        Invoke-LocalBash $command | Out-Host
    }
    else
    {
        throw 'linux-x64 target requires Windows with WSL or a Linux host.'
    }

    $destination = New-PlatformBuildDirectory $BuildRoot 'linux-x64'

    Copy-BuildFile $SourceDir $destination 'hashcat' $true
    Copy-BuildFilePattern $SourceDir $destination 'libhashcat.so*'
    Copy-CommonRuntime $SourceDir $destination

    return $destination
}

<#
.SYNOPSIS
Builds macOS artifacts on a macOS host.

.DESCRIPTION
Runs the upstream native macOS production build with the requested version tag and collects the binary, dylib files, plugins and runtime assets into a macOS platform directory.
#>
function Build-MacOs([string]$SourceDir, [string]$BuildRoot, [string]$Platform, [string]$ArchArgument)
{
    if (-not (Test-MacOsHost))
    {
        throw "$Platform target requires a macOS host. Run this script from PowerShell Core on macOS or use CI."
    }

    $sourceForShell = Resolve-AbsolutePath $SourceDir
    $cleanCommand = if ($Clean) { 'make clean' } else { 'true' }
    $makeFlags = @("PRODUCTION=1 VERSION_TAG=$(Quote-Bash $VersionTag)")

    if (-not [string]::IsNullOrWhiteSpace($ArchArgument))
    {
        $makeFlags += $ArchArgument
    }

    $command = @(
        'set -e',
        "cd $(Quote-Bash $sourceForShell)",
        $cleanCommand,
        "make -j$MakeJobs $($makeFlags -join ' ')"
    ) -join '; '

    Invoke-LocalBash $command | Out-Host

    $destination = New-PlatformBuildDirectory $BuildRoot $Platform

    Copy-BuildFile $SourceDir $destination 'hashcat' $true
    Copy-BuildFilePattern $SourceDir $destination '*.dylib'
    Copy-CommonRuntime $SourceDir $destination

    return $destination
}

<#
.SYNOPSIS
Creates a zip package for a platform directory.

.DESCRIPTION
Writes release\hashcat-<version>-<platform>.zip from the collected build\<platform> directory.
#>
function New-PlatformPackage([string]$RootDir, [string]$PlatformDirectory, [string]$Platform)
{
    $releaseDir = Join-Path $RootDir 'release'
    New-Item -ItemType Directory -Force -Path $releaseDir | Out-Null

    $archive = Join-Path $releaseDir "hashcat-$VersionTag-$Platform.zip"
    $stagingRoot = Join-Path $RootDir 'build\.package-staging'
    $stagingDir = Join-Path $stagingRoot $Platform

    if (Test-Path -LiteralPath $archive)
    {
        Remove-Item -Force -LiteralPath $archive
    }

    if (Test-Path -LiteralPath $stagingDir)
    {
        Remove-Item -Recurse -Force -LiteralPath $stagingDir
    }

    New-Item -ItemType Directory -Force -Path $stagingDir | Out-Null

    $items = @(Get-ChildItem -LiteralPath $PlatformDirectory -Force)
    if ($items.Count -eq 0)
    {
        throw "Cannot package empty platform directory: $PlatformDirectory"
    }

    foreach ($item in $items)
    {
        Copy-Item -LiteralPath $item.FullName -Destination $stagingDir -Recurse -Force
    }

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [System.IO.Compression.ZipFile]::CreateFromDirectory($stagingDir, $archive, [System.IO.Compression.CompressionLevel]::Optimal, $false)

    Remove-Item -Recurse -Force -LiteralPath $stagingDir

    return $archive
}

$ScriptDir = $PSScriptRoot
$RootDir = (Resolve-Path (Join-Path $ScriptDir '..')).Path
$SourceDir = Join-Path $RootDir 'source'
$BuildRoot = Join-Path $RootDir 'build'

if (-not (Test-Path -LiteralPath $SourceDir))
{
    throw "hashcat source directory is missing: $SourceDir"
}

if (-not (Test-Path -LiteralPath (Join-Path $SourceDir 'src\Makefile')))
{
    throw "hashcat Makefile is missing in source: $SourceDir"
}

New-Item -ItemType Directory -Force -Path $BuildRoot | Out-Null

$resolvedTargets = @()

foreach ($item in $Target)
{
    if ($item -eq 'all')
    {
        $resolvedTargets += Get-SupportedDesktopTargets
    }
    else
    {
        $resolvedTargets += $item
    }
}

$results = @()

foreach ($item in ($resolvedTargets | Select-Object -Unique))
{
    Write-Output "Building $item..."

    $directory = switch ($item)
    {
        'windows-x64' { Build-WindowsX64 $SourceDir $BuildRoot }
        'linux-x64' { Build-LinuxX64 $SourceDir $BuildRoot }
        'macos-x64' { Build-MacOs $SourceDir $BuildRoot 'macos-x64' 'HOST_ARCH=x86_64' }
        'macos-arm64' { Build-MacOs $SourceDir $BuildRoot 'macos-arm64' 'HOST_ARCH=arm64' }
        'macos-universal' { Build-MacOs $SourceDir $BuildRoot 'macos-universal' 'MACOS_UNIVERSAL_BINARY=1' }
        default { throw "Unknown build target: $item" }
    }

    $directory = @($directory) | Select-Object -Last 1
    $archive = $null

    if ($Package)
    {
        $archive = New-PlatformPackage $RootDir $directory $item
    }

    $results += [pscustomobject][ordered]@{
        Target = $item
        Directory = $directory
        Package = $archive
        Files = (Get-ChildItem -LiteralPath $directory -Recurse | Where-Object { -not $_.PSIsContainer } | Measure-Object).Count
        Bytes = (Get-ChildItem -LiteralPath $directory -Recurse | Where-Object { -not $_.PSIsContainer } | Measure-Object -Property Length -Sum).Sum
    }
}

$results | Format-Table -AutoSize
