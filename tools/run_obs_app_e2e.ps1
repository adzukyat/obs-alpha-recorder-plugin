param(
    [string]$BuildDir = (Join-Path $PSScriptRoot "..\out\build\windows-x64-msvc"),
    [string]$StageDir = (Join-Path $PSScriptRoot "..\out\stage\obs-app-e2e"),
    [string]$ObsRoot = $env:OBS_ROOT,
    [string]$Configuration = 'RelWithDebInfo',
    [int]$Port = 0,
    [int]$RecordSeconds = 5,
    [int]$Width = 1280,
    [int]$Height = 720,
    [switch]$SkipBuild,
    [switch]$SkipStage,
    [switch]$KeepObsOpen
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Resolve-RepoPath {
    param([string]$Path, [string]$BasePath)
    if ([System.IO.Path]::IsPathRooted($Path)) {
        return [System.IO.Path]::GetFullPath($Path)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $BasePath $Path))
}

function New-FreeTcpPort {
    $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Parse('127.0.0.1'), 0)
    $listener.Start()
    $selected = ([System.Net.IPEndPoint]$listener.LocalEndpoint).Port
    $listener.Stop()
    return $selected
}

function Write-Utf8NoBom {
    param([string]$Path, [string]$Text)
    $directory = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $directory)) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }
    [System.IO.File]::WriteAllText($Path, $Text, [System.Text.UTF8Encoding]::new($false))
}

function ConvertTo-Base64Sha256 {
    param([string]$Text)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($Text)
        return [Convert]::ToBase64String($sha.ComputeHash($bytes))
    }
    finally {
        $sha.Dispose()
    }
}

function Get-ObsAuthentication {
    param([string]$Password, [string]$Salt, [string]$Challenge)
    $secret = ConvertTo-Base64Sha256 -Text ($Password + $Salt)
    return ConvertTo-Base64Sha256 -Text ($secret + $Challenge)
}

function Send-WebSocketJson {
    param(
        [System.Net.WebSockets.ClientWebSocket]$Socket,
        [object]$Message
    )
    $json = $Message | ConvertTo-Json -Depth 32 -Compress
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($json)
    $segment = [ArraySegment[byte]]::new($bytes)
    [void]$Socket.SendAsync($segment, [System.Net.WebSockets.WebSocketMessageType]::Text, $true, [Threading.CancellationToken]::None).GetAwaiter().GetResult()
}

function Receive-WebSocketJson {
    param([System.Net.WebSockets.ClientWebSocket]$Socket)
    $buffer = New-Object byte[] 65536
    $stream = [System.IO.MemoryStream]::new()
    try {
        do {
            $segment = [ArraySegment[byte]]::new($buffer)
            $result = $Socket.ReceiveAsync($segment, [Threading.CancellationToken]::None).GetAwaiter().GetResult()
            if ($result.MessageType -eq [System.Net.WebSockets.WebSocketMessageType]::Close) {
                throw "obs-websocket closed the connection."
            }
            $stream.Write($buffer, 0, $result.Count)
        } while (-not $result.EndOfMessage)

        $json = [System.Text.Encoding]::UTF8.GetString($stream.ToArray())
        return $json | ConvertFrom-Json
    }
    finally {
        $stream.Dispose()
    }
}

