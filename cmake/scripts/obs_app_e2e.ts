#!/usr/bin/env bun

import { spawn, spawnSync } from "bun";
import { createHash, randomBytes } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, readdirSync, statSync, writeFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { platform } from "node:process";

type Args = {
  repoRoot: string;
  stageDir: string;
  buildDir?: string;
  configuration: string;
  port: number;
  recordSeconds: number;
  width: number;
  height: number;
  keepObsOpen: boolean;
};

function parseArgs(argv: string[]): Args {
  const args: Args = {
    repoRoot: "",
    stageDir: "",
    configuration: "RelWithDebInfo",
    port: 0,
    recordSeconds: 5,
    width: 1280,
    height: 720,
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
      case "--width":
        args.width = Number(value);
        ++index;
        break;
      case "--height":
        args.height = Number(value);
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

function resolveObsExecutable(stageDir: string): { exe: string; cwd: string } {
  const candidates =
    platform === "darwin"
      ? [
          { exe: join(stageDir, "MacOS", "OBS"), cwd: join(stageDir, "MacOS") },
          { exe: join(stageDir, "bin", "obs"), cwd: join(stageDir, "bin") },
        ]
      : platform === "win32"
        ? [{ exe: join(stageDir, "bin", "64bit", "obs64.exe"), cwd: join(stageDir, "bin", "64bit") }]
        : [{ exe: join(stageDir, "bin", "obs"), cwd: join(stageDir, "bin") }];

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
  while (Date.now() < deadline) {
    const status = await socket.request("GetRecordStatus");
    if (Boolean(status.outputActive) === active) {
      return;
    }
    await delay(500);
  }
  throw new Error(`Timed out waiting for recording active=${active}`);
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

function findTool(stageBin: string, name: string): string {
  const candidates = [join(stageBin, name), join(stageBin, `${name}.exe`)];
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

function checkedProcess(tool: string, args: string[], timeoutSeconds: number): void {
  const result = spawnSync({ cmd: [tool, ...args], stdout: "pipe", stderr: "pipe", timeout: timeoutSeconds * 1000 });
  if (result.exitCode !== 0) {
    throw new Error(`${tool} failed: ${new TextDecoder().decode(result.stderr)}`);
  }
}

async function main(): Promise<void> {
  const args = parseArgs(Bun.argv.slice(2));
  const port = args.port > 0 ? args.port : await freePort();
  const password = randomBytes(16).toString("hex");
  const { exe: obsExe, cwd: obsCwd } = resolveObsExecutable(args.stageDir);
  const artifactRoot = join(args.repoRoot, "out", "e2e", "obs-app", new Date().toISOString().replace(/[-:]/g, "").replace(/\..+$/, ""));
  mkdirSync(artifactRoot, { recursive: true });

  const portableConfig = join(args.stageDir, "config", "obs-studio");
  const profile = "AlphaRecorderE2E";
  const collection = "AlphaRecorderE2E";

  writeText(join(portableConfig, "global.ini"), `[General]
FirstRun=false
Pre31Migrated=true
MaxLogs=10
ProcessPriority=Normal

[Basic]
Profile=${profile}
ProfileDir=${profile}
SceneCollection=${collection}
SceneCollectionFile=${collection}
`);

  writeText(join(portableConfig, "basic", "profiles", profile, "basic.ini"), `[General]
Name=${profile}

[Output]
Mode=Advanced
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

[Video]
BaseCX=${args.width}
BaseCY=${args.height}
OutputCX=${args.width}
OutputCY=${args.height}
FPSType=0
FPSCommon=30
FPSInt=30
FPSNum=30
FPSDen=1
ScaleType=bicubic
ColorFormat=BGRA
ColorSpace=709
ColorRange=Partial

[Audio]
SampleRate=48000
ChannelSetup=Stereo

[AlphaRecorder]
enabled=false
finalization_format=prores_4444
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

  const stageBin = platform === "win32" ? join(args.stageDir, "bin", "64bit") : join(args.stageDir, "bin");
  const env = {
    ...process.env,
    PATH: `${stageBin}${platform === "win32" ? ";" : ":"}${process.env.PATH ?? ""}`,
    DYLD_LIBRARY_PATH:
      platform === "darwin" ? `${stageBin}:${process.env.DYLD_LIBRARY_PATH ?? ""}` : process.env.DYLD_LIBRARY_PATH,
  };

  const obs = spawn({
    cmd: [
      obsExe,
      "--portable",
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
    ],
    cwd: obsCwd,
    env,
    stdout: "inherit",
    stderr: "inherit",
  });

  let socket: ObsWebSocket | undefined = undefined;
  try {
    socket = await ObsWebSocket.connect(port, password);
    await socket.request("CallVendorRequest", {
      vendorName: "alpha_recorder",
      requestType: "SetSettings",
      requestData: { enabled: true, finalization_format: "prores_4444" },
    });
    const settings = await socket.request("CallVendorRequest", {
      vendorName: "alpha_recorder",
      requestType: "GetSettings",
      requestData: {},
    });
    if (!settings.responseData?.enabled) {
      throw new Error("Alpha Recorder did not report enabled=true through the vendor API");
    }

    await socket.request("SetRecordDirectory", { recordDirectory: artifactRoot });
    await socket.request("StartRecord");
    await waitForRecordState(socket, true);
    await delay(args.recordSeconds * 1000);
    const stopResponse = await socket.request("StopRecord");
    await waitForRecordState(socket, false, 240);

    let rgbPath = String(stopResponse?.outputPath ?? "");
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
    const sidecarPath = `${basePath}.alpha.sidecar`;
    const manifestPath = `${basePath}.alpha.manifest.json`;
    const alphaPath = `${basePath}.alpha.mov`;

    await waitForPath(rgbPath, 30);
    await waitForPath(sidecarPath, 60);
    await waitForPath(manifestPath, 60);
    await waitForPath(alphaPath, 120);

    const manifest = JSON.parse(readFileSync(manifestPath, "utf8"));
    if (Number(manifest.pair_count) <= 0 || Number(manifest.record_count) !== Number(manifest.pair_count)) {
      throw new Error(`Manifest counts are invalid: ${JSON.stringify(manifest)}`);
    }
    if (Number(manifest.sidecar_size_bytes) !== statSync(sidecarPath).size) {
      throw new Error("Manifest sidecar_size_bytes does not match the sidecar file size");
    }

    const ffprobe = findTool(stageBin, "ffprobe");
    const ffmpeg = findTool(stageBin, "ffmpeg");
    const rgbProbe = checkedJson(
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
    const alphaProbe = checkedJson(
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
    ) as any;
    checkedProcess(ffmpeg, ["-v", "error", "-i", rgbPath, "-frames:v", "1", "-f", "null", "-"], 30);
    checkedProcess(ffmpeg, ["-v", "error", "-i", alphaPath, "-frames:v", "1", "-f", "null", "-"], 180);

    const alphaStream = alphaProbe.streams?.[0] ?? {};
    if (alphaStream.codec_name !== "prores" || alphaStream.pix_fmt !== "yuva444p10le") {
      throw new Error(`Alpha movie probe did not report ProRes yuva444p10le: ${JSON.stringify(alphaProbe)}`);
    }

    console.log(
      JSON.stringify(
        {
          ok: true,
          artifactRoot,
          rgbPath,
          alphaPath,
          sidecarPath,
          manifestPath,
          pairCount: manifest.pair_count,
          rgbProbe,
          alphaProbe,
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
