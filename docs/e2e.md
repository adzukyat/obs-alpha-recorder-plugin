# E2E

The E2E harness is deterministic and repo-local. It runs the scenario through
the real OBS module boundary, writes real artifacts, and validates them on disk.

## Current flow

- The host starts libobs, loads the staged `alpha_recorder_output` module, and
  starts the output through the OBS API.
- The module uses the pair gate and sidecar writer to produce a raw RGB
  artifact, an alpha sidecar, and a manifest JSON beside the sidecar.
- The verifier parses the RGB file, sidecar container, and manifest contents and
  compares them to the scenario expectations.

If OBS is unavailable, the default configure fails. The E2E path only runs
through the real OBS output module and libobs APIs.

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

The E2E binaries accept command-line arguments first and only use a small set of
environment variables when the matching CLI argument is omitted:

- Host:
  - `ALPHA_RECORDER_STAGE_DIR`
- Verifier:
  - `ALPHA_RECORDER_E2E_ARTIFACT_ROOT`
  - `ALPHA_RECORDER_STAGE_DIR`

`tools/run_e2e.ps1` sets up the child-process environment before invoking CTest:

- Always sets `ALPHA_RECORDER_STAGE_DIR`
- Always sets `ALPHA_RECORDER_SCENARIO_DIR`

The script stages the configured OBS developer tree before invoking CTest.

## Stage layout

When an OBS tree is staged, the helper uses the standard layout:

- `bin/64bit`
- `data`
- `obs-plugins/64bit`

That keeps the OBS-side packaging path intact and gives the host a real module
directory to load through libobs.
