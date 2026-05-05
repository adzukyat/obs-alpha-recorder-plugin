#!/usr/bin/env bun

import { spawn, spawnSync } from "bun";
import { createHash, randomBytes } from "node:crypto";
import { existsSync, mkdirSync, readdirSync, statSync, writeFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { platform } from "node:process";

type Args = {
  repoRoot: string;
  stageDir: string;
  buildDir?: string;
  configuration: string;
  port: number;
  recordSeconds: number;
  maxRecordSeconds: number;
  width: number;
  height: number;
  fps: number;
  rgbEncoder: string;
  finalizationFormat: string;
  keepObsOpen: boolean;
};

function parseArgs(argv: string[]): Args {
  const args: Args = {
    repoRoot: "",
    stageDir: "",
    configuration: "RelWithDebInfo",
    port: 0,
    recordSeconds: 5,
    maxRecordSeconds: 30,
    width: 1920,
    height: 1080,
    fps: 60,
    rgbEncoder: "software",
    finalizationFormat: "mask_png_mov",
    keepObsOpen: false,
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
        args.recordSeconds = Number(value);
        ++index;
        break;
      case "--max-record-seconds":
        args.maxRecordSeconds = Number(value);
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
      case "--rgb-encoder":
        args.rgbEncoder = value;
        ++index;
        break;
      case "--finalization-format":
        args.finalizationFormat = value;
        ++index;
        break;
      case "--keep-obs-open":
        args.keepObsOpen = true;
        break;
      default:
        throw new Error(`Unknown argument: ${key}`);
    }
  }

  if (!args.repoRoot || !args.stageDir) {
    throw new Error("--repo-root and --stage-dir are required");
  }

  return args;
}

function verificationDurations(startSeconds: number, maxSeconds: number): number[] {
  const start = Math.max(1, Math.floor(startSeconds));
  const max = Math.max(start, Math.floor(maxSeconds));
  const durations: number[] = [];
  let current = start;

  while (current < max) {
    durations.push(current);
    current = Math.min(max, current * 2);
  }
  durations.push(max);
  return durations;
}

function simpleRgbEncoder(encoder: string): string {
  switch (encoder) {
    case "software":
      return platform === "darwin" ? "apple_h264" : "x264";
    case "hardware_hevc":
      if (platform === "darwin") {
        return "apple_hevc";
      }
      return "nvenc_hevc";
    default:
      throw new Error(`Unsupported RGB encoder profile: ${encoder}`);
  }
}

function normalizedFinalizationFormat(format: string): string {
  if (format === "mask_prores_422" || format === "prores_4444") {
    return "mask_png_mov";
  }
  if (format === "lossless_hevc") {
    return "mask_hevc_nvenc";
  }
  return format;
}