function Connect-ObsWebSocket {
    param([int]$Port, [string]$Password)
    $socket = [System.Net.WebSockets.ClientWebSocket]::new()
    $deadline = [DateTime]::UtcNow.AddSeconds(45)
    while ($true) {
        try {
            [void]$socket.ConnectAsync([Uri]"ws://127.0.0.1:$Port", [Threading.CancellationToken]::None).GetAwaiter().GetResult()
            break
        }
        catch {
            $socket.Dispose()
            if ([DateTime]::UtcNow -gt $deadline) {
                throw "Timed out connecting to obs-websocket on port $Port. Last error: $($_.Exception.Message)"
            }
            Start-Sleep -Milliseconds 500
            $socket = [System.Net.WebSockets.ClientWebSocket]::new()
        }
    }

    $hello = Receive-WebSocketJson -Socket $socket
    if ($hello.op -ne 0) {
        throw "Expected obs-websocket Hello, got op=$($hello.op)."
    }

    $identify = @{ rpcVersion = 1; eventSubscriptions = 0 }
    if ($null -ne $hello.d.authentication) {
        $identify.authentication = Get-ObsAuthentication -Password $Password -Salt $hello.d.authentication.salt -Challenge $hello.d.authentication.challenge
    }

    Send-WebSocketJson -Socket $socket -Message @{ op = 1; d = $identify }
    $identified = Receive-WebSocketJson -Socket $socket
    if ($identified.op -ne 2) {
        throw "Failed to identify with obs-websocket. Response: $($identified | ConvertTo-Json -Depth 16 -Compress)"
    }

    return $socket
}

$script:RequestCounter = 0
function Invoke-ObsRequest {
    param(
        [System.Net.WebSockets.ClientWebSocket]$Socket,
        [string]$RequestType,
        [hashtable]$RequestData = @{}
    )
    $script:RequestCounter += 1
    $requestId = "alpha-recorder-e2e-$script:RequestCounter"
    Send-WebSocketJson -Socket $Socket -Message @{
        op = 6
        d = @{
            requestType = $RequestType
            requestId = $requestId
            requestData = $RequestData
        }
    }

    while ($true) {
        $message = Receive-WebSocketJson -Socket $Socket
        if ($message.op -ne 7 -or $message.d.requestId -ne $requestId) {
            continue
        }

        if (-not $message.d.requestStatus.result) {
            throw "OBS request $RequestType failed: $($message.d.requestStatus | ConvertTo-Json -Depth 16 -Compress)"
        }

        if ($message.d.PSObject.Properties.Name -contains 'responseData') {
            return $message.d.responseData
        }

        return $null
    }
}

function Wait-ForRecordState {
    param([System.Net.WebSockets.ClientWebSocket]$Socket, [bool]$Active, [int]$TimeoutSeconds = 30)
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        $status = Invoke-ObsRequest -Socket $Socket -RequestType 'GetRecordStatus'
        if ([bool]$status.outputActive -eq $Active) {
            return
        }
        Start-Sleep -Milliseconds 500
    } while ([DateTime]::UtcNow -lt $deadline)

    throw "Timed out waiting for recording active=$Active."
}

function Wait-ForPath {
    param([string]$Path, [int]$TimeoutSeconds = 60)
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        if ((Test-Path -LiteralPath $Path) -and ((Get-Item -LiteralPath $Path).Length -gt 0)) {
            return
        }
        Start-Sleep -Milliseconds 500
    } while ([DateTime]::UtcNow -lt $deadline)

    throw "Timed out waiting for file: $Path"
}

function Invoke-CheckedProcess {
    param([string]$FilePath, [string[]]$Arguments)
    $oldErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = & $FilePath @Arguments 2>&1
    }
    finally {
        $ErrorActionPreference = $oldErrorActionPreference
    }

    if ($LASTEXITCODE -ne 0) {
        throw "Command failed ($LASTEXITCODE): $FilePath $($Arguments -join ' ')`n$output"
    }
    return ($output -join "`n")
}

function Invoke-CheckedProcessWithRetry {
    param(
        [string]$FilePath,
        [string[]]$Arguments,
        [int]$TimeoutSeconds = 120
    )
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    $lastError = $null
    do {
        try {
            return Invoke-CheckedProcess -FilePath $FilePath -Arguments $Arguments
        }
        catch {
            $lastError = $_.Exception.Message
            Start-Sleep -Seconds 1
        }
    } while ([DateTime]::UtcNow -lt $deadline)

    throw $lastError
}

