#!/usr/bin/env bun

import { spawn, spawnSync } from "bun";
import { createHash, randomBytes } from "node:crypto";
import { copyFileSync, createWriteStream, existsSync, mkdirSync, readFileSync, readdirSync, statSync, writeFileSync } from "node:fs";
import { basename, dirname, join, resolve } from "node:path";
import { platform } from "node:process";

type ExpectedResult =
  | "normal"
  | "sync-invalid"
  | "normal-or-sync-invalid"
  | "no-alpha"
  | "temp-preserved"
  | "split-published"
  | "split-isolated";
type ResolvedExpectedResult = Exclude<ExpectedResult, "normal-or-sync-invalid">;

type Args = {
  repoRoot: string;
  stageDir: string;
  buildDir?: string;
  artifactBase?: string;
  configuration: string;
  port: number;
  syncRecordSeconds: number;
  syncAttempts: number;
  durabilityRecordSeconds: number;
  width: number;
  height: number;
  fpsNum: number;
  fpsDen: number;
  recordFormat: string;
  outputMode: string;
  recordAudioEncoder: string;
  withAudio: boolean;
  rgbEncoder: string;
  finalizationFormat: string;
  hevcQualityProfile: string;
  hevcQualityCq: number;
  hevcPreset: string;
  hevcNvencTune: string;
  hevcGopSize: number;
  verifyNleTimeline: boolean;
  strictAllFrames: boolean;
  phaseSweepSteps: number;
  requireMainPtsGap: boolean;
  requirePacketReorder: boolean;
  requireReplayUnderflow: boolean;
  requireReplayCatchup: boolean;
  requireOverload: boolean;
  requireTailRepeat: boolean;
  pauseAtMs: number;
  pauseDurationMs: number;
  splitAtMs: number;
  overloadPulseAtMs: number;
  overloadPulseDurationMs: number;
  overloadPulseDelayMs: number;
  failCloseOnSyncProofFailure: boolean;
  testFault: string;
  faultSegment: number;
  replayGapPackets: number;
  expectedResult: ExpectedResult;
  keepObsOpen: boolean;
  allowOverload: boolean;
};

type OverloadMonitor = {
  seen: boolean;
  firstLine: string;
};

type ObsLogCheckpoint = Map<string, number>;

type LogSink = {
  write(text: string): unknown;
};

type AlphaRecorderFailureWatch = {
  portableConfig: string;
  artifactRoot: string;
  obsPid?: number;
  checkpoint?: ObsLogCheckpoint;
  lastWindowScanMs?: number;
  lastWindowFailure?: string | null;
  suppressFailure?: boolean;
};

const severeSkippedFramePercent = 5;
const severeRenderLagPercent = 20;
const severeSkippedFrameCount = 30;

function parseArgs(argv: string[]): Args {
  const args: Args = {
    repoRoot: "",
    stageDir: "",
    configuration: "RelWithDebInfo",
    port: 0,
    syncRecordSeconds: 2,
    syncAttempts: 5,
    durabilityRecordSeconds: 30,
    width: 1920,
    height: 1080,
    fpsNum: 60,
    fpsDen: 1,
    recordFormat: "mkv",
    outputMode: "simple",
    recordAudioEncoder: "aac",
    withAudio: false,
    rgbEncoder: "software",
    finalizationFormat: "mask_png_mov",
    hevcQualityProfile: "high_quality",
    hevcQualityCq: 19,
    hevcPreset: "nvenc_p3",
    hevcNvencTune: "hq",
    hevcGopSize: 0,
    verifyNleTimeline: false,
    strictAllFrames: false,
    phaseSweepSteps: 0,
    requireMainPtsGap: false,
    requirePacketReorder: false,
    requireReplayUnderflow: false,
    requireReplayCatchup: false,
    requireOverload: false,
    requireTailRepeat: false,
    pauseAtMs: -1,
    pauseDurationMs: 0,
    splitAtMs: -1,
    overloadPulseAtMs: -1,
    overloadPulseDurationMs: 0,
    overloadPulseDelayMs: 0,
    failCloseOnSyncProofFailure: true,
    testFault: "",
    faultSegment: 0,
    replayGapPackets: 24,
    expectedResult: "normal",
    keepObsOpen: false,
    allowOverload: false,
  };

  for (let index = 0; index < argv.length; ++index) {
    const key = argv[index];
    const value = argv[index + 1];
    switch (key) {
      case "--repo-root":
        args.repoRoot = resolve(value);
        ++index;
        break;
      case "--stage-dir":
        args.stageDir = resolve(value);
        ++index;
        break;
      case "--build-dir":
        args.buildDir = resolve(value);
        ++index;
        break;
      case "--artifact-base":
        args.artifactBase = resolve(value);
        ++index;
        break;
      case "--configuration":
        args.configuration = value;
        ++index;
        break;
      case "--port":
        args.port = Number(value);
        ++index;
        break;
      case "--record-seconds":
        args.syncRecordSeconds = Number(value);
        ++index;
        break;
      case "--sync-record-seconds":
        args.syncRecordSeconds = Number(value);
        ++index;
        break;
      case "--sync-attempts":
        args.syncAttempts = Number(value);
        ++index;
        break;
      case "--max-record-seconds":
        args.durabilityRecordSeconds = Number(value);
        ++index;
        break;
      case "--durability-record-seconds":
        args.durabilityRecordSeconds = Number(value);
        ++index;
        break;
      case "--width":
        args.width = Number(value);
        ++index;
        break;
      case "--height":
        args.height = Number(value);
        ++index;
        break;
      case "--fps":
        args.fpsNum = Number(value);
        args.fpsDen = 1;
        ++index;
        break;
      case "--fps-num":
        args.fpsNum = Number(value);
        ++index;
        break;
      case "--fps-den":
        args.fpsDen = Number(value);
        ++index;
        break;
      case "--record-format":
        args.recordFormat = value;
        ++index;
        break;
      case "--output-mode":
        args.outputMode = value;
        ++index;
        break;
      case "--record-audio-encoder":
        args.recordAudioEncoder = value;
        ++index;
        break;
      case "--with-audio":
        args.withAudio = true;
        break;
      case "--rgb-encoder":
        args.rgbEncoder = value;
        ++index;
        break;
      case "--finalization-format":
        args.finalizationFormat = value;
        ++index;
        break;
      case "--hevc-quality-profile":
        args.hevcQualityProfile = value;
        ++index;
        break;
      case "--hevc-quality-cq":
        args.hevcQualityCq = Number(value);
        ++index;
        break;
      case "--hevc-preset":
        args.hevcPreset = value;
        ++index;
        break;
      case "--hevc-nvenc-tune":
        args.hevcNvencTune = value;
        ++index;
        break;
      case "--hevc-gop-size":
        args.hevcGopSize = Number(value);
        ++index;
        break;
      case "--verify-nle-timeline":
        args.verifyNleTimeline = true;
        break;
      case "--strict-all-frames":
        args.strictAllFrames = true;
        break;
      case "--phase-sweep-steps":
        args.phaseSweepSteps = Number(value);
        ++index;
        break;
      case "--require-main-pts-gap":
        args.requireMainPtsGap = true;
        break;
      case "--require-packet-reorder":
        args.requirePacketReorder = true;
        break;
      case "--require-replay-underflow":
        args.requireReplayUnderflow = true;
        break;
      case "--require-replay-catchup":
        args.requireReplayCatchup = true;
        break;
      case "--require-overload":
        args.requireOverload = true;
        break;
      case "--require-tail-repeat":
        args.requireTailRepeat = true;
        break;
      case "--pause-at-ms":
        args.pauseAtMs = Number(value);
        ++index;
        break;
      case "--pause-duration-ms":
        args.pauseDurationMs = Number(value);
        ++index;
        break;
      case "--split-at-ms":
        args.splitAtMs = Number(value);
        ++index;
        break;
      case "--overload-pulse-at-ms":
        args.overloadPulseAtMs = Number(value);
        ++index;
        break;
      case "--overload-pulse-duration-ms":
        args.overloadPulseDurationMs = Number(value);
        ++index;
        break;
      case "--overload-pulse-delay-ms":
        args.overloadPulseDelayMs = Number(value);
        ++index;
        break;
      case "--best-effort-sync":
        args.failCloseOnSyncProofFailure = false;
        break;
      case "--test-fault":
        args.testFault = value;
        ++index;
        break;
      case "--fault-segment":
        args.faultSegment = Number(value);
        ++index;
        break;
      case "--replay-gap-packets":
        args.replayGapPackets = Number(value);
        ++index;
        break;
      case "--expect-result":
        if (
          value !== "normal" &&
          value !== "sync-invalid" &&
          value !== "normal-or-sync-invalid" &&
          value !== "no-alpha" &&
          value !== "temp-preserved" &&
          value !== "split-published" &&
          value !== "split-isolated"
        ) {
          throw new Error(`Unsupported expected result: ${value}`);
        }
        args.expectedResult = value;
        ++index;
        break;
      case "--keep-obs-open":
        args.keepObsOpen = true;
        break;
      case "--allow-overload":
        args.allowOverload = true;
        break;
      default:
        throw new Error(`Unknown argument: ${key}`);
    }
  }

  if (!args.repoRoot || !args.stageDir) {
    throw new Error("--repo-root and --stage-dir are required");
  }
  args.syncRecordSeconds = Math.max(0.1, args.syncRecordSeconds);
  args.syncAttempts = Math.max(1, Math.floor(args.syncAttempts));
  args.durabilityRecordSeconds = Math.max(0.1, args.durabilityRecordSeconds);
  args.fpsNum = Math.max(1, Math.floor(args.fpsNum));
  args.fpsDen = Math.max(1, Math.floor(args.fpsDen));
  args.phaseSweepSteps = Math.max(0, Math.floor(args.phaseSweepSteps));
  args.pauseAtMs = Math.floor(args.pauseAtMs);
  args.pauseDurationMs = Math.max(0, Math.floor(args.pauseDurationMs));
  args.splitAtMs = Math.floor(args.splitAtMs);
  args.overloadPulseAtMs = Math.floor(args.overloadPulseAtMs);
  args.overloadPulseDurationMs = Math.max(0, Math.floor(args.overloadPulseDurationMs));
  args.overloadPulseDelayMs = Math.max(0, Math.floor(args.overloadPulseDelayMs));
  args.faultSegment = Math.max(0, Math.floor(args.faultSegment));
  args.hevcQualityCq = Math.max(0, Math.min(51, Math.floor(args.hevcQualityCq)));
  args.hevcGopSize = Math.max(0, Math.min(1000, Math.floor(args.hevcGopSize)));

  return args;
}

type VerificationAttempt = {
  kind: "sync" | "durability";
  attemptIndex: number;
  durationSeconds: number;
  phaseIndex: number;
  phasePercent: number;
  stopPhasePercent: number;
};

function verificationAttempts(
  syncRecordSeconds: number,
  syncAttempts: number,
  durabilityRecordSeconds: number,
  phaseSweepSteps: number,
): VerificationAttempt[] {
  const attempts: VerificationAttempt[] = [];
  for (let attemptIndex = 1; attemptIndex <= syncAttempts; ++attemptIndex) {
    const phaseIndex = phaseSweepSteps > 0 ? (attemptIndex - 1) % phaseSweepSteps : 0;
    attempts.push({
      kind: "sync",
      attemptIndex,
      durationSeconds: syncRecordSeconds,
      phaseIndex,
      phasePercent: phaseSweepSteps > 0 ? (phaseIndex * 100) / phaseSweepSteps : 0,
      stopPhasePercent:
        phaseSweepSteps > 0 ? (((phaseIndex * 7) % phaseSweepSteps) * 100) / phaseSweepSteps : 0,
    });
  }
  attempts.push({
    kind: "durability",
    attemptIndex: 1,
    durationSeconds: durabilityRecordSeconds,
    phaseIndex: 0,
    phasePercent: 0,
    stopPhasePercent: 0,
  });
  return attempts;
}

function fpsValue(args: Pick<Args, "fpsNum" | "fpsDen">): number {
  return args.fpsNum / args.fpsDen;
}

async function runRecordingSchedule(
  socket: ObsWebSocket,
  durationMs: number,
  args: Args,
  overload: OverloadMonitor,
  failureWatch: AlphaRecorderFailureWatch,
): Promise<void> {
  const delayMonitor = args.allowOverload ? { seen: false, firstLine: "" } : overload;
  const events: Array<{ atMs: number; kind: "pause" | "split" | "overload-start" | "overload-stop" }> = [];
  if (args.pauseAtMs >= 0 && args.pauseAtMs < durationMs) {
    events.push({ atMs: args.pauseAtMs, kind: "pause" });
  }
  if (args.splitAtMs >= 0 && args.splitAtMs < durationMs) {
    events.push({ atMs: args.splitAtMs, kind: "split" });
  }
  if (
    args.overloadPulseAtMs >= 0 &&
    args.overloadPulseAtMs < durationMs &&
    args.overloadPulseDelayMs > 0 &&
    args.overloadPulseDurationMs > 0
  ) {
    events.push({ atMs: args.overloadPulseAtMs, kind: "overload-start" });
    events.push({
      atMs: Math.min(durationMs, args.overloadPulseAtMs + args.overloadPulseDurationMs),
      kind: "overload-stop",
    });
  }
  events.sort((left, right) => left.atMs - right.atMs);

  let elapsedActiveMs = 0;
  for (const event of events) {
    await delayUnlessOverloaded(event.atMs - elapsedActiveMs, delayMonitor, failureWatch);
    elapsedActiveMs = event.atMs;
    if (!args.allowOverload && overload.seen) {
      return;
    }
    if (event.kind === "pause") {
      await socket.request("PauseRecord");
      await waitForRecordPaused(socket, true, 10, failureWatch);
      await delayUnlessOverloaded(args.pauseDurationMs, delayMonitor, failureWatch);
      await socket.request("ResumeRecord");
      await waitForRecordPaused(socket, false, 10, failureWatch);
    } else if (event.kind === "split") {
      await socket.request("SplitRecordFile");
    } else {
      await socket.request("SetInputSettings", {
        inputName: "AlphaRecorderMovingAlpha",
        inputSettings: {
          render_delay_ms: event.kind === "overload-start" ? args.overloadPulseDelayMs : 0,
        },
        overlay: true,
      });
    }
  }

  await delayUnlessOverloaded(durationMs - elapsedActiveMs, delayMonitor, failureWatch);
}

function simpleRgbEncoder(encoder: string): string {
  switch (encoder) {
    case "software":
      return platform === "darwin" ? "apple_h264" : "x264";
    case "apple_hevc":
      if (platform !== "darwin") {
        throw new Error("RGB encoder profile apple_hevc is only supported on macOS");
      }
      return "apple_hevc";
    case "nvenc_hevc":
      if (platform === "darwin") {
        throw new Error("RGB encoder profile nvenc_hevc is only supported on Windows/Linux OBS runtimes");
      }
      return "nvenc_hevc";
    case "amd_hevc":
      if (platform === "darwin") {
        throw new Error("RGB encoder profile amd_hevc is only supported on Windows/Linux OBS runtimes");
      }
      return "amd_hevc";
    default:
      throw new Error(`Unsupported RGB encoder profile: ${encoder}`);
  }
}

function writeText(path: string, text: string): void {
  mkdirSync(dirname(path), { recursive: true });
  writeFileSync(path, text, "utf8");
}

function advancedRgbEncoder(encoder: string): string {
  switch (encoder) {
    case "software":
      return "obs_x264";
    case "nvenc_hevc":
      if (platform === "darwin") {
        throw new Error("RGB encoder profile nvenc_hevc is only supported on Windows/Linux OBS runtimes");
      }
      return "obs_nvenc_hevc_tex";
    case "amd_hevc":
      if (platform === "darwin") {
        throw new Error("RGB encoder profile amd_hevc is only supported on Windows/Linux OBS runtimes");
      }
      return "h265_texture_amf";
    default:
      throw new Error(`Unsupported advanced RGB encoder profile: ${encoder}`);
  }
}

function obsAudioEncoderId(encoder: string): string {
  switch (encoder) {
    case "aac":
      return "ffmpeg_aac";
    case "pcm_s16le":
      return "ffmpeg_pcm_s16le";
    default:
      throw new Error(`Unsupported recording audio encoder: ${encoder}`);
  }
}

function writeSineWave(path: string, durationSeconds: number, sampleRate = 48000): void {
  mkdirSync(dirname(path), { recursive: true });
  const samples = Math.max(1, Math.floor(durationSeconds * sampleRate));
  const channels = 2;
  const bitsPerSample = 16;
  const dataBytes = samples * channels * (bitsPerSample / 8);
  const buffer = Buffer.alloc(44 + dataBytes);
  let offset = 0;
  const ascii = (text: string): void => {
    buffer.write(text, offset, "ascii");
    offset += text.length;
  };
  const u16 = (value: number): void => {
    buffer.writeUInt16LE(value, offset);
    offset += 2;
  };
  const u32 = (value: number): void => {
    buffer.writeUInt32LE(value, offset);
    offset += 4;
  };
  ascii("RIFF");
  u32(36 + dataBytes);
  ascii("WAVE");
  ascii("fmt ");
  u32(16);
  u16(1);
  u16(channels);
  u32(sampleRate);
  u32(sampleRate * channels * (bitsPerSample / 8));
  u16(channels * (bitsPerSample / 8));
  u16(bitsPerSample);
  ascii("data");
  u32(dataBytes);
  for (let sample = 0; sample < samples; ++sample) {
    const t = sample / sampleRate;
    const value = Math.round(Math.sin(t * Math.PI * 2 * 440) * 0x1fff);
    for (let channel = 0; channel < channels; ++channel) {
      buffer.writeInt16LE(value, offset);
      offset += 2;
    }
  }
  writeFileSync(path, buffer);
}

