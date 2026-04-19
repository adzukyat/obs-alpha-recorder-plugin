# Architecture

This scaffold keeps the plugin split into clear layers so the later
implementation can land without restructuring the build.

## Layers

### `include/alpha_recorder`

Public headers shared across the project. These headers currently expose only
minimal state containers and module hooks so the build stays coherent.

### `src/core`

Core logic for pair handling and sidecar writing. This layer now owns the
frame-pair admission gate, the alpha sidecar container, and manifest summary
serialization.

### `src/obs`

The OBS module wrapper. This is where libobs entry points and OBS-facing glue
belong once the recording pipeline is implemented.

### `tests/unit`

Small self-check executables that verify the core scaffolding builds and runs
under CTest.

### `tests/e2e`

The deterministic repo-local E2E harness lives here. The host loads the E2E
module DLL, the module export writes real RGB, sidecar, and manifest artifacts,
and the verifier checks those files end to end.

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

The project is an OBS native plugin, not a standalone recorder. The core library
exists only to isolate pair handling and sidecar concerns from the OBS module
wrapper.

The plugin target is enabled when a valid OBS root is supplied. Until then, the
rest of the tree remains buildable so the scaffold stays usable while the
dependency is being acquired or staged.
