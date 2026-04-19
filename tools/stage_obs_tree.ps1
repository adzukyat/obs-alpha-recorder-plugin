param(
    [string]$ObsRoot = $env:OBS_ROOT,
    [string]$BuildDir = (Join-Path $PSScriptRoot "..\out\build\windows-x64-msvc"),
    [string]$StageDir = (Join-Path $PSScriptRoot "..\out\stage\obs"),
    [string]$Configuration = 'Debug',
    [string]$PluginName = 'alpha_recorder'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

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

if ([string]::IsNullOrWhiteSpace($ObsRoot)) {
    $ObsRoot = Get-ConfiguredObsRoot -ConfigPath $configPath
}

New-Item -ItemType Directory -Path $StageDir -Force | Out-Null

$pluginTargetDir = Join-Path $StageDir 'obs-plugins\64bit'
$obsBinTargetDir = Join-Path $StageDir 'bin\64bit'
$obsDataTargetDir = Join-Path $StageDir 'data'

New-Item -ItemType Directory -Path $pluginTargetDir, $obsBinTargetDir, $obsDataTargetDir -Force | Out-Null

$pluginPath = Resolve-PluginPath -BuildDir $BuildDir -Configuration $Configuration -PluginName $PluginName
if ($null -ne $pluginPath) {
    Copy-Item -LiteralPath $pluginPath -Destination $pluginTargetDir -Force
}

if (-not [string]::IsNullOrWhiteSpace($ObsRoot) -and (Test-Path -LiteralPath $ObsRoot)) {
    $obsBinSource = Join-Path $ObsRoot 'bin\64bit'
    $obsDataSource = Join-Path $ObsRoot 'data'

    if (Test-Path -LiteralPath $obsBinSource) {
        Copy-Item -Path (Join-Path $obsBinSource '*') -Destination $obsBinTargetDir -Recurse -Force
    }

    if (Test-Path -LiteralPath $obsDataSource) {
        Copy-Item -Path (Join-Path $obsDataSource '*') -Destination $obsDataTargetDir -Recurse -Force
    }
}

$manifest = [pscustomobject]@{
    obsRoot       = $ObsRoot
    buildDir      = $BuildDir
    stageDir      = $StageDir
    configuration = $Configuration
    pluginPath    = $pluginPath
    timestamp     = (Get-Date).ToString('o')
}

$manifestPath = Join-Path $StageDir 'stage.manifest.json'
$manifest | ConvertTo-Json | Set-Content -LiteralPath $manifestPath -Encoding utf8

Write-Host "Staged OBS tree at $StageDir"