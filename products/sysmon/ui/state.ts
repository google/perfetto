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

import m from 'mithril';
import {download} from '../../../ui/src/base/download_utils';
import {PRODUCT_CONFIGS, PRODUCT_SQL_PACKAGES} from '../../../ui/src/gen/products_sql';
import type {App} from '../../../ui/src/public/app';
import type {SqlPackage} from '../../../ui/src/public/extra_sql_packages';
import type {Engine} from '../../../ui/src/trace_processor/engine';
import {WasmEngineProxy} from '../../../ui/src/trace_processor/wasm_engine_proxy';
import type {ConnectionResult} from '../../../ui/src/plugins/dev.perfetto.Memscope/views/connection';
import type {BrushRange} from '../../common/ui/kit/charts';
import {fmtClockTime} from '../../common/ui/kit/format';
import {DualLiveSession} from '../../common/ui/live/dual_session';
import {
  loadAppsBundle,
  loadCpuBundle,
  loadMemoryBundle,
  loadPulse,
  loadStorageBundle,
  loadTaskManager,
  loadThermalBundle,
  type AppsBundle,
  type CpuBundle,
  type MemoryBundle,
  type PulseRow,
  type StorageBundle,
  type ThermalBundle,
  type TopProcessRow,
  type WindowArgs,
} from './queries';
import {CURATED_WORKLOADS} from './workloads';

export interface LiveSnapshotViewData {
  readonly win?: WindowArgs;
  readonly pulse?: PulseRow;
  readonly mem: MemoryBundle;
  readonly cpu: CpuBundle;
  readonly storage: StorageBundle;
  readonly thermal: ThermalBundle;
  readonly apps: AppsBundle;
  readonly topProcs: TopProcessRow[];
}

export type LiveSubtab =
  | 'overview'
  | 'apps'
  | 'memory'
  | 'cpu'
  | 'storage'
  | 'thermal';

export interface LiveProcessTab {
  readonly key: string;
  readonly processName: string;
  kept: boolean;
}

export type SnapSubtabKind =
  | 'start'
  | 'apps'
  | 'memory'
  | 'cpu'
  | 'wait'
  | 'storage'
  | 'thermal'
  | 'process'
  | 'details';

export interface SnapSubtab {
  readonly key: string;
  readonly kind: SnapSubtabKind;
  readonly label: string;
  readonly icon: string;
  readonly param?: string; // wait category or process name
  kept: boolean;
}

export interface CapturedSnapshot {
  readonly id: string;
  readonly index: number;
  readonly label: string;
  readonly capturedAtMs: number;
  readonly source: 'live' | 'file';
  readonly deviceName: string;
  readonly sizeBytes: number;
  readonly bytes: Uint8Array;
  readonly engine: WasmEngineProxy;
  readonly subtabs: SnapSubtab[];
  activeSubtabKey: string;
  brush?: BrushRange;
  pulse?: PulseRow;
}

export type ActiveTopView =
  | {readonly kind: 'live'}
  | {readonly kind: 'snapshot'; readonly id: string}
  | {readonly kind: 'compare'};

let snapEngineSeq = 0;

export function getSysmonSqlPackages(): SqlPackage[] {
  return PRODUCT_SQL_PACKAGES.filter((p) => p.name === 'sysmon');
}

export function resolveMemscopeBaseUrl(): string {
  const override = localStorage.getItem('sysmon.memscopeUrl');
  if (override) return override.replace(/\/+$/, '');
  const port = window.location.port;
  if (port === '10001' || window.location.hostname === 'localhost' || window.location.hostname === '127.0.0.1') {
    const targetPort = port === '10001' ? '10002' : '10002';
    return `${window.location.protocol}//${window.location.hostname}:${targetPort}`;
  }
  return 'https://memscope.perfetto.dev';
}

export interface LiveSessionLike {
  readonly deviceName: string;
  readonly status: string;
  readonly isPaused: boolean;
  readonly hasShell: boolean;
  readonly isWebUsb: boolean;
  readonly releasedForHandoff: boolean;
  readonly snapshotFlightRecorderReady: boolean;
  readonly error?: string;
  readonly engine?: Engine;
  readonly data?: LiveSnapshotViewData;
  pause(): void;
  resume(): void;
  dispose(): Promise<void>;
  takeInstantSnapshot(): Promise<Uint8Array>;
  runShell(cmd: string): Promise<string>;
  releaseForHandoff(): Promise<void>;
}

