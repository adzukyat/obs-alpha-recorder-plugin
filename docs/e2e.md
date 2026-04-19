# E2E

The E2E harness is deterministic and repo-local. It runs the scenario through a
DLL boundary, writes real artifacts, and validates them on disk.

## Current flow

- The host loads the E2E module DLL and invokes the exported scenario runner.
- The runner uses the pair gate and sidecar writer to produce a raw RGB
  artifact, an alpha sidecar, and a manifest JSON beside the sidecar.
- The verifier parses the RGB file, sidecar container, and manifest contents and
  compares them to the scenario expectations.

If libobs is unavailable, the repository still builds and exercises the
test-only E2E module export. The real OBS plugin target is still conditional on
OBS discovery.

## Scenario format

Scenario files are simple `key=value` files. The E2E harness currently expects:

- `name`
- `expected_pair_count`
- `expected_drop_count`
- `output_root`
- `rgb_artifact`
- `alpha_sidecar`
- `alpha_manifest`

The shipped scenarios cover:

- `tests/e2e/scenarios/basic_pair.scenario` for the happy path
- `tests/e2e/scenarios/drop_backpressure.scenario` for the drop/backpressure
  path

## Running

The repo-local entrypoint is CTest:

```powershell
ctest --test-dir .\out\build\windows-x64-msvc -C Debug -L e2e --output-on-failure
```

`tools/run_e2e.ps1` remains available when you want to stage an OBS tree first,
but the E2E tests themselves do not require a GUI OBS session.

## Environment variables

The E2E binaries only read environment variables as fallbacks when the matching
CLI argument is omitted:

- Host:
  - `ALPHA_RECORDER_E2E_ARTIFACT_ROOT`
  - `ALPHA_RECORDER_E2E_MODULE`
  - `ALPHA_RECORDER_STAGE_DIR`
- Verifier:
  - `ALPHA_RECORDER_E2E_ARTIFACT_ROOT`
  - `ALPHA_RECORDER_STAGE_DIR`

`tools/run_e2e.ps1` sets up the child-process environment before invoking CTest:

- Always sets `ALPHA_RECORDER_STAGE_DIR`
- Always sets `ALPHA_RECORDER_SCENARIO_DIR`
- When `-ObsRoot` is supplied, it also sets `ALPHA_RECORDER_OBS_ROOT` and
  `OBS_ROOT`

The script's `-ObsRoot` parameter defaults to the current `OBS_ROOT` environment
value when present.

## Stage layout

When an OBS tree is staged, the helper still uses the standard layout:

- `bin/64bit`
- `data`
- `obs-plugins/64bit`

That keeps the OBS-side packaging path intact while the repo-local fallback
module covers deterministic E2E coverage in environments without libobs.
