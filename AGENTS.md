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
  - HEVC encoder controls when NVENC/AMF mask output is selected:
    - Quality Profile.
    - CQ.
    - Preset.
- Missing settings default to Enabled ON.
- When no finalization format has been saved, the plugin prefers an available
  hardware HEVC encoder before falling back to PNG MOV.
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
- HEVC encoder tuning is persisted in OBS user config under:
  - `AlphaRecorder.hevc_quality_profile`
  - `AlphaRecorder.hevc_quality_cq`
  - `AlphaRecorder.hevc_preset`
- Supported finalization formats currently include:
  - `mask_png_mov` -> lossless grayscale PNG MOV `.mov`
  - `mask_hevc_nvenc` -> HEVC NVENC `.mp4`
  - `mask_hevc_amf` -> HEVC AMF `.mp4`
- HEVC options are exposed only when the matching runtime encoder can actually
  open on the current machine; encoder-name presence alone is not enough.
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
  - Extract the rendered Program texture's alpha on the GPU without changing
    OBS's normal recording color format.
  - Queue captured alpha planes and write them to the mask movie only as OBS's
    recording output frame count advances.
  - Pause alpha capture while OBS recording is paused; keep stop-edge capture
    active until OBS reports recording stopped.
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

1. Keep OBS's normal recording color format independent of Alpha Recorder so
   production recording can use hardware-friendly formats such as NV12/P010.
2. Retain the active recording output and register render, raw-video, and packet
   callbacks on `OBS_FRONTEND_EVENT_RECORDING_STARTING`; open the alpha movie
   writer on `OBS_FRONTEND_EVENT_RECORDING_STARTED`, once the recording path is
   available.
3. Capture the rendered Program texture after OBS renders the main mix, extract
   its alpha into an `GS_R8` mask texture on the GPU, then read it back through
   a small staging-surface ring before adding it to the pending-frame queue.
4. Use OBS raw-video callbacks as the final video-output cadence source. Use
   the encoded packet composition timestamp (`encoder_packet_time::cts`) to
   identify the raw-video cadence frame actually admitted into the RGB
   recording, rather than applying encoder-specific startup offsets. For
   texture encoders, use the next observed raw-video cadence frame after CTS,
   because OBS queues the current rendered texture while draining the previous
   raw-video timestamp. When OBS repeats the same cached output frame,
   duplicate the previous alpha mask frame so the mask movie mirrors RGB
   duplicate/drop behavior; if the first admitted RGB frame is already a
   duplicate, use the duplicate's raw content-origin timestamp to select the
   matching alpha frame.
5. Use video packet callbacks from the active recording output only to enqueue
   encoded-video packet ordering evidence, sorted by packet PTS. Resolve and
   hand off aligned mask frames on Alpha Recorder's alignment worker rather
   than inside OBS callbacks. Cache the texture-encoder path classification from
   recording output capabilities and OBS's active NV12/P010 texture state; do
   not query per-encoder texture/mix state from packet callbacks or from the
   `OBS_FRONTEND_EVENT_RECORDING_STARTING` transition.
6. Pause capture on recording pause. Do not pause on
   `OBS_FRONTEND_EVENT_RECORDING_STOPPING`; keep capturing until
   `OBS_FRONTEND_EVENT_RECORDING_STOPPED` so stop-edge frames can reconcile.
7. Close the mask movie on recording stop or split rotation. The RGB recording
   is never decoded or modified by Alpha Recorder.

Raw-video callbacks are used only as cadence evidence for the final OBS video
output. Packet callbacks are used only for encoded packet ordering. The RGB
recording is still never decoded or modified by Alpha Recorder.

Required OBS integration points:

- Frontend events:
  - `OBS_FRONTEND_EVENT_RECORDING_STARTING`
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
- Program alpha extraction:
  - `obs_add_main_rendered_callback()` / `obs_remove_main_rendered_callback()`
  - `obs_get_main_texture()`
  - `gs_texture_create(..., GS_R8, ..., GS_RENDER_TARGET)`
  - `gs_stage_texture()` / `gs_stagesurface_map()`
