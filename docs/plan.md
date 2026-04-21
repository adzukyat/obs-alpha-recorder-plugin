# Plan: Live Program Alpha Sidecar Recording

This document describes the **user contract** and the **implementation plan**
for the only goal of this plugin:

> When the user records in OBS, also write a lossless alpha sidecar aligned to
> the recorded video.

The design prioritizes OBS-native UX and minimal configuration.

---

## 1. Locked decisions

### 1.1 UX (minimal)

- The user continues to use OBS **Start Recording / Stop Recording**.
- The plugin operates **silently** when enabled tracking the main recording.
- A simple settings dialog located at:
  - Tools → Alpha Recorder Settings
- The dialog contains:
  - An "Enabled" toggle.
  - A "Finalization Format" dropdown (e.g., Apple ProRes 4444, Lossless HEVC).
- No scenario files.
- No separate “Start Alpha Recording” button.

### 1.2 Capture target

- Capture target is **Program output** (not Preview).

### 1.3 Settings

- The enabled flag is persisted in OBS user config.
- A "Post-Processing Format" setting allowing users to choose the final output
  format (e.g., Apple ProRes 4444 `.mov` for maximum NLE compatibility, or
  Hardware HEVC with alpha for smaller file sizes).

### 1.4 Alpha format

- Alpha is recorded as a lossless **R8 plane** (one byte per pixel).
- Compression is **LZ4 block** (CPU-light recording path).
- Format details are internal and fixed in v1 (not exposed in UI).

---

## 2. Non-goals (v1)

- Replacing OBS’s recording UX.
- Shipping a single “final” file with embedded alpha (e.g., MOV with alpha).
- Requiring NVIDIA-only HEVC alpha-layer.
- External capture apps.

---

## 3. User-visible contract

### 3.1 Lifecycle

When Enabled:

- On recording start:
  - Start an alpha session bound to the active recording output.
  - Determine the real recording file path.
  - Create sidecar + manifest alongside the recording.
- During recording:
  - Extract and buffer alpha frames.
  - Commit alpha records only for frames that were actually recorded.
- On recording stop:
  - Finalize sidecar (index/footer) and manifest.

When Disabled:

- No hooks installed.
- No disk output.

### 3.2 Output naming

The plugin follows OBS’s recording path and naming rules.

If OBS records:

- `C:\Recordings\MyRec.mkv`

Then the plugin writes:

- `C:\Recordings\MyRec.alpha.sidecar`
- `C:\Recordings\MyRec.alpha.manifest.json`

Suffixes are constants in v1.

### 3.3 Failure behavior (must be explicit)

- If the alpha session cannot start/finalize: show a modal error popup
  (MessageBox) and log details.
- If the alpha pipeline cannot keep up (bounded buffers exceeded): gracefully
  abort the alpha sidecar generation, log the error, and append an
  `ERR_OVERLOAD` flag to the manifest. **Do NOT stop the main recording**, as
  preserving the user's primary video is the highest priority.
- If finalization (FFmpeg/post-processing) fails on record stop: log the error
  and preserve the raw LZ4 sidecar + manifest so the user can attempt manual
  recovery.

Rationale: the plugin is enabled specifically to get a synchronized alpha
artifact, but destroying the standard recording due to a plugin fault is
unacceptable in production.

---

## 4. Technical design

The repo already contains core primitives (admission gate, sidecar writer,
manifest writer). v1 adds a live capture/alignment layer.

### 4.1 Alignment strategy: record what OBS actually recorded

We must not assume “rendered frame == recorded frame” because OBS may drop
frames due to encoder backpressure.

Plan:

0. Validate OBS Color Format is set to a format that preserves alpha (e.g.,
   BGRA). If not, refuse to start the alpha session and warn the user.
1. Capture raw frames (requesting BGRA/RGBA so alpha is available).
2. Compress the raw frame immediately via LZ4 to lower memory footprint.
3. Observe encoded video packets from the recording output.
4. Commit alpha records **only when a corresponding video packet exists** based
   on its Presentation Timestamp (PTS).

This makes the sidecar follow the real recording timeline.

### 4.2 Required OBS hooks

