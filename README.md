# Alpha Recorder for OBS :movie_camera:

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

## What You Get :sparkles:

- :white_check_mark: Normal OBS Start Recording / Stop Recording workflow.
- :white_check_mark: A separate grayscale alpha-mask movie for compositing.
- :white_check_mark: No need to switch OBS's main color format to RGBA.
- :white_check_mark: Pause, unpause, stop, and split-recording awareness.
- :white_check_mark: Hardware HEVC mask output when NVENC or AMF is available.
- :white_check_mark: Lossless PNG MOV fallback when hardware HEVC is not usable.

## Output Files :file_folder:

If OBS records this:

```text
C:\Recordings\MyRec.mkv
```

Alpha Recorder writes one of these beside it:

| Selected format | Alpha output | Good for |
| --- | --- | --- |
| `mask_png_mov` | `C:\Recordings\MyRec.alpha.mov` | Lossless grayscale masks |
| `mask_hevc_nvenc` | `C:\Recordings\MyRec.alpha.mp4` | NVIDIA HEVC sidecar masks |
| `mask_hevc_amf` | `C:\Recordings\MyRec.alpha.mp4` | AMD HEVC sidecar masks |

> [!NOTE]
> The alpha movie is a visible grayscale video, not a video file with its own
> transparency channel. White pixels represent high alpha, black pixels
> represent low alpha.

## Quick Start :rocket:

1. Build and stage the plugin from source.
2. Launch the staged OBS runtime.
3. Open `Tools > Alpha Recorder Settings`.
4. Make sure Alpha Recorder is enabled.
5. Choose a finalization format, or leave the default.
6. Record normally in OBS.

Missing settings default to enabled. If you have not chosen a finalization
format yet, Alpha Recorder tries an available hardware HEVC encoder first and
falls back to PNG MOV.

## Settings :gear:

The settings dialog lives at `Tools > Alpha Recorder Settings`.

| Setting | What it does |
| --- | --- |
| Enabled | Turns the sidecar alpha recording on or off |
| Finalization Format | Chooses PNG MOV, HEVC NVENC, or HEVC AMF |
| Quality Profile | Applies a complete HEVC tuning preset |
| CQ | Adjusts HEVC constant-quality value |
| Preset | Uses NVENC P1-P7 or AMF Speed/Balanced/Quality |
| Tune | NVENC-only tuning mode |
| Advanced HEVC options | GOP, B-frames, lookahead, and adaptive quantization |

HEVC controls appear only when the matching encoder can actually open on the
current machine.

## Build From Source :hammer_and_wrench:

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

## Test :test_tube:

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

## Automation :robot:

Automated tests can control settings through obs-websocket:

- Vendor: `alpha_recorder`
- Requests: `GetSettings`, `SetSettings`

## Troubleshooting :bulb:

| Symptom | Likely reason |
| --- | --- |
| HEVC option is missing | The matching NVENC or AMF encoder cannot open on this machine |
| A matrix target is skipped | Configure detected missing encoder/runtime support |
| No alpha file appears | Alpha Recorder may be disabled, or the alpha session failed to start |
| OBS recording is fine but alpha is missing | The alpha pipeline may have aborted to protect the main recording |
| WSL app E2E will not launch OBS | Check WSLg environment and prefer a native Linux checkout |

## Project Docs :books:

- [`SPECS.md`](SPECS.md) has the technical contract, architecture, and full
  validation matrix.
- [`AGENTS.md`](AGENTS.md) is the short working guide for agents editing this
  repository.