async function freePort(): Promise<number> {
  const server = Bun.serve({
    port: 0,
    fetch() {
      return new Response("ok");
    },
  });
  const port = server.port;
  await server.stop();
  return port;
}

function b64Sha256(text: string): string {
  return createHash("sha256").update(text, "utf8").digest("base64");
}

function delay(ms: number): Promise<void> {
  return new Promise((resolveDelay) => setTimeout(resolveDelay, ms));
}

async function delayUnlessOverloaded(
  ms: number,
  overload: OverloadMonitor,
  failureWatch?: AlphaRecorderFailureWatch,
): Promise<void> {
  const deadline = Date.now() + ms;
  while (Date.now() < deadline) {
    if (overload.seen) {
      return;
    }
    throwIfAlphaRecorderFailureDetected(failureWatch);
    const remainingMs = deadline - Date.now();
    if (remainingMs <= 0) {
      break;
    }
    await delay(Math.min(250, remainingMs));
  }
}

async function relayProcessStream(
  stream: ReadableStream<Uint8Array> | null,
  output: LogSink | null,
  overload: OverloadMonitor,
  signal?: AbortSignal,
): Promise<void> {
  if (stream == null) {
    return;
  }

  const decoder = new TextDecoder();
  const reader = stream.getReader();
  let bufferedLine = "";
  const abortReader = () => {
    void reader.cancel().catch(() => {});
  };
  if (signal?.aborted) {
    abortReader();
  }
  signal?.addEventListener("abort", abortReader, { once: true });

  try {
    while (!signal?.aborted) {
      const { done, value } = await reader.read();
      if (done) {
        break;
      }
      const text = decoder.decode(value, { stream: true });
      output?.write(text);

      bufferedLine += text;
      const lines = bufferedLine.split(/\r?\n/);
      bufferedLine = lines.pop() ?? "";
      for (const line of lines) {
        noteOverloadLine(line, overload);
      }
    }

    if (!signal?.aborted) {
      const tail = bufferedLine + decoder.decode();
      noteOverloadLine(tail, overload);
    }
  } finally {
    signal?.removeEventListener("abort", abortReader);
    reader.releaseLock();
  }
}

function noteOverloadLine(line: string, overload: OverloadMonitor): void {
  const severeSkippedFrames = isSevereSkippedFrameLine(line);
  const severeRenderLag = isSevereRenderLagLine(line);
  if (
    line.includes("Encoding overloaded!") ||
    severeSkippedFrames ||
    severeRenderLag
  ) {
    overload.seen = true;
    if (!overload.firstLine) {
      overload.firstLine = line.trim();
    }
  }
}

function resetOverloadMonitor(overload: OverloadMonitor): void {
  overload.seen = false;
  overload.firstLine = "";
}

function isSevereSkippedFrameLine(line: string): boolean {
  const skippedFrameMatch = line.match(
    /number of skipped frames due to encoding lag:\s*(\d+)(?:\/\d+)?(?:\s*\(([\d.]+)%\))?/i,
  );
  if (skippedFrameMatch == null) {
    return false;
  }

  const skippedFrames = Number(skippedFrameMatch[1]);
  if (skippedFrameMatch[2] == null) {
    return skippedFrames >= severeSkippedFrameCount;
  }

  return Number(skippedFrameMatch[2]) >= severeSkippedFramePercent;
}

function isSevereRenderLagLine(line: string): boolean {
  const renderLagMatch = line.match(/Number of lagged frames due to rendering lag\/stalls: \d+ \(([\d.]+)%\)/i);
  return renderLagMatch != null && Number(renderLagMatch[1]) >= severeRenderLagPercent;
}

async function stopRecordingAfterOverload(socket: ObsWebSocket, overload: OverloadMonitor): Promise<void> {
  if (!overload.seen) {
    return;
  }

  try {
    await socket.request("StopRecord");
    await waitForRecordState(socket, false);
  } catch (error) {
    console.warn(`Failed to stop recording after OBS reported encoding overload: ${String(error)}`);
  }
}

function createObsLogCheckpoint(portableConfig: string): ObsLogCheckpoint {
  return new Map(obsLogFiles(portableConfig).map((logFile) => [logFile, statSync(logFile).size]));
}

function scanObsLogsForOverload(portableConfig: string, overload: OverloadMonitor, checkpoint?: ObsLogCheckpoint): void {
  if (overload.seen) {
    return;
  }

  for (const logFile of obsLogFiles(portableConfig)) {
    const bytes = readFileSync(logFile);
    const checkpointSize = checkpoint?.get(logFile) ?? 0;
    const start = checkpointSize <= bytes.length ? checkpointSize : 0;
    const text = bytes.subarray(start).toString("utf8");
    const line = text
      .split(/\r?\n/)
      .find(
        (candidate) =>
          candidate.includes("Encoding overloaded!") ||
          isSevereSkippedFrameLine(candidate) ||
          isSevereRenderLagLine(candidate),
      );
    if (line != null) {
      overload.seen = true;
      overload.firstLine = line.trim();
      return;
    }
  }
}

function isAlphaRecorderFailureLine(line: string): boolean {
  if (!line.includes("Alpha Recorder")) {
    return false;
  }
  if (
    line.includes("Alpha Recorder performance telemetry:") ||
    line.includes("Alpha Recorder segment start:") ||
    line.includes("Alpha Recorder GPU texture telemetry:") ||
    line.includes("Alpha Recorder GPU texture bound temporary output")
  ) {
    return false;
  }

  return /Alpha Recorder.*(?:could not|failed|aborted|rejected|error|removed failed)/i.test(line);
}

function findAlphaRecorderLogError(portableConfig: string, checkpoint?: ObsLogCheckpoint): string | null {
  for (const logFile of obsLogFiles(portableConfig)) {
    const bytes = readFileSync(logFile);
    const checkpointSize = checkpoint?.get(logFile) ?? 0;
    const start = checkpointSize <= bytes.length ? checkpointSize : 0;
    const text = bytes.subarray(start).toString("utf8");
    const line = text
      .split(/\r?\n/)
      .find((candidate) => isAlphaRecorderFailureLine(candidate));
    if (line != null) {
      return `${basename(logFile)}: ${line.trim()}`;
    }
  }
  return null;
}

function findAlphaRecorderDialogTitle(obsPid?: number): string | null {
  if (platform !== "win32" || obsPid == null || obsPid <= 0) {
    return null;
  }

  const script = `
$ErrorActionPreference = 'SilentlyContinue'
$code = @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class AlphaRecorderWindowEnum {
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc lpEnumFunc, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern int GetWindowText(IntPtr hWnd, StringBuilder text, int count);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
}
'@
Add-Type -TypeDefinition $code | Out-Null
$target = [uint32]${obsPid}
$titles = New-Object System.Collections.Generic.List[string]
$callback = [AlphaRecorderWindowEnum+EnumWindowsProc]{
    param([IntPtr]$hWnd, [IntPtr]$lParam)
    [uint32]$pid = 0
    [AlphaRecorderWindowEnum]::GetWindowThreadProcessId($hWnd, [ref]$pid) | Out-Null
    if ($pid -eq $target -and [AlphaRecorderWindowEnum]::IsWindowVisible($hWnd)) {
        $builder = New-Object System.Text.StringBuilder 512
        [AlphaRecorderWindowEnum]::GetWindowText($hWnd, $builder, $builder.Capacity) | Out-Null
        $title = $builder.ToString()
        if ($title) { $titles.Add($title) | Out-Null }
    }
    return $true
}
[AlphaRecorderWindowEnum]::EnumWindows($callback, [IntPtr]::Zero) | Out-Null
$titles
`;
  const result = spawnSync({
    cmd: ["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", script],
    stdout: "pipe",
    stderr: "ignore",
    timeout: 3000,
  });
  if (result.exitCode !== 0) {
    return null;
  }

  const title = new TextDecoder()
    .decode(result.stdout)
    .split(/\r?\n/)
    .map((candidate) => candidate.trim())
    .find((candidate) => /^Alpha Recorder$/i.test(candidate) || /Alpha Recorder.*(?:error|failed|could not|aborted)/i.test(candidate));
  return title == null ? null : `OBS dialog title: ${title}`;
}

function findAlphaRecorderFailure(watch?: AlphaRecorderFailureWatch): string | null {
  if (watch == null) {
    return null;
  }
  if (watch.suppressFailure) {
    return null;
  }

  const logError = findAlphaRecorderLogError(watch.portableConfig, watch.checkpoint);
  if (logError != null) {
    return logError;
  }

  const now = Date.now();
  if (watch.lastWindowScanMs == null || now - watch.lastWindowScanMs >= 1000) {
    watch.lastWindowScanMs = now;
    watch.lastWindowFailure = findAlphaRecorderDialogTitle(watch.obsPid);
  }
  return watch.lastWindowFailure ?? null;
}

function throwIfAlphaRecorderFailureDetected(watch?: AlphaRecorderFailureWatch): void {
  const failure = findAlphaRecorderFailure(watch);
  if (failure == null) {
    return;
  }

  throw new Error(`OBS reported an Alpha Recorder error during OBS app E2E: ${failure}. Artifacts: ${watch?.artifactRoot ?? "<unknown>"}`);
}

function throwIfAlphaRecorderLoggedError(
  portableConfig: string,
  artifactRoot: string,
  checkpoint?: ObsLogCheckpoint,
): void {
  throwIfAlphaRecorderFailureDetected({ portableConfig, artifactRoot, checkpoint });
}

type AlphaRecorderTelemetryLine = {
  logFile: string;
  line: string;
  maskPath: string;
};

type GpuReplayTelemetry = {
  logFile: string;
  line: string;
  maskPath: string;
  queued: number;
  consumed: number;
  catchupSlots: number;
  compressedGapSlots: number;
  skippedStale: number;
  underflows: number;
  emitted: number;
  repeatedSlots: number;
  prefixRepeated: number;
  queuePending: number;
  generationSlots: number;
  ambiguousSlots: number;
  missingTextures: number;
};

function obsLogFiles(portableConfig: string): string[] {
  const logsDir = join(portableConfig, "logs");
  if (!existsSync(logsDir)) {
    return [];
  }

  return readdirSync(logsDir)
    .filter((name) => name.endsWith(".txt") || name.endsWith(".log"))
    .map((name) => join(logsDir, name))
    .sort((left, right) => statSync(right).mtimeMs - statSync(left).mtimeMs);
}

function collectAlphaRecorderPerformanceTelemetry(portableConfig: string): AlphaRecorderTelemetryLine[] {
  const telemetry: AlphaRecorderTelemetryLine[] = [];
  for (const logFile of obsLogFiles(portableConfig)) {
    const text = readFileSync(logFile, "utf8");
    for (const line of text.split(/\r?\n/)) {
      if (!line.includes("Alpha Recorder performance telemetry:")) {
        continue;
      }

      telemetry.push({
        logFile,
        line: line.trim(),
        maskPath: line.match(/path="([^"]+)"/)?.[1] ?? "",
      });
    }
  }
  return telemetry;
}

function collectGpuReplayTelemetry(portableConfig: string): GpuReplayTelemetry[] {
  const telemetry: GpuReplayTelemetry[] = [];
  for (const logFile of obsLogFiles(portableConfig)) {
    const text = readFileSync(logFile, "utf8");
    for (const line of text.split(/\r?\n/)) {
      if (!line.includes("Alpha Recorder GPU texture telemetry:")) {
        continue;
      }
      const replay = line.match(
        /replay=\{(?:queued=(\d+) )?consumed=(\d+) catchup_slots=(\d+) compressed_gap_slots=(\d+) skipped_stale=(\d+) underflows=(\d+) emitted=(\d+) repeated_slots=(\d+)(?: prefix_repeated=(\d+) queue_pending=(\d+))? generation_slots=(\d+) ambiguous_slots=(\d+)(?: missing_textures=(\d+))?\}/,
      );
      if (replay == null) {
        continue;
      }
      telemetry.push({
        logFile,
        line: line.trim(),
        maskPath: line.match(/path="([^"]+)"/)?.[1] ?? "",
        queued: Number(replay[1] ?? 0),
        consumed: Number(replay[2]),
        catchupSlots: Number(replay[3]),
        compressedGapSlots: Number(replay[4]),
        skippedStale: Number(replay[5]),
        underflows: Number(replay[6]),
        emitted: Number(replay[7]),
        repeatedSlots: Number(replay[8]),
        prefixRepeated: Number(replay[9] ?? 0),
        queuePending: Number(replay[10] ?? 0),
        generationSlots: Number(replay[11]),
        ambiguousSlots: Number(replay[12]),
        missingTextures: Number(replay[13] ?? 0),
      });
    }
  }
  return telemetry;
}

function findAlphaRecorderDiagnosticLogs(portableConfig: string): string[] {
  const results: string[] = [];
  const visit = (dir: string, depth: number): void => {
    if (depth > 6 || !existsSync(dir)) {
      return;
    }
    for (const entry of readdirSync(dir, { withFileTypes: true })) {
      const path = join(dir, entry.name);
      if (entry.isDirectory()) {
        visit(path, depth + 1);
      } else if (entry.isFile() && entry.name === "alpha-recorder.log") {
        results.push(path);
      }
    }
  };

  visit(join(portableConfig, "plugin_config"), 0);
  return results.sort((left, right) => statSync(right).mtimeMs - statSync(left).mtimeMs);
}

function copyAlphaRecorderDiagnosticLogs(portableConfig: string, artifactRoot: string): string[] {
  const copied: string[] = [];
  for (const [index, logFile] of findAlphaRecorderDiagnosticLogs(portableConfig).entries()) {
    const destination = join(artifactRoot, index === 0 ? "alpha-recorder.log" : `alpha-recorder-${index + 1}.log`);
    copyFileSync(logFile, destination);
    copied.push(destination);
  }
  return copied;
}

function compactTelemetryLine(line: string): string {
  const text = line.replace(/^.*Alpha Recorder performance telemetry:\s*/, "");
  const capture = text.match(/capture_total=\{count=(\d+) avg_ms=([\d.]+) max_ms=([\d.]+).*?captured=(\d+)\}/);
  const readback = text.match(/readback=\{count=(\d+) avg_ms=([\d.]+) max_ms=([\d.]+)\}/);
  const alignment = text.match(/alignment_worker=\{count=(\d+) avg_ms=([\d.]+) max_ms=([\d.]+) frames=(\d+) raw=(\d+) packets=(\d+)\}/);
  const queues = text.match(/queues=\{[^}]*writer_max_frames=(\d+) writer_max_bytes=([^}]+)\}/);
  const encode = text.match(/encode=\{count=(\d+) avg_ms=([\d.]+) max_ms=([\d.]+)\}/);
  const finalized = text.match(/finalize_ms=([\d.]+) queued=([^\s}]+)/);

  if (capture == null || readback == null || alignment == null || queues == null || encode == null || finalized == null) {
    return text;
  }

  return (
    `capture ${capture[4]}/${capture[1]} avg/max=${capture[2]}/${capture[3]}ms; ` +
    `readback avg/max=${readback[2]}/${readback[3]}ms; ` +
    `align frames=${alignment[4]} raw=${alignment[5]} packets=${alignment[6]} avg/max=${alignment[2]}/${alignment[3]}ms; ` +
    `writer encode avg/max=${encode[2]}/${encode[3]}ms queue_max=${queues[1]} frames/${queues[2]}; ` +
    `finalize=${finalized[1]}ms queued=${finalized[2]}`
  );
}

function linuxLibraryPathEntries(contentRoot: string, runtimeBin: string): string[] {
  const candidates = [
    runtimeBin,
    join(contentRoot, "lib"),
    join(contentRoot, "lib", "x86_64-linux-gnu"),
    join(contentRoot, "lib", "aarch64-linux-gnu"),
  ];
  return candidates.filter((candidate, index) => existsSync(candidate) && candidates.indexOf(candidate) === index);
}

function isWslRuntime(): boolean {
  if (platform !== "linux") {
    return false;
  }
  if (process.env.WSL_DISTRO_NAME != null || process.env.WSL_INTEROP != null) {
    return true;
  }
  try {
    return readFileSync("/proc/sys/kernel/osrelease", "utf8").toLowerCase().includes("microsoft");
  } catch {
    return false;
  }
}

function throwIfOverloaded(
  overload: OverloadMonitor,
  artifactRoot: string,
  width: number,
  height: number,
  fps: number,
  allowOverload: boolean,
): void {
  if (!overload.seen) {
    return;
  }
  if (allowOverload) {
    console.warn(
      `OBS reported overload during OBS app E2E at ${width}x${height}@${fps}; continuing because --allow-overload is set. ` +
        `First overload log line: ${overload.firstLine || "Encoding overloaded!"}. Artifacts: ${artifactRoot}`,
    );
    return;
  }
  throw new Error(
    `OBS reported overload, skipped frames, or severe render lag during OBS app E2E at ${width}x${height}@${fps}. ` +
      `Aborting because decoded sync verification would be ambiguous. ` +
      `First overload log line: ${overload.firstLine || "Encoding overloaded!"}. ` +
      `Artifacts: ${artifactRoot}`,
  );
}

