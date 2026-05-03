# AGENTS.md

This file is the working guide for agents editing this repository. Keep it in
sync with behavior, commands, and project decisions. When something changes,
do not forget to update `AGENTS.md` in the same change.

## Project Contract

Alpha Recorder is an OBS plugin that follows the normal OBS recording lifecycle:

> When the user records in OBS, also write a separate grayscale alpha-mask movie
> aligned with the recorded video.

The finalized alpha movie is a standalone grayscale mask video: the captured
alpha values are encoded as visible pixel intensity. It is not expected to carry
its own transparency/alpha channel.

The design prioritizes OBS-native UX, minimal configuration, and a real OBS E2E
path that can run without desktop automation.

## Locked Decisions

- Users continue to use OBS Start Recording and Stop Recording.
- The plugin operates silently when enabled.
- Settings are exposed from `Tools > Alpha Recorder Settings`.
- Settings include:
  - Enabled toggle.
  - Finalization Format dropdown.
- Settings can also be driven by obs-websocket vendor API for automated E2E:
  - Vendor: `alpha_recorder`
  - Requests: `GetSettings`, `SetSettings`
- Scenario files are retained only for synthetic/non-UI E2E paths.
- There is no separate "Start Alpha Recording" button.
- Capture target is Program output, not Preview.
- The enabled flag is persisted in OBS user config under
  `AlphaRecorder.enabled`.
- The finalization format is persisted in OBS user config under
  `AlphaRecorder.finalization_format`.
- Supported finalization formats currently include:
  - `mask_png_mov` -> lossless grayscale PNG MOV `.mov`
  - `mask_hevc_nvenc` -> HEVC NVENC `.mp4`
  - `mask_hevc_amf` -> HEVC AMF `.mp4`
- Legacy raw sidecar and manifest primitives remain for synthetic/non-UI E2E
  support, but the shipping OBS runtime writes the playable alpha mask movie
  directly.

## Non-Goals

- Replacing OBS's recording UX.
- Shipping one combined RGB+alpha file as the user's primary recording.
- Requiring NVIDIA-only HEVC alpha-layer or any other video-level alpha channel
  for the finalized mask movie.
- Requiring desktop automation for the automated OBS app E2E path.
- External capture apps.

## User-Visible Behavior

When enabled:

- On recording start:
  - Start an alpha session bound to the active OBS recording.
  - Determine the real recording file path.
  - Create the alpha mask movie alongside the recording.
- During recording:
  - Capture alpha-preserving raw Program frames.
  - Convert alpha into visible grayscale luma and encode it into the mask movie.
  - Pause alpha capture while OBS recording is paused or stopping.
- On recording stop:
  - Finalize the alpha mask movie in the selected finalization format.

When disabled:

- Runtime hooks are removed.
- No alpha mask movie is created.

The plugin follows OBS's recording path and naming rules. If OBS records
`C:\Recordings\MyRec.mkv`, Alpha Recorder writes:

- `C:\Recordings\MyRec.alpha.mov` for PNG MOV, or
  `C:\Recordings\MyRec.alpha.mp4` for HEVC NVENC/AMF.

Failure behavior:

- If the alpha session cannot start or finalize, log details and show the user a
  modal error when the failure is user-visible.
- If the alpha pipeline cannot keep up, gracefully abort alpha mask movie
  generation, log the error, and do not stop the main OBS recording.
- If finalization fails on record stop, log the error and leave any partial mask
  movie as a debugging artifact.
- The main OBS recording must remain the highest-priority artifact.

## Technical Design

The repo contains core primitives for admission gating, legacy sidecar support,
settings, and live mask movie encoding. The OBS integration adds live capture,
lifecycle wiring, settings, and automated control.

Current alignment strategy:

1. Validate OBS Color Format is alpha-preserving.
2. Capture raw Program frames using `obs_add_raw_video_callback()` with BGRA
   conversion.
3. Pause capture on recording pause and on
   `OBS_FRONTEND_EVENT_RECORDING_STOPPING`.
4. Encode each captured alpha plane as visible grayscale luma into the live
   alpha mask movie.
5. Close the mask movie on recording stop or split rotation. The RGB recording
   is never decoded or modified by Alpha Recorder.

This intentionally avoids brittle dependence on encoder packet callbacks.

Required OBS integration points:

- Frontend events:
  - `OBS_FRONTEND_EVENT_RECORDING_STARTED`
  - `OBS_FRONTEND_EVENT_RECORDING_PAUSED`
  - `OBS_FRONTEND_EVENT_RECORDING_UNPAUSED`
  - `OBS_FRONTEND_EVENT_RECORDING_STOPPING`
  - `OBS_FRONTEND_EVENT_RECORDING_STOPPED`
- Recording output/path access:
  - `obs_frontend_get_recording_output()`
  - `obs_frontend_get_current_record_output_path()`
- Recording output split handling:
  - `obs_output_get_signal_handler()`
  - `file_changed`
