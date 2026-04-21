# Usage Guide

This document walks through the plugin from a clean checkout to a verified run.
It covers setup, build, validation, and the shipping OBS workflow.

## What this plugin is

`alpha_recorder` is an OBS recording companion, not a scenario-driven test
module. The shipping workflow is:

- open Tools -> Alpha Recorder Settings
- enable the plugin and choose a Finalization Format
- use OBS Start Recording / Stop Recording as usual

Scenario files under `tests/e2e/scenarios` are E2E-only fixtures. They are not
read by the shipping OBS workflow.

The runtime contract is intentionally narrow:

- `enabled` is stored in OBS user config
- the plugin writes `*.alpha.sidecar` and `*.alpha.manifest.json` beside the
  recording
- on stop, and whenever OBS signals `file_changed` for a split recording, the
  plugin finalizes the sidecar and manifest, then exports the selected
  finalization format when it is supported
- if export fails, the raw sidecar and manifest remain on disk for manual
  recovery

## 1. Prerequisites

You need the following on Windows:

- Windows 10 or Windows 11
- Visual Studio 2022 or Build Tools 2022
- CMake
- PowerShell 7 (`pwsh`)
- Git
- a real OBS developer tree, or a tree staged from the pinned OBS source

Important: the regular OBS runtime installation is not enough. A path such as
`C:\Program Files\obs-studio` is useful as a runtime, but it does not contain
the headers and import libraries required to build this plugin.

## 2. Get the repository

Clone the repo and initialize the OBS source submodule:

```powershell
git clone <repository-url>
cd obs-alpha-recorder-plugin
git submodule update --init --recursive
```

The OBS source checkout lives in `deps/obs/obs-studio`. It is tracked as a git
submodule, so the repository can pin the upstream OBS source version used by
this project.

## 3. Prepare an OBS developer tree

You have two supported options.

### Option A: Build and stage OBS from source

This is the recommended path when you want the repo to prepare its own OBS
developer tree.

```powershell
pwsh .\tools\bootstrap_obs.ps1 -BuildFromSource
```

This command uses the pinned source in `deps/obs/obs-studio`, builds OBS under
`deps/obs/obs-build`, and stages a developer tree at
`deps/obs/obs-build/rundir/RelWithDebInfo`.

When the bootstrap finishes, it also writes `deps/obs/obs-root.cmake`. That file
tells CMake which OBS tree to use for later configure and build steps.

### Option B: Use an existing OBS developer tree

If you already have a compatible OBS developer tree, point the bootstrap script
at it:

```powershell
pwsh .\tools\bootstrap_obs.ps1 -ObsRoot C:\path\to\obs-developer-tree
```

The path must contain all of the following:

- `bin\64bit`
- `data`
- `obs-plugins\64bit`
- libobs headers and import libraries

Again, this must be a developer tree, not just a runtime install.

## 4. Configure and build

Once OBS is available, configure and build the plugin:

```powershell
cmake --preset windows-x64-msvc
cmake --build --preset windows-x64-msvc-relwithdebinfo
```

The RelWithDebInfo build produces these executables and libraries under
`out\build\windows-x64-msvc\bin\RelWithDebInfo`:

- `alpha_recorder.dll`
- `alpha_recorder_frontend.dll`
- `alpha_recorder_e2e_host.exe`
- `alpha_recorder_test_encoder.exe`
- `alpha_recorder_unit_pair_gate.exe`
- `alpha_recorder_unit_sidecar_writer.exe`

The build also copies both plugin DLLs into the OBS tree so the host process can
load them later:

- `${OBS_ROOT}\obs-plugins\64bit\alpha_recorder.dll`
- `${OBS_ROOT}\obs-plugins\64bit\alpha_recorder_frontend.dll`

### Install into a stock OBS release

If you want to copy the plugin into an existing OBS release, stage the
RelWithDebInfo output and copy both module DLLs from the staged plugin
directory.

```powershell
pwsh .\tools\stage_obs_tree.ps1
```

The staging helper now defaults to RelWithDebInfo, so no extra configuration
flag is needed.

Copy these files from `out\stage\obs\obs-plugins\64bit` into your OBS
installation's `obs-plugins\64bit` folder:

- `alpha_recorder.dll`
- `alpha_recorder_frontend.dll`

Do not use Debug output for a stock OBS install. The frontend module depends on
debug Qt and debug CRT DLLs that are not shipped with a normal OBS release.

## 5. Run the unit tests

The unit tests cover the core pair gate and sidecar writer logic. Run them with
CTest:

```powershell
ctest --test-dir .\out\build\windows-x64-msvc -C RelWithDebInfo -L unit --output-on-failure
```

Use this step to confirm the basic library logic before moving on to E2E.

## 6. Run the E2E flow

The recommended way to exercise the plugin end to end is the wrapper script:

```powershell
pwsh .\tools\run_e2e.ps1 -Configuration RelWithDebInfo
```

That script stages the OBS tree if needed, prepends the staged `bin\64bit`
directory to `PATH`, sets the environment that the host and verifier expect, and
then runs the `e2e` CTest label.