function resolveObsExecutable(stageDir: string): { exe: string; cwd: string; contentRoot: string; runtimeBin: string } {
  const candidates =
    platform === "darwin"
      ? [
          {
            exe: join(stageDir, "OBS.app", "Contents", "MacOS", "OBS"),
            cwd: join(stageDir, "OBS.app", "Contents", "MacOS"),
            contentRoot: join(stageDir, "OBS.app", "Contents"),
            runtimeBin: join(stageDir, "OBS.app", "Contents", "Frameworks"),
          },
          {
            exe: join(stageDir, "MacOS", "OBS"),
            cwd: join(stageDir, "MacOS"),
            contentRoot: stageDir,
            runtimeBin: join(stageDir, "Frameworks"),
          },
          { exe: join(stageDir, "bin", "obs"), cwd: join(stageDir, "bin"), contentRoot: stageDir, runtimeBin: join(stageDir, "bin") },
        ]
      : platform === "win32"
        ? [
            {
              exe: join(stageDir, "bin", "64bit", "obs64.exe"),
              cwd: join(stageDir, "bin", "64bit"),
              contentRoot: stageDir,
              runtimeBin: join(stageDir, "bin", "64bit"),
            },
          ]
        : [{ exe: join(stageDir, "bin", "obs"), cwd: join(stageDir, "bin"), contentRoot: stageDir, runtimeBin: join(stageDir, "bin") }];

  for (const candidate of candidates) {
    if (existsSync(candidate.exe)) {
      return candidate;
    }
  }

  throw new Error(`Staged OBS executable is missing under ${stageDir}`);
}

type ObsProcess = ReturnType<typeof spawn>;

async function waitForPromise(promise: Promise<unknown>, timeoutMs: number): Promise<boolean> {
  return await new Promise<boolean>((resolveWait) => {
    let settled = false;
    const timer = setTimeout(() => {
      if (!settled) {
        settled = true;
        resolveWait(false);
      }
    }, timeoutMs);

    promise.then(
      () => {
        if (!settled) {
          settled = true;
          clearTimeout(timer);
          resolveWait(true);
        }
      },
      () => {
        if (!settled) {
          settled = true;
          clearTimeout(timer);
          resolveWait(true);
        }
      },
    );
  });
}

function sendObsSignal(obs: ObsProcess, signal: "SIGTERM" | "SIGKILL"): void {
  if (obs.exitCode != null) {
    return;
  }

  try {
    obs.kill(signal);
  } catch (error) {
    console.warn(`Failed to send ${signal} to OBS pid ${obs.pid}: ${String(error)}`);
  }
}

function forceKillObs(obs: ObsProcess): void {
  if (obs.exitCode != null) {
    return;
  }

  if (platform === "win32") {
    const result = spawnSync({
      cmd: ["taskkill", "/PID", String(obs.pid), "/T", "/F"],
      stdout: "pipe",
      stderr: "pipe",
      timeout: 10000,
    });
    if (result.exitCode !== 0 && obs.exitCode == null) {
      const stderr = new TextDecoder().decode(result.stderr).trim();
      console.warn(`taskkill failed for OBS pid ${obs.pid}: ${stderr || `exit code ${result.exitCode}`}`);
      spawnSync({
        cmd: [
          "powershell",
          "-NoProfile",
          "-ExecutionPolicy",
          "Bypass",
          "-Command",
          `$p = Get-CimInstance Win32_Process -Filter "ProcessId=${obs.pid}"; if ($p) { Invoke-CimMethod -InputObject $p -MethodName Terminate | Out-Null }`,
        ],
        stdout: "ignore",
        stderr: "pipe",
        timeout: 10000,
      });
    }
    return;
  }

  sendObsSignal(obs, "SIGKILL");
}

async function terminateObs(obs: ObsProcess): Promise<void> {
  if (obs.exitCode != null) {
    return;
  }

  sendObsSignal(obs, "SIGTERM");
  if (await waitForPromise(obs.exited, 10000)) {
    return;
  }

  console.warn(`OBS pid ${obs.pid} did not exit within 10s after SIGTERM; forcing shutdown.`);
  forceKillObs(obs);
  if (!(await waitForPromise(obs.exited, 10000))) {
    obs.unref();
    throw new Error(`OBS pid ${obs.pid} did not exit after forced shutdown`);
  }
}

async function drainObsProcessRelays(relays: Promise<void>[], abortController: AbortController): Promise<void> {
  const relaysDrained = await waitForPromise(Promise.allSettled(relays), 5000);
  if (relaysDrained) {
    return;
  }

  abortController.abort();
  if (!(await waitForPromise(Promise.allSettled(relays), 2000))) {
    console.warn("Timed out waiting for OBS process log streams to drain after OBS exited.");
  }
}

class ObsWebSocket {
  private socket: WebSocket;
  private pending: Array<{ resolve: (value: unknown) => void; reject: (error: unknown) => void }> = [];
  private requestCounter = 0;

  private constructor(socket: WebSocket) {
    this.socket = socket;
    this.socket.addEventListener("message", (event) => {
      const pending = this.pending.shift();
      if (pending) {
        pending.resolve(JSON.parse(String(event.data)));
      }
    });
  }

  static async connect(port: number, password: string): Promise<ObsWebSocket> {
    const deadline = Date.now() + 45000;
    let lastError: unknown = undefined;

    while (Date.now() < deadline) {
      try {
        const socket = new WebSocket(`ws://127.0.0.1:${port}`);
        await new Promise<void>((resolveOpen, rejectOpen) => {
          const timer = setTimeout(() => rejectOpen(new Error("websocket open timed out")), 3000);
          socket.addEventListener("open", () => {
            clearTimeout(timer);
            resolveOpen();
          });
          socket.addEventListener("error", (event) => {
            clearTimeout(timer);
            rejectOpen(event);
          });
        });

        const client = new ObsWebSocket(socket);
        const hello = await client.receive(5000);
        if ((hello as any).op !== 0) {
          throw new Error(`Expected obs-websocket Hello, got ${JSON.stringify(hello)}`);
        }

        const identify: Record<string, unknown> = { rpcVersion: 1, eventSubscriptions: 0 };
        const auth = (hello as any).d?.authentication;
        if (auth) {
          identify.authentication = b64Sha256(b64Sha256(password + auth.salt) + auth.challenge);
        }

        client.send({ op: 1, d: identify });
        const identified = await client.receive(5000);
        if ((identified as any).op !== 2) {
          throw new Error(`Failed to identify with obs-websocket: ${JSON.stringify(identified)}`);
        }

        return client;
      } catch (error) {
        lastError = error;
        await delay(500);
      }
    }

    throw new Error(`Timed out connecting to obs-websocket on port ${port}: ${String(lastError)}`);
  }

  close(): void {
    this.socket.close();
  }

  send(message: unknown): void {
    this.socket.send(JSON.stringify(message));
  }

  receive(timeoutMs = 30000): Promise<unknown> {
    let pending!: { resolve: (value: unknown) => void; reject: (error: unknown) => void };
    const promise = new Promise<unknown>((resolveReceive, rejectReceive) => {
      pending = { resolve: resolveReceive, reject: rejectReceive };
      this.pending.push(pending);
    });
    const timer = setTimeout(() => {
      const index = this.pending.indexOf(pending);
      if (index >= 0) {
        this.pending.splice(index, 1);
      }
      pending.reject(new Error("obs-websocket receive timed out"));
    }, timeoutMs);
    return promise.finally(() => clearTimeout(timer));
  }

  async request(requestType: string, requestData: Record<string, unknown> = {}, timeoutMs = 30000): Promise<any> {
    this.requestCounter += 1;
    const requestId = `alpha-recorder-e2e-${this.requestCounter}`;
    this.send({ op: 6, d: { requestType, requestId, requestData } });

    const deadline = Date.now() + timeoutMs;
    while (true) {
      const remainingMs = deadline - Date.now();
      if (remainingMs <= 0) {
        throw new Error(`OBS request ${requestType} timed out`);
      }
      const message = (await this.receive(remainingMs)) as any;
      if (message.op !== 7 || message.d?.requestId !== requestId) {
        continue;
      }
      if (!message.d.requestStatus?.result) {
        throw new Error(`OBS request ${requestType} failed: ${JSON.stringify(message.d.requestStatus)}`);
      }
      return message.d.responseData ?? null;
    }
  }
}

async function waitForRecordState(
  socket: ObsWebSocket,
  active: boolean,
  timeoutSeconds = 30,
  failureWatch?: AlphaRecorderFailureWatch,
): Promise<void> {
  const deadline = Date.now() + timeoutSeconds * 1000;
  let lastStatus: unknown = undefined;
  while (Date.now() < deadline) {
    throwIfAlphaRecorderFailureDetected(failureWatch);
    const status = await socket.request("GetRecordStatus", {}, 5000);
    lastStatus = status;
    if (Boolean(status.outputActive) === active) {
      return;
    }
    await delay(500);
  }
  throw new Error(`Timed out waiting for recording active=${active}; last status=${JSON.stringify(lastStatus)}`);
}

async function waitForRecordPaused(
  socket: ObsWebSocket,
  paused: boolean,
  timeoutSeconds = 30,
  failureWatch?: AlphaRecorderFailureWatch,
): Promise<void> {
  const deadline = Date.now() + timeoutSeconds * 1000;
  let lastStatus: unknown = undefined;
  while (Date.now() < deadline) {
    throwIfAlphaRecorderFailureDetected(failureWatch);
    const status = await socket.request("GetRecordStatus", {}, 5000);
    lastStatus = status;
    if (Boolean(status.outputActive) && Boolean(status.outputPaused) === paused) {
      return;
    }
    await delay(100);
  }
  throw new Error(`Timed out waiting for recording paused=${paused}; last status=${JSON.stringify(lastStatus)}`);
}

async function startRecordingWithRetry(
  socket: ObsWebSocket,
  failureWatch?: AlphaRecorderFailureWatch,
  timeoutSeconds = 30,
): Promise<void> {
  const deadline = Date.now() + timeoutSeconds * 1000;
  let lastStatus: unknown = undefined;
  let lastStartError: unknown = undefined;

  while (Date.now() < deadline) {
    throwIfAlphaRecorderFailureDetected(failureWatch);
    const status = await socket.request("GetRecordStatus", {}, 5000);
    lastStatus = status;
    if (Boolean(status.outputActive)) {
      return;
    }

    try {
      await socket.request("StartRecord");
      lastStartError = undefined;
    } catch (error) {
      lastStartError = error;
    }

    const attemptDeadline = Math.min(deadline, Date.now() + 5000);
    while (Date.now() < attemptDeadline) {
      throwIfAlphaRecorderFailureDetected(failureWatch);
      await delay(250);
      const attemptStatus = await socket.request("GetRecordStatus", {}, 5000);
      lastStatus = attemptStatus;
      if (Boolean(attemptStatus.outputActive)) {
        return;
      }
    }

    // OBS can report inactive before the previous recording output has fully
    // released its encoders. Retry only while it remains inactive.
    await delay(500);
  }

  throw new Error(
    `Timed out starting recording; last status=${JSON.stringify(lastStatus)}` +
      (lastStartError == null ? "" : `; last StartRecord error=${String(lastStartError)}`),
  );
}

function stopWaitTimeoutSeconds(durationSeconds: number): number {
  return Math.max(60, Math.ceil(durationSeconds * 3));
}

async function requestWithStartupRetry(
  socket: ObsWebSocket,
  requestType: string,
  requestData: Record<string, unknown> = {},
  timeoutSeconds = 45,
): Promise<any> {
  const deadline = Date.now() + timeoutSeconds * 1000;
  let lastError: unknown = undefined;
  while (Date.now() < deadline) {
    try {
      return await socket.request(requestType, requestData);
    } catch (error) {
      lastError = error;
      if (!String(error).includes("OBS is not ready")) {
        throw error;
      }
      await delay(500);
    }
  }
  throw lastError;
}

async function waitForPath(path: string, timeoutSeconds: number): Promise<void> {
  const deadline = Date.now() + timeoutSeconds * 1000;
  while (Date.now() < deadline) {
    if (existsSync(path) && statSync(path).size > 0) {
      return;
    }
    await delay(500);
  }
  throw new Error(`Timed out waiting for file: ${path}`);
}

function assertNoInvalidAlphaArtifacts(
  artifactRoot: string,
  allowRetainedSyncInvalid = false,
): void {
  const invalidArtifacts = readdirSync(artifactRoot).filter(
    (entry) =>
      entry.includes(".alpha.tmp.") ||
      (!allowRetainedSyncInvalid && entry.includes(".sync-invalid")) ||
      entry.endsWith(".spool"),
  );
  if (invalidArtifacts.length > 0) {
    throw new Error(`Invalid alpha artifacts were left in the recording directory: ${invalidArtifacts.join(", ")}`);
  }
}

function alphaPathForRgb(rgbPath: string, expectedFinalizationFormat: string): string {
  const basePath = rgbPath.replace(/\.[^.\\/]+$/, "");
  const rgbExtension = rgbPath.match(/(\.[^.\\/]+)$/)?.[1]?.toLowerCase() ?? ".mp4";
  const alphaExtension =
    expectedFinalizationFormat === "mask_png_mov"
      ? ".mov"
      : rgbExtension === ".mkv" || rgbExtension === ".mov" || rgbExtension === ".mp4"
        ? rgbExtension
        : ".mp4";
  return `${basePath}.alpha${alphaExtension}`;
}

function invalidAlphaArtifacts(
  artifactRoot: string,
  allowRetainedSyncInvalid = false,
): string[] {
  return readdirSync(artifactRoot).filter(
    (entry) =>
      entry.includes(".alpha.tmp.") ||
      (!allowRetainedSyncInvalid && entry.includes(".sync-invalid")) ||
      entry.endsWith(".spool"),
  );
}

function syncInvalidPathForAlpha(alphaPath: string): string {
  const extension = alphaPath.match(/(\.[^.\\/]+)$/)?.[1] ?? "";
  return `${alphaPath.slice(0, alphaPath.length - extension.length)}.sync-invalid${extension}`;
}

function artifactSnapshot(artifactRoot: string): Set<string> {
  return new Set(existsSync(artifactRoot) ? readdirSync(artifactRoot) : []);
}

function isNonEmptyFile(path: string): boolean {
  return existsSync(path) && statSync(path).isFile() && statSync(path).size > 0;
}

function newArtifactPaths(artifactRoot: string, before: Set<string>): string[] {
  return readdirSync(artifactRoot)
    .filter((entry) => !before.has(entry))
    .map((entry) => join(artifactRoot, entry));
}

function newRgbRecordingPaths(artifactRoot: string, before: Set<string>): string[] {
  return newArtifactPaths(artifactRoot, before)
    .filter((path) => {
      const name = basename(path).toLowerCase();
      return /\.(mkv|mov|mp4)$/.test(name) &&
        !name.includes(".alpha") &&
        !name.includes(".sync-invalid") &&
        !name.includes(".tmp.");
    })
    .sort((left, right) => statSync(left).mtimeMs - statSync(right).mtimeMs);
}

async function waitForExpectedAlphaResult(
  artifactRoot: string,
  alphaPath: string,
  expectedResult: Args["expectedResult"],
  before: Set<string>,
  timeoutSeconds: number,
  failureWatch?: AlphaRecorderFailureWatch,
): Promise<string> {
  if (expectedResult === "normal") {
    await waitForAlphaOutputSettled(artifactRoot, alphaPath, timeoutSeconds, failureWatch);
    return alphaPath;
  }

  const deadline = Date.now() + timeoutSeconds * 1000;
  const syncInvalidPath = syncInvalidPathForAlpha(alphaPath);
  let cleanSince: number | null = null;
  let stableTempPath = "";
  let stableTempSize = -1;
  let stableTempSince = 0;
  let stableSplitSignature = "";
  let stableSplitSince = 0;
  while (Date.now() < deadline) {
    const newPaths = newArtifactPaths(artifactRoot, before);
    const newTemps = newPaths.filter((path) => basename(path).includes(".alpha.tmp."));
    if (expectedResult === "normal-or-sync-invalid") {
      if (
        isNonEmptyFile(alphaPath) &&
        !existsSync(syncInvalidPath) &&
        newTemps.length === 0
      ) {
        return alphaPath;
      }
      if (
        isNonEmptyFile(syncInvalidPath) &&
        !existsSync(alphaPath) &&
        newTemps.length === 0
      ) {
        return syncInvalidPath;
      }
    } else if (expectedResult === "sync-invalid") {
      if (
        existsSync(syncInvalidPath) &&
        statSync(syncInvalidPath).size > 0 &&
        newTemps.length === 0 &&
        !existsSync(alphaPath)
      ) {
        return syncInvalidPath;
      }
    } else if (expectedResult === "no-alpha") {
      if (!existsSync(alphaPath) && !existsSync(syncInvalidPath) && newTemps.length === 0) {
        cleanSince ??= Date.now();
        if (Date.now() - cleanSince >= 2000) {
          return "";
        }
      } else {
        cleanSince = null;
      }
    } else if (expectedResult === "temp-preserved") {
      const candidate = newTemps.find((path) => existsSync(path) && statSync(path).size > 0);
      if (candidate != null) {
        const size = statSync(candidate).size;
        if (candidate !== stableTempPath || size !== stableTempSize) {
          stableTempPath = candidate;
          stableTempSize = size;
          stableTempSince = Date.now();
        } else if (Date.now() - stableTempSince >= 1500 && !existsSync(alphaPath)) {
          return candidate;
        }
      }
    } else {
      const rgbArtifacts = newRgbRecordingPaths(artifactRoot, before);
      const publishedAlphaCount = rgbArtifacts.filter((path) =>
        isNonEmptyFile(alphaPathForRgb(path, "mask_hevc_nvenc")),
      ).length;
      if (expectedResult === "split-published" &&
          rgbArtifacts.length >= 2 &&
          publishedAlphaCount === rgbArtifacts.length &&
          newTemps.length === 0) {
        return "";
      }
      if (expectedResult === "split-isolated" &&
        rgbArtifacts.length >= 2 &&
        publishedAlphaCount >= 1 &&
        publishedAlphaCount < rgbArtifacts.length &&
        newTemps.length === 0
      ) {
        return "";
      }
      if (rgbArtifacts.length >= 2 && newTemps.length === 0) {
        const signature = rgbArtifacts
          .map((path) => {
            const candidate = alphaPathForRgb(path, "mask_hevc_nvenc");
            return `${basename(candidate)}:${existsSync(candidate) ? statSync(candidate).size : -1}`;
          })
          .join("|");
        if (signature !== stableSplitSignature) {
          stableSplitSignature = signature;
          stableSplitSince = Date.now();
        } else if (Date.now() - stableSplitSince >= 5000) {
          throw new Error(
            `Split alpha artifacts settled without satisfying expected result=${expectedResult}: ` +
              `rgb=${rgbArtifacts.length} publishedAlpha=${publishedAlphaCount} files=${signature}`,
          );
        }
      } else {
        stableSplitSignature = "";
        stableSplitSince = 0;
      }
    }
    await delay(250);
  }
  throw new Error(
    `Timed out waiting for expected alpha result=${expectedResult}: alpha=${alphaPath}; ` +
      `newArtifacts=${newArtifactPaths(artifactRoot, before).map((path) => basename(path)).join(",") || "<none>"}`,
  );
}

