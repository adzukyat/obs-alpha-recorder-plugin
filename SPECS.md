# SPECS.md

This document is the technical contract for Alpha Recorder. Keep it in sync
with runtime behavior, build orchestration, validation coverage, and project
decisions. Keep [`README.md`](README.md) user-friendly and
[`AGENTS.md`](AGENTS.md) focused on agent instructions.

## Project Contract

Alpha Recorder is an OBS plugin that follows the normal OBS recording
lifecycle:

> When the user records in OBS, also write a separate grayscale alpha-mask movie
> aligned with the recorded video.

The finalized alpha movie is a standalone grayscale mask video. Captured alpha
values are encoded as visible pixel intensity. It is not expected to carry its
own transparency or alpha channel.

The design prioritizes OBS-native UX, minimal configuration, and a real OBS app
E2E path that can run without desktop automation.

## Locked Decisions

- Users continue to use OBS Start Recording and Stop Recording.
- The plugin operates silently when enabled.
- Settings are exposed from `Tools > Alpha Recorder Settings`.
- There is no separate "Start Alpha Recording" button.
- Capture target is Program output, not Preview.
- Missing settings default to Enabled ON.
- When no finalization format has been saved, the plugin uses the safe PNG MOV
  fallback. Hardware HEVC formats are preserved when explicitly selected and are
  exposed only when the matching runtime backend is available.
- Scenario files are retained only for synthetic/non-UI E2E paths.
- External capture apps are not part of the product path.
- The shipping OBS runtime writes the playable alpha mask movie directly.
- Raw sidecar and manifest primitives remain only for synthetic/non-UI
  E2E support.
- Build and staging produce one normal user OBS plugin artifact named
  `alpha_recorder`.
- The `alpha_recorder` artifact contains both runtime recording hooks and
  `Tools > Alpha Recorder Settings`.
- The separate `alpha_recorder_e2e` module is test-only for deterministic and
  synthetic E2E support.
- `alpha_recorder_e2e` must not register runtime UI or obs-websocket hooks.

## Non-Goals

- Replacing OBS's recording UX.
- Shipping one combined RGB+alpha file as the user's primary recording.
- Requiring NVIDIA-only HEVC alpha-layer or any other video-level alpha channel
  for the finalized mask movie.
- Requiring desktop automation for the automated OBS app E2E path.
- Requiring users to change OBS's normal recording color format to RGBA.

## User-Visible Behavior

When enabled:

- On recording start:
  - Start an alpha session bound to the active OBS recording.
  - Determine the real recording file path.
  - Create the alpha mask movie alongside the recording.
- During recording:
  - Extract the rendered Program texture's alpha on the GPU without changing
    OBS's normal recording color format.
  - Queue captured alpha planes and write them to the mask movie only as OBS's
    recording output frame count advances.
  - Pause alpha capture while OBS recording is paused.
  - Keep stop-edge capture active until OBS reports recording stopped.
- On recording stop:
  - Finalize the alpha mask movie in the selected finalization format.

When disabled:

- Runtime hooks are removed.
- No alpha mask movie is created.

The plugin follows OBS's recording path and naming rules. If OBS records
`C:\Recordings\MyRec.mkv`, Alpha Recorder writes:

- `C:\Recordings\MyRec.alpha.mov` for PNG MOV.
- `C:\Recordings\MyRec.alpha.mp4` for HEVC NVENC, HEVC AMF, HEVC QSV, or HEVC
  VAAPI.

Failure behavior:

- If the alpha session cannot start or finalize, log details and show the user a
  modal error when the failure is user-visible.
- If alignment or mask writing cannot keep up, keep alpha mask movie generation
  running by repeating the previous alpha frame for recoverable gaps, log the
  repeat/drop counts, and do not stop or slow the main OBS recording.
- If finalization fails on record stop, log the error and leave any partial mask
  movie as a debugging artifact.
- The main OBS recording must remain the highest-priority artifact.

## Settings Contract

Settings include:

- Enabled toggle.
- Finalization Format dropdown.
- Installed version status. The dialog checks GitHub asynchronously for the
  latest published release and verified signed `v*` tags, then links to GitHub
  Releases when the installed version is older.
- HEVC encoder controls when HEVC mask output is selected:
  - Quality Profile.
  - CQ.
  - Encoder-specific Preset.
  - Tune for NVENC.
  - Advanced GOP, B-frames, AQ, NVENC GPU, and NVENC Split Encode
    controls.
- NVENC exposes P1 through P7 preset values, Tune, GPU index, and Split Encode
  mode.
- AMF exposes Speed, Balanced, and Quality presets without Tune.
- QSV exposes TU7 through TU1 preset values without NVENC Tune, GPU index, or
  Split Encode controls.
- VAAPI exposes a conservative default preset and common CQ/GOP/B-frame
  controls only.
- Quality Profile buttons apply full encoder presets, not CQ-only shortcuts.
- HEVC profile buttons have complete tuning semantics:
  - Lossless disables the lossy tuning path.
  - High Quality enables the quality preset with B-frames and AQ.
  - Balanced uses lighter B-frame and AQ settings.
  - Fast disables latency-heavy B-frame and AQ options.

Persisted OBS user config keys:

| Behavior | Config key |
| --- | --- |
| Enabled flag | `AlphaRecorder.enabled` |
| Finalization format | `AlphaRecorder.finalization_format` |
| HEVC quality profile | `AlphaRecorder.hevc_quality_profile` |
| HEVC CQ | `AlphaRecorder.hevc_quality_cq` |
| HEVC preset | `AlphaRecorder.hevc_preset` |
| HEVC NVENC tune | `AlphaRecorder.hevc_nvenc_tune` |
| HEVC GOP size | `AlphaRecorder.hevc_gop_size` |
| HEVC B-frames | `AlphaRecorder.hevc_b_frames` |
| HEVC adaptive quantization | `AlphaRecorder.hevc_adaptive_quantization` |
| HEVC NVENC Split Encode | `AlphaRecorder.hevc_nvenc_split_encode` |
| HEVC NVENC GPU index | `AlphaRecorder.hevc_nvenc_gpu_index` |
| Diagnostic logging | `AlphaRecorder.diagnostic_logging` |

NVENC Split Encode accepts `auto`, `disabled`, `forced`, `2`, and `3`. Non-auto
Split Encode settings are passed to OBS's NVENC texture encoder as
encoder-specific settings. Persisted values are preserved across machines; if the
current OBS/NVIDIA runtime rejects the selected Split Encode or GPU index, alpha
output startup fails with the OBS encoder error instead of silently changing the
user's setting. NVENC GPU index uses `-1` for OBS/NVIDIA default device selection
and `0+` for an explicit NVENC-capable GPU index.

Supported finalization formats:

| Format id | Output | Notes |
| --- | --- | --- |
| `mask_png_mov` | Lossless grayscale PNG MOV `.mov` | CPU-heavy fallback, disk-light relative to raw masks |
| `mask_hevc_nvenc` | HEVC NVENC `.mp4` | Uses OBS texture encoder `obs_nvenc_hevc_tex` |
| `mask_hevc_amf` | HEVC AMF `.mp4` | Uses OBS texture encoder `h265_texture_amf` |
| `mask_hevc_qsv` | HEVC QSV `.mp4` | Uses OBS texture encoder `obs_qsv11_hevc` |
| `mask_hevc_vaapi` | HEVC VAAPI `.mp4` | Uses OBS texture encoder `hevc_ffmpeg_vaapi_tex` |

HEVC GPU texture options are exposed only when OBS registers the matching HEVC
video encoder and that encoder advertises `OBS_ENCODER_CAP_PASS_TEXTURE`.
Encoder-name presence alone is not enough. CPU fallback HEVC writer paths retain
their own FFmpeg openability probes where applicable.

Settings can also be driven by obs-websocket vendor API for automated E2E:

- Vendor: `alpha_recorder`
- Requests: `GetSettings`, `SetSettings`

## Technical Design