async function extractLiveViewData(engine: Engine): Promise<LiveSnapshotViewData> {
  // Warm sysmon.pulse once first so the Pipeline-SQL rollup tables are
  // materialized once before fan-out queries run in parallel.
  const initialPulse = await loadPulse(engine);
  const win =
    initialPulse && initialPulse.durS > 65
      ? {
          startTs: initialPulse.endTs - 60_000_000_000n,
          endTs: initialPulse.endTs,
        }
      : undefined;
  const [pulse, mem, cpu, storage, thermal, apps, topProcs] =
    await Promise.all([
      win ? loadPulse(engine, win) : Promise.resolve(initialPulse),
      loadMemoryBundle(engine, win, 20),
      loadCpuBundle(engine, win, 12, true),
      loadStorageBundle(engine, win, 12, true),
      loadThermalBundle(engine, win, 12),
      loadAppsBundle(engine, win, 18),
      loadTaskManager(engine, win, 120, 15),
    ]);
  return {win, pulse, mem, cpu, storage, thermal, apps, topProcs};
}

export class SysmonState {
  liveSession?: LiveSessionLike;
  liveSubtab: string = 'overview';
  readonly liveProcessTabs: LiveProcessTab[] = [];
  liveAppsFilterBucketIdx?: number;
  liveTickGen = 0;

  readonly snapshots: CapturedSnapshot[] = [];
  activeView: ActiveTopView = {kind: 'live'};
  compareLeftId?: string;
  compareRightId?: string;

  capturingSnapshot = false;
  loadingTraceLabel?: string;
  runningWorkload?: string;
  lastWorkloadOutput?: string;
  lastError?: string;

  private snapSeq = 0;
  private app?: App;

  attachApp(app: App): void {
    this.app = app;
  }

  get activeSnapshot(): CapturedSnapshot | undefined {
    const v = this.activeView;
    if (v.kind !== 'snapshot') return undefined;
    return this.snapshots.find((s) => s.id === v.id);
  }

  connectDevice(conn: ConnectionResult): void {
    this.lastError = undefined;
    if (this.liveSession) {
      void this.liveSession.dispose();
    }
    const livePbtx = PRODUCT_CONFIGS['sysmon/live.pbtx'] ?? '';
    const snapPbtx = PRODUCT_CONFIGS['sysmon/snapshot.pbtx'] ?? livePbtx;
    this.liveSession = new DualLiveSession<LiveSnapshotViewData>({
      connection: conn,
      liveConfigPbtx: livePbtx,
      snapshotConfigPbtx: snapPbtx,
      sqlPackages: getSysmonSqlPackages(),
      extractLiveData: extractLiveViewData,
      onLiveTick: () => {
        this.liveTickGen++;
        m.redraw();
      },
    });
    this.activeView = {kind: 'live'};
    this.liveSubtab = 'overview';
    this.liveProcessTabs.length = 0;
    this.liveAppsFilterBucketIdx = undefined;
    this.app?.navigate('#!/sysmon');
    m.redraw();
  }

  // Loads a trace URL into a read-only Live session preview so the Live UI can
  // be inspected without a live attached device.
  async openLivePreviewUrl(url: string, title?: string): Promise<void> {
    const name = title ?? url.split('/').pop() ?? 'trace.pftrace';
    if (this.liveSession) {
      await this.liveSession.dispose();
    }
    const resp = await fetch(url);
    if (!resp.ok) {
      throw new Error(`Failed to fetch ${url} (${resp.status})`);
    }
    const bytes = new Uint8Array(await resp.arrayBuffer());
    const engine = new WasmEngineProxy(`sysmon-live-preview-${++snapEngineSeq}`);
    await engine.resetTraceProcessor({
      tokenizeOnly: false,
      cropTrackEvents: false,
      ingestFtraceInRawTable: true,
      analyzeTraceProtoContent: false,
      ftraceDropUntilAllCpusValid: false,
      forceFullSort: false,
    });
    await engine.parse(bytes);
    await engine.notifyEof();
    await engine.query('PERFETTO PRAGMA pipelines = 1;');
    for (const pkg of getSysmonSqlPackages()) {
      await engine.registerSqlPackages(pkg);
    }
    const data = await extractLiveViewData(engine);
    this.liveSession = {
      deviceName: name,
      status: `Replay · ${(bytes.byteLength / 1024).toFixed(0)} KB`,
      isPaused: true,
      hasShell: false,
      isWebUsb: false,
      releasedForHandoff: false,
      snapshotFlightRecorderReady: true,
      engine,
      data,
      pause: () => {},
      resume: () => {},
      dispose: async () => {
        engine[Symbol.dispose]();
      },
      takeInstantSnapshot: async () => bytes,
      runShell: async () => '',
      releaseForHandoff: async () => {},
    };
    this.liveTickGen++;
    this.activeView = {kind: 'live'};
    this.liveSubtab = 'overview';
    this.liveProcessTabs.length = 0;
    m.redraw();
  }