function Resolve-Tool {
    param([string]$Name, [string]$StageBinPath)
    $candidate = Join-Path $StageBinPath $Name
    if (Test-Path -LiteralPath $candidate) {
        return (Get-Item -LiteralPath $candidate).FullName
    }
    $command = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        return $command.Path
    }

    $scoopRoot = Join-Path $env:USERPROFILE 'scoop\apps'
    if (Test-Path -LiteralPath $scoopRoot) {
        $scoopMatch = Get-ChildItem -LiteralPath $scoopRoot -Recurse -Filter $Name -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($null -ne $scoopMatch) {
            return $scoopMatch.FullName
        }
    }

    throw "Unable to find $Name. Install FFmpeg or add it to PATH."
}

function Resolve-Cmake {
    $command = Get-Command cmake -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        return $command.Path
    }

    foreach ($root in @(
            [Environment]::GetFolderPath('ProgramFiles'),
            [Environment]::GetFolderPath('ProgramFilesX86')
        )) {
        if ([string]::IsNullOrWhiteSpace($root)) {
            continue
        }

        $direct = Join-Path $root 'CMake\bin\cmake.exe'
        if (Test-Path -LiteralPath $direct) {
            return (Get-Item -LiteralPath $direct).FullName
        }

        $vsMatches = Get-ChildItem -Path (Join-Path $root 'Microsoft Visual Studio\2022') -Recurse -Filter cmake.exe -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -like '*\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' } |
            Select-Object -First 1
        if ($null -ne $vsMatches) {
            return $vsMatches.FullName
        }
    }

    throw "Unable to find cmake.exe."
}

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$BuildDir = Resolve-RepoPath -Path $BuildDir -BasePath $repoRoot
$StageDir = Resolve-RepoPath -Path $StageDir -BasePath $repoRoot
if ($Port -le 0) {
    $Port = New-FreeTcpPort
}

if (-not $SkipBuild) {
    $cmake = Resolve-Cmake
    & $cmake --build $BuildDir --config $Configuration --target alpha_recorder_plugin alpha_recorder_frontend
    if ($LASTEXITCODE -ne 0) {
        throw "Plugin build failed."
    }
}

if (-not $SkipStage) {
    & (Join-Path $PSScriptRoot 'stage_obs_tree.ps1') -BuildDir $BuildDir -StageDir $StageDir -ObsRoot $ObsRoot -Configuration $Configuration
}

$stageBin = Join-Path $StageDir 'bin\64bit'
$obsExe = Join-Path $stageBin 'obs64.exe'
if (-not (Test-Path -LiteralPath $obsExe)) {
    throw "Staged OBS executable is missing: $obsExe"
}

