// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

import {traceConfigToPb} from '../../../../ui/src/base/proto_utils_wasm';
import {uuidv4} from '../../../../ui/src/base/uuid';
import protos from '../../../../ui/src/protos';
import type {SqlPackage} from '../../../../ui/src/public/extra_sql_packages';
import {WasmEngineProxy} from '../../../../ui/src/trace_processor/wasm_engine_proxy';
import type {AdbDevice} from '../../../../ui/src/plugins/dev.perfetto.RecordTraceV2/adb/adb_device';
import {AdbWebusbDevice} from '../../../../ui/src/plugins/dev.perfetto.RecordTraceV2/adb/webusb/adb_webusb_device';
import {createAdbTracingSession} from '../../../../ui/src/plugins/dev.perfetto.RecordTraceV2/adb/adb_tracing_session';
import type {TracingSession} from '../../../../ui/src/plugins/dev.perfetto.RecordTraceV2/interfaces/tracing_session';
import type {TracedWebsocketTarget} from '../../../../ui/src/plugins/dev.perfetto.RecordTraceV2/traced_over_websocket/traced_websocket_target';
import type {ConnectionResult} from '../../../../ui/src/plugins/dev.perfetto.Memscope/views/connection';

const LIVE_POLL_MS = 1_500;
const INITIAL_POLL_MS = 800;

let engineSeq = 0;

export interface DualSessionOpts<T = unknown> {
  readonly connection: ConnectionResult;
  readonly liveConfigPbtx: string;
  readonly snapshotConfigPbtx: string;
  readonly sqlPackages: ReadonlyArray<SqlPackage>;
  readonly extractLiveData?: (engine: WasmEngineProxy) => Promise<T>;
  readonly onLiveTick: () => void;
}

// Runs two concurrent Perfetto tracing sessions on the connected device:
//  - Session A (lightweight counters): polled periodically into a
//    double-buffered pair of WasmEngineProxy instances so readers (UI & Agent)
//    always query a fully parsed, SQL-package-registered front engine while
//    the next tick loads into the back engine.
//  - Session B (64 MB superset RING_BUFFER): sits silently on-device until
//    the user or Agent triggers an instant Snapshot.
export class DualLiveSession<T = unknown> {
  deviceName: string;
  readonly isWebUsb: boolean;

  private device?: AdbDevice;
  private linuxTarget?: TracedWebsocketTarget;
  private sessionA?: TracingSession;
  private sessionB?: TracingSession;

  // Double-buffered Wasm engines: `slots[frontIdx]` is immutable and safe to
  // query whenever `engineReady` is true; `poll()` always mutates the other slot.
  private slots: [WasmEngineProxy | undefined, WasmEngineProxy | undefined] = [
    undefined,
    undefined,
  ];
  private slotNeedsReset: [boolean, boolean] = [true, true];
  private frontIdx: 0 | 1 = 0;
  private engineReady = false;
  private liveData?: T;

  private timer?: ReturnType<typeof setTimeout>;
  private pollInFlight = false;
  private disposed = false;
  private paused = false;

  status = 'Starting live sessions…';
  error?: string;
  snapshotFlightRecorderReady = false;
  tickCount = 0;
  lastTickMs = 0;
  lastCloneMs = 0;
  lastParseMs = 0;
  lastQueryMs = 0;
  lastLiveSizeKb = 0;
  lastBufferPct?: number;
  releasedForHandoff = false;

  constructor(private readonly opts: DualSessionOpts<T>) {
    this.device = opts.connection.device;
    this.linuxTarget = opts.connection.linuxTarget;
    this.deviceName = opts.connection.deviceName;
    this.isWebUsb = this.device instanceof AdbWebusbDevice;
    void this.start();
  }

  get engine(): WasmEngineProxy | undefined {
    return this.engineReady ? this.slots[this.frontIdx] : undefined;
  }

  get data(): T | undefined {
    return this.liveData;
  }

  get isPaused(): boolean {
    return this.paused;
  }

  get hasShell(): boolean {
    return this.device !== undefined && !this.releasedForHandoff;
  }