  async disconnectDevice(): Promise<void> {
    const s = this.liveSession;
    this.liveSession = undefined;
    if (s) await s.dispose();
    if (this.snapshots.length > 0) {
      this.selectSnapshot(this.snapshots[this.snapshots.length - 1]!.id);
    }
    m.redraw();
  }

  async takeInstantSnapshot(
    customLabel?: string,
    openArea?: Exclude<SnapSubtabKind, 'start' | 'wait' | 'process' | 'details'>,
  ): Promise<CapturedSnapshot> {
    if (!this.liveSession) {
      throw new Error('Connect a device first to take a live snapshot.');
    }
    this.capturingSnapshot = true;
    this.lastError = undefined;
    m.redraw();
    try {
      const bytes = await this.liveSession.takeInstantSnapshot();
      const idx = ++this.snapSeq;
      const label = customLabel
        ? `#${idx} · ${customLabel}`
        : `Snap #${idx} · ${fmtClockTime(Date.now())}`;
      const snap = await this.ingestSnapshotBytes(
        bytes,
        label,
        'live',
        this.liveSession.deviceName,
        idx,
      );
      if (openArea) {
        this.openSnapSubtab(snap, openArea);
      }
      return snap;
    } catch (e) {
      this.lastError = e instanceof Error ? e.message : String(e);
      throw e;
    } finally {
      this.capturingSnapshot = false;
      m.redraw();
    }
  }

  async openTraceBytes(
    bytes: Uint8Array,
    title: string,
  ): Promise<CapturedSnapshot> {
    this.loadingTraceLabel = title;
    this.lastError = undefined;
    m.redraw();
    try {
      const idx = ++this.snapSeq;
      const short = title.replace(/\.(pftrace|perfetto-trace|pb)(\.gz)?$/i, '');
      const label = `#${idx} · ${short.slice(0, 26)}`;
      return await this.ingestSnapshotBytes(bytes, label, 'file', title, idx);
    } catch (e) {
      this.lastError = e instanceof Error ? e.message : String(e);
      throw e;
    } finally {
      this.loadingTraceLabel = undefined;
      m.redraw();
    }
  }

  async openTraceUrl(url: string, title?: string): Promise<CapturedSnapshot> {
    const name = title ?? url.split('/').pop() ?? 'trace.pftrace';
    this.loadingTraceLabel = name;
    this.lastError = undefined;
    m.redraw();
    try {
      const resp = await fetch(url);
      if (!resp.ok) {
        throw new Error(`Failed to fetch ${url} (${resp.status})`);
      }
      const buf = new Uint8Array(await resp.arrayBuffer());
      return await this.openTraceBytes(buf, name);
    } catch (e) {
      this.lastError = e instanceof Error ? e.message : String(e);
      throw e;
    } finally {
      this.loadingTraceLabel = undefined;
      m.redraw();
    }
  }

  private async ingestSnapshotBytes(
    bytes: Uint8Array,
    label: string,
    source: 'live' | 'file',
    deviceName: string,
    index: number,
  ): Promise<CapturedSnapshot> {
    const engine = new WasmEngineProxy(`sysmon-snap-${++snapEngineSeq}`);
    await engine.resetTraceProcessor({
      tokenizeOnly: false,
      cropTrackEvents: false,
      ingestFtraceInRawTable: true,
      analyzeTraceProtoContent: false,
      ftraceDropUntilAllCpusValid: false,
      forceFullSort: false,
    });
    await engine.parse(bytes);
    await engine.notifyEof();
    await engine.query('PERFETTO PRAGMA pipelines = 1;');
    for (const pkg of getSysmonSqlPackages()) {
      await engine.registerSqlPackages(pkg);
    }

    const pulse = await loadPulse(engine);
    const id = `snap-${index}`;
    const snap: CapturedSnapshot = {
      id,
      index,
      label,
      capturedAtMs: Date.now(),
      source,
      deviceName,
      sizeBytes: bytes.byteLength,
      bytes,
      engine,
      subtabs: [],
      activeSubtabKey: 'start',
      pulse,
    };
    this.snapshots.push(snap);
    if (this.snapshots.length >= 2) {
      this.compareLeftId = this.snapshots[this.snapshots.length - 2]!.id;
      this.compareRightId = snap.id;
    }
    this.activeView = {kind: 'snapshot', id};
    this.app?.navigate(`#!/sysmon?view=${encodeURIComponent(id)}`);
    m.redraw();
    return snap;
  }