$password = [Guid]::NewGuid().ToString('N')
$portableConfig = Join-Path $StageDir 'config\obs-studio'
$profileName = 'AlphaRecorderE2E'
$collectionName = 'AlphaRecorderE2E'
$artifactRoot = Join-Path $repoRoot ('out\e2e\obs-app\' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Path $artifactRoot -Force | Out-Null

Write-Utf8NoBom -Path (Join-Path $portableConfig 'global.ini') -Text @"
[General]
FirstRun=false
Pre31Migrated=true
MaxLogs=10
ProcessPriority=Normal

[Basic]
Profile=$profileName
ProfileDir=$profileName
SceneCollection=$collectionName
SceneCollectionFile=$collectionName
"@

Write-Utf8NoBom -Path (Join-Path $portableConfig "basic\profiles\$profileName\basic.ini") -Text @"
[General]
Name=$profileName

[Output]
Mode=Advanced
FilenameFormatting=%CCYY-%MM-%DD %hh-%mm-%ss

[AdvOut]
RecType=Standard
RecFilePath=$($artifactRoot.Replace('\', '/'))
RecFormat2=mkv
RecTracks=1
RecEncoder=obs_x264
Encoder=obs_x264
ApplyServiceSettings=true
RecUseRescale=false
TrackIndex=1
RecSplitFileType=Time

[Video]
BaseCX=$Width
BaseCY=$Height
OutputCX=$Width
OutputCY=$Height
FPSType=0
FPSCommon=30
FPSInt=30
FPSNum=30
FPSDen=1
ScaleType=bicubic
ColorFormat=BGRA
ColorSpace=709
ColorRange=Partial

[Audio]
SampleRate=48000
ChannelSetup=Stereo

[AlphaRecorder]
enabled=false
finalization_format=prores_4444
"@

Write-Utf8NoBom -Path (Join-Path $portableConfig "basic\scenes\$collectionName.json") -Text @"
{
  "name": "$collectionName",
  "sources": [
    {
      "name": "Scene",
      "id": "scene",
      "versioned_id": "scene",
      "settings": { "id_counter": 1, "custom_size": false, "items": [] },
      "mixers": 0,
      "sync": 0,
      "flags": 0,
      "volume": 1.0,
      "balance": 0.5,
      "enabled": true,
      "muted": false,
      "hotkeys": {},
      "private_settings": {}
    }
  ],
  "current_scene": "Scene",
  "current_program_scene": "Scene",
  "groups": [],
  "quick_transitions": [],
  "transitions": [],
  "saved_projectors": [],
  "preview_locked": false,
  "scaling_enabled": false,
  "scaling_level": 0,
  "scaling_off_x": 0.0,
  "scaling_off_y": 0.0,
  "virtual-camera": { "type2": 3 }
}
"@

Write-Utf8NoBom -Path (Join-Path $portableConfig 'plugin_config\obs-websocket\config.json') -Text (@{
    first_load = $false
    server_enabled = $true
    server_port = $Port
    alerts_enabled = $false
    auth_required = $true
    server_password = $password
} | ConvertTo-Json -Depth 4)

$obsProcess = $null
$socket = $null
try {
    $obsArgs = @(
        '--portable',
        '--multi',
        '--profile', $profileName,
        '--collection', $collectionName,
        '--websocket_port', [string]$Port,
        '--websocket_password', $password,
        '--websocket_ipv4_only'
    )

    $obsProcess = Start-Process -FilePath $obsExe -ArgumentList $obsArgs -WorkingDirectory $stageBin -PassThru
    $socket = Connect-ObsWebSocket -Port $Port -Password $password

    Invoke-ObsRequest -Socket $socket -RequestType 'CallVendorRequest' -RequestData @{
        vendorName = 'alpha_recorder'
        requestType = 'SetSettings'
        requestData = @{
            enabled = $true
            finalization_format = 'prores_4444'
        }
    } | Out-Null

    $settings = Invoke-ObsRequest -Socket $socket -RequestType 'CallVendorRequest' -RequestData @{
        vendorName = 'alpha_recorder'
        requestType = 'GetSettings'
        requestData = @{}
    }

    if (-not $settings.responseData.enabled) {
        throw "Alpha Recorder did not report enabled=true through the vendor API."
    }

    Invoke-ObsRequest -Socket $socket -RequestType 'SetRecordDirectory' -RequestData @{ recordDirectory = $artifactRoot } | Out-Null
    Invoke-ObsRequest -Socket $socket -RequestType 'StartRecord' | Out-Null
    Wait-ForRecordState -Socket $socket -Active $true
    Start-Sleep -Seconds $RecordSeconds
    $stopResponse = Invoke-ObsRequest -Socket $socket -RequestType 'StopRecord'
    Wait-ForRecordState -Socket $socket -Active $false -TimeoutSeconds 240

    $rgbPath = [string]$stopResponse.outputPath
    if ([string]::IsNullOrWhiteSpace($rgbPath)) {
        $rgbPath = (Get-ChildItem -LiteralPath $artifactRoot -Filter '*.mkv' | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
    }

    if ([string]::IsNullOrWhiteSpace($rgbPath)) {
        throw "OBS did not report or create an RGB recording path."
    }

    $basePath = [System.IO.Path]::Combine([System.IO.Path]::GetDirectoryName($rgbPath), [System.IO.Path]::GetFileNameWithoutExtension($rgbPath))
    $sidecarPath = "$basePath.alpha.sidecar"
    $manifestPath = "$basePath.alpha.manifest.json"
    $alphaPath = "$basePath.alpha.mov"

    Wait-ForPath -Path $rgbPath -TimeoutSeconds 30
    Wait-ForPath -Path $sidecarPath -TimeoutSeconds 60
    Wait-ForPath -Path $manifestPath -TimeoutSeconds 60
    Wait-ForPath -Path $alphaPath -TimeoutSeconds 120

    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ([int64]$manifest.pair_count -le 0 -or [int64]$manifest.record_count -ne [int64]$manifest.pair_count) {
        throw "Manifest counts are invalid: $($manifest | ConvertTo-Json -Depth 8 -Compress)"
    }

    $sidecarSize = (Get-Item -LiteralPath $sidecarPath).Length
    if ([int64]$manifest.sidecar_size_bytes -ne [int64]$sidecarSize) {
        throw "Manifest sidecar_size_bytes does not match the sidecar file size."
    }

    $ffprobe = Resolve-Tool -Name 'ffprobe.exe' -StageBinPath $stageBin
    $ffmpeg = Resolve-Tool -Name 'ffmpeg.exe' -StageBinPath $stageBin

    $rgbProbe = Invoke-CheckedProcessWithRetry -FilePath $ffprobe -Arguments @('-v', 'error', '-select_streams', 'v:0', '-show_entries', 'stream=codec_name,width,height,pix_fmt,nb_frames,duration', '-show_entries', 'format=duration,size', '-of', 'json', $rgbPath) -TimeoutSeconds 30
    $alphaProbe = Invoke-CheckedProcessWithRetry -FilePath $ffprobe -Arguments @('-v', 'error', '-select_streams', 'v:0', '-show_entries', 'stream=codec_name,width,height,pix_fmt,nb_frames,duration', '-show_entries', 'format=duration,size', '-of', 'json', $alphaPath) -TimeoutSeconds 180
    Invoke-CheckedProcessWithRetry -FilePath $ffmpeg -Arguments @('-v', 'error', '-i', $rgbPath, '-frames:v', '1', '-f', 'null', '-') -TimeoutSeconds 30 | Out-Null
    Invoke-CheckedProcessWithRetry -FilePath $ffmpeg -Arguments @('-v', 'error', '-i', $alphaPath, '-frames:v', '1', '-f', 'null', '-') -TimeoutSeconds 180 | Out-Null

    $alphaProbeJson = $alphaProbe | ConvertFrom-Json
    if ($alphaProbeJson.streams.Count -lt 1 -or $alphaProbeJson.streams[0].codec_name -ne 'prores' -or $alphaProbeJson.streams[0].pix_fmt -ne 'yuva444p10le') {
        throw "Alpha movie probe did not report ProRes yuva444p10le: $alphaProbe"
    }

    [pscustomobject]@{
        ok = $true
        artifactRoot = $artifactRoot
        rgbPath = $rgbPath
        alphaPath = $alphaPath
        sidecarPath = $sidecarPath
        manifestPath = $manifestPath
        pairCount = $manifest.pair_count
        rgbProbe = ($rgbProbe | ConvertFrom-Json)
        alphaProbe = $alphaProbeJson
    } | ConvertTo-Json -Depth 12
}
finally {
    if ($null -ne $socket) {
        $socket.Dispose()
    }

    if ($null -ne $obsProcess -and -not $obsProcess.HasExited -and -not $KeepObsOpen) {
        try {
            $obsProcess.CloseMainWindow() | Out-Null
            if (-not $obsProcess.WaitForExit(15000)) {
                Stop-Process -Id $obsProcess.Id -Force
            }
        }
        catch {
            Stop-Process -Id $obsProcess.Id -Force -ErrorAction SilentlyContinue
        }
    }
}