function writeText(path: string, text: string): void {
  mkdirSync(dirname(path), { recursive: true });
  writeFileSync(path, text, "utf8");
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

class ObsWebSocket {
  private socket: WebSocket;
  private pending: Array<(value: unknown) => void> = [];
  private requestCounter = 0;

  private constructor(socket: WebSocket) {
    this.socket = socket;
    this.socket.addEventListener("message", (event) => {
      const resolver = this.pending.shift();
      if (resolver) {
        resolver(JSON.parse(String(event.data)));
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
        const hello = await client.receive();
        if ((hello as any).op !== 0) {
          throw new Error(`Expected obs-websocket Hello, got ${JSON.stringify(hello)}`);
        }

        const identify: Record<string, unknown> = { rpcVersion: 1, eventSubscriptions: 0 };
        const auth = (hello as any).d?.authentication;
        if (auth) {
          identify.authentication = b64Sha256(b64Sha256(password + auth.salt) + auth.challenge);
        }

        client.send({ op: 1, d: identify });
        const identified = await client.receive();
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

  receive(): Promise<unknown> {
    return new Promise((resolveReceive) => this.pending.push(resolveReceive));
  }

  async request(requestType: string, requestData: Record<string, unknown> = {}): Promise<any> {
    this.requestCounter += 1;
    const requestId = `alpha-recorder-e2e-${this.requestCounter}`;
    this.send({ op: 6, d: { requestType, requestId, requestData } });

    while (true) {
      const message = (await this.receive()) as any;
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

async function waitForRecordState(socket: ObsWebSocket, active: boolean, timeoutSeconds = 30): Promise<void> {
  const deadline = Date.now() + timeoutSeconds * 1000;
  let lastStatus: unknown = undefined;
  while (Date.now() < deadline) {
    const status = await socket.request("GetRecordStatus");
    lastStatus = status;
    if (Boolean(status.outputActive) === active) {
      return;
    }
    await delay(500);
  }
  throw new Error(`Timed out waiting for recording active=${active}; last status=${JSON.stringify(lastStatus)}`);
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

function decodeFrameCodes(ffmpeg: string, path: string, pixFmt: "rgb24" | "gray"): number[] {
  const crop = frameCodeCropFilter();
  const channels = pixFmt === "gray" ? 1 : 3;
  const frameSize = crop.width * crop.height * channels;
  const bytes = checkedOutput(
    ffmpeg,
    ["-v", "error", "-i", path, "-an", "-vf", crop.filter, "-fps_mode", "passthrough", "-f", "rawvideo", "-pix_fmt", pixFmt, "-"],
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

function verifyRgbAlphaFrameSync(ffmpeg: string, ffprobe: string, rgbPath: string, alphaPath: string, width: number, height: number, fps: number): void {
  const toleratedTerminalFrames = 3;
  const toleratedPerFrameMismatches = 3;
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
  if (frameCountDelta > toleratedTerminalFrames) {
    throw new Error(
      `Decoded RGB/alpha frame counts differ beyond tolerance: rgb=${rgbFrames} alpha=${alphaFrames}; ` +
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
  let frameCodeMismatches = 0;
  let maskBoundsMismatches = 0;
  const firstFrameCodeMismatches: string[] = [];
  const firstMaskBoundsMismatches: string[] = [];

  for (let frame = 0; frame < comparedFrames; ++frame) {
    if (rgbCodes[frame] !== alphaCodes[frame]) {
      ++frameCodeMismatches;
      if (firstFrameCodeMismatches.length < toleratedPerFrameMismatches + 1) {
        firstFrameCodeMismatches.push(`${frame}:${rgbCodes[frame]}/${alphaCodes[frame]}`);
      }
    }

    if (!boundsAreSimilar(rgbBounds[frame], alphaBounds[frame])) {
      ++maskBoundsMismatches;
      if (firstMaskBoundsMismatches.length < toleratedPerFrameMismatches + 1) {
        const candidates = localCandidateOffsets(rgbBounds, alphaBounds, frame);
        firstMaskBoundsMismatches.push(
          `${frame}:rgb=${boundsText(rgbBounds[frame])} alpha=${boundsText(alphaBounds[frame])} local=${candidates.length === 0 ? "none" : candidates.join(",")}`,
        );
      }
    }
  }

  if (frameCodeMismatches > toleratedPerFrameMismatches) {
    throw new Error(
      `RGB/alpha frame-code mismatches exceed tolerance: mismatches=${frameCodeMismatches}/${comparedFrames}; ` +
        `examples=${firstFrameCodeMismatches.join("; ")}; ` +
        `bestFrameCodeOffset=${bestCodeOffset.offset} matched=${bestCodeOffset.matches}/${bestCodeOffset.total}`,
    );
  }

  if (maskBoundsMismatches > toleratedPerFrameMismatches) {
    throw new Error(
      `RGB/alpha mask bounds mismatches exceed tolerance: mismatches=${maskBoundsMismatches}/${comparedFrames}; ` +
        `examples=${firstMaskBoundsMismatches.join("; ")}; ` +
        `bestContentOffset=${bestOffset.offset} matched=${bestOffset.matches}/${bestOffset.total}`,
    );
  }

}

async function verifyRecordingOutputs(
  stageBin: string,
  repoRoot: string,
  artifactRoot: string,
  rgbPath: string,
  expectedFinalizationFormat: string,
  rgbEncoder: string,
  width: number,
  height: number,
  fps: number,
): Promise<{ rgbPath: string; alphaPath: string; rgbProbe: any; alphaProbe: any }> {
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

  const basePath = rgbPath.replace(/\.[^.\\/]+$/, "");
  const alphaExtension = expectedFinalizationFormat === "mask_png_mov" ? ".mov" : ".mp4";
  const alphaPath = `${basePath}.alpha${alphaExtension}`;

  await waitForPath(rgbPath, 30);
  await waitForPath(alphaPath, 120);

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
  if (rgbEncoder === "hardware_hevc" && rgbStream.codec_name !== "hevc") {
    throw new Error(`RGB recording probe did not report HEVC for hardware_hevc profile: ${JSON.stringify(rgbProbe)}`);
  }

  const alphaStream = alphaProbe.streams?.[0] ?? {};
  const alphaPixFmt = String(alphaStream.pix_fmt ?? "");
  if (alphaPixFmt.startsWith("yuva") || alphaPixFmt === "rgba" || alphaPixFmt === "bgra" || alphaPixFmt === "argb") {
    throw new Error(`Alpha mask movie unexpectedly carries an alpha-capable pixel format: ${JSON.stringify(alphaProbe)}`);
  }
  if (expectedFinalizationFormat === "mask_hevc_nvenc" || expectedFinalizationFormat === "mask_hevc_amf") {
    if (alphaStream.codec_name !== "hevc") {
      throw new Error(`Alpha movie probe did not report HEVC: ${JSON.stringify(alphaProbe)}`);
    }
  } else if (alphaStream.codec_name !== "png") {
    throw new Error(`Alpha movie probe did not report PNG MOV: ${JSON.stringify(alphaProbe)}`);
  }

  verifyRgbAlphaFrameSync(ffmpeg, ffprobe, rgbPath, alphaPath, width, height, fps);
  return { rgbPath, alphaPath, rgbProbe, alphaProbe };
}

async function main(): Promise<void> {
  const args = parseArgs(Bun.argv.slice(2));
  const port = args.port > 0 ? args.port : await freePort();
  const password = randomBytes(16).toString("hex");
  const { exe: obsExe, cwd: obsCwd, contentRoot, runtimeBin } = resolveObsExecutable(args.stageDir);
  const artifactRoot = join(args.repoRoot, "out", "e2e", "obs-app", new Date().toISOString().replace(/[-:]/g, "").replace(/\..+$/, ""));
  mkdirSync(artifactRoot, { recursive: true });

  const homeRoot = join(artifactRoot, "home");
  const portableConfig =
    platform === "darwin" ? join(homeRoot, "Library", "Application Support", "obs-studio") : join(contentRoot, "config", "obs-studio");
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
RecFormat2=mkv
RecTracks=1
RecEncoder=obs_x264
Encoder=obs_x264
ApplyServiceSettings=true
RecUseRescale=false
TrackIndex=1
RecSplitFileType=Time

[SimpleOutput]
FilePath=${artifactRoot.replaceAll("\\", "/")}
RecFormat2=mkv
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
    PATH: `${stageBin}${platform === "win32" ? ";" : ":"}${process.env.PATH ?? ""}`,
    DYLD_LIBRARY_PATH:
      platform === "darwin" ? `${stageBin}:${process.env.DYLD_LIBRARY_PATH ?? ""}` : process.env.DYLD_LIBRARY_PATH,
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
    stdout: "inherit",
    stderr: "inherit",
  });

  let socket: ObsWebSocket | undefined = undefined;
  try {
    socket = await ObsWebSocket.connect(port, password);
    const setSettings = await requestWithStartupRetry(socket, "CallVendorRequest", {
      vendorName: "alpha_recorder",
      requestType: "SetSettings",
      requestData: { enabled: true, finalization_format: args.finalizationFormat },
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
    const expectedFinalizationFormat = normalizedFinalizationFormat(args.finalizationFormat);
    if (settings.responseData?.finalization_format !== expectedFinalizationFormat) {
      throw new Error(`Alpha Recorder did not accept finalization_format=${args.finalizationFormat}: ${JSON.stringify(settings)}`);
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

    await socket.request("SetRecordDirectory", { recordDirectory: artifactRoot });

    const attempts: Array<{ durationSeconds: number; rgbPath: string; alphaPath: string; rgbProbe: any; alphaProbe: any }> = [];
    for (const durationSeconds of verificationDurations(args.recordSeconds, args.maxRecordSeconds)) {
      console.log(`Running OBS app E2E recording for ${durationSeconds}s`);
      await socket.request("StartRecord");
      await waitForRecordState(socket, true);
      await delay(durationSeconds * 1000);
      const stopResponse = await socket.request("StopRecord");
      await waitForRecordState(socket, false);
      const outputs = await verifyRecordingOutputs(
        stageBin,
        args.repoRoot,
        artifactRoot,
        String(stopResponse?.outputPath ?? ""),
        expectedFinalizationFormat,
        args.rgbEncoder,
        args.width,
        args.height,
        args.fps,
      );
      attempts.push({ durationSeconds, ...outputs });
    }

    const lastAttempt = attempts[attempts.length - 1];

    console.log(
      JSON.stringify(
        {
          ok: true,
          artifactRoot,
          attempts,
          rgbPath: lastAttempt?.rgbPath,
          alphaPath: lastAttempt?.alphaPath,
          rgbProbe: lastAttempt?.rgbProbe,
          alphaProbe: lastAttempt?.alphaProbe,
        },
        null,
        2,
      ),
    );
  } finally {
    socket?.close();
    if (!args.keepObsOpen) {
      obs.kill();
    }
  }
}

await main();
