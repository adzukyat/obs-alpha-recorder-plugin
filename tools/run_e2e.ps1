param(
    [string]$BuildDir = (Join-Path $PSScriptRoot "..\out\build\windows-x64-msvc"),
    [string]$StageDir = (Join-Path $PSScriptRoot "..\out\stage\obs"),
    [string]$ObsRoot = $env:OBS_ROOT,
    [string]$Configuration = 'Debug',
    [switch]$SkipStage
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

function Resolve-CtestPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$BuildDir
    )

    $cachePath = Join-Path $BuildDir 'CMakeCache.txt'
    if (Test-Path -LiteralPath $cachePath) {
        $cacheContent = Get-Content -LiteralPath $cachePath -Raw
        $cacheMatch = [regex]::Match($cacheContent, '(?m)^CMAKE_COMMAND:INTERNAL=(.+)$')
        if ($cacheMatch.Success) {
            $cmakePath = $cacheMatch.Groups[1].Value.Trim()
            if (Test-Path -LiteralPath $cmakePath) {
                $ctestPath = Join-Path (Split-Path -Parent $cmakePath) 'ctest.exe'
                if (Test-Path -LiteralPath $ctestPath) {
                    return (Get-Item -LiteralPath $ctestPath).FullName
                }
            }
        }
    }

    $ctestCommand = Get-Command ctest -ErrorAction SilentlyContinue
    if ($null -ne $ctestCommand) {
        return $ctestCommand.Path
    }

    $cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
    if ($null -ne $cmakeCommand) {
        $ctestPath = Join-Path (Split-Path -Parent $cmakeCommand.Path) 'ctest.exe'
        if (Test-Path -LiteralPath $ctestPath) {
            return (Get-Item -LiteralPath $ctestPath).FullName
        }
    }

    foreach ($programFiles in @(
            [Environment]::GetFolderPath('ProgramFiles'),
            [Environment]::GetFolderPath('ProgramFilesX86')
        )) {
        if ([string]::IsNullOrWhiteSpace($programFiles)) {
            continue
        }

        $ctestPath = Join-Path $programFiles 'CMake\bin\ctest.exe'
        if (Test-Path -LiteralPath $ctestPath) {
            return (Get-Item -LiteralPath $ctestPath).FullName
        }
    }

    throw "Unable to locate ctest.exe. Expected it beside the CMake command recorded in $cachePath, alongside cmake.exe, or in a standard CMake install directory."
}

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$scenarioDir = Join-Path $repoRoot 'tests\e2e\scenarios'

$BuildDir = Resolve-RepoPath -Path $BuildDir -BasePath $repoRoot
$StageDir = Resolve-RepoPath -Path $StageDir -BasePath $repoRoot
if (-not [string]::IsNullOrWhiteSpace($ObsRoot)) {
    $ObsRoot = Resolve-RepoPath -Path $ObsRoot -BasePath $repoRoot
}

if (-not $SkipStage) {
    & (Join-Path $PSScriptRoot 'stage_obs_tree.ps1') `
        -BuildDir $BuildDir `
        -StageDir $StageDir `
        -ObsRoot $ObsRoot `
        -Configuration $Configuration
}

$stagePluginPath = Join-Path (Join-Path $StageDir 'obs-plugins\64bit') 'alpha_recorder_e2e.dll'
if (-not (Test-Path -LiteralPath $stagePluginPath)) {
    throw "Expected staged e2e plugin DLL is missing: $stagePluginPath"
}

$stageBinPath = Join-Path $StageDir 'bin\64bit'
if (-not (Test-Path -LiteralPath $stageBinPath)) {
    throw "Expected staged OBS runtime bin directory is missing: $stageBinPath"
}

$env:PATH = "$stageBinPath;$env:PATH"

$env:ALPHA_RECORDER_STAGE_DIR = $StageDir
$env:ALPHA_RECORDER_SCENARIO_DIR = $scenarioDir

$ctestArgs = @(
    '--test-dir', $BuildDir,
    '-C', $Configuration,
    '-L', 'e2e',
    '--output-on-failure'
)

$ctestPath = Resolve-CtestPath -BuildDir $BuildDir
& $ctestPath @ctestArgs