async function waitForAlphaOutputSettled(
  artifactRoot: string,
  alphaPath: string,
  timeoutSeconds: number,
  failureWatch?: AlphaRecorderFailureWatch,
  allowRetainedSyncInvalid = false,
): Promise<void> {
  const deadline = Date.now() + timeoutSeconds * 1000;
  const syncInvalidPath = syncInvalidPathForAlpha(alphaPath);
  let lastInvalidArtifacts: string[] = [];
  let cleanMissingSince: number | null = null;
  let emptyOutputSince: number | null = null;
  while (Date.now() < deadline) {
    throwIfAlphaRecorderFailureDetected(failureWatch);
    if (isNonEmptyFile(syncInvalidPath)) {
      throw new Error(
        `Alpha Recorder retained sync-invalid evidence instead of publishing the expected alpha output: ` +
          `alpha=${alphaPath}; syncInvalid=${syncInvalidPath}`,
      );
    }
    lastInvalidArtifacts = invalidAlphaArtifacts(
      artifactRoot,
      allowRetainedSyncInvalid,
    );
    if (existsSync(alphaPath) && statSync(alphaPath).size > 0 && lastInvalidArtifacts.length === 0) {
      return;
    }
    if (!existsSync(alphaPath) && lastInvalidArtifacts.length === 0) {
      cleanMissingSince ??= Date.now();
      if (Date.now() - cleanMissingSince >= 3000) {
        throw new Error(
          `Alpha Recorder did not publish an alpha output and left no temporary failure artifact: alpha=${alphaPath}; ` +
            `Artifacts: ${artifactRoot}`,
        );
      }
    } else {
      cleanMissingSince = null;
    }
    if (existsSync(alphaPath) && statSync(alphaPath).size === 0 && lastInvalidArtifacts.length === 0) {
      emptyOutputSince ??= Date.now();
      if (Date.now() - emptyOutputSince >= 3000) {
        throw new Error(`Alpha Recorder left an empty published output: alpha=${alphaPath}`);
      }
    } else {
      emptyOutputSince = null;
    }
    await delay(500);
  }
  throw new Error(
    `Timed out waiting for alpha output finalization: alpha=${alphaPath}; ` +
      `invalidArtifacts=${lastInvalidArtifacts.join(", ") || "<none>"}`,
  );
}

function findTool(stageBin: string, repoRoot: string, name: string): string {
  const candidates = [join(stageBin, name), join(stageBin, `${name}.exe`)];
  const depsRoot = join(repoRoot, "deps", "obs", "obs-studio", ".deps");
  if (platform !== "linux" && existsSync(depsRoot)) {
    for (const depsName of readdirSync(depsRoot)) {
      candidates.push(join(depsRoot, depsName, "bin", name), join(depsRoot, depsName, "bin", `${name}.exe`));
    }
  }
  for (const candidate of candidates) {
    if (existsSync(candidate)) {
      return candidate;
    }
  }

  const which = spawnSync({ cmd: [platform === "win32" ? "where" : "which", name], stdout: "pipe", stderr: "ignore" });
  if (which.exitCode === 0) {
    const text = new TextDecoder().decode(which.stdout).trim().split(/\r?\n/)[0];
    if (text) {
      return text;
    }
  }

  throw new Error(`Unable to find ${name}; install FFmpeg or add it to PATH`);
}

function checkedJson(tool: string, args: string[], timeoutSeconds: number): unknown {
  const result = spawnSync({ cmd: [tool, ...args], stdout: "pipe", stderr: "pipe", timeout: timeoutSeconds * 1000 });
  if (result.exitCode !== 0) {
    throw new Error(`${tool} failed: ${new TextDecoder().decode(result.stderr)}`);
  }
  return JSON.parse(new TextDecoder().decode(result.stdout));
}

async function checkedJsonWithRetry(tool: string, args: string[], timeoutSeconds: number): Promise<unknown> {
  const deadline = Date.now() + timeoutSeconds * 1000;
  let lastError: unknown = undefined;
  while (Date.now() < deadline) {
    try {
      return checkedJson(tool, args, 30);
    } catch (error) {
      lastError = error;
      await delay(1000);
    }
  }
  throw lastError;
}

function checkedProcess(tool: string, args: string[], timeoutSeconds: number): void {
  const result = spawnSync({ cmd: [tool, ...args], stdout: "pipe", stderr: "pipe", timeout: timeoutSeconds * 1000 });
  if (result.exitCode !== 0) {
    throw new Error(`${tool} failed: ${new TextDecoder().decode(result.stderr)}`);
  }
}

function checkedOutput(tool: string, args: string[], timeoutSeconds: number): Uint8Array {
  const result = spawnSync({ cmd: [tool, ...args], stdout: "pipe", stderr: "pipe", timeout: timeoutSeconds * 1000 });
  if (result.exitCode !== 0) {
    throw new Error(`${tool} failed: ${new TextDecoder().decode(result.stderr)}`);
  }
  return result.stdout;
}

async function checkedProcessWithRetry(tool: string, args: string[], timeoutSeconds: number): Promise<void> {
  const deadline = Date.now() + timeoutSeconds * 1000;
  let lastError: unknown = undefined;
  while (Date.now() < deadline) {
    try {
      checkedProcess(tool, args, 30);
      return;
    } catch (error) {
      lastError = error;
      await delay(1000);
    }
  }
  throw lastError;
}

type Bounds = {
  minX: number;
  minY: number;
  maxX: number;
  maxY: number;
  count: number;
};

const frameCodeBits = 12;
const frameCodeTileSize = 24;
const frameCodeGap = 4;
const frameCodeX = 16;
const frameCodeY = 16;

function emptyBounds(): Bounds {
  return { minX: Number.POSITIVE_INFINITY, minY: Number.POSITIVE_INFINITY, maxX: -1, maxY: -1, count: 0 };
}

function boundsText(bounds: Bounds): string {
  if (bounds.count === 0) {
    return "empty";
  }
  return `${bounds.minX},${bounds.minY}-${bounds.maxX},${bounds.maxY} count=${bounds.count}`;
}

function rgbFrameBounds(frame: Uint8Array, width: number, height: number): Bounds {
  const bounds = emptyBounds();
  for (let y = 0; y < height; ++y) {
    for (let x = 0; x < width; ++x) {
      const offset = (y * width + x) * 3;
      const r = frame[offset];
      const g = frame[offset + 1];
      const b = frame[offset + 2];
      if (r + g + b > 120) {
        bounds.minX = Math.min(bounds.minX, x);
        bounds.minY = Math.min(bounds.minY, y);
        bounds.maxX = Math.max(bounds.maxX, x);
        bounds.maxY = Math.max(bounds.maxY, y);
        bounds.count += 1;
      }
    }
  }
  return bounds;
}

function grayFrameBounds(frame: Uint8Array, width: number, height: number): Bounds {
  const bounds = emptyBounds();
  for (let y = 0; y < height; ++y) {
    for (let x = 0; x < width; ++x) {
      const value = frame[y * width + x];
      if (value > 128) {
        bounds.minX = Math.min(bounds.minX, x);
        bounds.minY = Math.min(bounds.minY, y);
        bounds.maxX = Math.max(bounds.maxX, x);
        bounds.maxY = Math.max(bounds.maxY, y);
        bounds.count += 1;
      }
    }
  }
  return bounds;
}

function requireSimilarBounds(frameIndex: number, rgb: Bounds, alpha: Bounds): void {
  if (rgb.count === 0 || alpha.count === 0) {
    throw new Error(`Frame ${frameIndex} did not contain both RGB and alpha masks: rgb=${boundsText(rgb)} alpha=${boundsText(alpha)}`);
  }

  if (!boundsAreSimilar(rgb, alpha)) {
    throw new Error(`Frame ${frameIndex} RGB/alpha mask bounds differ: rgb=${boundsText(rgb)} alpha=${boundsText(alpha)}`);
  }
}

function boundsAreSimilar(rgb: Bounds, alpha: Bounds): boolean {
  const tolerance = 2;
  if (rgb.count === 0 || alpha.count === 0) {
    return false;
  }

  const deltas = [
    Math.abs(rgb.minX - alpha.minX),
    Math.abs(rgb.minY - alpha.minY),
    Math.abs(rgb.maxX - alpha.maxX),
    Math.abs(rgb.maxY - alpha.maxY),
  ];
  return !deltas.some((delta) => delta > tolerance);
}

function bestGlobalFrameOffset(rgbBounds: Bounds[], alphaBounds: Bounds[]): { offset: number; matches: number; total: number } {
  const searchRadius = Math.min(180, Math.max(rgbBounds.length, alphaBounds.length) - 1);
  let best = { offset: 0, matches: -1, total: 0 };

  for (let offset = -searchRadius; offset <= searchRadius; ++offset) {
    let matches = 0;
    let total = 0;
    for (let rgbIndex = 0; rgbIndex < rgbBounds.length; ++rgbIndex) {
      const alphaIndex = rgbIndex + offset;
      if (alphaIndex < 0 || alphaIndex >= alphaBounds.length) {
        continue;
      }
      ++total;
      if (boundsAreSimilar(rgbBounds[rgbIndex], alphaBounds[alphaIndex])) {
        ++matches;
      }
    }

    if (matches > best.matches || (matches === best.matches && Math.abs(offset) < Math.abs(best.offset))) {
      best = { offset, matches, total };
    }
  }

  return best;
}

function localCandidateOffsets(rgb: Bounds[], alpha: Bounds[], frameIndex: number): number[] {
  const offsets: number[] = [];
  const searchRadius = Math.min(30, Math.max(rgb.length, alpha.length) - 1);
  for (let offset = -searchRadius; offset <= searchRadius; ++offset) {
    const alphaIndex = frameIndex + offset;
    if (alphaIndex >= 0 && alphaIndex < alpha.length && boundsAreSimilar(rgb[frameIndex], alpha[alphaIndex])) {
      offsets.push(offset);
    }
  }
  return offsets;
}

function frameTimes(ffprobe: string, path: string): number[] {
  const probe = JSON.parse(
    new TextDecoder().decode(
      checkedOutput(
        ffprobe,
        ["-v", "error", "-select_streams", "v:0", "-show_entries", "frame=best_effort_timestamp_time", "-of", "json", path],
        180,
      ),
    ),
  ) as { frames?: Array<{ best_effort_timestamp_time?: string }> };

  return (probe.frames ?? [])
    .map((frame) => Number(frame.best_effort_timestamp_time))
    .filter((timestamp) => Number.isFinite(timestamp));
}

function packetPresentationReorderDepth(ffprobe: string, path: string): number {
  const probe = checkedJson(
    ffprobe,
    [
      "-v",
      "error",
      "-select_streams",
      "v:0",
      "-show_packets",
      "-show_entries",
      "packet=pts,dts",
      "-of",
      "json",
      path,
    ],
    180,
  ) as { packets?: Array<{ pts?: number | string; dts?: number | string }> };

  const packets = (probe.packets ?? [])
    .map((packet, decodeIndex) => {
      const pts = Number(packet.pts);
      const dts = Number(packet.dts);
      return {
        decodeIndex,
        pts,
        dts: Number.isFinite(dts) ? dts : pts,
      };
    })
    .filter((packet) => Number.isFinite(packet.pts));

  if (packets.length === 0) {
    return 0;
  }

  const presentationOrder = [...packets].sort((left, right) => {
    if (left.pts !== right.pts) {
      return left.pts - right.pts;
    }
    if (left.dts !== right.dts) {
      return left.dts - right.dts;
    }
    return left.decodeIndex - right.decodeIndex;
  });
  const presentationRankByDecodeIndex = new Map<number, number>();
  presentationOrder.forEach((packet, presentationRank) => {
    presentationRankByDecodeIndex.set(packet.decodeIndex, presentationRank);
  });

  let reorderDepth = 0;
  for (const packet of packets) {
    const presentationRank = presentationRankByDecodeIndex.get(packet.decodeIndex);
    if (presentationRank == null) {
      continue;
    }
    reorderDepth = Math.max(reorderDepth, presentationRank - packet.decodeIndex);
  }
  return reorderDepth;
}

function frameCodeCropFilter(): { filter: string; width: number; height: number } {
  const width = (frameCodeBits + 2) * frameCodeTileSize + (frameCodeBits + 1) * frameCodeGap;
  const height = frameCodeTileSize;
  return { filter: `crop=${width}:${height}:${frameCodeX}:${frameCodeY}`, width, height };
}

function decodeFrameCodesFromRaw(frameBytes: Uint8Array, frames: number, frameSize: number, channels: number, width: number): number[] {
  const codes: number[] = [];
  const sampleStart = Math.floor(frameCodeTileSize * 0.3);
  const sampleEnd = Math.ceil(frameCodeTileSize * 0.7);
  const samplesPerTile = (sampleEnd - sampleStart) * (sampleEnd - sampleStart);

  const tileFilled = (frame: number, tile: number): boolean => {
    const tileX = tile * (frameCodeTileSize + frameCodeGap);
    let lit = 0;
    for (let y = sampleStart; y < sampleEnd; ++y) {
      for (let x = sampleStart; x < sampleEnd; ++x) {
        const pixel = frame * frameSize + (y * width + tileX + x) * channels;
        const value = channels === 1 ? frameBytes[pixel] : frameBytes[pixel] + frameBytes[pixel + 1] + frameBytes[pixel + 2];
        const threshold = channels === 1 ? 128 : 180;
        if (value > threshold) {
          ++lit;
        }
      }
    }
    return lit >= samplesPerTile * 0.6;
  };

  for (let frame = 0; frame < frames; ++frame) {
    if (!tileFilled(frame, 0) || !tileFilled(frame, frameCodeBits + 1)) {
      throw new Error(`Frame ${frame} is missing frame-code sync markers`);
    }

    let code = 0;
    for (let bit = 0; bit < frameCodeBits; ++bit) {
      if (tileFilled(frame, bit + 1)) {
        code |= 1 << bit;
      }
    }
    codes.push(code);
  }

  return codes;
}

function decodeFrameCodes(ffmpeg: string, path: string, pixFmt: "rgb24" | "gray", inputOptions: string[] = []): number[] {
  const crop = frameCodeCropFilter();
  const channels = pixFmt === "gray" ? 1 : 3;
  const frameSize = crop.width * crop.height * channels;
  const bytes = checkedOutput(
    ffmpeg,
    [
      "-v",
      "error",
      ...inputOptions,
      "-i",
      path,
      "-an",
      "-vf",
      crop.filter,
      "-fps_mode",
      "passthrough",
      "-f",
      "rawvideo",
      "-pix_fmt",
      pixFmt,
      "-",
    ],
    180,
  );

  if (bytes.length % frameSize !== 0) {
    throw new Error(`Decoded frame-code byte count is not frame-aligned for ${path}: ${bytes.length}`);
  }

  return decodeFrameCodesFromRaw(bytes, bytes.length / frameSize, frameSize, channels, crop.width);
}