- Recorded-video cadence:
  - `obs_add_raw_video_callback()` / `obs_remove_raw_video_callback()`
- Encoded-video packet ordering:
  - `obs_output_add_packet_callback()` /
    `obs_output_remove_packet_callback()`
- Automation:
  - obs-websocket vendor registration during `obs_module_post_load()`.
  - Do not call obs-websocket vendor-request unregister APIs from
    `obs_module_unload()`; OBS shutdown can invalidate the cached
    obs-websocket proc handler before Alpha Recorder unloads.

If the main Program texture produces only opaque alpha in a future OBS/runtime
configuration, the fallback is a dedicated render path: render the active
Program scene to an RGBA target that preserves alpha, then feed that into the
same mask movie encoder pipeline.

After OBS stops recording, the plugin finalizes the live alpha mask movie using
the selected finalization format.

## Current Status

Completed:

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
  duplicate the previous alpha frame rather than consuming a newer pending
  alpha frame, packet composition timestamps skip unadmitted startup cadence,
  texture-encoder packets resolve through the observed successor cadence frame,
  and startup duplicates keep their raw content-origin timestamp.
- Live alpha mask movie creation next to the OBS recording.
- Bounded asynchronous mask movie encoding so slow fallback encoders abort the
  alpha output instead of blocking OBS recording.
- Alignment resolution and mask-writer handoff run outside OBS callbacks, and
  the live path queues captured alpha buffers without a second full-frame copy.
- Texture-encoder path classification is cached before packet handling so
  packet callbacks only enqueue packet PTS/CTS evidence.
- Lightweight per-segment performance telemetry for capture/readback,
  alignment-worker batches, writer queue depth, and mask encode timing.
- Alpha mask movie finalization on stop and split rotation.
- File split handling through OBS `file_changed`.
- obs-websocket vendor API for test automation:
  - `alpha_recorder.GetSettings`
  - `alpha_recorder.SetSettings`
- obs-websocket settings coverage for HEVC quality profile, CQ, and preset.
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
- OBS app E2E targets for reproducing decoded sync issues, registered only when
  the matching runtime encoder probe succeeds:
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

Focused unit regression:

```sh
ctest --test-dir out/build/macos-arm64 -C RelWithDebInfo -R alpha_recorder.unit.recording_session_cadence --output-on-failure
```

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

For WSL app E2E, validate that WSLg exposes `DISPLAY`, `WAYLAND_DISPLAY`,
`XDG_RUNTIME_DIR`, and the Wayland socket before launching OBS. Use a native
Linux checkout such as `a native Linux checkout`; avoid building
from a Windows-mounted `/mnt/*` checkout for normal validation.

Sync-bug exposure matrix:

The aggregate target depends only on runnable profiles. Directly building a
skipped target prints its skip reason and does not launch OBS; the configure
summary lists skipped target names and the encoder-open failure that caused each
skip.

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

The CMake target:

- Builds and stages OBS plus the plugin into an isolated app/runtime tree.
- On Linux, stages the installed OBS `bin`, library, plugin, and
  `share/obs` layout and launches `bin/obs` with `LD_LIBRARY_PATH` pointing at
  the staged runtime libraries. Under WSL, the harness defaults
  `QT_QPA_PLATFORM` to `xcb` when it is unset because this pinned OBS build is
  more reliable through XWayland than direct Wayland in SSH-launched runs.
- On macOS, stages an app-style `OBS.app/Contents` tree and launches the
  bundle executable rather than a loose standalone binary.
- Creates an isolated OBS profile and scene collection. On macOS this uses an
  isolated `HOME`/`CFFIXED_USER_HOME`; on Linux it uses isolated `HOME` and
  `XDG_CONFIG_HOME` because the pinned OBS app may not enable portable mode.
