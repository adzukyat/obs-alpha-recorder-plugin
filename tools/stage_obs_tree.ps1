param(
    [string]$ObsRoot = $env:OBS_ROOT,
    [string]$BuildDir = (Join-Path $PSScriptRoot "..\out\build\windows-x64-msvc"),
    [string]$StageDir = (Join-Path $PSScriptRoot "..\out\stage\obs"),
    [string]$Configuration = 'RelWithDebInfo',
    [string]$PluginName = 'alpha_recorder'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Resolve-RepoPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [string]$BasePath
    )

    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }

    return [System.IO.Path]::GetFullPath((Join-Path $BasePath $Path))
}

function Resolve-PluginPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BuildDir,

        [Parameter(Mandatory = $true)]
        [string]$Configuration,

        [Parameter(Mandatory = $true)]
        [string]$PluginName
    )

    $candidateRoots = @(
        (Join-Path (Join-Path $BuildDir 'bin') $Configuration),
        (Join-Path $BuildDir $Configuration),
        (Join-Path $BuildDir 'bin'),
        $BuildDir
    )

    foreach ($candidateRoot in $candidateRoots) {
        if ([string]::IsNullOrWhiteSpace($candidateRoot) -or -not (Test-Path -LiteralPath $candidateRoot)) {
            continue
        }

        $candidatePath = Join-Path $candidateRoot "$PluginName.dll"
        if (Test-Path -LiteralPath $candidatePath) {
            return (Get-Item -LiteralPath $candidatePath).FullName
        }
    }

    return $null
}

function Get-ConfiguredObsRoot {
    param(
        [Parameter(Mandatory = $true)]
        [string]$ConfigPath
    )

    if (-not (Test-Path -LiteralPath $ConfigPath)) {
        return $null
    }

    $content = Get-Content -LiteralPath $ConfigPath -Raw
    $match = [regex]::Match($content, 'set\(\s*OBS_ROOT\s+"([^"]+)"')
    if ($match.Success) {
        return $match.Groups[1].Value
    }

    return $null
}

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$configPath = Join-Path $repoRoot 'deps\obs\obs-root.cmake'

$BuildDir = Resolve-RepoPath -Path $BuildDir -BasePath $repoRoot
$StageDir = Resolve-RepoPath -Path $StageDir -BasePath $repoRoot

if ([string]::IsNullOrWhiteSpace($ObsRoot)) {
    $ObsRoot = Get-ConfiguredObsRoot -ConfigPath $configPath
}

if ([string]::IsNullOrWhiteSpace($ObsRoot)) {
    throw 'An OBS developer tree is required. Set OBS_ROOT or write deps/obs/obs-root.cmake before staging.'
}

$ObsRoot = Resolve-RepoPath -Path $ObsRoot -BasePath $repoRoot

if (-not (Test-Path -LiteralPath $ObsRoot)) {
    throw "OBS root does not exist: $ObsRoot"
}

New-Item -ItemType Directory -Path $StageDir -Force | Out-Null

$pluginTargetDir = Join-Path $StageDir 'obs-plugins\64bit'
$obsBinTargetDir = Join-Path $StageDir 'bin\64bit'
$obsDataTargetDir = Join-Path $StageDir 'data'

New-Item -ItemType Directory -Path $pluginTargetDir, $obsBinTargetDir, $obsDataTargetDir -Force | Out-Null

$pluginNames = @($PluginName, 'alpha_recorder_frontend', 'alpha_recorder_e2e')
$mainPluginPath = $null
$frontendPluginPath = $null
$e2ePluginPath = $null
foreach ($currentPluginName in $pluginNames) {
    $currentPluginPath = Resolve-PluginPath -BuildDir $BuildDir -Configuration $Configuration -PluginName $currentPluginName
    if ($null -eq $currentPluginPath) {
        if ($currentPluginName -eq $PluginName) {
            throw "Failed to locate $PluginName.dll in the build tree: $BuildDir"
        }

        continue
    }

    Copy-Item -LiteralPath $currentPluginPath -Destination $pluginTargetDir -Force

    if ($currentPluginName -eq $PluginName) {
        $mainPluginPath = $currentPluginPath
    }
    elseif ($currentPluginName -eq 'alpha_recorder_e2e') {
        $e2ePluginPath = $currentPluginPath
    }
    else {
        $frontendPluginPath = $currentPluginPath
    }
}

$obsBinSource = Join-Path $ObsRoot 'bin\64bit'
$obsDataSource = Join-Path $ObsRoot 'data'

if (-not (Test-Path -LiteralPath $obsBinSource)) {
    throw "OBS root is missing bin\\64bit: $ObsRoot"
}

if (-not (Test-Path -LiteralPath $obsDataSource)) {
    throw "OBS root is missing data: $ObsRoot"
}

Copy-Item -Path (Join-Path $obsBinSource '*') -Destination $obsBinTargetDir -Recurse -Force
Copy-Item -Path (Join-Path $obsDataSource '*') -Destination $obsDataTargetDir -Recurse -Force

$manifest = [pscustomobject]@{
    obsRoot            = $ObsRoot
    buildDir           = $BuildDir
    stageDir           = $StageDir
    configuration      = $Configuration
    pluginPath         = $mainPluginPath
    frontendPluginPath = $frontendPluginPath
    e2ePluginPath      = $e2ePluginPath
    timestamp          = (Get-Date).ToString('o')
}

$manifestPath = Join-Path $StageDir 'stage.manifest.json'
$manifest | ConvertTo-Json | Set-Content -LiteralPath $manifestPath -Encoding utf8

Write-Host "Staged OBS tree at $StageDir"