The repo contains core primitives for admission gating, sidecar support,
settings, and live mask movie encoding. The OBS integration adds live capture,
lifecycle wiring, settings, and automated control.

The current tree provides:

- OBS recording lifecycle hooks plus a Tools menu settings dialog.
- A core static library for pair gating, sidecar primitives, settings,
  and live mask movie encoding.
- Unit test executables registered with CTest.
- Deterministic E2E executables and CMake-native staging helpers.
- A cross-platform OBS app E2E path driven by CMake and obs-websocket.
- CMake presets for Windows x64 MSVC, macOS, and Linux x64.

## Alignment Strategy

1. Keep OBS's normal recording color format independent of Alpha Recorder so
   production recording can use hardware-friendly formats such as NV12/P010.
2. Retain the active recording output and register render, raw-video, and packet
   callbacks on `OBS_FRONTEND_EVENT_RECORDING_STARTING`.
3. Open the alpha movie writer on `OBS_FRONTEND_EVENT_RECORDING_STARTED`, once
   the recording path is available.
4. Capture the rendered Program texture after OBS renders the main mix, extract
   its alpha into a `GS_R8` mask texture on the GPU, then read it back through a
   small staging-surface ring before adding it to the pending-frame queue.
5. Use OBS raw-video callbacks as the final video-output cadence source.
6. Use encoded packet composition timestamp (`encoder_packet_time::cts`) to
   identify the raw-video cadence frame actually admitted into the RGB
   recording, rather than applying encoder-specific startup offsets.
7. For texture encoders, use the next observed raw-video cadence frame after
   CTS, because OBS queues the current rendered texture while draining the
   previous raw-video timestamp.
8. When OBS repeats the same cached output frame, duplicate the previous alpha
   mask frame so the mask movie mirrors RGB duplicate/drop behavior.
9. If the first admitted RGB frame is already a duplicate, use the duplicate's
   raw content-origin timestamp to select the matching alpha frame.
10. Use video packet callbacks from the active recording output only to enqueue
    encoded-video packet ordering evidence, sorted by packet PTS.
11. Resolve and hand off aligned mask frames on Alpha Recorder's alignment
    worker rather than inside OBS callbacks.
12. Cache the texture-encoder path classification from recording output
    capabilities and OBS's active NV12/P010 texture state.
13. Do not query per-encoder texture/mix state from packet callbacks or from the
    `OBS_FRONTEND_EVENT_RECORDING_STARTING` transition.
14. Pause capture on recording pause.
15. Do not pause on `OBS_FRONTEND_EVENT_RECORDING_STOPPING`; keep capturing
    until `OBS_FRONTEND_EVENT_RECORDING_STOPPED` so stop-edge frames can
    reconcile.
16. Close the mask movie on recording stop or split rotation.
17. Never decode or modify the RGB recording in Alpha Recorder.

Raw-video callbacks are used only as cadence evidence for the final OBS video
output. Packet callbacks are used only for encoded packet ordering.

If the main Program texture produces only opaque alpha in a future OBS/runtime
configuration, the fallback is a dedicated render path: render the active
Program scene to an RGBA target that preserves alpha, then feed that into the
same mask movie encoder pipeline.

## Required OBS Integration Points

Frontend events:

- `OBS_FRONTEND_EVENT_RECORDING_STARTING`
- `OBS_FRONTEND_EVENT_RECORDING_STARTED`
- `OBS_FRONTEND_EVENT_RECORDING_PAUSED`
- `OBS_FRONTEND_EVENT_RECORDING_UNPAUSED`
- `OBS_FRONTEND_EVENT_RECORDING_STOPPING`
- `OBS_FRONTEND_EVENT_RECORDING_STOPPED`

Recording output/path access:

- `obs_frontend_get_recording_output()`
- `obs_frontend_get_current_record_output_path()`

Recording output split handling:

- `obs_output_get_signal_handler()`
- `file_changed`

Program alpha extraction:

- `obs_add_main_rendered_callback()` / `obs_remove_main_rendered_callback()`
- `obs_get_main_texture()`
- `gs_texture_create(..., GS_R8, ..., GS_RENDER_TARGET)`
- `gs_stage_texture()` / `gs_stagesurface_map()`