- Raw video frames:
  - `obs_add_raw_video_callback()` with BGRA conversion.
- Automation:
  - obs-websocket vendor registration during `obs_module_post_load()`.
  - Do not call obs-websocket vendor-request unregister APIs from
    `obs_module_unload()`; OBS shutdown can invalidate the cached
    obs-websocket proc handler before Alpha Recorder unloads.

If raw callbacks produce only opaque alpha in a future OBS/runtime
configuration, the fallback is a dedicated render path: render the active
Program scene to an RGBA target that preserves alpha, then feed that into the
same mask movie encoder pipeline.

After OBS stops recording, the plugin finalizes the live alpha mask movie using
the selected finalization format.

## Current Status

Completed:

- `Tools > Alpha Recorder Settings` dialog with Enabled and Finalization Format.
- OBS user config persistence for enabled state and finalization format.
- Runtime hook registration/unregistration based on current settings.
- Recording lifecycle integration for start, pause, unpause, stopping, and stop.
- Live alpha mask movie creation next to the OBS recording.
- Alpha mask movie finalization on stop and split rotation.
- File split handling through OBS `file_changed`.
- obs-websocket vendor API for test automation:
  - `alpha_recorder.GetSettings`
  - `alpha_recorder.SetSettings`
- CMake-native OBS bootstrap, staging, deterministic E2E, and OBS app E2E
  scripts:
  - `cmake/scripts/BootstrapObs.cmake`
  - `cmake/scripts/StageObsTree.cmake`
  - `cmake/scripts/RunE2E.cmake`
  - `cmake/scripts/RunObsAppE2E.cmake`
- Cross-platform OBS app E2E helper, run with Bun:
  - `cmake/scripts/obs_app_e2e.ts`
- Deterministic split-rotation E2E scenario registered in CTest:
  - `tests/e2e/scenarios/split_rotation.scenario`
- HEVC OBS app E2E targets:
  - `alpha_recorder_run_obs_app_e2e_hevc_nvenc`
  - `alpha_recorder_run_obs_app_e2e_hevc_amf`
- Optional CTest registration behind:
  - `ALPHA_RECORDER_ENABLE_OBS_APP_E2E`
- OBS staging updates that copy the full OBS plugin set before overlaying Alpha
  Recorder binaries.

Still useful follow-up work:

- Broaden automated coverage for pause/unpause scenarios.
- Add stress coverage for encoder pressure or unusually slow finalization.
- Improve localized error text and recovery guidance.

## Validation

Manual validation:

- Enable/disable from `Tools > Alpha Recorder Settings`.
- Start and stop OBS recording.
- Confirm the RGB recording still exists and plays.
- Confirm the alpha mask movie exists and plays.

Automated OBS app E2E:

```sh
cmake --build --preset macos-arm64-relwithdebinfo --target alpha_recorder_run_obs_app_e2e
cmake --build --preset windows-x64-msvc-relwithdebinfo --target alpha_recorder_run_obs_app_e2e
```

HEVC OBS app E2E:

```sh
cmake --build --preset windows-x64-msvc-relwithdebinfo --target alpha_recorder_run_obs_app_e2e_hevc_nvenc
cmake --build --preset windows-x64-msvc-relwithdebinfo --target alpha_recorder_run_obs_app_e2e_hevc_amf
```

The CMake target:

- Builds and stages OBS plus the plugin into an isolated app/runtime tree.
- On macOS, stages an app-style `OBS.app/Contents` tree and launches the
  bundle executable rather than a loose standalone binary.
- Creates an isolated OBS profile and scene collection. On macOS this uses an
  isolated `HOME`/`CFFIXED_USER_HOME` because the pinned OBS app does not enable
  portable mode.
- Enables obs-websocket.
- Launches real OBS (`obs64.exe` on Windows, `OBS.app/Contents/MacOS/OBS` on
  macOS, or `bin/obs` for loose runtimes).
- Enables Alpha Recorder through `CallVendorRequest` using
  `alpha_recorder.SetSettings`.
- Starts and stops OBS recording through obs-websocket.
- Waits for RGB recording and alpha mask movie outputs.
- Uses `ffprobe` and `ffmpeg` to confirm both RGB and alpha outputs are playable.
- Verifies the PNG MOV alpha movie reports `png` and does not use an alpha
  pixel format.
- For HEVC targets, verifies the alpha output is `.mp4`, `ffprobe` reports
  `hevc`, and the output does not use an alpha pixel format.

CTest can register this slow app-level test when configured with
`ALPHA_RECORDER_ENABLE_OBS_APP_E2E=ON`.

## Open Questions

- Whether all supported OBS/runtime combinations preserve meaningful alpha in
  the raw callback.
- Whether the dedicated render fallback is needed for any common production
  scene setup.
- How strict the exported-alpha frame count should be under severe encoder
  backpressure or unusual stop timing.
