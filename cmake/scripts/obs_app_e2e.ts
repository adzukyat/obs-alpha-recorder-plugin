#!/usr/bin/env bun

import { spawn, spawnSync } from "bun";
import { createHash, randomBytes } from "node:crypto";
import { copyFileSync, createWriteStream, existsSync, mkdirSync, readFileSync, readdirSync, statSync, writeFileSync } from "node:fs";
import { basename, dirname, join, resolve } from "node:path";
import { platform } from "node:process";

type Args = {
  repoRoot: string;
  stageDir: string;
  buildDir?: string;
  configuration: string;
  port: number;
  syncRecordSeconds: number;
  syncAttempts: number;
  durabilityRecordSeconds: number;
  width: number;
  height: number;
  fps: number;
  recordFormat: string;
  withAudio: boolean;
  rgbEncoder: string;
  finalizationFormat: string;
  hevcQualityProfile: string;
  hevcQualityCq: number;
  hevcPreset: string;
  hevcNvencTune: string;
  hevcGopSize: number;
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
    fps: 60,
    recordFormat: "mkv",
    withAudio: false,
    rgbEncoder: "software",
    finalizationFormat: "mask_png_mov",
    hevcQualityProfile: "high_quality",
    hevcQualityCq: 19,
    hevcPreset: "nvenc_p3",
    hevcNvencTune: "hq",
    hevcGopSize: 0,
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
        args.fps = Number(value);
        ++index;
        break;
      case "--record-format":
        args.recordFormat = value;
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
  args.syncRecordSeconds = Math.max(1, Math.floor(args.syncRecordSeconds));
  args.syncAttempts = Math.max(1, Math.floor(args.syncAttempts));
  args.durabilityRecordSeconds = Math.max(1, Math.floor(args.durabilityRecordSeconds));
  args.hevcQualityCq = Math.max(0, Math.min(51, Math.floor(args.hevcQualityCq)));
  args.hevcGopSize = Math.max(0, Math.min(1000, Math.floor(args.hevcGopSize)));

  return args;
}

type VerificationAttempt = {
  kind: "sync" | "durability";
  attemptIndex: number;
  durationSeconds: number;
};