Recorded-video cadence:

- `obs_add_raw_video_callback()` / `obs_remove_raw_video_callback()`

Encoded-video packet ordering:

- `obs_output_add_packet_callback()` / `obs_output_remove_packet_callback()`

Automation:

- obs-websocket vendor registration during `obs_module_post_load()`.
- Do not call obs-websocket vendor-request unregister APIs from
  `obs_module_unload()`. OBS shutdown can invalidate the cached obs-websocket
  proc handler before Alpha Recorder unloads.

## Build and Staging Contract

The OBS module target and the E2E harness require a real OBS developer tree.
`OBS_ROOT` must point to a tree with libobs headers, import libraries,
`bin/64bit`, and `data`.

The OBS source checkout is tracked as a git submodule at
`deps/obs/obs-studio`. When bootstrapped from source, the staged developer tree
lives under `deps/obs/obs-build/rundir/RelWithDebInfo` alongside the pinned
source and build trees.

Preferred bootstrap:

```sh
git submodule update --init --recursive
cmake -DREPO_ROOT="$PWD" -DBUILD_FROM_SOURCE=ON -P cmake/scripts/BootstrapObs.cmake
```

If an OBS source checkout already exists, use `-DBUILD_FROM_SOURCE=ON` to stage
it into the OBS build tree's runtime prefix without recloning. To refresh the
submodule checkout from the pinned OBS tag, add `-DCLONE_SOURCE=ON`. If a real
OBS developer tree already exists, pass its root with `-DOBS_ROOT=...` to
validate it and write `deps/obs/obs-root.cmake`.

On macOS, OBS bootstrap uses OBS Studio's required Xcode generator for the OBS
dependency build. On Windows, it uses Visual Studio by default. On Linux, it
uses Ninja and stages the installed OBS runtime layout under
`deps/obs/obs-build/rundir` by default.

`deps/obs/obs-root.cmake` is an optional CMake fragment generated by
`cmake/scripts/BootstrapObs.cmake`. `OBS_ROOT` may also be supplied through the
environment or the CMake preset. `cmake/FindOBS.cmake` looks for libobs headers
and import libraries under the configured root and creates an imported
`OBS::libobs` target when found.

Configure and build:

```sh
cmake --preset macos-arm64
cmake --build --preset macos-arm64-relwithdebinfo
```

Windows:

```powershell
cmake --preset windows-x64-msvc
cmake --build --preset windows-x64-msvc-relwithdebinfo
```

Linux:

```sh
cmake --preset linux-x64
cmake --build --preset linux-x64-relwithdebinfo
```

The default preset fails fast if `OBS_ROOT` does not resolve to a real developer
tree.

Stage a portable OBS tree with Alpha Recorder overlaid:

```sh
cmake --build --preset macos-arm64-relwithdebinfo --target alpha_recorder_stage_obs_tree
```

Staging overlays one normal user plugin artifact, `alpha_recorder`. When
deterministic E2E support is built, staging may also overlay the separate
test-only `alpha_recorder_e2e` module.

Staged plugin locations:

- Windows: `out/stage/obs/obs-plugins/64bit`.
- macOS: `out/stage/obs/obs-plugins`.
- Linux: staged OBS library plugin directory such as
  `out/stage/obs/lib/x86_64-linux-gnu/obs-plugins`.

Linux stages the installed OBS `bin`, library, plugin, and `share/obs` layout.
The harness launches `bin/obs` with `LD_LIBRARY_PATH` pointing at the staged
runtime libraries. Under WSL, the harness defaults `QT_QPA_PLATFORM` to `xcb`
when it is unset because this pinned OBS build is more reliable through XWayland
than direct Wayland in SSH-launched runs.

macOS stages an app-style `OBS.app/Contents` tree and launches the bundle
executable rather than a loose standalone binary.

## Release Packaging Contract

GitHub Actions release builds are tag-driven. Release tags must match
`vX.X.X`-style names and must be annotated tags whose signatures GitHub verifies
before any build or release job runs. Lightweight tags and unverified tags are
rejected.

