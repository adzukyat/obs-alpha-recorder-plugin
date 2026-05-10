# :movie_camera: Alpha Recorder for OBS

Alpha Recorder is an OBS plugin that records a separate grayscale alpha-mask
movie next to your normal OBS recording.

Use OBS the way you already do:

1. Open OBS.
2. Start Recording.
3. Stop Recording.
4. Find an extra `.alpha.mov` or `.alpha.mp4` file beside your video.

> [!IMPORTANT]
> Alpha Recorder does not replace your normal OBS recording. The RGB recording
> stays the primary output, and the alpha mask is written as a sidecar file.

## :sparkles: What You Get

- :white_check_mark: Normal OBS Start Recording / Stop Recording workflow.
- :white_check_mark: A separate grayscale alpha-mask movie for compositing.
- :white_check_mark: No need to switch OBS's main color format to RGBA.
- :white_check_mark: Pause, unpause, stop, and split-recording awareness.
- :white_check_mark: Hardware HEVC mask output when NVENC or AMF is available.
- :white_check_mark: Lossless PNG MOV fallback when hardware HEVC is not usable.

## :file_folder: Output Files

If OBS records this:

```text
~/Recordings/MyRec.mkv
```

Alpha Recorder writes one of these beside it:

| Selected format | Alpha output | Good for |
| --- | --- | --- |
| `mask_png_mov` | `~/Recordings/MyRec.alpha.mov` | Lossless grayscale masks |
| `mask_hevc_nvenc` | `~/Recordings/MyRec.alpha.mp4` | NVIDIA HEVC sidecar masks |
| `mask_hevc_amf` | `~/Recordings/MyRec.alpha.mp4` | AMD HEVC sidecar masks |

> [!NOTE]
> The alpha movie is a visible grayscale video, not a video file with its own
> transparency channel. White pixels represent high alpha, black pixels
> represent low alpha.

## :rocket: Quick Start

1. Download the package for your platform from
   [Releases](https://github.com/adzukyat/obs-alpha-recorder-plugin/releases).
2. Extract the package.
3. Close OBS if it is running.
4. Copy the plugin from the extracted package into your OBS plugin directory:

| Platform | Copy from the extracted package | Copy to |
| --- | --- | --- |
| Windows | `alpha-recorder/obs-plugins/64bit/alpha_recorder.dll` | `C:\Program Files\obs-studio\obs-plugins\64bit\alpha_recorder.dll` |
| macOS | `alpha-recorder/obs-plugins/alpha_recorder.plugin` | `/Applications/OBS.app/Contents/PlugIns/alpha_recorder.plugin` |
| Linux | `alpha-recorder/lib/obs-plugins/libalpha_recorder.so` | `/usr/lib/x86_64-linux-gnu/obs-plugins/libalpha_recorder.so` or `/usr/lib/obs-plugins/libalpha_recorder.so` |

If OBS is installed somewhere else, copy the plugin into the matching plugin
directory inside that OBS installation. On macOS, right-click `OBS.app`, choose
`Show Package Contents`, then open `Contents/PlugIns`.

5. Launch OBS.
6. Open `Tools > Alpha Recorder Settings`.
7. Make sure Alpha Recorder is enabled.
8. Choose a finalization format, or leave the default.
9. Record normally in OBS.

## :gear: Settings

The settings dialog lives at `Tools > Alpha Recorder Settings`.

<img width="540" height="581" src="https://github.com/user-attachments/assets/ecfaecf7-79db-4c6d-8e28-62fc2aa7f350" />

HEVC controls appear only when the matching encoder can actually open on the
current machine.

## :hammer_and_wrench: Build From Source

Alpha Recorder builds against a real OBS developer tree. The repo includes OBS
as a submodule and CMake helpers for bootstrapping the pinned OBS runtime.

### 1. Initialize OBS

```sh
git submodule update --init --recursive
```

### 2. Bootstrap OBS

```sh
cmake -DREPO_ROOT="$PWD" -DBUILD_FROM_SOURCE=ON -P cmake/scripts/BootstrapObs.cmake
```

### 3. Configure and build

Windows:

```powershell
cmake --preset windows-x64-msvc
cmake --build --preset windows-x64-msvc-relwithdebinfo
```

macOS:

```sh
cmake --preset macos-arm64
cmake --build --preset macos-arm64-relwithdebinfo
```

Linux or WSL Ubuntu:

```sh
cmake --preset linux-x64
cmake --build --preset linux-x64-relwithdebinfo
```

### 4. Stage a runnable OBS tree

```sh
cmake --build --preset macos-arm64-relwithdebinfo --target alpha_recorder_stage_obs_tree
```

Use the matching preset for your platform. The staged runtime is written under
`out/stage/obs`.

> [!TIP]
> Linux and WSL need OBS build dependencies, FFmpeg tooling, and Bun for the
> app-level E2E harness. The complete validation notes live in
> [`SPECS.md`](SPECS.md).

## :test_tube: Test

Run focused unit tests:

```sh
ctest --test-dir out/build/macos-arm64 -C RelWithDebInfo -L unit --output-on-failure
```

Run deterministic E2E tests:

```sh
cmake --build --preset macos-arm64-relwithdebinfo --target alpha_recorder_run_e2e
```

Run the real OBS app E2E path:

```sh
cmake --build --preset macos-arm64-relwithdebinfo --target alpha_recorder_run_obs_app_e2e
```

Platform presets are available for Windows, macOS, and Linux:

- `windows-x64-msvc-relwithdebinfo`
- `macos-arm64-relwithdebinfo`
- `linux-x64-relwithdebinfo`

The OBS app E2E target launches an isolated OBS runtime, enables Alpha Recorder
through obs-websocket, records real RGB and alpha outputs, and verifies decoded
sync. Runtime-specific targets that cannot run on the current machine are
skipped with a clear reason.

> [!WARNING]
> AMF targets have not been tested since I do not own a Radeon GPU.
> Feel free to submit patches if you find any bugs.

## :bulb: Troubleshooting

| Symptom | Likely reason |
| --- | --- |
| HEVC option is missing | The matching NVENC or AMF encoder cannot open on this machine |
| No alpha file appears | Alpha Recorder may be disabled, or the alpha session failed to start |
