# alpha_recorder_plugin

`alpha_recorder` is an OBS native plugin that follows the normal OBS recording
lifecycle. Enable it from Tools -> Alpha Recorder Settings, then use OBS Start
Recording / Stop Recording as usual. When enabled, it extracts the Program
texture's alpha on the GPU and writes a playable grayscale alpha-mask movie
beside each recording while OBS records the normal RGB video. On stop, and
whenever OBS signals `file_changed` for a split recording, it finalizes the
current mask movie segment.

Supported mask formats are lossless PNG MOV (`mask_png_mov`, `.mov`), HEVC
NVENC (`mask_hevc_nvenc`, `.mp4`), and HEVC AMF (`mask_hevc_amf`, `.mp4`). The
mask movie is 8-bit grayscale content encoded as visible luma, not a video with
an alpha channel.

Alpha Recorder defaults to enabled. If no finalization format has been saved,
it prefers an available hardware HEVC encoder and falls back to PNG MOV. HEVC
options are available only when the matching runtime encoder can actually open
on the current machine.

Mask encoding runs behind a bounded asynchronous queue. If the selected mask
encoder cannot keep up, Alpha Recorder aborts the mask movie output instead of
blocking or slowing the main OBS recording. OBS's normal NV12/P010 hardware
encoder path does not need to be changed to RGBA for alpha capture. Alpha
Recorder tracks OBS's final raw-video output cadence so repeated/dropped RGB
output frames are mirrored in the alpha mask movie. Encoded packet composition
timestamps identify the first raw-video cadence frame actually admitted into
the RGB recording, without encoder-specific fixed startup offsets. Texture
encoders use the next observed raw-video cadence frame after the packet CTS so
the alpha movie follows the rendered texture that OBS actually queued. OBS
callbacks only enqueue cadence and packet-ordering evidence; Alpha Recorder
resolves aligned mask frames on its own worker and hands captured buffers to the
mask writer without a second full-frame copy. The live packet path uses a cached
texture-encoder classification; it does not query OBS encoder/mix texture state
from packet callbacks or the recording-starting transition.

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

Each run also records lightweight performance telemetry from the live alpha
pipeline. The plugin logs one per-segment summary covering capture/readback CPU
time, GPU submission timing, alignment-worker batches, writer queue depth, and
mask encode timing; the OBS app E2E harness copies those summaries into
`alpha-recorder-performance.json` under the run artifact root. CMake stdout is
kept compact: detailed OBS stdout/stderr is written to `obs-process.log`, and
full probe data is written to `obs-app-summary.json` in the artifact root.

Each OBS app E2E target first runs a startup sync gate: five short 2-second
recordings in one OBS launch, each requiring decoded zero frame-code offset.
After those pass, the same OBS launch runs one 30-second durability recording
to catch sustained capture, writer, encoder, and stop-edge failures.

```sh
cmake --build --preset windows-x64-msvc-relwithdebinfo --target alpha_recorder_run_obs_app_e2e
```

OBS app E2E targets are named by the actual RGB encoder profile and alpha
format. Load variants use `fhd60` (`1920x1080@60`), `wqhd60`
(`2560x1440@60`), and `4k30` (`3840x2160@30`). Software RGB encoding is omitted
only from WQHD/60 and 4K/30 load variants because those turn into machine-pressure
checks, but `fhd60_rgb_sw_alpha_png_mov` is retained as the hardware-independent
load baseline. Cross-vendor hardware pairs such as RGB NVENC with alpha AMF are
also omitted because they require both NVIDIA and AMD encoders on the same test
machine. The matrix is runtime-aware: configure probes `hevc_nvenc` and
`hevc_amf` on the target machine with a realistic 1080p FFmpeg encode, registers
only the matching NVENC or AMF targets, and prints a configure-time summary of
targets skipped because the encoder could not open.

The aggregate `alpha_recorder_run_obs_app_e2e` target depends only on runnable
targets. Directly building a skipped target prints its skip reason and does not
launch OBS; rerun configure on a machine where the matching encoder opens to run
that profile.

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
- live alpha mask movie encoding as 8-bit grayscale PNG MOV or HEVC, behind a
  bounded asynchronous writer queue
- OBS recording lifecycle hooks, settings persistence, Tools menu integration,
  and obs-websocket vendor automation
- GPU-side Program alpha extraction that keeps OBS's normal recording color
  format independent of Alpha Recorder, with staged readback buffered through a
  small staging-surface ring
- raw-video cadence tracking that mirrors OBS duplicate/drop behavior in the
  alpha mask movie without encoder-specific fixed frame offsets
- off-callback alignment resolution and no-copy live mask-frame handoff into
  the bounded writer queue
- cached texture-encoder path classification so OBS packet callbacks only carry
  packet PTS/CTS evidence and do not enter encoder/mix texture queries
- per-segment live-path performance telemetry for capture/readback, alignment,
  writer queue depth, and mask encode timing, exported by OBS app E2E artifacts
- focused unit regression coverage for repeated raw-video output frames,
  proving the alpha mask duplicates the previous frame instead of consuming a
  newer pending frame, plus runtime packet-CTS admission and texture-encoder
  successor-cadence coverage for startup alignment
- split recording handling through OBS `file_changed`
- deterministic E2E scenarios that validate RGB raw artifacts, alpha mask
  artifacts, and split-rotation behavior through the OBS module boundary
- cross-platform OBS app E2E harness that verifies RGB and alpha mask movie
  outputs, including zero frame-code offset plus frame-by-frame sync between a
  moving colored object and its grayscale alpha mask on PNG MOV and HEVC paths,
  with only small start/terminal/count mismatches tolerated
- named OBS app E2E matrix targets for software RGB, explicit NVENC HEVC RGB,
  explicit AMF HEVC RGB, and hardware RGB FHD/60, WQHD/60, and 4K/30 variants
  when the matching encoder opens on the target machine;
  software RGB is omitted from load variants
- OBS app E2E aborts with a clear overload error if OBS logs
  `Encoding overloaded!`, severe skipped frames due to encoding lag, or severe
  render lag, so machine pressure is not reported as a sync failure while tiny
  skipped-frame summaries do not stop an otherwise clean run

The test-only scenario path remains confined to the E2E harness; the shipping
plugin uses OBS Start Recording / Stop Recording plus Tools -> Alpha Recorder
Settings.