If you already have a staged tree and want to run CTest directly, use:

```powershell
ctest --test-dir .\out\build\windows-x64-msvc -C RelWithDebInfo -L e2e --output-on-failure
```

If you want to debug the flow manually, run the host and verifier yourself. This
is useful when you want to inspect the files between the two steps or attach a
debugger to the host process.

```powershell
.\out\build\windows-x64-msvc\bin\RelWithDebInfo\alpha_recorder_e2e_host.exe `
    --scenario .\tests\e2e\scenarios\basic_pair.scenario `
    --stage-dir .\deps\obs\obs-build\rundir\RelWithDebInfo `
    --artifact-root .\out\artifacts\alpha_recorder
```

After the host completes, run the verifier against the same artifact root:

```powershell
.\out\build\windows-x64-msvc\bin\RelWithDebInfo\alpha_recorder_test_encoder.exe `
    --scenario .\tests\e2e\scenarios\basic_pair.scenario `
    --artifact-root .\out\artifacts\alpha_recorder
```

The shipped scenarios are:

- `tests/e2e/scenarios/basic_pair.scenario`
- `tests/e2e/scenarios/drop_backpressure.scenario`

These files are E2E-only and are not part of the shipping OBS recording path.

The default E2E artifact root is under the build tree:

- `out\build\windows-x64-msvc\Testing\Temporary\alpha_recorder_e2e\<scenario>`

## 7. Understand the output contract

When Enabled is on, a normal OBS recording produces three related artifacts:

- the main recording file that OBS already writes
- `*.alpha.sidecar` for the alpha records
- `*.alpha.manifest.json` for the session summary

The sidecar is finalized with an index/footer on stop and on OBS `file_changed`
split events. The manifest is written beside it and renamed into place so a
failed finalize does not truncate the last good manifest.

After each finalization, the plugin tries to export a final alpha movie beside
the sidecar.

- Apple ProRes 4444 is the supported export path and writes `.mov`
- Lossless HEVC is listed in the settings UI, but it is disabled because the
  bundled exporter does not support it yet

If export fails, keep the raw sidecar and manifest for manual recovery. The main
OBS recording is left alone.

## 8. Recover the output

If finalization or export fails, look for the recording pair next to the OBS
video file:

- `MyRec.alpha.sidecar`
- `MyRec.alpha.manifest.json`

Those files are the recovery source. The manifest records the chosen
finalization format, the sidecar path, the pair count, and a `status_flags`
array that may include `ERR_OVERLOAD` when capture could not keep up.

If an old OBS config still says `finalization_format=lossless_hevc`, the loader
normalizes it back to the supported default. The visible fallback is ProRes
4444.

## 9. Inspect the artifacts

After a successful run, the most important locations are:

- build outputs: `out\build\windows-x64-msvc\bin\RelWithDebInfo`
- staged OBS plugin DLLs: `out\stage\obs\obs-plugins\64bit`
- E2E artifacts:
  `out\build\windows-x64-msvc\Testing\Temporary\alpha_recorder_e2e`

The live recording workflow writes the sidecar and manifest beside the main OBS
recording path. The E2E harness writes its own scenario-specific artifacts under
the build tree.

## 10. Common problems

### `Could NOT find OBS`

The CMake configure step could not locate a valid OBS developer tree. Make sure
`deps/obs/obs-root.cmake` exists or set `OBS_ROOT` to a real developer tree.

### Finalization format is disabled

Lossless HEVC is visible in the settings dialog, but the bundled exporter does
not support it yet. Use ProRes 4444 for the shipping path.

### Alpha-preserving video format is required

The runtime path needs OBS to use an alpha-preserving video format such as BGRA
or RGBA. If the plugin refuses to start the alpha session, check the OBS video
configuration.

### `stage dir is missing the alpha_recorder plugin`

The plugin DLLs were not copied into the OBS tree yet. Re-run the RelWithDebInfo
build so the post-build copy step can stage `alpha_recorder.dll` and
`alpha_recorder_frontend.dll`.

### `C:\Program Files\obs-studio` does not work

That path is a runtime installation, not a developer tree. It is fine for
running OBS, but not enough for building this plugin or loading the headers and
import libraries required by CMake.

### The host cannot load OBS DLLs

Use `tools/run_e2e.ps1` or make sure the staged `bin\64bit` directory is on
`PATH` before starting the E2E host.

### The verifier cannot find artifacts

Check the scenario file and the `artifact_root` path. The verifier expects the
host to have already written the RGB, sidecar, and manifest files.

## 11. Fast start checklist

If you just want the shortest successful path, run these commands in order:

```powershell
git submodule update --init --recursive
pwsh .\tools\bootstrap_obs.ps1 -BuildFromSource
cmake --preset windows-x64-msvc
cmake --build --preset windows-x64-msvc-relwithdebinfo
pwsh .\tools\run_e2e.ps1 -Configuration RelWithDebInfo
```

If all five commands succeed, the plugin is ready and the full start-to-finish
workflow has been verified.