function bestFrameCodeOffset(rgbCodes: number[], alphaCodes: number[]): { offset: number; matches: number; total: number } {
  const searchRadius = Math.min(180, Math.max(rgbCodes.length, alphaCodes.length) - 1);
  let best = { offset: 0, matches: -1, total: 0 };

  for (let offset = -searchRadius; offset <= searchRadius; ++offset) {
    let matches = 0;
    let total = 0;
    for (let rgbIndex = 0; rgbIndex < rgbCodes.length; ++rgbIndex) {
      const alphaIndex = rgbIndex + offset;
      if (alphaIndex < 0 || alphaIndex >= alphaCodes.length) {
        continue;
      }
      ++total;
      if (rgbCodes[rgbIndex] === alphaCodes[alphaIndex]) {
        ++matches;
      }
    }

    if (matches > best.matches || (matches === best.matches && Math.abs(offset) < Math.abs(best.offset))) {
      best = { offset, matches, total };
    }
  }

  return best;
}

type OffsetSummary = { offset: number; matches: number; total: number };
type SyncVerification = {
  rgbFrames: number;
  alphaFrames: number;
  expectedFrameCodeOffset: number;
  mainReorderDepth: number;
  alphaConstantPacketDuration?: number;
  mainPacketTiming: PacketTimingSummary;
  alphaPacketTiming: PacketTimingSummary;
  bestFrameCodeOffset: OffsetSummary;
  bestContentOffset: OffsetSummary;
  overloadPrefixFrameCodeOffset?: OffsetSummary;
  overloadPrefixContentOffset?: OffsetSummary;
  overloadTerminalFrameCodeOffset?: OffsetSummary;
  overloadTerminalContentOffset?: OffsetSummary;
  frameCodeMismatches: number;
  maskBoundsMismatches: number;
  first60FrameCodeMismatches: number;
  last60FrameCodeMismatches: number;
  frameCodeMismatchChangePoints: number[];
  rgbConsecutiveDuplicateCodes: number;
  alphaConsecutiveDuplicateCodes: number;
  terminalRepeatOnly: boolean;
};

type PacketTimingSummary = {
  packetCount: number;
  uniqueDurations: number;
  firstDuration: number;
  monotonicPts: boolean;
  firstPacketKeyframe: boolean;
  firstPts: number;
  lastPts: number;
  ptsStep: number;
  duplicatePts: number;
  gapCount: number;
  gridViolations: number;
};

function integerGcd(left: number, right: number): number {
  let a = Math.abs(Math.trunc(left));
  let b = Math.abs(Math.trunc(right));
  while (b !== 0) {
    const remainder = a % b;
    a = b;
    b = remainder;
  }
  return a;
}

function packetTimingSummary(ffprobe: string, path: string): PacketTimingSummary {
  const probe = checkedJson(
    ffprobe,
    [
      "-v",
      "error",
      "-select_streams",
      "v:0",
      "-show_packets",
      "-show_entries",
      "packet=pts,duration,flags",
      "-of",
      "json",
      path,
    ],
    180,
  ) as { packets?: Array<{ pts?: number | string; duration?: number | string; flags?: string }> };

  const packets = (probe.packets ?? [])
    .map((packet) => ({
      pts: Number(packet.pts),
      duration: Number(packet.duration),
      flags: packet.flags ?? "",
    }))
    .filter((packet) => Number.isFinite(packet.pts));

  const durations = new Set(packets.map((packet) => packet.duration).filter((duration) => Number.isFinite(duration)));
  let monotonicPts = true;
  for (let index = 1; index < packets.length; ++index) {
    if (packets[index].pts <= packets[index - 1].pts) {
      monotonicPts = false;
      break;
    }
  }
  const sortedPts = packets.map((packet) => packet.pts).sort((left, right) => left - right);
  let duplicatePts = 0;
  let gcdStep = 0;
  let minimumPositiveDelta = 0;
  let maximumPositiveDelta = 0;
  for (let index = 1; index < sortedPts.length; ++index) {
    const delta = sortedPts[index] - sortedPts[index - 1];
    if (delta === 0) {
      ++duplicatePts;
    } else if (delta > 0) {
      gcdStep = gcdStep === 0 ? delta : integerGcd(gcdStep, delta);
      minimumPositiveDelta = minimumPositiveDelta === 0 ? delta : Math.min(minimumPositiveDelta, delta);
      maximumPositiveDelta = Math.max(maximumPositiveDelta, delta);
    }
  }
  const roundedCadence =
    minimumPositiveDelta > 0 &&
    maximumPositiveDelta - minimumPositiveDelta <= 1;
  const ptsStep = roundedCadence ? minimumPositiveDelta : gcdStep;
  let gapCount = 0;
  let gridViolations = 0;
  if (ptsStep > 0 && sortedPts.length > 0) {
    for (let index = 1; index < sortedPts.length; ++index) {
      const delta = sortedPts[index] - sortedPts[index - 1];
      const maximumCadenceDelta = roundedCadence ? ptsStep + 1 : ptsStep;
      if (delta > maximumCadenceDelta) {
        ++gapCount;
      }
      if (!roundedCadence && (sortedPts[index] - sortedPts[0]) % ptsStep !== 0) {
        ++gridViolations;
      }
    }
  }

  return {
    packetCount: packets.length,
    uniqueDurations: durations.size,
    firstDuration: packets[0]?.duration ?? 0,
    monotonicPts,
    firstPacketKeyframe: packets[0]?.flags.includes("K") ?? false,
    firstPts: sortedPts[0] ?? 0,
    lastPts: sortedPts.at(-1) ?? 0,
    ptsStep,
    duplicatePts,
    gapCount,
    gridViolations,
  };
}

function consecutiveDuplicateCount(values: number[]): number {
  let duplicates = 0;
  for (let index = 1; index < values.length; ++index) {
    if (values[index] === values[index - 1]) {
      ++duplicates;
    }
  }
  return duplicates;
}

function bestFrameCodeOffsetInRange(
  rgbCodes: number[],
  alphaCodes: number[],
  startFrame: number,
  endFrame: number,
  searchRadius = 30,
): OffsetSummary {
  const start = Math.max(0, Math.min(startFrame, rgbCodes.length));
  const end = Math.max(start, Math.min(endFrame, rgbCodes.length));
  let best: OffsetSummary = { offset: 0, matches: -1, total: 0 };

  for (let offset = -searchRadius; offset <= searchRadius; ++offset) {
    let matches = 0;
    let total = 0;
    for (let rgbIndex = start; rgbIndex < end; ++rgbIndex) {
      const alphaIndex = rgbIndex + offset;
      if (alphaIndex < 0 || alphaIndex >= alphaCodes.length) {
        continue;
      }
      ++total;
      if (rgbCodes[rgbIndex] === alphaCodes[alphaIndex]) {
        ++matches;
      }
    }

    if (matches > best.matches || (matches === best.matches && Math.abs(offset) < Math.abs(best.offset))) {
      best = { offset, matches, total };
    }
  }

  return best.matches < 0 ? { offset: 0, matches: 0, total: 0 } : best;
}

function bestBoundsOffsetInRange(
  rgbBounds: Bounds[],
  alphaBounds: Bounds[],
  startFrame: number,
  endFrame: number,
  searchRadius = 30,
): OffsetSummary {
  const start = Math.max(0, Math.min(startFrame, rgbBounds.length));
  const end = Math.max(start, Math.min(endFrame, rgbBounds.length));
  let best: OffsetSummary = { offset: 0, matches: -1, total: 0 };

  for (let offset = -searchRadius; offset <= searchRadius; ++offset) {
    let matches = 0;
    let total = 0;
    for (let rgbIndex = start; rgbIndex < end; ++rgbIndex) {
      const alphaIndex = rgbIndex + offset;
      if (alphaIndex < 0 || alphaIndex >= alphaBounds.length) {
        continue;
      }
      ++total;
      if (boundsAreSimilar(rgbBounds[rgbIndex], alphaBounds[alphaIndex])) {
        ++matches;
      }
    }

    if (matches > best.matches || (matches === best.matches && Math.abs(offset) < Math.abs(best.offset))) {
      best = { offset, matches, total };
    }
  }

  return best.matches < 0 ? { offset: 0, matches: 0, total: 0 } : best;
}