  selectLive(subtab?: LiveSubtab): void {
    this.activeView = {kind: 'live'};
    if (subtab) this.liveSubtab = subtab;
    this.app?.navigate('#!/sysmon?view=live');
    m.redraw();
  }

  openLiveProcessTab(processName: string, keep = false): void {
    if (!processName) return;
    this.activeView = {kind: 'live'};
    const key = `proc:${processName}`;
    const existing = this.liveProcessTabs.find((t) => t.key === key);
    if (existing) {
      if (keep) existing.kept = true;
      this.liveSubtab = existing.key;
      m.redraw();
      return;
    }
    const tab: LiveProcessTab = {key, processName, kept: keep};
    const previewIdx = this.liveProcessTabs.findIndex((t) => !t.kept);
    if (!keep && previewIdx >= 0) {
      this.liveProcessTabs[previewIdx] = tab;
    } else {
      this.liveProcessTabs.push(tab);
    }
    this.liveSubtab = tab.key;
    m.redraw();
  }

  closeLiveProcessTab(key: string): void {
    const idx = this.liveProcessTabs.findIndex((t) => t.key === key);
    if (idx < 0) return;
    this.liveProcessTabs.splice(idx, 1);
    if (this.liveSubtab === key) {
      this.liveSubtab =
        this.liveProcessTabs[Math.min(idx, this.liveProcessTabs.length - 1)]
          ?.key ?? 'overview';
    }
    m.redraw();
  }

  selectSnapshot(id: string): void {
    if (!this.snapshots.some((s) => s.id === id)) return;
    this.activeView = {kind: 'snapshot', id};
    this.app?.navigate(`#!/sysmon?view=${encodeURIComponent(id)}`);
    m.redraw();
  }

  selectCompare(): void {
    if (this.snapshots.length < 2) return;
    if (!this.compareLeftId || !this.snapshots.some((s) => s.id === this.compareLeftId)) {
      this.compareLeftId = this.snapshots[0]!.id;
    }
    if (!this.compareRightId || !this.snapshots.some((s) => s.id === this.compareRightId)) {
      this.compareRightId = this.snapshots[this.snapshots.length - 1]!.id;
    }
    this.activeView = {kind: 'compare'};
    this.app?.navigate('#!/sysmon?view=compare');
    m.redraw();
  }

  closeSnapshot(id: string): void {
    const idx = this.snapshots.findIndex((s) => s.id === id);
    if (idx < 0) return;
    const [removed] = this.snapshots.splice(idx, 1);
    removed?.engine[Symbol.dispose]();
    if (this.activeView.kind === 'snapshot' && this.activeView.id === id) {
      const next = this.snapshots[Math.min(idx, this.snapshots.length - 1)];
      if (next) this.selectSnapshot(next.id);
      else if (this.liveSession) this.selectLive();
      else this.app?.navigate('#!/');
    } else if (this.activeView.kind === 'compare' && this.snapshots.length < 2) {
      if (this.snapshots[0]) this.selectSnapshot(this.snapshots[0].id);
      else if (this.liveSession) this.selectLive();
    }
    m.redraw();
  }

