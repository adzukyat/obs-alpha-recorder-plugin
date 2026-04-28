param(
    [string]$ObsRoot = $env:OBS_ROOT,
    [string]$VersionFile = (Join-Path $PSScriptRoot "..\deps\obs.version"),
    [string]$ConfigFile = (Join-Path $PSScriptRoot "..\deps\obs\obs-root.cmake"),
    [string]$SourceDir = (Join-Path $PSScriptRoot "..\deps\obs\obs-studio"),
    [string]$BuildDir = (Join-Path $PSScriptRoot "..\deps\obs\obs-build"),
    [string]$InstallDir = $null,
    [string]$SourceUrl = 'https://github.com/obsproject/obs-studio.git',
    [string]$Generator = $(if ($IsMacOS) { 'Ninja' } else { 'Visual Studio 17 2022' }),
    [string]$Architecture = $(if ($IsMacOS) { '' } else { 'x64' }),
    [string]$Configuration = 'RelWithDebInfo',
    [switch]$CloneSource,
    [switch]$BuildFromSource
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Invoke-ExternalCommand {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,
        [Parameter(Mandatory = $true)]
        [string[]]$Arguments,
        [Parameter(Mandatory = $true)]
        [string]$FailureMessage
    )

    & $FilePath @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "$FailureMessage (exit code $LASTEXITCODE)"
    }
}

function Get-ObsVersionTag {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        throw "OBS version manifest not found: $Path"
    }

    foreach ($line in Get-Content -LiteralPath $Path) {
        if ($line -match '^tag=(.+)$') {
            return $Matches[1].Trim()
        }
    }

    throw "OBS version manifest does not define a tag: $Path"
}

function Test-AnyPathExists {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Candidates
    )

    foreach ($candidate in $Candidates) {
        if (Test-Path -LiteralPath $candidate) {
            return $true
        }
    }

    return $false
}

function Test-ObsRuntimeLayout {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if ($IsMacOS) {
        $runtimeCandidates = @(
            (Join-Path $Path 'bin/obs.dylib'),
            (Join-Path $Path 'bin/libobs.dylib')
        )

        return (Test-Path -LiteralPath (Join-Path $Path 'bin')) -and
        (Test-Path -LiteralPath (Join-Path $Path 'data')) -and
        (Test-Path -LiteralPath (Join-Path $Path 'obs-plugins')) -and
        (Test-AnyPathExists -Candidates $runtimeCandidates)
    }

    $runtimeCandidates = @(
        (Join-Path $Path 'bin\64bit\obs.dll'),
        (Join-Path $Path 'bin\64bit\libobs.dll'),
        (Join-Path $Path 'bin\obs.dll'),
        (Join-Path $Path 'bin\libobs.dll')
    )

    return (Test-Path -LiteralPath (Join-Path $Path 'bin\64bit')) -and
    (Test-Path -LiteralPath (Join-Path $Path 'data')) -and
    (Test-Path -LiteralPath (Join-Path $Path 'obs-plugins\64bit')) -and
    (Test-AnyPathExists -Candidates $runtimeCandidates)
}

function Test-ObsDeveloperLayout {
    param(
        [Parameter(Mandatory = $true)]
        [string]$SourceDir,
        [Parameter(Mandatory = $true)]
        [string]$BuildDir,
        [Parameter(Mandatory = $true)]
        [string]$Configuration
    )

    $libraryCandidates = if ($IsMacOS) {
        @(
            (Join-Path $BuildDir 'libobs/libobs.dylib'),
            (Join-Path (Join-Path $BuildDir 'libobs') $Configuration | Join-Path -ChildPath 'libobs.dylib')
        )
    } else {
        @(
            (Join-Path (Join-Path (Join-Path $BuildDir 'libobs') $Configuration) 'obs.lib')
        )
    }

    $headerCandidates = @(
        (Join-Path $SourceDir 'libobs\obs-module.h'),
        (Join-Path $SourceDir 'libobs\obs.h'),
        (Join-Path $SourceDir 'libobs\obs-config.h')
    )

    $configCandidates = @(
        (Join-Path $BuildDir 'config\obsconfig.h')
    )

    return (Test-AnyPathExists -Candidates $headerCandidates) -and
    (Test-AnyPathExists -Candidates $configCandidates) -and
    (Test-AnyPathExists -Candidates $libraryCandidates)
}

function Write-ObsRootConfig {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$ConfigPath
    )

    $resolvedPath = [System.IO.Path]::GetFullPath((Resolve-Path -LiteralPath $Path).Path)
    $cmakePath = $resolvedPath.Replace('\', '/')

    $configDirectory = Split-Path -Parent $ConfigPath
    if (-not (Test-Path -LiteralPath $configDirectory)) {
        New-Item -ItemType Directory -Path $configDirectory -Force | Out-Null
    }

    $contents = @"
set(OBS_ROOT "$cmakePath" CACHE PATH "Path to a staged OBS developer tree" FORCE)
"@

    Set-Content -LiteralPath $ConfigPath -Value $contents -Encoding utf8
}

