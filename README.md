# alpha_recorder_plugin

`alpha_recorder` is an OBS native plugin that follows the normal OBS recording
lifecycle. Enable it from Tools -> Alpha Recorder Settings, then use OBS Start
Recording / Stop Recording as usual. When enabled, it captures alpha-preserving
Program frames and writes a playable grayscale alpha-mask movie beside each
recording while OBS records the normal RGB video. On stop, and whenever OBS
signals `file_changed` for a split recording, it finalizes the current mask
movie segment.

Supported mask formats are lossless PNG MOV (`mask_png_mov`, `.mov`), HEVC
NVENC (`mask_hevc_nvenc`, `.mp4`), and HEVC AMF (`mask_hevc_amf`, `.mp4`). The
mask movie is 8-bit grayscale content encoded as visible luma, not a video with
an alpha channel.

Scenario files live only under `tests/e2e/scenarios`. They are inputs for the
deterministic E2E harness, not part of the shipping plugin path.

Settings can also be driven by the obs-websocket vendor API for automated tests:

- Vendor: `alpha_recorder`
- Requests: `GetSettings`, `SetSettings`

The OBS module target and the E2E harness both require a real OBS developer
tree. `OBS_ROOT` must point to a tree with libobs headers, import libraries,
`bin/64bit`, and `data`. The OBS source checkout is tracked as a git submodule
at `deps/obs/obs-studio`; when bootstrapped from source, that staged developer
tree lives under `deps/obs/obs-build/rundir/RelWithDebInfo` alongside the pinned
source and build trees.

The current tree is intentionally minimal. It provides:

- OBS recording lifecycle hooks plus a Tools menu settings dialog
- a core static library for pair gating, legacy sidecar primitives, settings,
  and live mask movie encoding
- unit test executables registered with CTest
- deterministic E2E executables and CMake-native staging helpers
- a cross-platform OBS app E2E path driven by CMake and obs-websocket
- CMake presets for Windows x64 MSVC and macOS

## Build

The preferred path is to point CMake at a real OBS developer tree that contains
libobs headers, libraries, runtime DLLs, and plugin directories.

1. Initialize the OBS source submodule:

```powershell
git submodule update --init --recursive
```

2. Produce a real OBS developer tree from source:

```sh
cmake -DREPO_ROOT="$PWD" -DBUILD_FROM_SOURCE=ON -P cmake/scripts/BootstrapObs.cmake
```

On macOS this uses OBS Studio's required Xcode generator for the OBS dependency
build. On Windows it uses Visual Studio by default.

If you already have an OBS source checkout, use `-DBUILD_FROM_SOURCE=ON` to
stage it into the OBS build tree's runtime prefix without recloning. If you want
the bootstrap script to refresh the submodule checkout from the pinned OBS tag,
use `-DCLONE_SOURCE=ON`. If you already have a real OBS developer tree, pass its
root with `-DOBS_ROOT=...` to validate it and write
`deps/obs/obs-root.cmake`.

2. Configure and build:

```powershell
cmake --preset windows-x64-msvc
cmake --build --preset windows-x64-msvc-relwithdebinfo
```

On macOS, use the matching macOS preset:

```sh
cmake --preset macos-arm64
cmake --build --preset macos-arm64-relwithdebinfo
```

The default preset fails fast if `OBS_ROOT` does not resolve to a real developer
tree.

To stage a portable OBS tree with Alpha Recorder overlaid, build the
`alpha_recorder_stage_obs_tree` target:

```sh
cmake --build --preset windows-x64-msvc-relwithdebinfo --target alpha_recorder_stage_obs_tree
```

On Windows, the staged plugin DLLs land under
`out/stage/obs/obs-plugins/64bit`. On macOS, bundles land under
`out/stage/obs/obs-plugins`.

## Test

Unit tests are regular CTest executables.

```powershell
ctest --test-dir .\out\build\windows-x64-msvc -C RelWithDebInfo -L unit --output-on-failure
```

E2E tests are deterministic CTest executables. The host loads the staged
`alpha_recorder_e2e.dll` module, produces RGB raw and alpha mask artifacts,
and the verifier parses those files rather than checking file existence alone.
The scenario files under `tests/e2e/scenarios` are E2E-only inputs.

```powershell
ctest --test-dir .\out\build\windows-x64-msvc -C RelWithDebInfo -L e2e --output-on-failure
```

The E2E host starts libobs, loads the staged `alpha_recorder_e2e.dll` module,
and the verifier checks the generated RGB and alpha mask artifacts.

The CMake target `alpha_recorder_run_e2e` stages the OBS tree and runs the
deterministic E2E CTest label with the staged runtime on the environment path.

```sh
cmake --build --preset windows-x64-msvc-relwithdebinfo --target alpha_recorder_run_e2e
```

The real OBS app E2E path launches portable OBS, enables Alpha Recorder through
obs-websocket, starts and stops recording, then verifies the RGB recording,
and exported alpha mask movie.

```sh
cmake --build --preset windows-x64-msvc-relwithdebinfo --target alpha_recorder_run_obs_app_e2e
```

On macOS:

```sh
cmake --build --preset macos-arm64-relwithdebinfo --target alpha_recorder_run_obs_app_e2e
```

CTest can register this slow app-level test when configured with
`ALPHA_RECORDER_ENABLE_OBS_APP_E2E=ON`.

The app-level E2E is cross-platform at the harness layer: CMake launches
`cmake/scripts/RunObsAppE2E.cmake`, which stages OBS and calls a Bun
obs-websocket client. Platform-specific differences are limited to executable
and runtime layout resolution (`obs64.exe` on Windows, `MacOS/OBS` or `bin/obs`
on macOS). The staged OBS root still needs the platform's real OBS runtime,
plugins, FFmpeg tools, obs-websocket plugin, and `bun` on PATH.

## OBS/libobs resolution

- `deps/obs/obs-root.cmake` is an optional CMake fragment generated by
  `cmake/scripts/BootstrapObs.cmake`.
- `OBS_ROOT` may also be supplied through the environment or the CMake preset.
- `cmake/FindOBS.cmake` looks for libobs headers and import libraries under the
  configured root and creates an imported `OBS::libobs` target when found.

## Status

Implemented in the core library and live OBS workflow:

- pair admission logic with all-or-nothing frame-pair acceptance
- live alpha mask movie encoding as 8-bit grayscale PNG MOV or HEVC
- OBS recording lifecycle hooks, settings persistence, Tools menu integration,
  and obs-websocket vendor automation
- raw Program frame capture through OBS's alpha-preserving video callback path
- split recording handling through OBS `file_changed`
- deterministic E2E scenarios that validate RGB raw artifacts, alpha mask
  artifacts, and split-rotation behavior through the OBS module boundary
- cross-platform OBS app E2E harness that verifies RGB and alpha mask movie
  outputs

The test-only scenario path remains confined to the E2E harness; the shipping
plugin uses OBS Start Recording / Stop Recording plus Tools -> Alpha Recorder
Settings.
