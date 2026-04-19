# Usage Guide

This document walks through the plugin from a clean checkout to a verified run.
It covers setup, build, validation, and the runtime contract exposed by the
module.

## What this plugin is

`alpha_recorder` is an OBS output module, not a traditional UI plugin. It does
not add a dock, menu item, or source filter. Instead, it registers an output
named `alpha_recorder_output` that accepts a scenario file and an output
directory, runs the scenario, and writes artifacts to disk.

The current module contract is intentionally narrow:

- `scenario_path` points to a scenario file
- `artifact_root` points to a writable output directory
- the module writes a raw RGB artifact, an alpha sidecar, and a manifest JSON

If you want to use the plugin from another OBS-based application, you load the
module, create `alpha_recorder_output`, set those two settings, and start the
output.

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
cmake --build --preset windows-x64-msvc-debug
```

The build produces these executables and libraries under
`out\build\windows-x64-msvc`:

- `alpha_recorder.dll`
- `alpha_recorder_e2e_host.exe`
- `alpha_recorder_test_encoder.exe`
- `alpha_recorder_unit_pair_gate.exe`
- `alpha_recorder_unit_sidecar_writer.exe`

The build also copies the plugin DLL into the OBS tree so the host process can
load it later:

- `${OBS_ROOT}\obs-plugins\64bit\alpha_recorder.dll`

## 5. Run the unit tests

The unit tests cover the core pair gate and sidecar writer logic. Run them with
CTest:

```powershell
ctest --test-dir .\out\build\windows-x64-msvc -C Debug -L unit --output-on-failure
```

Use this step to confirm the basic library logic before moving on to E2E.

## 6. Run the E2E flow

The recommended way to exercise the plugin end to end is the wrapper script:

```powershell
pwsh .\tools\run_e2e.ps1
```

That script stages the OBS tree if needed, prepends the staged `bin\64bit`
directory to `PATH`, sets the environment that the host and verifier expect, and
then runs the `e2e` CTest label.

If you already have a staged tree and want to run CTest directly, use:

```powershell
ctest --test-dir .\out\build\windows-x64-msvc -C Debug -L e2e --output-on-failure
```

If you want to debug the flow manually, run the host and verifier yourself. This
is useful when you want to inspect the files between the two steps or attach a
debugger to the host process.

```powershell
.\out\build\windows-x64-msvc\bin\Debug\alpha_recorder_e2e_host.exe `
    --scenario .\tests\e2e\scenarios\basic_pair.scenario `
    --stage-dir .\deps\obs\obs-build\rundir\RelWithDebInfo `
    --artifact-root .\out\artifacts\alpha_recorder
```

After the host completes, run the verifier against the same artifact root:

```powershell
.\out\build\windows-x64-msvc\bin\Debug\alpha_recorder_test_encoder.exe `
    --scenario .\tests\e2e\scenarios\basic_pair.scenario `
    --artifact-root .\out\artifacts\alpha_recorder
```

The shipped scenarios are:

- `tests/e2e/scenarios/basic_pair.scenario`
- `tests/e2e/scenarios/drop_backpressure.scenario`

The default E2E artifact root is under the build tree:

- `out\build\windows-x64-msvc\Testing\Temporary\alpha_recorder_e2e\<scenario>`

## 7. Understand the output contract

The output module is one-shot and scenario-driven. When it starts, it reads the
scenario file, generates the artifacts, and writes them to the requested output
directory.

The output set is always the same:

- `rgb.raw` for the raw RGB stream
- `alpha.sidecar` for the alpha sidecar container
- `alpha.manifest.json` for the session summary

The scenario file controls the expected pair count, expected drop count, and
artifact names. The verifier checks that the generated files match those
expectations, rather than just checking that the files exist.

## 8. Use the module from your own OBS-based app

If you want to embed the plugin into another OBS-based process, load the module
the same way the E2E host does and then create the output instance.

The important settings are:

- `scenario_path`
- `artifact_root`

Minimal example:

```cpp
obs_data_t *settings = obs_data_create();
obs_data_set_string(settings, "scenario_path", "C:/work/scenarios/basic_pair.scenario");
obs_data_set_string(settings, "artifact_root", "C:/work/alpha_recorder_output");

obs_output_t *output = obs_output_create("alpha_recorder_output", "alpha_recorder", settings, nullptr);
obs_data_release(settings);

if (output != nullptr) {
    if (!obs_output_start(output)) {
        const char *error = obs_output_get_last_error(output);
        // Handle the failure message.
    }
}
```

The module validates both settings at start time. If either one is missing, the
start call fails and the output never begins.

## 9. Inspect the artifacts

After a successful run, the most important locations are:

- build outputs: `out\build\windows-x64-msvc\bin\Debug`
- staged OBS plugin: `${OBS_ROOT}\obs-plugins\64bit`
- E2E artifacts:
  `out\build\windows-x64-msvc\Testing\Temporary\alpha_recorder_e2e`

Within each scenario directory, you should see the RGB file, sidecar, and
manifest together.

## 10. Common problems

### `Could NOT find OBS`

The CMake configure step could not locate a valid OBS developer tree. Make sure
`deps/obs/obs-root.cmake` exists or set `OBS_ROOT` to a real developer tree.

### `stage dir is missing the alpha_recorder plugin`

The plugin DLL was not copied into the OBS tree yet. Re-run the build so the
post-build copy step can stage `alpha_recorder.dll`.

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
cmake --build --preset windows-x64-msvc-debug
pwsh .\tools\run_e2e.ps1
```

If all five commands succeed, the plugin is ready and the full start-to-finish
workflow has been verified.