The canonical project release version lives in the root `VERSION` file. CMake
uses that file for `project(... VERSION ...)` and generates
`alpha_recorder/version.hpp`, which is the runtime source for manifest
`project_version` metadata. The release workflow also rejects a signed release
tag when its `vX.X.X` payload does not match `VERSION`.

The release artifacts contain the user plugin package layout plus `README.md`,
`LICENSE`, and `VERSION`; they do not include the staged OBS runtime or the
test-only `alpha_recorder_e2e` module.

- Windows: `alpha-recorder-vX.X.X-windows-x64.zip`.
- macOS: `alpha-recorder-vX.X.X-macos-arm64.zip`.
- Linux: `alpha-recorder-vX.X.X-linux-x64.tar.gz`.

## Linux and WSL Notes

On Linux or WSL Ubuntu, install build/runtime prerequisites first. The exact OBS
dependency set can grow with the pinned OBS tag, but this is the expected
baseline on Ubuntu 24.04:

```sh
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build git pkg-config curl unzip zip \
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
  libqrcodegencpp-dev libpci-dev
curl -fsSL https://bun.sh/install | bash
```

For WSL app E2E, validate that WSLg exposes `DISPLAY`, `WAYLAND_DISPLAY`,
`XDG_RUNTIME_DIR`, and the Wayland socket before launching OBS. Use a native
Linux checkout; avoid building from a Windows-mounted `/mnt/*` checkout for
normal validation.

## Validation Contract

Manual validation:

- Enable/disable from `Tools > Alpha Recorder Settings`.
- Start and stop OBS recording.
- Confirm the RGB recording still exists and plays.
- Confirm the alpha mask movie exists and plays.

Focused unit regression:

```sh
ctest --test-dir out/build/macos-arm64 -C RelWithDebInfo -R alpha_recorder.unit.recording_session_cadence --output-on-failure
```

All unit tests:

```sh
ctest --test-dir out/build/macos-arm64 -C RelWithDebInfo -L unit --output-on-failure
```

Deterministic E2E:

```sh
cmake --build --preset macos-arm64-relwithdebinfo --target alpha_recorder_run_e2e
```

The deterministic E2E host starts libobs, loads the staged
`alpha_recorder_e2e` module, produces RGB raw and alpha mask artifacts, and the
verifier parses those files rather than checking file existence alone. Scenario
files under `tests/e2e/scenarios` are E2E-only inputs.

Automated OBS app E2E:

```sh
cmake --build --preset macos-arm64-relwithdebinfo --target alpha_recorder_run_obs_app_e2e
cmake --build --preset windows-x64-msvc-relwithdebinfo --target alpha_recorder_run_obs_app_e2e
cmake --build --preset linux-x64-relwithdebinfo --target alpha_recorder_run_obs_app_e2e
```

Linux/WSL validation bootstrap:

```sh
cmake -DREPO_ROOT="$PWD" -DBUILD_FROM_SOURCE=ON -P cmake/scripts/BootstrapObs.cmake
cmake --preset linux-x64
cmake --build --preset linux-x64-relwithdebinfo
ctest --preset linux-x64-relwithdebinfo --output-on-failure
cmake --build --preset linux-x64-relwithdebinfo --target alpha_recorder_run_e2e
cmake --build --preset linux-x64-relwithdebinfo --target alpha_recorder_run_obs_app_e2e_fhd60_rgb_sw_alpha_png_mov
```

CTest can register the slow app-level test when configured with
`ALPHA_RECORDER_ENABLE_OBS_APP_E2E=ON`.

## OBS App E2E Behavior

The app-level E2E is cross-platform at the harness layer. CMake launches
`cmake/scripts/RunObsAppE2E.cmake`, which stages OBS and calls a Bun
obs-websocket client.

Platform-specific differences are limited to executable and runtime layout
resolution:

- Windows: `obs64.exe`.
- macOS: `OBS.app/Contents/MacOS/OBS` or `bin/obs`.
- Linux/WSL: `bin/obs` with Linux `LD_LIBRARY_PATH`.

