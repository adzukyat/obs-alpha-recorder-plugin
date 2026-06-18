# AGENTS.md

This is the working guide for agents editing this repository. Keep it in sync
with behavior, commands, and project decisions.

## Documentation Map

- [`README.md`](README.md) is for users: what Alpha Recorder does, how to use
  it, and the shortest practical build/test path.
- [`SPECS.md`](SPECS.md) is the technical contract: architecture, settings,
  runtime behavior, validation matrix, and open questions.
- `AGENTS.md` is the agent guide: project shape, editing rules, and the commands
  most likely to matter while working.

## Project Summary

Alpha Recorder is an OBS plugin that follows the normal OBS recording lifecycle:

> When the user records in OBS, also write a separate grayscale alpha-mask movie
> aligned with the recorded video.

The alpha movie is a standalone grayscale mask video. The captured alpha values
are encoded as visible pixel intensity; the file is not expected to carry its
own transparency channel.

## User-Facing Contract

- Users start and stop recording with OBS's normal controls.
- The plugin operates silently when enabled.
- There is no separate "Start Alpha Recording" button.
- Capture target is Program output, not Preview.
- If alignment or mask writing cannot keep up, keep alpha output time-aligned by
  repeating the previous alpha frame rather than aborting, and do not stop or
  slow the main OBS recording.
- Alpha output is written beside the OBS recording. For example:
  - `.alpha.mov` for PNG MOV.
  - `.alpha.mp4` for HEVC NVENC/AMF.

## Editing Guardrails

- Prefer CMake-native orchestration and existing presets/targets over wrapper
  scripts.
- Keep OBS's normal recording color format independent of Alpha Recorder. Do not
  require switching OBS globally to RGBA.
- Preserve runtime-aware HEVC handling: expose NVENC/AMF options only when the
  matching encoder can actually open on the current machine.
- Preserve the explicit NVENC and AMF split in settings, target names, and docs.
- For sync changes, preserve the alignment contract in `SPECS.md`: raw-video
  cadence is authoritative, packet callbacks carry ordering evidence, and the
  app E2E verifier requires zero decoded frame-code offset.
- If a target is unavailable, prefer a clear runtime skip reason over an
  impossible or trap target.

## Common Commands

Bootstrap the pinned OBS source/runtime tree:

```sh
git submodule update --init --recursive
cmake -DREPO_ROOT="$PWD" -DBUILD_FROM_SOURCE=ON -P cmake/scripts/BootstrapObs.cmake
```

Build on macOS:

```sh
cmake --preset macos-arm64
cmake --build --preset macos-arm64-relwithdebinfo
```

Build on Windows:

```powershell
cmake --preset windows-x64-msvc
cmake --build --preset windows-x64-msvc-relwithdebinfo
```

Build on Linux/WSL:

```sh
cmake --preset linux-x64
cmake --build --preset linux-x64-relwithdebinfo
```

Run the focused cadence regression:

```sh
ctest --test-dir out/build/macos-arm64 -C RelWithDebInfo -R alpha_recorder.unit.recording_session_cadence --output-on-failure
```

Run deterministic E2E:

```sh
cmake --build --preset macos-arm64-relwithdebinfo --target alpha_recorder_run_e2e
```

Run real OBS app E2E:

```sh
cmake --build --preset macos-arm64-relwithdebinfo --target alpha_recorder_run_obs_app_e2e
cmake --build --preset windows-x64-msvc-relwithdebinfo --target alpha_recorder_run_obs_app_e2e
cmake --build --preset linux-x64-relwithdebinfo --target alpha_recorder_run_obs_app_e2e
```

Package the built user plugin for release:

```sh
cmake -DREPO_ROOT="$PWD" -DBUILD_DIR="$PWD/out/build/macos-arm64" -DPACKAGE_DIR="$PWD/out/package/alpha-recorder" -P cmake/scripts/PackagePlugin.cmake
```
