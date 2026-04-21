# Architecture

This scaffold keeps the shipping OBS workflow and the E2E harness split into
clear layers so the implementation can land without restructuring the build.

## Layers

### `include/alpha_recorder`

Public headers shared across the project. These headers currently expose only
minimal state containers and module hooks so the build stays coherent.

### `src/core`

Core logic for pair handling and sidecar writing. This layer now owns the
frame-pair admission gate, the alpha sidecar container, and manifest summary
serialization.

### `src/obs`

The shipping OBS module wrapper. This is where the libobs entry points, the
Tools -> Alpha Recorder Settings dialog, and the recording lifecycle hooks live.

### `tests/unit`

Small self-check executables that verify the core scaffolding builds and runs
under CTest.

### `tests/e2e`

The deterministic E2E harness lives here. The host starts libobs, loads the
staged test module through the OBS boundary, and the harness writes RGB,
sidecar, and manifest artifacts that the verifier checks end to end. The
scenario files in this tree are test-only inputs and do not participate in the
shipping path.

### `tools`

PowerShell helpers for bootstrapping OBS configuration, staging a test tree, and
running E2E tests.

### `cmake`

Project-wide build helpers:

- `FindOBS.cmake` discovers libobs and creates an imported target
- `ProjectOptions.cmake` centralizes language and output settings
- `Warnings.cmake` applies target warning flags
- `FetchDeps.cmake` provides a hook for future test dependency fetching

## Build intent

The project is an OBS native plugin, not a standalone recorder. The shipping
workflow is OBS Start Recording / Stop Recording plus Tools -> Alpha Recorder
Settings. The core library exists only to isolate pair handling, sidecar
writing, manifest writing, and export concerns from the OBS module wrapper.

The OBS module and E2E harness are only enabled when a valid OBS developer tree
is supplied. If `OBS_ROOT` does not point at a real tree with libobs headers,
import libraries, `bin/64bit`, and `data`, configuration fails instead of
silently falling back to a substitute path.

`tools/bootstrap_obs.ps1` can validate an existing staged developer tree or
fetch, build, and stage the pinned OBS tag into
`deps/obs/obs-build/rundir/RelWithDebInfo` before writing the
`deps/obs/obs-root.cmake` fragment consumed by CMake. The source checkout stays
in `deps/obs/obs-studio`, the build tree stays in `deps/obs/obs-build`, and the
staged runtime/developer tree lives under the build tree's `rundir` prefix.