- Frontend events (lifecycle):
  - `OBS_FRONTEND_EVENT_RECORDING_STARTED`
  - `OBS_FRONTEND_EVENT_RECORDING_STOPPED`

- Recording output access:
  - `obs_frontend_get_recording_output()`
  - `obs_output_add_packet_callback()` (video packet timestamps)

- Recording output signals (file changes / split files):
  - `obs_output_get_signal_handler()` + connect to the recording output’s “file
    changed” signal (exact signal name verified during implementation).

- Raw video frames:
  - `obs_add_raw_video_callback()` with conversion requesting BGRA/RGBA.

### 4.3 Synchronization model

- Maintain a bounded queue of recent alpha frames (pre-compressed as LZ4 blocks)
  keyed by Presentation Timestamp (PTS) to minimize memory usage waiting for
  encoders.
- On each encoded video packet callback:
  - Resolve the packet’s PTS.
  - Pop the matching LZ4 alpha blook from the queue.
  - Write it to the sidecar.
- Periodically evict/drop compressed blocks that never receive a packet within a
  bounded window.

Overload is defined as:

- Queue depth or memory budget exceeded, or
- Packet/alpha matching falls behind beyond a threshold.

Overload triggers the failure behavior in section 3.3.

### 4.4 Alpha availability risk

If the raw callback always produces alpha=255 (fully opaque), v1 switches to a
dedicated render path:

- Render the active Program scene to an RGBA target that preserves alpha, then
  feed that into the same extraction + alignment pipeline.

### 4.5 Post-processing and Finalization

Instead of requiring users to manage intermediate sidecar files manually, the
plugin orchestrates post-processing (via an embedded FFmpeg or similar lib)
immediately after the recording stops.

Depending on the user's setting, the plugin will:

1. Parse the completed JSON manifest.
2. Unpack the LZ4 `R8` sequence.
3. Encode a final standalone movie file into identical framerates and timestamps
   as the recorded file.
4. Options available:
   - **PNG sequence (`.mov`)**: Maximum compatibility and encoding speed but
     large file sizes.
   - **Lossless HEVC (`.mp4` / `.mov`)**: Leverages hardware encoding when
     available for tiny file sizes, but may not import natively into all NLEs.
   - **ProRes 4444 (`.mov`)**: Maximum NLE compatibility across macOS/Windows
     but generates large files.
   - **Raw RGBA sequence**: No post-processing, just keep the LZ4-compressed R8
     sidecar + manifest for advanced users to handle manually.

---

## 5. Implementation plan

### Phase 0 — Remove scenario-file UX from the user path

- Keep scenarios as E2E-only inputs.
- Replace the current launcher dialog behavior with the single Enabled toggle.

### Phase 1 — Frontend settings dialog + persistence

- Add a basic "Alpha Recorder Settings" dialog in the Tools menu.
- Persist Enabled state and preferred Finalization Format.

### Phase 2 — Recording lifecycle wiring

- On recording started:
  - locate recording output
  - register packet callback
  - register file-change signal
  - register raw video callback

- On recording stopped:
  - unregister callbacks
  - finalize sidecar/manifest
  - trigger post-processing into selected output format

### Phase 3 — Alpha extraction + writer integration

- Convert raw frames to R8 alpha.
- LZ4 compress and buffer.
- Commit records on packet callback.

### Phase 4 — Hardening

- File split / file changed: rotate to a new sidecar.
- Pause/unpause handling.
- Shutdown safety on OBS exit.
- Clear, localized error messages.

### Phase 5 — Validation

- Manual:
  - Start/stop recording with Enabled on/off.
  - Confirm sidecar is created and finalized.
  - Stress encoder (induce OBS dropped frames) and verify sidecar frame count
    matches recorded frames.

- Automated:
  - Keep and extend unit tests for sidecar writer + gate.
  - Add a small integration-style test around timestamp matching if feasible.

---

## 6. Known open questions (answered during implementation spikes)

- Exact timebase mapping between raw frame timestamps and encoded packet
  timestamps (PTS vs DTS vs nanoseconds).
- The recording output’s file-change signal name and payload shape.
- Whether BGRA raw frames preserve meaningful alpha, or whether we must render
  program scene ourselves.