  // lat-style preview-tab management inside a Snapshot workspace:
  // Opening an area/process/wait puts it in a preview tab (replaced by the
  // next browse unless you go deeper from it or double-click to keep it).
  openSnapSubtab(
    snap: CapturedSnapshot,
    kind: Exclude<SnapSubtabKind, 'start'>,
    param?: string,
    keep = false,
  ): void {
    // Going deeper from an existing preview tab keeps it first.
    const cur = snap.subtabs.find((t) => t.key === snap.activeSubtabKey);
    if (cur) cur.kept = true;

    const key = param ? `${kind}:${param}` : kind;
    const existing = snap.subtabs.find((t) => t.key === key);
    if (existing) {
      if (keep) existing.kept = true;
      snap.activeSubtabKey = existing.key;
      m.redraw();
      return;
    }

    const meta: Record<
      Exclude<SnapSubtabKind, 'start'>,
      {label: string; icon: string}
    > = {
      apps: {label: 'Apps & States', icon: 'apps'},
      memory: {label: 'Memory', icon: 'memory'},
      cpu: {label: 'CPU', icon: 'speed'},
      wait: {
        label:
          param === 'io_wait'
            ? 'Waits · Disk I/O'
            : param === 'uninterruptible'
              ? 'Waits · Uninterruptible'
              : 'Waits · Runnable',
        icon: 'hourglass_top',
      },
      storage: {label: 'Storage', icon: 'hard_drive'},
      thermal: {label: 'Thermal & Power', icon: 'thermostat'},
      process: {label: param ?? 'Process', icon: 'apps'},
      details: {label: 'Recording details', icon: 'info'},
    };

    const tab: SnapSubtab = {
      key,
      kind,
      label: meta[kind].label,
      icon: meta[kind].icon,
      param,
      kept: keep || kind === 'details',
    };

    const previewIdx = snap.subtabs.findIndex((t) => !t.kept);
    if (!tab.kept && previewIdx >= 0) {
      snap.subtabs[previewIdx] = tab;
    } else {
      snap.subtabs.push(tab);
    }
    snap.activeSubtabKey = tab.key;
    m.redraw();
  }

  closeSnapSubtab(snap: CapturedSnapshot, key: string): void {
    const idx = snap.subtabs.findIndex((t) => t.key === key);
    if (idx < 0) return;
    snap.subtabs.splice(idx, 1);
    if (snap.activeSubtabKey === key) {
      snap.activeSubtabKey =
        snap.subtabs[Math.min(idx, snap.subtabs.length - 1)]?.key ?? 'start';
    }
    m.redraw();
  }

  getWindowArgs(snap: CapturedSnapshot): WindowArgs | undefined {
    if (!snap.brush || !snap.pulse) return undefined;
    const span = Number(snap.pulse.endTs - snap.pulse.startTs);
    if (span <= 0) return undefined;
    const startTs =
      snap.pulse.startTs + BigInt(Math.round(span * snap.brush.startFrac));
    const endTs =
      snap.pulse.startTs + BigInt(Math.round(span * snap.brush.endFrac));
    return {startTs, endTs};
  }

  async downloadSnapshot(snap: CapturedSnapshot): Promise<void> {
    const safe = snap.label.replace(/[^a-zA-Z0-9._-]+/g, '_');
    await download({
      content: new Uint8Array(snap.bytes),
      fileName: `sysmon_${safe}.pftrace`,
    });
  }

  async runWorkload(opts: {
    readonly workload: string;
    readonly command?: string;
    readonly durationS?: number;
    readonly snapshotAfter?: boolean;
  }): Promise<{
    readonly workload: string;
    readonly output: string;
    readonly snapshotLabel?: string;
  }> {
    if (!this.liveSession) {
      throw new Error('No connected device.');
    }
    const durS = opts.durationS ?? 6;
    let cmd = opts.command;
    if (opts.workload !== 'custom') {
      const found = CURATED_WORKLOADS.find((w) => w.id === opts.workload);
      if (!found) throw new Error(`Unknown workload "${opts.workload}".`);
      cmd = found.buildCommand(durS);
    }
    if (!cmd) throw new Error('Missing shell command.');

    this.runningWorkload = opts.workload;
    this.lastError = undefined;
    m.redraw();
    try {
      const output = await this.liveSession.runShell(cmd);
      this.lastWorkloadOutput = output;
      let snapshotLabel: string | undefined;
      if (opts.snapshotAfter) {
        const snap = await this.takeInstantSnapshot(opts.workload);
        snapshotLabel = snap.label;
      }
      return {workload: opts.workload, output, snapshotLabel};
    } finally {
      this.runningWorkload = undefined;
      m.redraw();
    }
  }

  buildMemscopeHandoffUrl(processName?: string): string {
    const base = resolveMemscopeBaseUrl();
    if (!processName) return `${base}/#!/memscope`;
    return `${base}/#!/memscope?process=${encodeURIComponent(processName)}`;
  }

  async openInMemscope(processName?: string): Promise<void> {
    const url = this.buildMemscopeHandoffUrl(processName);
    if (this.liveSession?.isWebUsb) {
      await this.liveSession.releaseForHandoff();
    }
    window.open(url, '_blank', 'noopener');
  }
}

export const sysmonState = new SysmonState();