function verificationAttempts(syncRecordSeconds: number, syncAttempts: number, durabilityRecordSeconds: number): VerificationAttempt[] {
  const attempts: VerificationAttempt[] = [];
  for (let attemptIndex = 1; attemptIndex <= syncAttempts; ++attemptIndex) {
    attempts.push({ kind: "sync", attemptIndex, durationSeconds: syncRecordSeconds });
  }
  attempts.push({ kind: "durability", attemptIndex: 1, durationSeconds: durabilityRecordSeconds });
  return attempts;
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

function assertNoInvalidAlphaArtifacts(artifactRoot: string): void {
  const invalidArtifacts = readdirSync(artifactRoot).filter(
    (entry) => entry.includes(".alpha.tmp.") || entry.includes(".sync-invalid"),
  );
  if (invalidArtifacts.length > 0) {
    throw new Error(`Invalid alpha artifacts were left in the recording directory: ${invalidArtifacts.join(", ")}`);
  }
}

function alphaPathForRgb(rgbPath: string, expectedFinalizationFormat: string): string {
  const basePath = rgbPath.replace(/\.[^.\\/]+$/, "");
  const alphaExtension = expectedFinalizationFormat === "mask_png_mov" ? ".mov" : ".mp4";
  return `${basePath}.alpha${alphaExtension}`;
}

function invalidAlphaArtifacts(artifactRoot: string): string[] {
  return readdirSync(artifactRoot).filter((entry) => entry.includes(".alpha.tmp.") || entry.includes(".sync-invalid"));
}

async function waitForAlphaOutputSettled(
  artifactRoot: string,
  alphaPath: string,
  timeoutSeconds: number,
  failureWatch?: AlphaRecorderFailureWatch,
): Promise<void> {
  const deadline = Date.now() + timeoutSeconds * 1000;
  let lastInvalidArtifacts: string[] = [];
  let cleanMissingSince: number | null = null;
  while (Date.now() < deadline) {
    throwIfAlphaRecorderFailureDetected(failureWatch);
    lastInvalidArtifacts = invalidAlphaArtifacts(artifactRoot);
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
  if (existsSync(depsRoot)) {
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
  bestFrameCodeOffset: OffsetSummary;
  bestContentOffset: OffsetSummary;
  overloadTerminalFrameCodeOffset?: OffsetSummary;
  overloadTerminalContentOffset?: OffsetSummary;
  frameCodeMismatches: number;
  maskBoundsMismatches: number;
};

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

  const frameCountDelta = Math.abs(rgbFrames - alphaFrames);
  const toleratedFrameCountDelta = overloadObserved ? toleratedTerminalFrames : 0;
  if (frameCountDelta > toleratedFrameCountDelta) {
    throw new Error(
      `Decoded RGB/alpha frame counts differ beyond tolerance: rgb=${rgbFrames} alpha=${alphaFrames}; ` +
        `tolerance=${toleratedFrameCountDelta}; ` +
        `frameCodes=${rgbCodes.length}/${alphaCodes.length}; ` +
        `bestFrameCodeOffset=${bestCodeOffset.offset} matched=${bestCodeOffset.matches}/${bestCodeOffset.total}; ` +
        `bestContentOffset=${bestOffset.offset} matched=${bestOffset.matches}/${bestOffset.total}`,
    );
  }

  if (bestCodeOffset.offset !== 0) {
    throw new Error(
      `RGB/alpha frame-code offset is not zero: offset=${bestCodeOffset.offset} matched=${bestCodeOffset.matches}/${bestCodeOffset.total}; ` +
        `frames=${rgbFrames}/${alphaFrames}; bestContentOffset=${bestOffset.offset} matched=${bestOffset.matches}/${bestOffset.total}`,
    );
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
  const overloadOffsetStable =
    overloadObserved &&
    bestCodeOffset.offset === 0 &&
    overloadTerminalFrameCodeOffset.offset === 0 &&
    overloadTerminalContentOffset.offset === 0 &&
    overloadTerminalFrameCodeOffset.total > 0 &&
    overloadTerminalContentOffset.total > 0;

  if (overloadObserved && !overloadOffsetStable) {
    throw new Error(
      `RGB/alpha offset drifted after overload: ` +
        `globalFrameCode=${bestCodeOffset.offset} matched=${bestCodeOffset.matches}/${bestCodeOffset.total}; ` +
        `globalContent=${bestOffset.offset} matched=${bestOffset.matches}/${bestOffset.total}; ` +
        `terminalFrameCode=${overloadTerminalFrameCodeOffset.offset} ` +
        `matched=${overloadTerminalFrameCodeOffset.matches}/${overloadTerminalFrameCodeOffset.total}; ` +
        `terminalContent=${overloadTerminalContentOffset.offset} ` +
        `matched=${overloadTerminalContentOffset.matches}/${overloadTerminalContentOffset.total}; ` +
        `terminalWindow=${overloadTerminalWindow}`,
    );
  }
  if (
    overloadObserved &&
    overloadOffsetStable &&
    bestOffset.offset !== 0
  ) {
    console.warn(
      `RGB/alpha mask bounds preferred a non-zero offset under overload, but frame-code offset remains zero: ` +
        `globalContent=${bestOffset.offset} matched=${bestOffset.matches}/${bestOffset.total}; ` +
        `terminalContent=${overloadTerminalContentOffset.offset} ` +
        `matched=${overloadTerminalContentOffset.matches}/${overloadTerminalContentOffset.total}; ` +
        `globalFrameCode=${bestCodeOffset.offset} matched=${bestCodeOffset.matches}/${bestCodeOffset.total}; ` +
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

  for (let frame = 0; frame < comparedFrames; ++frame) {
    if (rgbCodes[frame] !== alphaCodes[frame]) {
      ++frameCodeMismatches;
      if (frame < toleratedBoundaryFrames) {
        ++startFrameCodeMismatches;
      } else if (frame >= comparedFrames - toleratedBoundaryFrames) {
        ++terminalFrameCodeMismatches;
      } else {
        ++interiorFrameCodeMismatches;
      }
      if (firstFrameCodeMismatches.length < toleratedPerFrameMismatches + 1) {
        firstFrameCodeMismatches.push(`${frame}:${rgbCodes[frame]}/${alphaCodes[frame]}`);
      }
    }

    if (!boundsAreSimilar(rgbBounds[frame], alphaBounds[frame])) {
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
          `${frame}:rgb=${boundsText(rgbBounds[frame])} alpha=${boundsText(alphaBounds[frame])} local=${candidates.length === 0 ? "none" : candidates.join(",")}`,
        );
      }
    }
  }

  if (
    interiorFrameCodeMismatches > toleratedPerFrameMismatches ||
    startFrameCodeMismatches > toleratedBoundaryFrames ||
    terminalFrameCodeMismatches > toleratedBoundaryFrames
  ) {
    if (overloadOffsetStable) {
      console.warn(
        `RGB/alpha frame-code mismatches exceed strict tolerance under overload, but offset remains zero: ` +
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
    if (overloadOffsetStable) {
      console.warn(
        `RGB/alpha mask bounds mismatches exceed strict tolerance under overload, but offset remains zero: ` +
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

  return {
    rgbFrames,
    alphaFrames,
    bestFrameCodeOffset: bestCodeOffset,
    bestContentOffset: bestOffset,
    overloadTerminalFrameCodeOffset: overloadObserved ? overloadTerminalFrameCodeOffset : undefined,
    overloadTerminalContentOffset: overloadObserved ? overloadTerminalContentOffset : undefined,
    frameCodeMismatches,
    maskBoundsMismatches,
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
  assertNoInvalidAlphaArtifacts(artifactRoot);

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

  const syncVerification = verifyRgbAlphaFrameSync(ffmpeg, ffprobe, rgbPath, alphaPath, width, height, fps, overloadObserved);
  return { rgbPath, alphaPath, rgbProbe, alphaProbe, syncVerification };
}

async function main(): Promise<void> {
  const args = parseArgs(Bun.argv.slice(2));
  const port = args.port > 0 ? args.port : await freePort();
  const password = randomBytes(16).toString("hex");
  const { exe: obsExe, cwd: obsCwd, contentRoot, runtimeBin } = resolveObsExecutable(args.stageDir);
  const artifactRoot = join(args.repoRoot, "out", "e2e", "obs-app", new Date().toISOString().replace(/[-:]/g, "").replace(/\..+$/, ""));
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
Mode=Simple
FilenameFormatting=%CCYY-%MM-%DD %hh-%mm-%ss

[AdvOut]
RecType=Standard
RecFilePath=${artifactRoot.replaceAll("\\", "/")}
RecFormat2=${args.recordFormat}
RecTracks=1
RecEncoder=obs_x264
Encoder=obs_x264
ApplyServiceSettings=true
RecUseRescale=false
TrackIndex=1
RecSplitFileType=Time

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

[Video]
BaseCX=${args.width}
BaseCY=${args.height}
OutputCX=${args.width}
OutputCY=${args.height}
FPSType=0
FPSCommon=${args.fps}
FPSInt=${args.fps}
FPSNum=${args.fps}
FPSDen=1
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

    const recordedAttempts: Array<VerificationAttempt & { rgbPath: string; overloaded: boolean }> = [];
    const attempts: Array<{
      kind: "sync" | "durability";
      attemptIndex: number;
      durationSeconds: number;
      overloaded: boolean;
      rgbPath: string;
      alphaPath: string;
      rgbProbe: any;
      alphaProbe: any;
      syncVerification: SyncVerification;
    }> = [];
    for (const attempt of verificationAttempts(args.syncRecordSeconds, args.syncAttempts, args.durabilityRecordSeconds)) {
      const durationSeconds = attempt.durationSeconds;
      const label = attempt.kind === "sync" ? `sync ${attempt.attemptIndex}/${args.syncAttempts}` : "durability";
      console.log(`Running OBS app E2E ${label} recording for ${durationSeconds}s...`);
      const attemptLogCheckpoint = createObsLogCheckpoint(portableConfig);
      const attemptFailureWatch: AlphaRecorderFailureWatch = {
        portableConfig,
        artifactRoot,
        obsPid: obs.pid,
        checkpoint: attemptLogCheckpoint,
      };
      resetOverloadMonitor(overload);
      await socket.request("StartRecord");
      await waitForRecordState(socket, true, 30, attemptFailureWatch);
      if (args.allowOverload) {
        await delayUnlessOverloaded(durationSeconds * 1000, { seen: false, firstLine: "" }, attemptFailureWatch);
      } else {
        await delayUnlessOverloaded(durationSeconds * 1000, overload, attemptFailureWatch);
        await stopRecordingAfterOverload(socket, overload);
        throwIfOverloaded(overload, artifactRoot, args.width, args.height, args.fps, false);
      }
      throwIfAlphaRecorderFailureDetected(attemptFailureWatch);
      const stopResponse = await socket.request("StopRecord");
      await waitForRecordState(socket, false, stopWaitTimeoutSeconds(durationSeconds), attemptFailureWatch);
      const rgbPath = String(stopResponse?.outputPath ?? "");
      if (rgbPath) {
        await waitForAlphaOutputSettled(
          artifactRoot,
          alphaPathForRgb(rgbPath, expectedFinalizationFormat),
          stopWaitTimeoutSeconds(durationSeconds),
          attemptFailureWatch,
        );
      }
      scanObsLogsForOverload(portableConfig, overload, attemptLogCheckpoint);
      const attemptOverloaded = overload.seen;
      throwIfOverloaded(overload, artifactRoot, args.width, args.height, args.fps, args.allowOverload);
      throwIfAlphaRecorderLoggedError(portableConfig, artifactRoot, attemptLogCheckpoint);
      recordedAttempts.push({ ...attempt, rgbPath, overloaded: attemptOverloaded });
    }

    await cleanupObs();

    for (const attempt of recordedAttempts) {
      const durationSeconds = attempt.durationSeconds;
      const label = attempt.kind === "sync" ? `sync ${attempt.attemptIndex}/${args.syncAttempts}` : "durability";
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
        args.fps,
        args.allowOverload && attempt.overloaded,
      );
      attempts.push({ ...attempt, ...outputs });
      console.log(
        `  ok ${label} ${durationSeconds}s: rgb=${outputs.syncVerification.rgbFrames} alpha=${outputs.syncVerification.alphaFrames} ` +
          `overloaded=${attempt.overloaded} ` +
          `frameCodeOffset=${outputs.syncVerification.bestFrameCodeOffset.offset} ` +
          `contentOffset=${outputs.syncVerification.bestContentOffset.offset} ` +
          `mismatches=${outputs.syncVerification.frameCodeMismatches}/${outputs.syncVerification.maskBoundsMismatches}`,
      );
    }

    const lastAttempt = attempts[attempts.length - 1];
    const performanceTelemetry = collectAlphaRecorderPerformanceTelemetry(portableConfig);
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
          width: args.width,
          height: args.height,
          fps: args.fps,
          syncRecordSeconds: args.syncRecordSeconds,
          syncAttempts: args.syncAttempts,
          durabilityRecordSeconds: args.durabilityRecordSeconds,
          allowOverload: args.allowOverload,
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
          diagnosticLogs,
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