  private async start(): Promise<void> {
    try {
      // TCP/forwarded ADB connections omit `model:` from `adb devices -l`;
      // resolve `ro.product.model` once so the header shows the real model.
      if (this.device && this.deviceName.startsWith('Android device')) {
        try {
          const prop = await this.device.shell('getprop ro.product.model');
          const model = prop.ok ? prop.value.trim() : '';
          if (model) {
            this.deviceName = this.deviceName.replace(/^Android device/, model);
          }
        } catch {
          // Keep fallback name.
        }
      }

      const liveCfg = await parsePbtxWithName(
        this.opts.liveConfigPbtx,
        `sysmon-live-${uuidv4().slice(0, 8)}`,
      );
      const resA = await this.startTracing(liveCfg);
      if (!resA.ok) {
        this.error = `Live session failed: ${resA.error}`;
        this.status = this.error;
        this.opts.onLiveTick();
        return;
      }
      this.sessionA = resA.value;
      this.status = 'Live session running…';
      this.opts.onLiveTick();

      // Start Session B (silent superset ring buffer). Best-effort: if the
      // device refuses a second session, instant Snapshot falls back to Session A.
      try {
        const snapCfg = await parsePbtxWithName(
          this.opts.snapshotConfigPbtx,
          `sysmon-snap-${uuidv4().slice(0, 8)}`,
        );
        const resB = await this.startTracing(snapCfg);
        if (resB.ok) {
          this.sessionB = resB.value;
          this.snapshotFlightRecorderReady = true;
        }
      } catch {
        this.snapshotFlightRecorderReady = false;
      }

      this.schedulePoll(INITIAL_POLL_MS);
    } catch (e) {
      this.error = e instanceof Error ? e.message : String(e);
      this.status = this.error;
      this.opts.onLiveTick();
    }
  }

  private async startTracing(config: protos.ITraceConfig) {
    if (this.linuxTarget) {
      return this.linuxTarget.startTracing(config);
    }
    return createAdbTracingSession(this.device!, config);
  }

  private schedulePoll(delayMs: number): void {
    if (this.disposed || this.paused) return;
    clearTimeout(this.timer);
    this.timer = setTimeout(() => void this.poll(), Math.max(0, delayMs));
  }

  private async poll(): Promise<void> {
    if (this.disposed || this.paused || !this.sessionA || this.pollInFlight) {
      return;
    }
    this.pollInFlight = true;
    const t0 = performance.now();
    try {
      const [snap, bufPct] = await Promise.all([
        this.sessionA.snapshot(),
        this.sessionA.getBufferUsagePct(),
      ]);
      if (this.disposed) return;
      if (!snap.ok) {
        this.status = `Live poll error: ${snap.error}`;
        this.opts.onLiveTick();
        return;
      }
      const tClone = performance.now();
      const bytes = snap.value;
      const backIdx: 0 | 1 = this.engineReady
        ? ((1 - this.frontIdx) as 0 | 1)
        : 0;
      let back = this.slots[backIdx];
      if (!back) {
        back = new WasmEngineProxy(`sysmon-live-${++engineSeq}`);
        this.slots[backIdx] = back;
        this.slotNeedsReset[backIdx] = true;
      }

      // Only send TPM_RESET_TRACE_PROCESSOR on a slot's initial load or after
      // a failed tick; after notifyEof(), Rpc::Parse automatically resets with
      // the saved TraceProcessorConfig. Enable pipelines and register SQL
      // packages AFTER notifyEof() (matching load_trace.ts) so Rpc::Parse can
      // never wipe them out.
      const needsReset = this.slotNeedsReset[backIdx];
      this.slotNeedsReset[backIdx] = true;
      if (needsReset) {
        await back.resetTraceProcessor({
          tokenizeOnly: false,
          cropTrackEvents: false,
          ingestFtraceInRawTable: true,
          analyzeTraceProtoContent: false,
          ftraceDropUntilAllCpusValid: false,
          forceFullSort: false,
        });
      }
      await back.parse(bytes);
      await back.notifyEof();
      this.slotNeedsReset[backIdx] = false;
      await back.query('PERFETTO PRAGMA pipelines = 1;');
      for (const pkg of this.opts.sqlPackages) {
        await back.registerSqlPackages(pkg);
      }
      if (this.disposed) return;
      const tParse = performance.now();

      const nextData = this.opts.extractLiveData
        ? await this.opts.extractLiveData(back)
        : undefined;
      if (this.disposed) return;
      const tQuery = performance.now();

      // Atomically publish the fully parsed, queried back engine as front.
      this.frontIdx = backIdx;
      this.engineReady = true;
      if (nextData !== undefined) {
        this.liveData = nextData;
      }

      this.tickCount++;
      this.lastCloneMs = Math.round(tClone - t0);
      this.lastParseMs = Math.round(tParse - tClone);
      this.lastQueryMs = Math.round(tQuery - tParse);
      this.lastTickMs = Math.round(tQuery - t0);
      this.lastLiveSizeKb = Math.round(bytes.byteLength / 1024);
      this.lastBufferPct = bufPct;
      this.error = undefined;
      this.status = `Live · ${this.lastLiveSizeKb} KB · ${this.lastTickMs}ms`;
      this.opts.onLiveTick();
    } catch (e) {
      this.error = e instanceof Error ? e.message : String(e);
      this.status = `Live tick error: ${this.error}`;
      this.opts.onLiveTick();
    } finally {
      this.pollInFlight = false;
      if (!this.disposed && !this.paused) {
        this.schedulePoll(Math.max(400, LIVE_POLL_MS - this.lastTickMs));
      }
    }
  }