function Get-GitExactTag {
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoPath
    )

    $tagName = & git -C $RepoPath describe --tags --exact-match HEAD 2>$null
    if ($LASTEXITCODE -ne 0) {
        return $null
    }

    if ($tagName -is [array]) {
        $tagName = $tagName | Select-Object -First 1
    }

    return $tagName.Trim()
}

function Resolve-CmakePath {
    $cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
    if ($null -ne $cmakeCommand) {
        return $cmakeCommand.Path
    }

    $searchPatterns = @()
    $programFilesRoot = [Environment]::GetFolderPath('ProgramFiles')
    $programFilesX86Root = [Environment]::GetFolderPath('ProgramFilesX86')

    foreach ($programFilesPath in @($programFilesRoot, $programFilesX86Root)) {
        if (-not [string]::IsNullOrWhiteSpace($programFilesPath)) {
            $searchPatterns += (Join-Path $programFilesPath 'CMake\bin\cmake.exe')
            $searchPatterns += (Join-Path $programFilesPath 'Microsoft Visual Studio\2022\*\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe')
        }
    }

    foreach ($searchPattern in $searchPatterns) {
        $candidates = Get-ChildItem -Path $searchPattern -ErrorAction SilentlyContinue
        if ($candidates) {
            return $candidates[0].FullName
        }
    }

    throw 'cmake is required to build the OBS developer tree'
}

function Initialize-ObsSourceTree {
    param(
        [Parameter(Mandatory = $true)]
        [string]$SourceUrl,
        [Parameter(Mandatory = $true)]
        [string]$SourceDir,
        [Parameter(Mandatory = $true)]
        [string]$VersionTag,
        [switch]$CloneSource
    )

    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        throw 'git is required to fetch the OBS source tree'
    }

    $sourceParent = Split-Path -Parent $SourceDir
    if (-not (Test-Path -LiteralPath $sourceParent)) {
        New-Item -ItemType Directory -Path $sourceParent -Force | Out-Null
    }

    if (Test-Path -LiteralPath $SourceDir) {
        if (-not (Test-Path -LiteralPath (Join-Path $SourceDir '.git'))) {
            throw "Existing OBS source directory is not a git checkout: $SourceDir"
        }

        $currentTag = Get-GitExactTag -RepoPath $SourceDir
        if ($CloneSource) {
            Invoke-ExternalCommand -FilePath git -Arguments @('-C', $SourceDir, 'fetch', '--depth', '1', 'origin', '--tags') -FailureMessage "Failed to refresh the OBS source checkout at $SourceDir"
            Invoke-ExternalCommand -FilePath git -Arguments @('-C', $SourceDir, 'checkout', '--force', $VersionTag) -FailureMessage "Failed to switch OBS source checkout to tag $VersionTag"
        }
        elseif ($currentTag -ne $VersionTag) {
            throw "OBS source checkout at $SourceDir is on '$currentTag' but the pinned tag is '$VersionTag'. Pass -CloneSource to refresh it."
        }

        Invoke-ExternalCommand -FilePath git -Arguments @('-C', $SourceDir, 'submodule', 'update', '--init', '--recursive') -FailureMessage "Failed to update OBS submodules in $SourceDir"
        return
    }

    if (-not $CloneSource) {
        throw "No OBS source tree found at $SourceDir. Pass -CloneSource to fetch and build the pinned OBS tag, or populate it first and rerun with -BuildFromSource."
    }

    Invoke-ExternalCommand -FilePath git -Arguments @('clone', '--depth', '1', '--branch', $VersionTag, '--recurse-submodules', $SourceUrl, $SourceDir) -FailureMessage "Failed to clone OBS source tag $VersionTag"
    Invoke-ExternalCommand -FilePath git -Arguments @('-C', $SourceDir, 'submodule', 'update', '--init', '--recursive') -FailureMessage "Failed to initialize OBS submodules in $SourceDir"
}

function Invoke-ObsBuildAndInstall {
    param(
        [Parameter(Mandatory = $true)]
        [string]$SourceDir,
        [Parameter(Mandatory = $true)]
        [string]$BuildDir,
        [Parameter(Mandatory = $true)]
        [string]$InstallDir,
        [Parameter(Mandatory = $true)]
        [string]$Generator,
        [Parameter(Mandatory = $true)]
        [string]$Architecture,
        [Parameter(Mandatory = $true)]
        [string]$Configuration
    )

    $cmakePath = Resolve-CmakePath

    $configureArguments = @(
        '-S', $SourceDir,
        '-B', $BuildDir,
        '-G', $Generator,
        "-DCMAKE_INSTALL_PREFIX=$InstallDir"
    )

    if (-not [string]::IsNullOrWhiteSpace($Architecture)) {
        $configureArguments += '-A'
        $configureArguments += $Architecture
    }

    Invoke-ExternalCommand -FilePath $cmakePath -Arguments $configureArguments -FailureMessage "Failed to configure OBS from source at $SourceDir"
    Invoke-ExternalCommand -FilePath $cmakePath -Arguments @('--build', $BuildDir, '--config', $Configuration, '--target', 'install') -FailureMessage "Failed to build and install OBS from $BuildDir"
}