function verifyRgbAlphaFrameSync(
  ffmpeg: string,
  ffprobe: string,
  rgbPath: string,
  alphaPath: string,
  width: number,
  height: number,
  fps: number,
  overloadObserved: boolean,
  lifecycleBoundaryObserved: boolean,
  startupBoundaryObserved: boolean,
  verifyNleTimeline: boolean,
  strictAllFrames: boolean,
): SyncVerification {
  const toleratedTerminalFrames = 3;
  const toleratedPerFrameMismatches = 3;
  const toleratedBoundaryFrames = Math.max(toleratedPerFrameMismatches, Math.ceil(fps * 0.15));
  const verifyWidth = Math.min(width, 320);
  const verifyHeight = Math.max(2, Math.round((height * verifyWidth) / width / 2) * 2);
  const scaleFilter = `scale=${verifyWidth}:${verifyHeight}:flags=neighbor`;
  const rgbBytes = checkedOutput(
    ffmpeg,
    ["-v", "error", "-i", rgbPath, "-an", "-vf", scaleFilter, "-fps_mode", "passthrough", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"],
    180,
  );
  const alphaBytes = checkedOutput(
    ffmpeg,
    ["-v", "error", "-i", alphaPath, "-an", "-vf", scaleFilter, "-fps_mode", "passthrough", "-f", "rawvideo", "-pix_fmt", "gray", "-"],
    180,
  );
  const rgbFrameSize = verifyWidth * verifyHeight * 3;
  const alphaFrameSize = verifyWidth * verifyHeight;

  if (rgbBytes.length % rgbFrameSize !== 0) {
    throw new Error(`Decoded RGB byte count is not frame-aligned: ${rgbBytes.length}`);
  }
  if (alphaBytes.length % alphaFrameSize !== 0) {
    throw new Error(`Decoded alpha byte count is not frame-aligned: ${alphaBytes.length}`);
  }

  const rgbFrames = rgbBytes.length / rgbFrameSize;
  const alphaFrames = alphaBytes.length / alphaFrameSize;
  const rgbCodes = decodeFrameCodes(ffmpeg, rgbPath, "rgb24");
  const alphaCodes = decodeFrameCodes(ffmpeg, alphaPath, "gray");
  const bestCodeOffset = bestFrameCodeOffset(rgbCodes, alphaCodes);
  const mainReorderDepth = packetPresentationReorderDepth(ffprobe, rgbPath);
  const expectedFrameCodeOffset = 0;
  const mainTiming = packetTimingSummary(ffprobe, rgbPath);
  const alphaTiming = packetTimingSummary(ffprobe, alphaPath);

  const rgbBounds = Array.from({ length: rgbFrames }, (_, frame) =>
    rgbFrameBounds(rgbBytes.subarray(frame * rgbFrameSize, (frame + 1) * rgbFrameSize), verifyWidth, verifyHeight),
  );
  const alphaBounds = Array.from({ length: alphaFrames }, (_, frame) =>
    grayFrameBounds(alphaBytes.subarray(frame * alphaFrameSize, (frame + 1) * alphaFrameSize), verifyWidth, verifyHeight),
  );
  const bestOffset = bestGlobalFrameOffset(rgbBounds, alphaBounds);

  if (rgbCodes.length !== rgbFrames || alphaCodes.length !== alphaFrames) {
    throw new Error(`Frame-code counts differ from decoded frames: rgb=${rgbCodes.length}/${rgbFrames} alpha=${alphaCodes.length}/${alphaFrames}`);
  }

  const expectedAlphaFrameDelta = 0;
  const frameCountDelta = alphaFrames - rgbFrames;
  // OBS output muxers can differ by one admitted terminal frame even when all
  // shared presentation slots are content-identical. Keep offset/content checks
  // strict and reserve the wider allowance for an observed overload only.
  const toleratedFrameCountDelta = strictAllFrames && !overloadObserved
    ? 0
    : overloadObserved
      ? toleratedTerminalFrames
      : 1;
  if (Math.abs(frameCountDelta - expectedAlphaFrameDelta) > toleratedFrameCountDelta) {
    throw new Error(
      `Decoded RGB/alpha frame counts do not match expectation: rgb=${rgbFrames} alpha=${alphaFrames}; ` +
        `expectedDelta=${expectedAlphaFrameDelta} tolerance=${toleratedFrameCountDelta}; ` +
        `frameCodes=${rgbCodes.length}/${alphaCodes.length}; ` +
        `bestFrameCodeOffset=${bestCodeOffset.offset} matched=${bestCodeOffset.matches}/${bestCodeOffset.total}; ` +
        `bestContentOffset=${bestOffset.offset} matched=${bestOffset.matches}/${bestOffset.total}`,
    );
  }

  if (verifyNleTimeline) {
    if (
      alphaTiming.packetCount !== alphaFrames ||
      alphaTiming.uniqueDurations !== 1 ||
      !alphaTiming.monotonicPts ||
      !alphaTiming.firstPacketKeyframe
    ) {
      throw new Error(
        `Alpha NLE timeline is not exact CFR: ` +
          `packets=${alphaTiming.packetCount}/${alphaFrames} ` +
          `uniqueDurations=${alphaTiming.uniqueDurations} ` +
          `firstDuration=${alphaTiming.firstDuration} ` +
          `monotonicPts=${alphaTiming.monotonicPts} ` +
          `firstPacketKeyframe=${alphaTiming.firstPacketKeyframe} ` +
          `ptsStep=${alphaTiming.ptsStep} gaps=${alphaTiming.gapCount} ` +
          `duplicates=${alphaTiming.duplicatePts} gridViolations=${alphaTiming.gridViolations}`,
      );
    }
  }

  const rgbTimes = frameTimes(ffprobe, rgbPath);
  const alphaTimes = frameTimes(ffprobe, alphaPath);
  if (rgbTimes.length > 0 && alphaTimes.length > 0) {
    const timestampTolerance = Math.max(0.002, 0.35 / Math.max(1, fps));
    const startDelta = Math.abs(rgbTimes[0] - alphaTimes[0]);
    if (startDelta > timestampTolerance) {
      console.warn(
        `RGB/alpha first-frame timestamps differ by ${startDelta.toFixed(6)}s: ` +
          `rgb=${rgbTimes[0].toFixed(6)} alpha=${alphaTimes[0].toFixed(6)}; content offset remains authoritative`,
      );
    }
  } else {
    console.warn(`Could not decode RGB/alpha frame timestamps: rgb=${rgbTimes.length}/${rgbFrames} alpha=${alphaTimes.length}/${alphaFrames}`);
  }

  const comparedFrames = Math.min(rgbFrames, alphaFrames);
  const overloadPrefixWindow = Math.min(
    comparedFrames,
    Math.max(toleratedBoundaryFrames * 2, Math.ceil(fps * 0.5)),
  );
  const overloadPrefixFrameCodeOffset = bestFrameCodeOffsetInRange(
    rgbCodes,
    alphaCodes,
    0,
    overloadPrefixWindow,
  );
  const overloadPrefixContentOffset = bestBoundsOffsetInRange(
    rgbBounds,
    alphaBounds,
    0,
    overloadPrefixWindow,
  );
  const overloadTerminalWindow = Math.min(
    comparedFrames,
    Math.max(toleratedBoundaryFrames * 2, Math.ceil(fps * 0.5)),
  );
  const overloadTerminalStart = Math.max(0, comparedFrames - overloadTerminalWindow);
  const overloadTerminalFrameCodeOffset = bestFrameCodeOffsetInRange(
    rgbCodes,
    alphaCodes,
    overloadTerminalStart,
    comparedFrames,
  );
  const overloadTerminalContentOffset = bestBoundsOffsetInRange(
    rgbBounds,
    alphaBounds,
    overloadTerminalStart,
    comparedFrames,
  );
  const hasConfidentZeroOffset = (result: OffsetSummary): boolean =>
    result.offset === 0 &&
    result.total > 0 &&
    result.matches >= Math.max(1, result.total - toleratedBoundaryFrames);
  const overloadOffsetStable =
    overloadObserved &&
    hasConfidentZeroOffset(overloadPrefixFrameCodeOffset) &&
    hasConfidentZeroOffset(overloadPrefixContentOffset) &&
    hasConfidentZeroOffset(overloadTerminalFrameCodeOffset) &&
    hasConfidentZeroOffset(overloadTerminalContentOffset);
  const lifecycleMismatchBudget = Math.max(6, Math.ceil(fps * 0.15));
  const lifecycleTerminalWindow = Math.min(comparedFrames, Math.max(60, Math.ceil(fps)));
  const lifecycleTerminalStart = Math.max(0, comparedFrames - lifecycleTerminalWindow);
  const lifecycleCleanSuffixEnd = Math.max(
    lifecycleTerminalStart,
    comparedFrames - toleratedTerminalFrames,
  );
  const lifecycleOffsetStable =
    lifecycleBoundaryObserved &&
    bestCodeOffset.offset === 0 &&
    bestOffset.offset === 0 &&
    overloadTerminalFrameCodeOffset.offset === 0 &&
    overloadTerminalContentOffset.offset === 0 &&
    overloadTerminalFrameCodeOffset.total > 0 &&
    overloadTerminalContentOffset.total > 0;
  const startupOffsetRecovered =
    startupBoundaryObserved &&
    hasConfidentZeroOffset(overloadTerminalFrameCodeOffset) &&
    hasConfidentZeroOffset(overloadTerminalContentOffset);

  if (
    !overloadObserved &&
    !lifecycleBoundaryObserved &&
    !startupBoundaryObserved &&
    (bestCodeOffset.offset !== expectedFrameCodeOffset || bestOffset.offset !== 0)
  ) {
    throw new Error(
      `RGB/alpha offset does not match the strict expected value: ` +
        `frameCodeOffset=${bestCodeOffset.offset} expected=${expectedFrameCodeOffset} ` +
        `matched=${bestCodeOffset.matches}/${bestCodeOffset.total}; ` +
        `contentOffset=${bestOffset.offset} matched=${bestOffset.matches}/${bestOffset.total}`,
    );
  }
  if (lifecycleBoundaryObserved && !lifecycleOffsetStable) {
    throw new Error(
      `RGB/alpha offset did not recover after the pause/resume boundary: ` +
        `globalFrameCode=${bestCodeOffset.offset} matched=${bestCodeOffset.matches}/${bestCodeOffset.total}; ` +
        `globalContent=${bestOffset.offset} matched=${bestOffset.matches}/${bestOffset.total}; ` +
        `terminalFrameCode=${overloadTerminalFrameCodeOffset.offset} ` +
        `matched=${overloadTerminalFrameCodeOffset.matches}/${overloadTerminalFrameCodeOffset.total}; ` +
        `terminalContent=${overloadTerminalContentOffset.offset} ` +
        `matched=${overloadTerminalContentOffset.matches}/${overloadTerminalContentOffset.total}`,
    );
  }
  if (startupBoundaryObserved && !startupOffsetRecovered) {
    throw new Error(
      `RGB/alpha offset did not recover after the split prefix: ` +
        `terminalFrameCode=${overloadTerminalFrameCodeOffset.offset} ` +
        `matched=${overloadTerminalFrameCodeOffset.matches}/${overloadTerminalFrameCodeOffset.total}; ` +
        `terminalContent=${overloadTerminalContentOffset.offset} ` +
        `matched=${overloadTerminalContentOffset.matches}/${overloadTerminalContentOffset.total}`,
    );
  }
  if (overloadObserved && !overloadOffsetStable && !startupOffsetRecovered) {
    throw new Error(
      `RGB/alpha offset did not recover after the declared recovery window: ` +
        `globalFrameCode=${bestCodeOffset.offset} matched=${bestCodeOffset.matches}/${bestCodeOffset.total}; ` +
        `globalContent=${bestOffset.offset} matched=${bestOffset.matches}/${bestOffset.total}; ` +
        `prefixFrameCode=${overloadPrefixFrameCodeOffset.offset} ` +
        `matched=${overloadPrefixFrameCodeOffset.matches}/${overloadPrefixFrameCodeOffset.total}; ` +
        `prefixContent=${overloadPrefixContentOffset.offset} ` +
        `matched=${overloadPrefixContentOffset.matches}/${overloadPrefixContentOffset.total}; ` +
        `terminalFrameCode=${overloadTerminalFrameCodeOffset.offset} ` +
        `matched=${overloadTerminalFrameCodeOffset.matches}/${overloadTerminalFrameCodeOffset.total}; ` +
        `terminalContent=${overloadTerminalContentOffset.offset} ` +
        `matched=${overloadTerminalContentOffset.matches}/${overloadTerminalContentOffset.total}; ` +
        `prefixWindow=${overloadPrefixWindow} terminalWindow=${overloadTerminalWindow}`,
    );
  }
  if (
    overloadObserved &&
    overloadOffsetStable &&
    bestOffset.offset !== 0
  ) {
    console.warn(
      `RGB/alpha global best offset was non-zero inside the declared recovery window, but clean prefix and suffix remained at zero: ` +
        `globalContent=${bestOffset.offset} matched=${bestOffset.matches}/${bestOffset.total}; ` +
        `globalFrameCode=${bestCodeOffset.offset} matched=${bestCodeOffset.matches}/${bestCodeOffset.total}; ` +
        `prefixFrameCode=${overloadPrefixFrameCodeOffset.offset} ` +
        `matched=${overloadPrefixFrameCodeOffset.matches}/${overloadPrefixFrameCodeOffset.total}; ` +
        `terminalContent=${overloadTerminalContentOffset.offset} ` +
        `matched=${overloadTerminalContentOffset.matches}/${overloadTerminalContentOffset.total}; ` +
        `terminalFrameCode=${overloadTerminalFrameCodeOffset.offset} ` +
        `matched=${overloadTerminalFrameCodeOffset.matches}/${overloadTerminalFrameCodeOffset.total}`,
    );
  }

  let frameCodeMismatches = 0;
  let maskBoundsMismatches = 0;
  let startFrameCodeMismatches = 0;
  let terminalFrameCodeMismatches = 0;
  let interiorFrameCodeMismatches = 0;
  let startMaskBoundsMismatches = 0;
  let terminalMaskBoundsMismatches = 0;
  let interiorMaskBoundsMismatches = 0;
  const firstFrameCodeMismatches: string[] = [];
  const firstMaskBoundsMismatches: string[] = [];
  const frameCodeMismatchFlags: boolean[] = [];
  const maskBoundsMismatchFlags: boolean[] = [];

  for (let frame = 0; frame < comparedFrames; ++frame) {
    const alphaFrame = frame;
    const alphaFrameCode = alphaFrame >= 0 && alphaFrame < alphaCodes.length ? alphaCodes[alphaFrame] : undefined;
    const alphaBound = alphaFrame >= 0 && alphaFrame < alphaBounds.length ? alphaBounds[alphaFrame] : undefined;
    if (rgbCodes[frame] !== alphaFrameCode) {
      frameCodeMismatchFlags.push(true);
      ++frameCodeMismatches;
      if (frame < toleratedBoundaryFrames) {
        ++startFrameCodeMismatches;
      } else if (frame >= comparedFrames - toleratedBoundaryFrames) {
        ++terminalFrameCodeMismatches;
      } else {
        ++interiorFrameCodeMismatches;
      }
      if (firstFrameCodeMismatches.length < toleratedPerFrameMismatches + 1) {
        firstFrameCodeMismatches.push(`${frame}:${rgbCodes[frame]}/${alphaFrameCode ?? "missing"}@${alphaFrame}`);
      }
    } else {
      frameCodeMismatchFlags.push(false);
    }

    if (alphaBound == null || !boundsAreSimilar(rgbBounds[frame], alphaBound)) {
      maskBoundsMismatchFlags.push(true);
      ++maskBoundsMismatches;
      if (frame < toleratedBoundaryFrames) {
        ++startMaskBoundsMismatches;
      } else if (frame >= comparedFrames - toleratedBoundaryFrames) {
        ++terminalMaskBoundsMismatches;
      } else {
        ++interiorMaskBoundsMismatches;
      }
      if (firstMaskBoundsMismatches.length < toleratedPerFrameMismatches + 1) {
        const candidates = localCandidateOffsets(rgbBounds, alphaBounds, frame);
        firstMaskBoundsMismatches.push(
          `${frame}:rgb=${boundsText(rgbBounds[frame])} ` +
            `alpha=${alphaBound == null ? "missing" : boundsText(alphaBound)}@${alphaFrame} ` +
            `local=${candidates.length === 0 ? "none" : candidates.join(",")}`,
        );
      }
    } else {
      maskBoundsMismatchFlags.push(false);
    }
  }

  const startupCleanSuffixStart = Math.max(
    0,
    comparedFrames - Math.max(60, Math.ceil(fps)),
  );
  const startupCleanSuffixEnd = Math.max(
    startupCleanSuffixStart,
    comparedFrames - toleratedTerminalFrames,
  );
  const startupFrameCodeRecovered =
    startupOffsetRecovered &&
    frameCodeMismatchFlags
      .slice(startupCleanSuffixStart, startupCleanSuffixEnd)
      .every((mismatch) => !mismatch);
  const startupContentRecovered =
    startupOffsetRecovered &&
    maskBoundsMismatchFlags
      .slice(startupCleanSuffixStart, startupCleanSuffixEnd)
      .every((mismatch) => !mismatch);

  if (
    interiorFrameCodeMismatches > toleratedPerFrameMismatches ||
    startFrameCodeMismatches > toleratedBoundaryFrames ||
    terminalFrameCodeMismatches > toleratedBoundaryFrames
  ) {
    if (lifecycleOffsetStable &&
        frameCodeMismatches <= lifecycleMismatchBudget &&
        frameCodeMismatchFlags
          .slice(lifecycleTerminalStart, lifecycleCleanSuffixEnd)
          .every((mismatch) => !mismatch)) {
      console.warn(
        `RGB/alpha frame-code mismatches were confined to a pause/resume boundary and recovered to offset zero: ` +
          `mismatches=${frameCodeMismatches}/${comparedFrames}; budget=${lifecycleMismatchBudget}; ` +
          `examples=${firstFrameCodeMismatches.join("; ")}; terminalWindow=${lifecycleTerminalWindow}`,
      );
    } else if (startupFrameCodeRecovered) {
      console.warn(
        `RGB/alpha frame-code mismatches were confined to an unrecoverable split prefix and recovered to offset zero: ` +
          `mismatches=${frameCodeMismatches}/${comparedFrames}; ` +
          `examples=${firstFrameCodeMismatches.join("; ")}; ` +
          `terminalFrameCodeOffset=${overloadTerminalFrameCodeOffset.offset} ` +
          `matched=${overloadTerminalFrameCodeOffset.matches}/${overloadTerminalFrameCodeOffset.total}`,
      );
    } else if (overloadOffsetStable) {
      console.warn(
        `RGB/alpha frame-code mismatches exceed strict tolerance inside the declared recovery window, but clean prefix and suffix offsets remain zero: ` +
          `mismatches=${frameCodeMismatches}/${comparedFrames}; ` +
          `start=${startFrameCodeMismatches}/${toleratedBoundaryFrames} ` +
          `terminal=${terminalFrameCodeMismatches}/${toleratedBoundaryFrames} ` +
          `interior=${interiorFrameCodeMismatches}/${toleratedPerFrameMismatches}; ` +
          `examples=${firstFrameCodeMismatches.join("; ")}; ` +
          `bestFrameCodeOffset=${bestCodeOffset.offset} matched=${bestCodeOffset.matches}/${bestCodeOffset.total}; ` +
          `terminalFrameCodeOffset=${overloadTerminalFrameCodeOffset.offset} ` +
          `matched=${overloadTerminalFrameCodeOffset.matches}/${overloadTerminalFrameCodeOffset.total}; ` +
          `bestContentOffset=${bestOffset.offset} matched=${bestOffset.matches}/${bestOffset.total}`,
      );
    } else {
      throw new Error(
        `RGB/alpha frame-code mismatches exceed tolerance: mismatches=${frameCodeMismatches}/${comparedFrames}; ` +
          `start=${startFrameCodeMismatches}/${toleratedBoundaryFrames} ` +
          `terminal=${terminalFrameCodeMismatches}/${toleratedBoundaryFrames} ` +
          `interior=${interiorFrameCodeMismatches}/${toleratedPerFrameMismatches}; ` +
          `examples=${firstFrameCodeMismatches.join("; ")}; ` +
          `bestFrameCodeOffset=${bestCodeOffset.offset} matched=${bestCodeOffset.matches}/${bestCodeOffset.total}`,
      );
    }
  }

  if (
    interiorMaskBoundsMismatches > toleratedPerFrameMismatches ||
    startMaskBoundsMismatches > toleratedBoundaryFrames ||
    terminalMaskBoundsMismatches > toleratedBoundaryFrames
  ) {
    if (
      lifecycleOffsetStable &&
      maskBoundsMismatches <= lifecycleMismatchBudget &&
      maskBoundsMismatchFlags
        .slice(lifecycleTerminalStart, lifecycleCleanSuffixEnd)
        .every((mismatch) => !mismatch)
    ) {
      console.warn(
        `RGB/alpha mask mismatches were confined to a pause/resume boundary and recovered to offset zero: ` +
          `mismatches=${maskBoundsMismatches}/${comparedFrames}; budget=${lifecycleMismatchBudget}; ` +
          `examples=${firstMaskBoundsMismatches.join("; ")}; terminalWindow=${lifecycleTerminalWindow}`,
      );
    } else if (startupContentRecovered) {
      console.warn(
        `RGB/alpha mask mismatches were confined to an unrecoverable split prefix and recovered to offset zero: ` +
          `mismatches=${maskBoundsMismatches}/${comparedFrames}; ` +
          `examples=${firstMaskBoundsMismatches.join("; ")}; ` +
          `terminalContentOffset=${overloadTerminalContentOffset.offset} ` +
          `matched=${overloadTerminalContentOffset.matches}/${overloadTerminalContentOffset.total}`,
      );
    } else if (overloadOffsetStable) {
      console.warn(
        `RGB/alpha mask bounds mismatches exceed strict tolerance inside the declared recovery window, but clean prefix and suffix offsets remain zero: ` +
          `mismatches=${maskBoundsMismatches}/${comparedFrames}; ` +
          `start=${startMaskBoundsMismatches}/${toleratedBoundaryFrames} ` +
          `terminal=${terminalMaskBoundsMismatches}/${toleratedBoundaryFrames} ` +
          `interior=${interiorMaskBoundsMismatches}/${toleratedPerFrameMismatches}; ` +
          `examples=${firstMaskBoundsMismatches.join("; ")}; ` +
          `bestContentOffset=${bestOffset.offset} matched=${bestOffset.matches}/${bestOffset.total}; ` +
          `terminalContentOffset=${overloadTerminalContentOffset.offset} ` +
          `matched=${overloadTerminalContentOffset.matches}/${overloadTerminalContentOffset.total}; ` +
          `bestFrameCodeOffset=${bestCodeOffset.offset} matched=${bestCodeOffset.matches}/${bestCodeOffset.total}`,
      );
    } else {
      throw new Error(
        `RGB/alpha mask bounds mismatches exceed tolerance: mismatches=${maskBoundsMismatches}/${comparedFrames}; ` +
          `start=${startMaskBoundsMismatches}/${toleratedBoundaryFrames} ` +
          `terminal=${terminalMaskBoundsMismatches}/${toleratedBoundaryFrames} ` +
          `interior=${interiorMaskBoundsMismatches}/${toleratedPerFrameMismatches}; ` +
          `examples=${firstMaskBoundsMismatches.join("; ")}; ` +
          `bestContentOffset=${bestOffset.offset} matched=${bestOffset.matches}/${bestOffset.total}`,
      );
    }
  }

  const first60FrameCodeMismatches = frameCodeMismatchFlags
    .slice(0, Math.min(60, frameCodeMismatchFlags.length))
    .filter(Boolean).length;
  const last60FrameCodeMismatches = frameCodeMismatchFlags
    .slice(Math.max(0, frameCodeMismatchFlags.length - 60))
    .filter(Boolean).length;
  const frameCodeMismatchChangePoints: number[] = [];
  for (let index = 1; index < frameCodeMismatchFlags.length; ++index) {
    if (frameCodeMismatchFlags[index] !== frameCodeMismatchFlags[index - 1]) {
      frameCodeMismatchChangePoints.push(index);
    }
  }
  const terminalRepeatOnly =
    frameCodeMismatches === 1 &&
    maskBoundsMismatches <= 1 &&
    comparedFrames >= 2 &&
    frameCodeMismatchFlags[comparedFrames - 1] &&
    frameCodeMismatchFlags.slice(0, comparedFrames - 1).every((mismatch) => !mismatch) &&
    (maskBoundsMismatches === 0 ||
      (maskBoundsMismatchFlags[comparedFrames - 1] &&
        maskBoundsMismatchFlags.slice(0, comparedFrames - 1).every((mismatch) => !mismatch))) &&
    alphaCodes[comparedFrames - 1] === alphaCodes[comparedFrames - 2] &&
    rgbCodes[comparedFrames - 1] !== rgbCodes[comparedFrames - 2];

  if (
    strictAllFrames &&
    !overloadObserved &&
    !lifecycleBoundaryObserved &&
    (frameCodeMismatches !== 0 || maskBoundsMismatches !== 0) &&
    !terminalRepeatOnly
  ) {
    throw new Error(
      `Strict full-frame sync failed: frameCodeMismatches=${frameCodeMismatches}/${comparedFrames} ` +
        `maskBoundsMismatches=${maskBoundsMismatches}/${comparedFrames} ` +
        `first60=${first60FrameCodeMismatches} last60=${last60FrameCodeMismatches} ` +
        `changePoints=${frameCodeMismatchChangePoints.slice(0, 20).join(",") || "<none>"} ` +
        `examples=${firstFrameCodeMismatches.join("; ") || "<none>"}`,
    );
  }
  if (mainTiming.duplicatePts !== 0 || mainTiming.gridViolations !== 0) {
    throw new Error(
      `Main packet PTS grid is invalid: duplicates=${mainTiming.duplicatePts} ` +
        `gridViolations=${mainTiming.gridViolations} ptsStep=${mainTiming.ptsStep}`,
    );
  }
  if (alphaTiming.duplicatePts !== 0 || alphaTiming.gridViolations !== 0) {
    throw new Error(
      `Alpha packet PTS grid is invalid: duplicates=${alphaTiming.duplicatePts} ` +
        `gridViolations=${alphaTiming.gridViolations} ptsStep=${alphaTiming.ptsStep}`,
    );
  }

  return {
    rgbFrames,
    alphaFrames,
    expectedFrameCodeOffset,
    mainReorderDepth,
    alphaConstantPacketDuration: alphaTiming.firstDuration,
    mainPacketTiming: mainTiming,
    alphaPacketTiming: alphaTiming,
    bestFrameCodeOffset: bestCodeOffset,
    bestContentOffset: bestOffset,
    overloadPrefixFrameCodeOffset: overloadObserved ? overloadPrefixFrameCodeOffset : undefined,
    overloadPrefixContentOffset: overloadObserved ? overloadPrefixContentOffset : undefined,
    overloadTerminalFrameCodeOffset: overloadObserved ? overloadTerminalFrameCodeOffset : undefined,
    overloadTerminalContentOffset: overloadObserved ? overloadTerminalContentOffset : undefined,
    frameCodeMismatches,
    maskBoundsMismatches,
    first60FrameCodeMismatches,
    last60FrameCodeMismatches,
    frameCodeMismatchChangePoints,
    rgbConsecutiveDuplicateCodes: consecutiveDuplicateCount(rgbCodes),
    alphaConsecutiveDuplicateCodes: consecutiveDuplicateCount(alphaCodes),
    terminalRepeatOnly,
  };
}

async function verifyRecordingOutputs(
  stageBin: string,
  repoRoot: string,
  artifactRoot: string,
  portableConfig: string,
  rgbPath: string,
  expectedFinalizationFormat: string,
  rgbEncoder: string,
  width: number,
  height: number,
  fps: number,
  overloadObserved: boolean,
  lifecycleBoundaryObserved: boolean,
  verifyNleTimeline: boolean,
  strictAllFrames: boolean,
  allowRetainedSyncInvalid = false,
): Promise<{ rgbPath: string; alphaPath: string; rgbProbe: any; alphaProbe: any; syncVerification: SyncVerification }> {
  if (!rgbPath) {
    const mkvs = readdirSync(artifactRoot)
      .filter((name) => name.endsWith(".mkv"))
      .map((name) => join(artifactRoot, name))
      .sort((left, right) => statSync(right).mtimeMs - statSync(left).mtimeMs);
    rgbPath = mkvs[0] ?? "";
  }
  if (!rgbPath) {
    throw new Error("OBS did not report or create an RGB recording path");
  }

  const alphaPath = alphaPathForRgb(rgbPath, expectedFinalizationFormat);

  await waitForPath(rgbPath, 30);
  if (!existsSync(alphaPath) || statSync(alphaPath).size <= 0) {
    throwIfAlphaRecorderLoggedError(portableConfig, artifactRoot);
    throw new Error(`Alpha Recorder did not publish an alpha output: alpha=${alphaPath}; Artifacts: ${artifactRoot}`);
  }
  assertNoInvalidAlphaArtifacts(artifactRoot, allowRetainedSyncInvalid);

  const ffprobe = findTool(stageBin, repoRoot, "ffprobe");
  const ffmpeg = findTool(stageBin, repoRoot, "ffmpeg");
  const rgbProbe = await checkedJsonWithRetry(
    ffprobe,
    [
      "-v",
      "error",
      "-select_streams",
      "v:0",
      "-show_entries",
      "stream=codec_name,width,height,pix_fmt,nb_frames,duration",
      "-show_entries",
      "format=duration,size",
      "-of",
      "json",
      rgbPath,
    ],
    30,
  );
  const alphaProbe = (await checkedJsonWithRetry(
    ffprobe,
    [
      "-v",
      "error",
      "-select_streams",
      "v:0",
      "-show_entries",
      "stream=codec_name,width,height,pix_fmt,nb_frames,duration",
      "-show_entries",
      "format=duration,size",
      "-of",
      "json",
      alphaPath,
    ],
    180,
  )) as any;
  await checkedProcessWithRetry(ffmpeg, ["-v", "error", "-i", rgbPath, "-frames:v", "1", "-f", "null", "-"], 30);
  await checkedProcessWithRetry(ffmpeg, ["-v", "error", "-i", alphaPath, "-frames:v", "1", "-f", "null", "-"], 180);

  const rgbStream = rgbProbe.streams?.[0] ?? {};
  if ((rgbEncoder === "apple_hevc" || rgbEncoder === "nvenc_hevc" || rgbEncoder === "amd_hevc") && rgbStream.codec_name !== "hevc") {
    throw new Error(`RGB recording probe did not report HEVC for ${rgbEncoder} profile: ${JSON.stringify(rgbProbe)}`);
  }

  const alphaStream = alphaProbe.streams?.[0] ?? {};
  const alphaPixFmt = String(alphaStream.pix_fmt ?? "");
  if (alphaPixFmt.startsWith("yuva") || alphaPixFmt === "rgba" || alphaPixFmt === "bgra" || alphaPixFmt === "argb") {
    throw new Error(`Alpha mask movie unexpectedly carries an alpha-capable pixel format: ${JSON.stringify(alphaProbe)}`);
  }
  if (expectedFinalizationFormat.startsWith("mask_hevc_")) {
    if (alphaStream.codec_name !== "hevc") {
      throw new Error(`Alpha movie probe did not report HEVC: ${JSON.stringify(alphaProbe)}`);
    }
  } else if (alphaStream.codec_name !== "png") {
    throw new Error(`Alpha movie probe did not report PNG MOV: ${JSON.stringify(alphaProbe)}`);
  }

  const syncVerification = verifyRgbAlphaFrameSync(
    ffmpeg,
    ffprobe,
    rgbPath,
    alphaPath,
    width,
    height,
    fps,
    overloadObserved,
    lifecycleBoundaryObserved,
    false,
    verifyNleTimeline,
    strictAllFrames,
  );
  return { rgbPath, alphaPath, rgbProbe, alphaProbe, syncVerification };
}

async function verifyExpectedNonNormalOutput(
  stageBin: string,
  repoRoot: string,
  rgbPath: string,
  alphaArtifactPath: string,
  width: number,
  height: number,
  fps: number,
): Promise<SyncVerification | undefined> {
  await waitForPath(rgbPath, 30);
  const ffprobe = findTool(stageBin, repoRoot, "ffprobe");
  const ffmpeg = findTool(stageBin, repoRoot, "ffmpeg");
  await checkedProcessWithRetry(ffmpeg, ["-v", "error", "-i", rgbPath, "-frames:v", "1", "-f", "null", "-"], 30);
  if (!alphaArtifactPath) {
    return undefined;
  }

  await checkedProcessWithRetry(
    ffmpeg,
    ["-v", "error", "-i", alphaArtifactPath, "-frames:v", "1", "-f", "null", "-"],
    180,
  );
  try {
    return verifyRgbAlphaFrameSync(
      ffmpeg,
      ffprobe,
      rgbPath,
      alphaArtifactPath,
      width,
      height,
      fps,
      false,
      false,
      false,
      false,
      false,
    );
  } catch (error) {
    const diagnostic =
      error instanceof Error ? error.message : String(error);
    console.log(
      `Retained diagnostic alpha is decodable but not sync-certified, as expected: ${diagnostic}`,
    );
    return undefined;
  }
}

async function verifyArtifactDecodes(
  stageBin: string,
  repoRoot: string,
  rgbPath: string,
  alphaArtifactPath: string,
): Promise<void> {
  await waitForPath(rgbPath, 30);
  const ffmpeg = findTool(stageBin, repoRoot, "ffmpeg");
  await checkedProcessWithRetry(
    ffmpeg,
    ["-v", "error", "-i", rgbPath, "-frames:v", "1", "-f", "null", "-"],
    30,
  );
  if (alphaArtifactPath) {
    await checkedProcessWithRetry(
      ffmpeg,
      ["-v", "error", "-i", alphaArtifactPath, "-frames:v", "1", "-f", "null", "-"],
      180,
    );
  }
}

async function main(): Promise<void> {
  const args = parseArgs(Bun.argv.slice(2));
  const port = args.port > 0 ? args.port : await freePort();
  const password = randomBytes(16).toString("hex");
  const { exe: obsExe, cwd: obsCwd, contentRoot, runtimeBin } = resolveObsExecutable(args.stageDir);
  const artifactBase = args.artifactBase ?? join(args.repoRoot, "out", "e2e", "obs-app");
  const artifactRoot = join(artifactBase, new Date().toISOString().replace(/[-:]/g, "").replace(/\..+$/, ""));
  mkdirSync(artifactRoot, { recursive: true });
  const obsProcessLogPath = join(artifactRoot, "obs-process.log");

  const homeRoot = join(artifactRoot, "home");
  const portableConfig =
    platform === "darwin"
      ? join(homeRoot, "Library", "Application Support", "obs-studio")
      : platform === "linux"
        ? join(homeRoot, ".config", "obs-studio")
        : join(contentRoot, "config", "obs-studio");
  const simpleVideoEncoder = simpleRgbEncoder(args.rgbEncoder);
  const outputMode = args.outputMode === "advanced-standard" ? "Advanced" : "Simple";
  if (args.outputMode !== "simple" && args.outputMode !== "advanced-standard") {
    throw new Error(`Unsupported output mode: ${args.outputMode}`);
  }
  const advancedVideoEncoder = advancedRgbEncoder(args.rgbEncoder);
  const recordAudioEncoder = obsAudioEncoderId(args.recordAudioEncoder);
  if (platform === "darwin") {
    mkdirSync(join(homeRoot, "Library", "Logs", "DiagnosticReports"), { recursive: true });
  }
  const profile = "AlphaRecorderE2E";
  const collection = "AlphaRecorderE2E";

  writeText(join(portableConfig, "global.ini"), `[General]
FirstRun=false
Pre31Migrated=true
LastVersion=536936449
MaxLogs=10
ProcessPriority=Normal
MacOSPermissionsDialogLastShown=1

[Basic]
Profile=${profile}
ProfileDir=${profile}
SceneCollection=${collection}
SceneCollectionFile=${collection}
`);

  writeText(join(portableConfig, "user.ini"), `[General]
FirstRun=false
ConfirmOnExit=false
HotkeyFocusType=NeverDisableHotkeys

[Basic]
Profile=${profile}
ProfileDir=${profile}
SceneCollection=${collection}
SceneCollectionFile=${collection}.json
ConfigOnNewProfile=false
`);

  writeText(join(portableConfig, "basic", "profiles", profile, "basic.ini"), `[General]
Name=${profile}

[Output]
Mode=${outputMode}
FilenameFormatting=%CCYY-%MM-%DD %hh-%mm-%ss

[AdvOut]
RecType=Standard
RecFilePath=${artifactRoot.replaceAll("\\", "/")}
RecFormat2=${args.recordFormat}
RecTracks=1
RecEncoder=${advancedVideoEncoder}
Encoder=obs_x264
ApplyServiceSettings=true
RecUseRescale=false
TrackIndex=1
RecSplitFileType=Time
AudioEncoder=ffmpeg_aac
RecAudioEncoder=${recordAudioEncoder}
RecSplitFile=${args.splitAtMs >= 0}

[SimpleOutput]
FilePath=${artifactRoot.replaceAll("\\", "/")}
RecFormat2=${args.recordFormat}
VBitrate=2500
ABitrate=160
UseAdvanced=true
Preset=ultrafast
x264Settings=tune=zerolatency bframes=0 sync-lookahead=0 rc-lookahead=0
RecQuality=Stream
RecRB=false
RecTracks=1
StreamEncoder=${simpleVideoEncoder}
RecEncoder=${simpleVideoEncoder}
StreamAudioEncoder=aac
RecAudioEncoder=${args.recordAudioEncoder}

[Video]
BaseCX=${args.width}
BaseCY=${args.height}
OutputCX=${args.width}
OutputCY=${args.height}
FPSType=${args.fpsDen === 1 ? 1 : 2}
FPSCommon=${fpsValue(args)}
FPSInt=${Math.round(fpsValue(args))}
FPSNum=${args.fpsNum}
FPSDen=${args.fpsDen}
ScaleType=bicubic
ColorFormat=NV12
ColorSpace=709
ColorRange=Partial

[Audio]
SampleRate=48000
ChannelSetup=Stereo

[AlphaRecorder]
enabled=false
finalization_format=${args.finalizationFormat}
`);
  writeText(
    join(portableConfig, "basic", "profiles", profile, "recordEncoder.json"),
    JSON.stringify(
      {
        keyint_sec: 1,
        preset: "ultrafast",
        profile: "high",
        tune: "zerolatency",
        x264opts: "bframes=0 sync-lookahead=0 rc-lookahead=0",
      },
      null,
      2,
    ),
  );

  writeText(
    join(portableConfig, "basic", "scenes", `${collection}.json`),
    JSON.stringify(
      {
        name: collection,
        sources: [
          {
            name: "Scene",
            id: "scene",
            versioned_id: "scene",
            settings: { id_counter: 1, custom_size: false, items: [] },
            mixers: 0,
            sync: 0,
            flags: 0,
            volume: 1.0,
            balance: 0.5,
            enabled: true,
            muted: false,
            hotkeys: {},
            private_settings: {},
          },
        ],
        current_scene: "Scene",
        current_program_scene: "Scene",
        groups: [],
        quick_transitions: [],
        transitions: [],
        saved_projectors: [],
        preview_locked: false,
        scaling_enabled: false,
        scaling_level: 0,
        scaling_off_x: 0.0,
        scaling_off_y: 0.0,
        "virtual-camera": { type2: 3 },
      },
      null,
      2,
    ),
  );

  writeText(
    join(portableConfig, "plugin_config", "obs-websocket", "config.json"),
    JSON.stringify(
      {
        first_load: false,
        server_enabled: true,
        server_port: port,
        alerts_enabled: false,
        auth_required: true,
        server_password: password,
      },
      null,
      2,
    ),
  );

  const stageBin = runtimeBin;
  const env = {
    ...process.env,
    ...(platform === "darwin" ? { CFFIXED_USER_HOME: homeRoot, HOME: homeRoot } : {}),
    ...(platform === "linux" ? { HOME: homeRoot, XDG_CONFIG_HOME: join(homeRoot, ".config") } : {}),
    PATH: `${stageBin}${platform === "win32" ? ";" : ":"}${process.env.PATH ?? ""}`,
    DYLD_LIBRARY_PATH:
      platform === "darwin" ? `${stageBin}:${process.env.DYLD_LIBRARY_PATH ?? ""}` : process.env.DYLD_LIBRARY_PATH,
    LD_LIBRARY_PATH:
      platform === "linux"
        ? `${linuxLibraryPathEntries(contentRoot, stageBin).join(":")}:${process.env.LD_LIBRARY_PATH ?? ""}`
        : process.env.LD_LIBRARY_PATH,
    ...(platform === "linux" && process.env.QT_QPA_PLATFORM == null && isWslRuntime() ? { QT_QPA_PLATFORM: "xcb" } : {}),
    ALPHA_RECORDER_E2E_TEST: "1",
    ...(args.testFault ? { ALPHA_RECORDER_E2E_FAULT: args.testFault } : {}),
    ...(args.faultSegment > 0 ? { ALPHA_RECORDER_E2E_FAULT_SEGMENT: String(args.faultSegment) } : {}),
    ...(args.testFault === "replay-evidence-gap"
      ? {
          ALPHA_RECORDER_E2E_REPLAY_GAP_PACKETS: String(args.replayGapPackets),
        }
      : {}),
  };

  const obsCommand = [
    obsExe,
    "--multi",
    "--profile",
    profile,
    "--collection",
    collection,
    "--websocket_port",
    String(port),
    "--websocket_password",
    password,
    "--websocket_ipv4_only",
  ];
  if (platform !== "darwin") {
    obsCommand.splice(1, 0, "--portable");
  }

  const obs = spawn({
    cmd: obsCommand,
    cwd: obsCwd,
    env,
    stdout: "pipe",
    stderr: "pipe",
  });
  const overload: OverloadMonitor = { seen: false, firstLine: "" };
  const obsProcessLog = createWriteStream(obsProcessLogPath, { flags: "a" });
  obsProcessLog.write(`# OBS app E2E process log\n# Command: ${obsCommand.join(" ")}\n\n`);
  const relayAbortController = new AbortController();
  const stdoutRelay = relayProcessStream(obs.stdout, obsProcessLog, overload, relayAbortController.signal);
  const stderrRelay = relayProcessStream(obs.stderr, obsProcessLog, overload, relayAbortController.signal);

  console.log(`OBS app E2E artifacts: ${artifactRoot}`);
  console.log(`OBS process log: ${obsProcessLogPath}`);

  let socket: ObsWebSocket | undefined = undefined;
  let obsCleanedUp = false;
  const cleanupObs = async (): Promise<void> => {
    if (obsCleanedUp) {
      return;
    }

    socket?.close();
    socket = undefined;
    if (args.keepObsOpen) {
      obs.unref();
      relayAbortController.abort();
      await waitForPromise(Promise.allSettled([stdoutRelay, stderrRelay]), 2000);
      await new Promise<void>((resolveEnd) => obsProcessLog.end(resolveEnd));
      obsCleanedUp = true;
      return;
    }

    await terminateObs(obs);
    await drainObsProcessRelays([stdoutRelay, stderrRelay], relayAbortController);
    await new Promise<void>((resolveEnd) => obsProcessLog.end(resolveEnd));
    obsCleanedUp = true;
  };

  try {
    socket = await ObsWebSocket.connect(port, password);
    const setSettings = await requestWithStartupRetry(socket, "CallVendorRequest", {
      vendorName: "alpha_recorder",
      requestType: "SetSettings",
      requestData: {
        enabled: true,
        finalization_format: args.finalizationFormat,
        hevc_quality_profile: args.hevcQualityProfile,
        hevc_quality_cq: args.hevcQualityCq,
        hevc_preset: args.hevcPreset,
        hevc_nvenc_tune: args.hevcNvencTune,
        hevc_gop_size: args.hevcGopSize,
        diagnostic_logging: args.allowOverload,
        fail_close_on_sync_proof_failure: args.failCloseOnSyncProofFailure,
      },
    });
    if (setSettings.responseData?.ok === false) {
      throw new Error(`Alpha Recorder rejected settings: ${JSON.stringify(setSettings.responseData)}`);
    }
    const settings = await requestWithStartupRetry(socket, "CallVendorRequest", {
      vendorName: "alpha_recorder",
      requestType: "GetSettings",
      requestData: {},
    });
    if (!settings.responseData?.enabled) {
      throw new Error("Alpha Recorder did not report enabled=true through the vendor API");
    }
    const expectedFinalizationFormat = args.finalizationFormat;
    if (settings.responseData?.finalization_format !== expectedFinalizationFormat) {
      throw new Error(`Alpha Recorder did not accept finalization_format=${args.finalizationFormat}: ${JSON.stringify(settings)}`);
    }
    if (
      Number(settings.responseData?.hevc_gop_size) !== args.hevcGopSize
    ) {
      throw new Error(
        `Alpha Recorder did not accept HEVC GOP setting: expected gop=${args.hevcGopSize}; got ${JSON.stringify(settings.responseData)}`,
      );
    }

    await socket.request("CreateInput", {
      sceneName: "Scene",
      inputName: "AlphaRecorderMovingAlpha",
      inputKind: "alpha_recorder_e2e_moving_alpha",
      inputSettings: {
        width: args.width,
        height: args.height,
        box_size: Math.max(32, Math.min(96, Math.floor(Math.min(args.width, args.height) / 4))),
        step: 17,
        color: 0xff00ffff,
      },
      sceneItemEnabled: true,
    });

    if (args.withAudio) {
      const audioPath = join(artifactRoot, "e2e-tone.wav");
      writeSineWave(audioPath, args.syncRecordSeconds * args.syncAttempts + args.durabilityRecordSeconds + 30);
      await socket.request("CreateInput", {
        sceneName: "Scene",
        inputName: "AlphaRecorderAudioTone",
        inputKind: "ffmpeg_source",
        inputSettings: {
          is_local_file: true,
          local_file: audioPath,
          looping: true,
          restart_on_activate: true,
        },
        sceneItemEnabled: true,
      });
    }

    await socket.request("SetRecordDirectory", { recordDirectory: artifactRoot });

    const recordedAttempts: Array<VerificationAttempt & {
      rgbPath: string;
      alphaArtifactPath: string;
      overloaded: boolean;
      actualResult: ResolvedExpectedResult;
    }> = [];
    const attempts: Array<{
      kind: "sync" | "durability";
      attemptIndex: number;
      durationSeconds: number;
      phaseIndex: number;
      phasePercent: number;
      stopPhasePercent: number;
      overloaded: boolean;
      rgbPath: string;
      alphaPath: string;
      rgbProbe: any;
      alphaProbe: any;
      syncVerification?: SyncVerification;
      observedMainReorderDepth: number;
      observedMainPacketTiming: PacketTimingSummary;
    }> = [];
    for (const attempt of verificationAttempts(
      args.syncRecordSeconds,
      args.syncAttempts,
      args.durabilityRecordSeconds,
      args.phaseSweepSteps,
    )) {
      const durationSeconds = attempt.durationSeconds;
      const label = attempt.kind === "sync" ? `sync ${attempt.attemptIndex}/${args.syncAttempts}` : "durability";
      const frameIntervalMs = 1000 / fpsValue(args);
      const startPhaseDelayMs = (attempt.phasePercent / 100) * frameIntervalMs;
      const stopPhaseDelayMs = (attempt.stopPhasePercent / 100) * frameIntervalMs;
      console.log(
        `Running OBS app E2E ${label} recording for ${durationSeconds}s ` +
          `(startPhase=${attempt.phasePercent.toFixed(1)}% stopPhase=${attempt.stopPhasePercent.toFixed(1)}%)...`,
      );
      const attemptLogCheckpoint = createObsLogCheckpoint(portableConfig);
      const attemptArtifactSnapshot = artifactSnapshot(artifactRoot);
      const attemptFailureWatch: AlphaRecorderFailureWatch = {
        portableConfig,
        artifactRoot,
        obsPid: obs.pid,
        checkpoint: attemptLogCheckpoint,
        suppressFailure: args.expectedResult !== "normal",
      };
      resetOverloadMonitor(overload);
      if (startPhaseDelayMs > 0) {
        await delay(startPhaseDelayMs);
      }
      await startRecordingWithRetry(socket, attemptFailureWatch);
      await runRecordingSchedule(
        socket,
        durationSeconds * 1000 + stopPhaseDelayMs,
        args,
        overload,
        attemptFailureWatch,
      );
      if (args.overloadPulseDelayMs > 0) {
        await socket.request("SetInputSettings", {
          inputName: "AlphaRecorderMovingAlpha",
          inputSettings: { render_delay_ms: 0 },
          overlay: true,
        });
      }
      if (!args.allowOverload) {
        await stopRecordingAfterOverload(socket, overload);
        throwIfOverloaded(overload, artifactRoot, args.width, args.height, fpsValue(args), false);
      }
      throwIfAlphaRecorderFailureDetected(attemptFailureWatch);
      const stopResponse = await socket.request("StopRecord");
      await waitForRecordState(socket, false, stopWaitTimeoutSeconds(durationSeconds), attemptFailureWatch);
      const rgbPath = String(stopResponse?.outputPath ?? "");
      let alphaArtifactPath = "";
      const normalAlphaPath = rgbPath
        ? alphaPathForRgb(rgbPath, expectedFinalizationFormat)
        : "";
      if (rgbPath) {
        alphaArtifactPath = await waitForExpectedAlphaResult(
          artifactRoot,
          normalAlphaPath,
          args.expectedResult,
          attemptArtifactSnapshot,
          stopWaitTimeoutSeconds(durationSeconds),
          attemptFailureWatch,
        );
      }
      const actualResult: ResolvedExpectedResult =
        args.expectedResult === "normal-or-sync-invalid"
          ? alphaArtifactPath === normalAlphaPath
            ? "normal"
            : "sync-invalid"
          : args.expectedResult;
      scanObsLogsForOverload(portableConfig, overload, attemptLogCheckpoint);
      const attemptOverloaded = overload.seen;
      throwIfOverloaded(overload, artifactRoot, args.width, args.height, fpsValue(args), args.allowOverload);
      if (actualResult === "normal") {
        throwIfAlphaRecorderLoggedError(portableConfig, artifactRoot, attemptLogCheckpoint);
      }
      const splitRgbPaths = args.splitAtMs >= 0
        ? newRgbRecordingPaths(artifactRoot, attemptArtifactSnapshot)
        : [];
      const attemptRgbPaths = splitRgbPaths.length > 0 ? splitRgbPaths : [rgbPath];
      for (const attemptRgbPath of attemptRgbPaths) {
        const attemptAlphaPath =
          actualResult === "normal" ||
          actualResult === "split-published" ||
          actualResult === "split-isolated"
            ? alphaPathForRgb(attemptRgbPath, expectedFinalizationFormat)
            : attemptRgbPath === rgbPath
              ? alphaArtifactPath
              : "";
        if (actualResult === "normal") {
          await waitForAlphaOutputSettled(
            artifactRoot,
            attemptAlphaPath,
            stopWaitTimeoutSeconds(durationSeconds),
            attemptFailureWatch,
            args.expectedResult === "normal-or-sync-invalid",
          );
        }
        recordedAttempts.push({
          ...attempt,
          rgbPath: attemptRgbPath,
          alphaArtifactPath: isNonEmptyFile(attemptAlphaPath) ? attemptAlphaPath : "",
          overloaded: attemptOverloaded,
          actualResult,
        });
      }
    }

    await cleanupObs();

    for (const attempt of recordedAttempts) {
      const durationSeconds = attempt.durationSeconds;
      const label = attempt.kind === "sync" ? `sync ${attempt.attemptIndex}/${args.syncAttempts}` : "durability";
      const ffprobe = findTool(stageBin, args.repoRoot, "ffprobe");
      const observedMainReorderDepth = packetPresentationReorderDepth(ffprobe, attempt.rgbPath);
      const observedMainPacketTiming = packetTimingSummary(ffprobe, attempt.rgbPath);
      if (attempt.actualResult !== "normal") {
        const splitLifecycleResult =
          attempt.actualResult === "split-published" ||
          attempt.actualResult === "split-isolated";
        let syncVerification = splitLifecycleResult
          ? undefined
          : await verifyExpectedNonNormalOutput(
              stageBin,
              args.repoRoot,
              attempt.rgbPath,
              attempt.alphaArtifactPath,
              args.width,
              args.height,
              fpsValue(args),
            );
        if (splitLifecycleResult) {
          await verifyArtifactDecodes(
            stageBin,
            args.repoRoot,
            attempt.rgbPath,
            attempt.alphaArtifactPath,
          );
          if (attempt.alphaArtifactPath) {
            const ffmpeg = findTool(stageBin, args.repoRoot, "ffmpeg");
            syncVerification = verifyRgbAlphaFrameSync(
              ffmpeg,
              ffprobe,
              attempt.rgbPath,
              attempt.alphaArtifactPath,
              args.width,
              args.height,
              fpsValue(args),
              attempt.overloaded,
              false,
              true,
              false,
              false,
            );
          }
        }
        attempts.push({
          ...attempt,
          alphaPath: attempt.alphaArtifactPath,
          rgbProbe: undefined,
          alphaProbe: undefined,
          syncVerification,
          observedMainReorderDepth,
          observedMainPacketTiming,
        });
        console.log(
          `  ok expected ${attempt.actualResult} ${label}: rgb=${basename(attempt.rgbPath)} ` +
            `alphaArtifact=${attempt.alphaArtifactPath ? basename(attempt.alphaArtifactPath) : "<none>"} ` +
            `contentOffset=${syncVerification?.bestContentOffset.offset ?? "n/a"} ` +
            `mainReorderDepth=${observedMainReorderDepth}`,
        );
        continue;
      }
      const outputs = await verifyRecordingOutputs(
        stageBin,
        args.repoRoot,
        artifactRoot,
        portableConfig,
        attempt.rgbPath,
        expectedFinalizationFormat,
        args.rgbEncoder,
        args.width,
        args.height,
        fpsValue(args),
        (args.allowOverload && attempt.overloaded) ||
          args.testFault === "replay-evidence-gap",
        args.pauseAtMs >= 0,
        args.verifyNleTimeline,
        args.strictAllFrames,
        args.expectedResult === "normal-or-sync-invalid",
      );
      attempts.push({
        ...attempt,
        ...outputs,
        observedMainReorderDepth,
        observedMainPacketTiming,
      });
      console.log(
        `  ok ${label} ${durationSeconds}s: rgb=${outputs.syncVerification.rgbFrames} alpha=${outputs.syncVerification.alphaFrames} ` +
          `overloaded=${attempt.overloaded} ` +
          `frameCodeOffset=${outputs.syncVerification.bestFrameCodeOffset.offset} ` +
          `expectedFrameCodeOffset=${outputs.syncVerification.expectedFrameCodeOffset} ` +
          `mainReorderDepth=${outputs.syncVerification.mainReorderDepth} ` +
          `mainPtsGaps=${outputs.syncVerification.mainPacketTiming.gapCount} ` +
          `alphaConstantPacketDuration=${outputs.syncVerification.alphaConstantPacketDuration ?? "n/a"} ` +
          `contentOffset=${outputs.syncVerification.bestContentOffset.offset} ` +
          `mismatches=${outputs.syncVerification.frameCodeMismatches}/${outputs.syncVerification.maskBoundsMismatches} ` +
          `duplicates=${outputs.syncVerification.rgbConsecutiveDuplicateCodes}/${outputs.syncVerification.alphaConsecutiveDuplicateCodes}`,
      );
    }

    const lastAttempt = attempts[attempts.length - 1];
    const performanceTelemetry = collectAlphaRecorderPerformanceTelemetry(portableConfig);
    const gpuReplayTelemetry = collectGpuReplayTelemetry(portableConfig);
    const observedConditions = {
      mainPtsGap: attempts.some((attempt) => attempt.observedMainPacketTiming.gapCount > 0),
      packetReorder: attempts.some((attempt) => attempt.observedMainReorderDepth > 0),
      replayUnderflow: gpuReplayTelemetry.some((entry) => entry.underflows > 0),
      replayCatchup: gpuReplayTelemetry.some(
        (entry) => entry.catchupSlots > 0 && entry.skippedStale > 0,
      ),
      overload: attempts.some((attempt) => attempt.overloaded),
      tailRepeat: gpuReplayTelemetry.some((entry) => entry.repeatedSlots > 0),
    };
    const missingRequiredConditions: string[] = [];
    if (args.requireMainPtsGap && !observedConditions.mainPtsGap) {
      missingRequiredConditions.push("main PTS gap");
    }
    if (args.requirePacketReorder && !observedConditions.packetReorder) {
      missingRequiredConditions.push("packet reorder");
    }
    if (args.requireReplayUnderflow && !observedConditions.replayUnderflow) {
      missingRequiredConditions.push("replay underflow");
    }
    if (args.requireReplayCatchup && !observedConditions.replayCatchup) {
      missingRequiredConditions.push("replay backlog catch-up");
    }
    if (args.requireOverload && !observedConditions.overload) {
      missingRequiredConditions.push("OBS overload");
    }
    if (args.requireTailRepeat && !observedConditions.tailRepeat) {
      missingRequiredConditions.push("tail repeat");
    }
    const diagnosticLogs = copyAlphaRecorderDiagnosticLogs(portableConfig, artifactRoot);
    const performanceTelemetryPath = join(artifactRoot, "alpha-recorder-performance.json");
    const summaryPath = join(artifactRoot, "obs-app-summary.json");
    writeText(
      performanceTelemetryPath,
      JSON.stringify(
        {
          artifactRoot,
          rgbEncoder: args.rgbEncoder,
          finalizationFormat: expectedFinalizationFormat,
          hevc: {
            qualityProfile: args.hevcQualityProfile,
            qualityCq: args.hevcQualityCq,
            preset: args.hevcPreset,
            nvencTune: args.hevcNvencTune,
            gopSize: args.hevcGopSize,
          },
          nleTimeline: {
            verify: args.verifyNleTimeline,
          },
          width: args.width,
          height: args.height,
          fps: { num: args.fpsNum, den: args.fpsDen, value: fpsValue(args) },
          syncRecordSeconds: args.syncRecordSeconds,
          syncAttempts: args.syncAttempts,
          durabilityRecordSeconds: args.durabilityRecordSeconds,
          allowOverload: args.allowOverload,
          strictAllFrames: args.strictAllFrames,
          phaseSweepSteps: args.phaseSweepSteps,
          observedConditions,
          missingRequiredConditions,
          diagnosticLogs,
          attempts: attempts.map((attempt) => ({
            kind: attempt.kind,
            attemptIndex: attempt.attemptIndex,
            durationSeconds: attempt.durationSeconds,
            overloaded: attempt.overloaded,
            rgbPath: attempt.rgbPath,
            alphaPath: attempt.alphaPath,
            syncVerification: attempt.syncVerification,
          })),
          telemetry: performanceTelemetry,
          gpuReplayTelemetry,
        },
        null,
        2,
      ),
    );
    writeText(
      summaryPath,
      JSON.stringify(
        {
          ok: true,
          artifactRoot,
          obsProcessLogPath,
          performanceTelemetryPath,
          performanceTelemetry,
          syncRecordSeconds: args.syncRecordSeconds,
          syncAttempts: args.syncAttempts,
          durabilityRecordSeconds: args.durabilityRecordSeconds,
          allowOverload: args.allowOverload,
          strictAllFrames: args.strictAllFrames,
          phaseSweepSteps: args.phaseSweepSteps,
          observedConditions,
          missingRequiredConditions,
          diagnosticLogs,
          gpuReplayTelemetry,
          attempts,
          rgbPath: lastAttempt?.rgbPath,
          alphaPath: lastAttempt?.alphaPath,
          rgbProbe: lastAttempt?.rgbProbe,
          alphaProbe: lastAttempt?.alphaProbe,
          syncVerification: lastAttempt?.syncVerification,
        },
        null,
        2,
      ),
    );

    if (missingRequiredConditions.length > 0) {
      throw new Error(
        `OBS app E2E did not exercise required runtime conditions: ${missingRequiredConditions.join(", ")}. ` +
          `Observed=${JSON.stringify(observedConditions)}. Artifacts: ${artifactRoot}`,
      );
    }

    console.log("OBS app E2E passed.");
    console.log(`  summary: ${summaryPath}`);
    console.log(`  telemetry: ${performanceTelemetryPath}`);
    console.log(`  rgb: ${lastAttempt?.rgbPath}`);
    console.log(`  alpha: ${lastAttempt?.alphaPath}`);
    for (const telemetry of performanceTelemetry) {
      const label = telemetry.maskPath ? basename(telemetry.maskPath) : "alpha segment";
      console.log(`  perf ${label}: ${compactTelemetryLine(telemetry.line)}`);
    }
  } finally {
    await cleanupObs();
  }
}

await main();