- Enables obs-websocket.
- Launches real OBS (`obs64.exe` on Windows, `OBS.app/Contents/MacOS/OBS` on
  macOS, or `bin/obs` for Linux/loose runtimes).
- Enables Alpha Recorder through `CallVendorRequest` using
  `alpha_recorder.SetSettings`.
- Starts and stops OBS recording through obs-websocket.
- Runs a startup sync gate first: five short 2-second recordings in one OBS
  launch, each requiring decoded zero frame-code offset. After those pass, runs
  one 30-second durability recording to catch sustained capture, writer,
  encoder, and stop-edge failures.
- Uses OBS's default hardware-friendly NV12 color format in the app-level E2E
  profile while Alpha Recorder extracts alpha through its own GPU-side path.
- Can run the RGB recording profile with software encoding, explicit NVENC
  HEVC, or explicit AMF HEVC. Hardware RGB matrix targets pair with PNG MOV or
  the same vendor's HEVC alpha output.
- The OBS app E2E target matrix is runtime-aware rather than OS-only. Configure
  probes `hevc_nvenc` and `hevc_amf` on the target machine with a realistic
  1080p FFmpeg encode, registers only matching alpha HEVC targets whose encoder
  opens, additionally requires the OBS NVENC plugin before registering RGB
  NVENC targets, and prints a clear summary of skipped targets and probe
  failures.
- Load matrix targets are `fhd60` (`1920x1080@60`), `wqhd60`
  (`2560x1440@60`), and `4k30` (`3840x2160@30`). `fhd60_rgb_sw_alpha_png_mov` is
  retained as the hardware-independent load baseline; software RGB is not
  registered for WQHD/60 or 4K/30 load targets because those turn into
  machine-pressure checks.
- Cross-vendor hardware pairs are intentionally not registered in the standard
  matrix: RGB NVENC plus alpha AMF, or RGB AMF plus alpha NVENC, require a test
  machine with both NVIDIA and AMD hardware encoders.
- NVIDIA/AMD-specific alpha HEVC targets may appear on any platform where the
  matching encoder opens through the runtime probe. RGB hardware targets are
  also skipped if the staged OBS runtime lacks the matching OBS encoder plugin.
- If OBS logs `Encoding overloaded!`, severe skipped frames due to encoding
  lag, or severe render lag, the app E2E stops recording and fails immediately
  with an overload-specific error instead of continuing to decoded sync
  verification. Tiny skipped-frame summaries are not treated as overload by
  themselves.
- Waits for RGB recording and alpha mask movie outputs.
- Writes `alpha-recorder-performance.json` under the OBS app E2E artifact root,
  containing Alpha Recorder's per-segment performance telemetry log lines for
  comparing capture/readback, alignment, writer-queue, and encode pressure.
- Keeps CMake stdout compact by writing detailed OBS stdout/stderr to
  `obs-process.log` and full probe/result details to `obs-app-summary.json`
  under the artifact root.
- Uses `ffprobe` and `ffmpeg` to confirm both RGB and alpha outputs are playable.
- Adds a test-only moving colored object over a transparent background with an
  opaque binary frame-code strip, then decodes RGB and alpha mask frames for
  PNG MOV and HEVC targets and verifies zero frame-code offset plus moving mask
  bounds frame-by-frame, allowing only small start/terminal/count mismatches.
- Verifies the PNG MOV alpha movie reports `png` and does not use an alpha
  pixel format.
- For HEVC targets, also verifies the alpha output is `.mp4`, `ffprobe` reports
  `hevc`, and the output does not use an alpha pixel format.

CTest can register this slow app-level test when configured with
`ALPHA_RECORDER_ENABLE_OBS_APP_E2E=ON`.

## Open Questions

- Whether all supported OBS/runtime combinations preserve meaningful alpha in
  the main Program texture after rendering.
- Whether the dedicated render fallback is needed for any common production
  scene setup.
- How strict the exported-alpha frame count should be under severe encoder
  backpressure or unusual stop timing.