  pause(): void {
    this.paused = true;
    clearTimeout(this.timer);
    this.opts.onLiveTick();
  }

  resume(): void {
    if (!this.paused || this.disposed) return;
    this.paused = false;
    this.schedulePoll(200);
    this.opts.onLiveTick();
  }

  // Instant retroactive Snapshot: clones Session B (full superset) without
  // stopping Live.
  async takeInstantSnapshot(): Promise<Uint8Array> {
    const target = this.sessionB ?? this.sessionA;
    if (!target) {
      throw new Error('No active tracing session on device.');
    }
    const res = await target.snapshot();
    if (!res.ok) {
      throw new Error(`Snapshot failed: ${res.error}`);
    }
    return res.value;
  }

  async runShell(cmd: string): Promise<string> {
    if (!this.device) {
      throw new Error('Shell commands require an ADB Android connection.');
    }
    const res = await this.device.shell(cmd);
    if (!res.ok) {
      throw new Error(`Shell failed: ${res.error}`);
    }
    return res.value;
  }

  async releaseForHandoff(): Promise<void> {
    if (!this.isWebUsb) return;
    this.releasedForHandoff = true;
    this.pause();
    await this.stopRemoteSessions();
    try {
      this.device?.close();
    } catch {
      // Ignore already-closed USB device.
    }
    this.device = undefined;
    this.status = 'Device paused for Memscope';
    this.opts.onLiveTick();
  }

  private async stopRemoteSessions(): Promise<void> {
    const a = this.sessionA;
    const b = this.sessionB;
    this.sessionA = undefined;
    this.sessionB = undefined;
    await Promise.allSettled([a?.cancel(), b?.cancel()]);
  }

  async dispose(): Promise<void> {
    this.disposed = true;
    this.engineReady = false;
    clearTimeout(this.timer);
    try {
      await this.stopRemoteSessions();
    } finally {
      try {
        this.device?.close();
      } catch {
        // Ignore already-closed USB device.
      }
      this.device = undefined;
      for (let i = 0; i < this.slots.length; i++) {
        this.slots[i]?.[Symbol.dispose]();
        this.slots[i] = undefined;
      }
    }
  }
}

async function parsePbtxWithName(
  pbtx: string,
  uniqueSessionName: string,
): Promise<protos.ITraceConfig> {
  const withName = `${pbtx}\nunique_session_name: "${uniqueSessionName}"\n`;
  const pb = await traceConfigToPb(withName);
  if (!pb.ok) {
    throw new Error(`Invalid trace config: ${pb.error}`);
  }
  return protos.TraceConfig.decode(pb.value);
}