function Write-ObsBootstrapManifest {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,
        [Parameter(Mandatory = $true)]
        [string]$SourceUrl,
        [Parameter(Mandatory = $true)]
        [string]$VersionTag,
        [Parameter(Mandatory = $true)]
        [string]$SourceDir,
        [Parameter(Mandatory = $true)]
        [string]$BuildDir,
        [Parameter(Mandatory = $true)]
        [string]$InstallDir,
        [Parameter(Mandatory = $true)]
        [string]$Generator,
        [Parameter(Mandatory = $true)]
        [string]$Architecture,
        [Parameter(Mandatory = $true)]
        [string]$Configuration
    )

    $manifestDirectory = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $manifestDirectory)) {
        New-Item -ItemType Directory -Path $manifestDirectory -Force | Out-Null
    }

    $manifest = [pscustomobject]@{
        sourceUrl     = $SourceUrl
        tag           = $VersionTag
        sourceDir     = [System.IO.Path]::GetFullPath((Resolve-Path -LiteralPath $SourceDir).Path)
        buildDir      = [System.IO.Path]::GetFullPath((Resolve-Path -LiteralPath $BuildDir).Path)
        installDir    = [System.IO.Path]::GetFullPath((Resolve-Path -LiteralPath $InstallDir).Path)
        generator     = $Generator
        architecture  = $Architecture
        configuration = $Configuration
        timestamp     = (Get-Date).ToString('o')
    }

    $manifest | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $Path -Encoding utf8
}

$tag = Get-ObsVersionTag -Path $VersionFile
$buildFromSource = $CloneSource.IsPresent -or $BuildFromSource.IsPresent

if (-not $buildFromSource) {
    if ([string]::IsNullOrWhiteSpace($ObsRoot)) {
        throw 'An OBS developer tree is required. Provide -ObsRoot to validate one, or pass -CloneSource/-BuildFromSource to fetch and build one from source.'
    }

    if (-not (Test-Path -LiteralPath $ObsRoot)) {
        throw "OBS root does not exist: $ObsRoot"
    }

    if (-not (Test-ObsRuntimeLayout -Path $ObsRoot)) {
        throw "OBS root does not look like a runtime OBS tree with bin\\64bit, obs.dll, obs-plugins\\64bit, and data: $ObsRoot"
    }

    if (-not (Test-ObsDeveloperLayout -SourceDir $SourceDir -BuildDir $BuildDir -Configuration $Configuration)) {
        throw "Pinned OBS source/build trees do not provide the required developer files: source=$SourceDir build=$BuildDir"
    }

    Write-ObsRootConfig -Path $ObsRoot -ConfigPath $ConfigFile
    Write-Host "Validated staged OBS developer tree at $ObsRoot"
    Write-Host "Wrote OBS CMake config to $ConfigFile"
    return
}

if ([string]::IsNullOrWhiteSpace($InstallDir)) {
    $InstallDir = (Join-Path (Join-Path $BuildDir 'rundir') $Configuration)
}

Initialize-ObsSourceTree -SourceUrl $SourceUrl -SourceDir $SourceDir -VersionTag $tag -CloneSource:$CloneSource
Invoke-ObsBuildAndInstall -SourceDir $SourceDir -BuildDir $BuildDir -InstallDir $InstallDir -Generator $Generator -Architecture $Architecture -Configuration $Configuration

if (-not (Test-ObsRuntimeLayout -Path $InstallDir)) {
    throw "OBS build completed, but the staged tree is missing runtime files (bin\\64bit, obs.dll, obs-plugins\\64bit, or data): $InstallDir"
}

if (-not (Test-ObsDeveloperLayout -SourceDir $SourceDir -BuildDir $BuildDir -Configuration $Configuration)) {
    throw "OBS build completed, but the pinned OBS source/build trees are missing developer files: source=$SourceDir build=$BuildDir"
}

Write-ObsRootConfig -Path $InstallDir -ConfigPath $ConfigFile

$manifestPath = Join-Path $PSScriptRoot '..\deps\obs\obs-source.manifest.json'
Write-ObsBootstrapManifest -Path $manifestPath -SourceUrl $SourceUrl -VersionTag $tag -SourceDir $SourceDir -BuildDir $BuildDir -InstallDir $InstallDir -Generator $Generator -Architecture $Architecture -Configuration $Configuration

Write-Host "Built and staged OBS developer tree to $InstallDir"
Write-Host "Wrote OBS CMake config to $ConfigFile"