The staged OBS root needs the platform's real OBS runtime, plugins, FFmpeg
tools, obs-websocket plugin, and `bun` on PATH.

The OBS app E2E target:

- Builds and stages OBS plus the plugin into an isolated app/runtime tree.
- Creates an isolated OBS profile and scene collection.
- On macOS, uses isolated `HOME` and `CFFIXED_USER_HOME`.
- On Linux, uses isolated `HOME` and `XDG_CONFIG_HOME` because the pinned OBS
  app may not enable portable mode.
- Enables obs-websocket.
- Launches real OBS.
- Enables Alpha Recorder through `CallVendorRequest` using
  `alpha_recorder.SetSettings`.
- Starts and stops OBS recording through obs-websocket.
- Runs a startup sync gate first: five short 2-second recordings in one OBS
  launch, each requiring decoded zero frame-code offset.
- Runs one 30-second durability recording after the startup sync gate passes to
  catch sustained capture, writer, encoder, and stop-edge failures.
- Uses OBS's default hardware-friendly NV12 color format in the app-level E2E
  profile while Alpha Recorder extracts alpha through its own GPU-side path.
- Can run the RGB recording profile with software encoding, explicit NVENC
  HEVC, or explicit AMF HEVC.
- Pairs standard hardware RGB matrix targets with PNG MOV or the same vendor's
  HEVC alpha output; QSV/VAAPI alpha paths are available through the manual
  `RunObsAppE2E.cmake` finalization-format switch when matching hardware is
  present.
- Waits for RGB recording and alpha mask movie outputs.
- Writes `alpha-recorder-performance.json` under the OBS app E2E artifact root.
- Keeps CMake stdout compact by writing detailed OBS stdout/stderr to
  `obs-process.log` and full probe/result details to `obs-app-summary.json`
  under the artifact root.
- Uses `ffprobe` and `ffmpeg` to confirm both RGB and alpha outputs are
  playable.
- Adds a test-only moving colored object over a transparent background with an
  opaque binary frame-code strip.
- Decodes RGB and alpha mask frames for PNG MOV and HEVC targets.
- Verifies zero frame-code offset plus moving mask bounds frame-by-frame,
  allowing only small start/terminal/count mismatches.
- Verifies the PNG MOV alpha movie reports `png` and does not use an alpha
  pixel format.
- For HEVC targets, verifies the alpha output is `.mp4`, `ffprobe` reports
  `hevc`, and the output does not use an alpha pixel format.

The plugin logs one per-segment OBS performance summary covering
capture/readback CPU time, GPU submission timing, alignment-worker batches,
alignment recovery counts, queue depths, writer overflow repeat count, and
mask encode timing. If diagnostic logging is enabled, the same summary plus
segment-start encoder settings, dynamic alignment/writer queue limits, NVENC
option readback, and encode-stage timing breakdowns are appended to the plugin
diagnostic log file. The OBS app E2E harness copies OBS performance summaries into
`alpha-recorder-performance.json`.

If OBS logs `Encoding overloaded!`, severe skipped frames due to encoding lag,
or severe render lag, the app E2E stops recording and fails immediately with an
overload-specific error instead of continuing to decoded sync verification.
Tiny skipped-frame summaries are not treated as overload by themselves.

## OBS App E2E Matrix

OBS app E2E targets are registered only when the matching runtime encoder probe
succeeds. The aggregate target depends only on runnable profiles. Directly
building a skipped target prints its skip reason and does not launch OBS; the
configure summary lists skipped target names and the encoder-open failure that
caused each skip.

Registered target families include:

- `alpha_recorder_run_obs_app_e2e_fhd60_rgb_sw_alpha_png_mov`
- `alpha_recorder_run_obs_app_e2e_fhd60_rgb_sw_alpha_hevc_nvenc`
- `alpha_recorder_run_obs_app_e2e_fhd60_rgb_sw_alpha_hevc_amf`
- `alpha_recorder_run_obs_app_e2e_fhd60_rgb_nvenc_hevc_alpha_png_mov`
- `alpha_recorder_run_obs_app_e2e_fhd60_rgb_nvenc_hevc_alpha_hevc_nvenc`
- `alpha_recorder_run_obs_app_e2e_fhd60_rgb_amf_hevc_alpha_png_mov`
- `alpha_recorder_run_obs_app_e2e_fhd60_rgb_amf_hevc_alpha_hevc_amf`
- `alpha_recorder_run_obs_app_e2e_wqhd60_rgb_nvenc_hevc_alpha_*`
- `alpha_recorder_run_obs_app_e2e_wqhd60_rgb_amf_hevc_alpha_*`
- `alpha_recorder_run_obs_app_e2e_4k30_rgb_nvenc_hevc_alpha_*`
- `alpha_recorder_run_obs_app_e2e_4k30_rgb_amf_hevc_alpha_*`

Example sync-bug exposure matrix:

```sh
cmake --build --preset windows-x64-msvc-relwithdebinfo --target alpha_recorder_run_obs_app_e2e_fhd60_rgb_sw_alpha_png_mov
cmake --build --preset windows-x64-msvc-relwithdebinfo --target alpha_recorder_run_obs_app_e2e_fhd60_rgb_sw_alpha_hevc_nvenc
cmake --build --preset windows-x64-msvc-relwithdebinfo --target alpha_recorder_run_obs_app_e2e_fhd60_rgb_sw_alpha_hevc_amf
cmake --build --preset windows-x64-msvc-relwithdebinfo --target alpha_recorder_run_obs_app_e2e_fhd60_rgb_nvenc_hevc_alpha_png_mov
cmake --build --preset windows-x64-msvc-relwithdebinfo --target alpha_recorder_run_obs_app_e2e_fhd60_rgb_nvenc_hevc_alpha_hevc_nvenc
cmake --build --preset windows-x64-msvc-relwithdebinfo --target alpha_recorder_run_obs_app_e2e_fhd60_rgb_amf_hevc_alpha_png_mov
cmake --build --preset windows-x64-msvc-relwithdebinfo --target alpha_recorder_run_obs_app_e2e_fhd60_rgb_amf_hevc_alpha_hevc_amf
cmake --build --preset windows-x64-msvc-relwithdebinfo --target alpha_recorder_run_obs_app_e2e_wqhd60_rgb_nvenc_hevc_alpha_png_mov
cmake --build --preset windows-x64-msvc-relwithdebinfo --target alpha_recorder_run_obs_app_e2e_4k30_rgb_nvenc_hevc_alpha_png_mov
cmake --build --preset windows-x64-msvc-relwithdebinfo --target alpha_recorder_run_obs_app_e2e_4k30_rgb_amf_hevc_alpha_png_mov
```

Runtime matrix rules:

- Configure probes `hevc_nvenc` and `hevc_amf` on the target machine with a
  realistic 1080p FFmpeg encode for the legacy CPU HEVC writer and standard
  hardware matrix targets.
- Configure registers only matching alpha HEVC targets whose encoder opens.
- RGB NVENC targets additionally require the staged OBS NVENC plugin.
- The target matrix is runtime-aware rather than OS-only.
- Load matrix targets are `fhd60` (`1920x1080@60`), `wqhd60`
  (`2560x1440@60`), and `4k30` (`3840x2160@30`).
- `fhd60_rgb_sw_alpha_png_mov` is retained as the hardware-independent load
  baseline.
- Software RGB is not registered for WQHD/60 or 4K/30 load targets because
  those turn into machine-pressure checks.
- Cross-vendor hardware pairs are intentionally not registered in the standard
  matrix.
- RGB NVENC plus alpha AMF, or RGB AMF plus alpha NVENC, require a test machine
  with both NVIDIA and AMD hardware encoders.
- NVENC/AMF/QSV/VAAPI alpha HEVC formats appear in the plugin UI/API only where
  OBS registers the matching texture encoder.
- RGB hardware targets are skipped if the staged OBS runtime lacks the matching
  OBS encoder plugin.

