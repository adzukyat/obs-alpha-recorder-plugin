param(
    [string]$ObsRoot = $env:OBS_ROOT,
    [string]$VersionFile = (Join-Path $PSScriptRoot "..\deps\obs.version"),
    [string]$ConfigFile = (Join-Path $PSScriptRoot "..\deps\obs\obs-root.cmake"),
    [string]$SourceDir = (Join-Path $PSScriptRoot "..\deps\obs\obs-studio"),
    [switch]$CloneSource
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

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

function Test-ObsRootLayout {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $headerCandidates = @(
        (Join-Path $Path 'include\obs-module.h'),
        (Join-Path $Path 'libobs\include\obs-module.h'),
        (Join-Path $Path 'libobs\obs-module.h')
    )

    $libraryCandidates = @(
        (Join-Path $Path 'lib\libobs.lib'),
        (Join-Path $Path 'lib\obs.lib'),
        (Join-Path $Path 'build\libobs\Debug\libobs.lib'),
        (Join-Path $Path 'build\libobs\Release\libobs.lib')
    )

    $hasHeader = $false
    foreach ($candidate in $headerCandidates) {
        if (Test-Path -LiteralPath $candidate) {
            $hasHeader = $true
            break
        }
    }

    $hasLibrary = $false
    foreach ($candidate in $libraryCandidates) {
        if (Test-Path -LiteralPath $candidate) {
            $hasLibrary = $true
            break
        }
    }

    return $hasHeader -and $hasLibrary
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
set(OBS_ROOT "$cmakePath" CACHE PATH "Path to an OBS install or staged tree" FORCE)
"@

    Set-Content -LiteralPath $ConfigPath -Value $contents -Encoding utf8
}

$tag = Get-ObsVersionTag -Path $VersionFile

if ([string]::IsNullOrWhiteSpace($ObsRoot)) {
    Write-Host "No OBS_ROOT provided. Set OBS_ROOT or pass -ObsRoot to write the CMake config snippet."
}
else {
    if (-not (Test-Path -LiteralPath $ObsRoot)) {
        throw "OBS root does not exist: $ObsRoot"
    }

    if (-not (Test-ObsRootLayout -Path $ObsRoot)) {
        throw "OBS root does not look like an install or staged tree with libobs headers and libraries: $ObsRoot"
    }

    Write-ObsRootConfig -Path $ObsRoot -ConfigPath $ConfigFile
    Write-Host "Wrote OBS CMake config to $ConfigFile"
}

if ($CloneSource) {
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
        throw 'git is required to clone the OBS source tree'
    }

    if (-not (Test-Path -LiteralPath $SourceDir)) {
        $sourceUrl = 'https://github.com/obsproject/obs-studio.git'
        git clone --depth 1 --branch $tag $sourceUrl $SourceDir
    }

    $manifest = [pscustomobject]@{
        sourceUrl = 'https://github.com/obsproject/obs-studio.git'
        tag       = $tag
        sourceDir = $SourceDir
        timestamp = (Get-Date).ToString('o')
    }

    $manifestPath = Join-Path $PSScriptRoot '..\deps\obs\obs-source.manifest.json'
    $manifestDirectory = Split-Path -Parent $manifestPath
    if (-not (Test-Path -LiteralPath $manifestDirectory)) {
        New-Item -ItemType Directory -Path $manifestDirectory -Force | Out-Null
    }

    $manifest | ConvertTo-Json | Set-Content -LiteralPath $manifestPath -Encoding utf8
    Write-Host "Cloned OBS source tree metadata to $manifestPath"
}