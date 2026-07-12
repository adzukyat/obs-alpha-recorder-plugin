**English** | [日本語](README.ja.md)

<img width="1920" height="1080" alt="Alpha Recorder Hero" src="https://github.com/user-attachments/assets/0d18331a-3550-4cf7-a78a-a810488a6b42" />

# :movie_camera: Alpha Recorder for OBS

Alpha Recorder is an OBS plugin that lets you record while preserving alpha. It
writes the alpha channel as a separate grayscale mask beside the main video, so
you do not need a special color space or video codec.

Use OBS the way you already do:

1. Open OBS.
2. Start Recording.
3. Stop Recording.
4. Find an extra `.alpha.mov`, `.alpha.mp4`, or `.alpha.mkv` file beside your video.

> [!IMPORTANT]
> Alpha Recorder does not add alpha to the normal recording file. The main video
> remains RGB, and the mask video is written as a separate file. Composite it in
> a video editor or other tool as needed.

## :sparkles: Features

- Alpha-mask output using OBS texture HEVC encoders (NVENC / AMF / QSV / VAAPI
  when the matching OBS encoder is available).
- Lossless alpha-mask output in PNG MOV format.
- Simple switching between encoder settings.
- Fully synchronized start position and frames between alpha and the main video.
- Works with OBS on Windows, macOS, and Linux.

## :rocket: Quick Start

1. Download the package for your platform from
   [Releases](https://github.com/adzukyat/obs-alpha-recorder-plugin/releases).
2. Extract the package.
3. Close OBS if it is running.
4. Copy the plugin from the extracted package into your OBS plugin directory:

| Platform | Copy from               | Copy to                                          |
| -------- | ----------------------- | ------------------------------------------------ |
| Windows  | `alpha_recorder.dll`    | `C:\Program Files\obs-studio\obs-plugins\64bit\` |
| macOS    | `alpha_recorder.plugin` | `/Applications/OBS.app/Contents/PlugIns/`        |
| Linux    | `alpha_recorder.so`     | `/usr/lib/obs-plugins/`                          |

If OBS is installed somewhere else, copy the plugin into the matching plugin
directory inside that OBS installation. On Linux, the directory may differ
depending on the distribution and installation method, so check the OBS
documentation as needed.

5. Launch OBS.
6. Open `Tools > Alpha Recorder Settings`.
7. Make sure Alpha Recorder is enabled.
8. Choose a finalization format, or leave the default.
9. Record normally in OBS.

## :gear: Settings

<img width="540" height="581" src="https://github.com/user-attachments/assets/ecfaecf7-79db-4c6d-8e28-62fc2aa7f350" />

HEVC controls appear only when the matching encoder can actually open on the
current machine.

## :bulb: Troubleshooting

| Symptom                         | Likely reason                                                                |
| ------------------------------- | ---------------------------------------------------------------------------- |
| HEVC option is missing          | The matching OBS texture HEVC encoder is not available on this machine       |
| No alpha file appears           | "Enabled" is turned off in Alpha Recorder settings                           |
| Alpha becomes black with Spout2 | Change "Composite Mode" from Opaque to Default in the Spout2 source settings |

## :hammer_and_wrench: Build

Alpha Recorder is a C++ OBS plugin and includes OBS as a submodule. Building
requires CMake and an OBS build environment. The OBS app E2E target also
requires the Bun runtime.

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

Linux:

```sh
cmake --preset linux-x64
cmake --build --preset linux-x64-relwithdebinfo
```

After this point, adjust the `--preset` value and directories for your
environment:

- `windows-x64-msvc-relwithdebinfo`
- `macos-arm64-relwithdebinfo`
- `linux-x64-relwithdebinfo`

### 4. Stage a runnable OBS tree

```sh
cmake --build --preset windows-x64-msvc-relwithdebinfo --target alpha_recorder_stage_obs_tree
```

Use the matching preset for your platform. The staged runtime is written under
`out/stage/obs`.

> [!TIP]
> Linux needs OBS build dependencies and FFmpeg tools. On Ubuntu 24.04, for
> example, you can install them with the following command:

```sh
sudo apt update && \
sudo apt install -y build-essential cmake ninja-build git pkg-config curl unzip zip \
  extra-cmake-modules ffmpeg libavcodec-dev libavformat-dev libavutil-dev \
  libavfilter-dev libavdevice-dev libswscale-dev libjansson-dev uthash-dev \
  qt6-base-dev qt6-base-private-dev qt6-svg-dev libx11-dev libx11-xcb-dev libxcomposite-dev libxdamage-dev \
  libxrandr-dev libxinerama-dev libxkbcommon-dev libxkbcommon-x11-dev \
  libxcb-render0-dev libxcb-shape0-dev libxcb-xfixes0-dev libxcb-shm0-dev \
  libxcb-composite0-dev libxcb-randr0-dev libxcb-xinerama0-dev libxcb-xinput-dev \
  libgl1-mesa-dev libegl1-mesa-dev libwayland-dev wayland-protocols \
  libasound2-dev libpulse-dev libpipewire-0.3-dev libv4l-dev libudev-dev libdrm-dev libva-dev libvpl-dev \
  libcurl4-openssl-dev libmbedtls-dev libspeexdsp-dev libasio-dev libsimde-dev \
  libwebsocketpp-dev libx264-dev libluajit-5.1-dev swig python3-dev nlohmann-json3-dev \
  libqrcodegencpp-dev libpci-dev && \
curl -fsSL https://bun.sh/install | bash
```

## :test_tube: Test

Run focused unit tests:

```sh
ctest --test-dir out/build/windows-x64-msvc -C RelWithDebInfo -L unit --output-on-failure
```

Run deterministic E2E tests:

```sh
cmake --build --preset windows-x64-msvc-relwithdebinfo --target alpha_recorder_run_e2e
```

Run the real OBS app E2E path:

```sh
cmake --build --preset windows-x64-msvc-relwithdebinfo --target alpha_recorder_run_obs_app_e2e
```

The OBS app E2E target launches a real OBS instance and automatically runs the
full flow from recording to frame sync verification through WebSocket.
Runtime-specific targets that cannot run on the current machine are skipped.

> [!WARNING]
> NVENC is the only hardware HEVC path currently verified by the maintainer's
> Windows machine. AMF, QSV, and VAAPI use OBS's texture encoder backends and are
> gated at runtime, but still need backend-specific hardware validation.