## Current Status

Completed:

- Pair admission logic with all-or-nothing frame-pair acceptance.
- `Tools > Alpha Recorder Settings` dialog with Enabled, Finalization Format,
  and HEVC encoder tuning controls.
- OBS user config persistence for enabled state, finalization format, and HEVC
  encoder tuning.
- Runtime-aware finalization format defaults and availability filtering.
- Runtime hook registration/unregistration based on current settings.
- Recording lifecycle integration for start, pause, unpause, stopping, and stop.
- GPU-side Program alpha extraction that does not require switching OBS's main
  Color Format from NV12/P010 to RGBA, with staged readback buffered through a
  small staging-surface ring.
- Raw-video cadence tracking so alpha output mirrors OBS duplicate/drop behavior
  instead of assuming every rendered frame reaches the RGB recording or applying
  encoder-specific fixed frame offsets.
- Focused unit regression coverage proving repeated raw-video output frames
  duplicate the previous alpha frame rather than consuming a newer pending alpha
  frame.
- Packet composition timestamp coverage for skipping unadmitted startup cadence.
- Texture-encoder packet coverage for resolving through the observed successor
  cadence frame.
- Startup duplicate coverage preserving raw content-origin timestamp.
- Live alpha mask movie creation next to the OBS recording.
- Bounded asynchronous mask movie encoding with dynamic resolution/FPS-aware
  alignment and writer limits; recoverable alignment gaps and writer
  backpressure repeat previous alpha frames instead of aborting alpha output or
  blocking OBS recording.
- Alignment resolution and mask-writer handoff outside OBS callbacks.
- Live path queues captured alpha buffers without a second full-frame copy.
- Texture-encoder path classification cached before packet handling so packet
  callbacks only enqueue packet PTS/CTS evidence.
- Lightweight per-segment performance telemetry for capture/readback,
  alignment-worker batches/recovery, queue depths, split-encode option readback,
  and mask encode-stage timing.
- Optional diagnostic log file with a settings-dialog reveal button.
- Alpha mask movie finalization on stop and split rotation.
- File split handling through OBS `file_changed`.
- obs-websocket vendor API for `alpha_recorder.GetSettings` and
  `alpha_recorder.SetSettings`.
- obs-websocket settings coverage for HEVC quality profile, CQ, preset, NVENC
  tune, GOP, B-frames, adaptive quantization, NVENC Split Encode,
  and NVENC GPU index.
- CMake-native OBS bootstrap, staging, deterministic E2E, and OBS app E2E
  scripts:
  - `cmake/scripts/BootstrapObs.cmake`
  - `cmake/scripts/StageObsTree.cmake`
  - `cmake/scripts/RunE2E.cmake`
  - `cmake/scripts/RunObsAppE2E.cmake`
- Cross-platform OBS app E2E helper, run with Bun:
  - `cmake/scripts/obs_app_e2e.ts`
- Deterministic split-rotation E2E scenario:
  - `tests/e2e/scenarios/split_rotation.scenario`
- Deterministic E2E validation of RGB raw artifacts, alpha mask artifacts, and
  split-rotation behavior through the OBS module boundary.
- Cross-platform OBS app E2E harness that verifies RGB and alpha mask movie
  outputs, including zero frame-code offset plus frame-by-frame sync between a
  moving colored object and its grayscale alpha mask on PNG MOV and HEVC paths.
- Optional CTest registration behind:
  - `ALPHA_RECORDER_ENABLE_OBS_APP_E2E`
- OBS staging updates that copy the full OBS plugin set before overlaying Alpha
  Recorder binaries.

Still useful follow-up work:

- Broaden automated coverage for pause/unpause scenarios.
- Add stress coverage for encoder pressure or unusually slow finalization.
- Improve localized error text and recovery guidance.

## Open Questions

- Whether all supported OBS/runtime combinations preserve meaningful alpha in
  the main Program texture after rendering.
- Whether the dedicated render fallback is needed for any common production
  scene setup.
- How strict the exported-alpha frame count should be under severe encoder
  backpressure or unusual stop timing.
