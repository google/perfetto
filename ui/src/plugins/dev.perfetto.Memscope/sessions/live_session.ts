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

import protos from '../../../protos';
import {uuidv4} from '../../../base/uuid';
import {deferChunkedTask} from '../../../base/chunked_task';
import {NUM, NUM_NULL, STR} from '../../../trace_processor/query_result';
import {WasmEngineProxy} from '../../../trace_processor/wasm_engine_proxy';
import {AdbDevice} from '../../dev.perfetto.RecordTraceV2/adb/adb_device';
import {createAdbTracingSession} from '../../dev.perfetto.RecordTraceV2/adb/adb_tracing_session';
import {TracingSession} from '../../dev.perfetto.RecordTraceV2/interfaces/tracing_session';
import {TracedWebsocketTarget} from '../../dev.perfetto.RecordTraceV2/traced_over_websocket/traced_websocket_target';
import {ConnectionResult} from '../views/connection';

const SNAPSHOT_INTERVAL_MS = 3_000; // How over to take a snapshot of the runnign trace and extract data.
const INITIAL_SNAPSHOT_INTERVAL_MS = 1_000; // Use a shorter interval for the first snapshot to get data on screen faster.
const POLLING_INTERVAL_MS = 1_000; // Recording config polling interval for process stats and system stats.

let engineCounter = 0;

export interface TsValue {
  ts: number;
  value: number;
}

export interface LmkEvent {
  ts: number;
  pid: number;
  processName: string;
  oomScoreAdj: number;
}

export interface ProcessInfo {
  upid: number;
  pid: number;
  processName: string;
  startTs: number | null;
  debuggable: boolean;
}

export interface SnapshotData {
  // counterName → sorted array of {ts, value}
  systemCounters: Map<string, TsValue[]>;

  // upid → counterName → sorted array of {ts, value}
  processCountersByUpid: Map<number, Map<string, TsValue[]>>;

  // upid → metadata
  processInfo: Map<number, ProcessInfo>;

  // LMK events sorted by ts.
  lmkEvents: LmkEvent[];

  // Whether the device is a userdebug build.
  isUserDebug: boolean;

  // Trace start timestamp from trace_bounds, used as the x=0 origin for all charts.
  ts0: number;

  // X-axis bounds in seconds relative to ts0, derived from the earliest/latest
  // counter sample. Use these as xAxisMin/xAxisMax on all line charts.
  xMin: number;
  xMax: number;
}

export type OnSnapshotCallback = (data: SnapshotData) => void;

/**
 * Connects to a device or Linux host, starts a memory tracing session, and
 * polls snapshots on a timer. Each snapshot clones the running trace, loads
 * it into a reusable TraceProcessor engine, extracts structured data, and
 * notifies registered callbacks.
 */
export class LiveSession {
  private session?: TracingSession;
  private engine?: WasmEngineProxy;
  private readonly sessionName: string;
  private readonly device?: AdbDevice;
  private readonly linuxTarget?: TracedWebsocketTarget;
  private timer?: ReturnType<typeof setTimeout>;
  private snapshotInFlight = false;
  private isDisposed = false;
  private readonly onSnapshotCallbacks: OnSnapshotCallback[] = [];

  readonly deviceName: string;
  snapshotCount = 0;
  status = 'Starting tracing session...';
  isPaused = false;
  data?: SnapshotData;
  lastTraceBuffer?: ArrayBuffer;

  // Timing breakdown for the last snapshot (ms).
  lastSnapshotMs = 0;
  lastSnapshotSizeKb = 0;
  lastBufferUsagePct?: number;
  lastCloneMs = 0;
  lastParseMs = 0;
  lastQueryMs = 0;
  lastExtractMs = 0;
  earliestEventTs = 0;

  // True when the last snapshot took longer than the configured interval.
  snapshotOverrun = false;

  constructor(conn: ConnectionResult) {
    this.device = conn.device;
    this.linuxTarget = conn.linuxTarget;
    this.deviceName = conn.deviceName;
    this.sessionName = `livemem-${uuidv4().substring(0, 8)}`;
    this.startAndPoll();
  }

  /** Register a callback invoked after each successful snapshot. */
  onSnapshot(cb: OnSnapshotCallback): void {
    this.onSnapshotCallbacks.push(cb);
  }

  /** Pauses snapshot polling. */
  pause(): void {
    if (this.isPaused) return;
    this.isPaused = true;
    clearTimeout(this.timer);
  }

  /** Resumes snapshot polling. */
  resume(): void {
    if (!this.isPaused) return;
    this.isPaused = false;
    if (this.session && !this.snapshotInFlight) {
      this.scheduleSnapshot();
    }
  }

  /** Toggles between paused and running. */
  togglePause(): void {
    if (this.isPaused) {
      this.resume();
    } else {
      this.pause();
    }
  }

  /** Stops the tracing session, polling, and disposes of the engine. */
  async dispose(): Promise<void> {
    // Mark disposed first so any in-flight poll() bails out before touching
    // the engine again, and the finally block doesn't reschedule.
    this.isDisposed = true;
    clearTimeout(this.timer);
    if (this.session) {
      await this.session.cancel();
      this.session = undefined;
    }
    if (this.engine) {
      this.engine[Symbol.dispose]();
      this.engine = undefined;
    }
  }

  private async startAndPoll(): Promise<void> {
    const config = createMonitoringConfig(this.sessionName);
    const result = this.linuxTarget
      ? await this.linuxTarget.startTracing(config)
      : await createAdbTracingSession(this.device!, config);
    if (!result.ok) {
      this.status = `Failed to start: ${result.error}`;
      return;
    }
    this.session = result.value;
    this.status = 'Session started. Waiting for first snapshot...';
    this.scheduleSnapshot(INITIAL_SNAPSHOT_INTERVAL_MS); // Initial delay
  }

  private scheduleSnapshot(delayMs = 0) {
    this.timer = setTimeout(() => this.poll(), Math.max(0, delayMs));
  }

  private async poll(): Promise<void> {
    if (!this.session || this.snapshotInFlight) return;
    this.snapshotInFlight = true;

    try {
      const t0 = performance.now();

      const [cloneResult, bufferUsagePct] = await Promise.all([
        this.session.snapshot(),
        this.session.getBufferUsagePct(),
      ]);
      if (!cloneResult.ok) {
        this.status = `Snapshot failed: ${cloneResult.error}`;
        return;
      }
      this.lastTraceBuffer = cloneResult.value.buffer as ArrayBuffer;
      const tClone = performance.now();

      // dispose() may have been called while we were awaiting the snapshot.
      // Bail out before touching the engine — getOrCreateEngine() would
      // otherwise resurrect a fresh one that nothing will ever dispose.
      if (this.isDisposed) return;
      const engine = this.getOrCreateEngine();
      await engine.resetTraceProcessor({
        tokenizeOnly: false,
        cropTrackEvents: false,
        ingestFtraceInRawTable: false,
        analyzeTraceProtoContent: false,
        ftraceDropUntilAllCpusValid: false,
        forceFullSort: false,
      });
      await engine.parse(cloneResult.value);
      await engine.notifyEof();
      const tParse = performance.now();

      const result = await extractSnapshotData(engine);
      this.data = result.data;

      for (const cb of this.onSnapshotCallbacks) {
        cb(this.data);
      }

      this.lastCloneMs = Math.round(tClone - t0);
      this.lastParseMs = Math.round(tParse - tClone);
      this.lastQueryMs = result.queryMs;
      this.lastExtractMs = result.extractMs;
      this.lastSnapshotMs = Math.round(performance.now() - t0);
      this.lastSnapshotSizeKb = this.lastTraceBuffer.byteLength / 1024;
      this.lastBufferUsagePct = bufferUsagePct;
      this.snapshotCount++;
      this.status = `Snapshot #${this.snapshotCount} OK (${this.lastSnapshotMs}ms)`;
    } catch (e) {
      this.status = `Snapshot error: ${e}`;
    } finally {
      this.snapshotInFlight = false;
      if (!this.isPaused && !this.isDisposed) {
        const nextDelay = SNAPSHOT_INTERVAL_MS - this.lastSnapshotMs;
        this.snapshotOverrun = nextDelay < 0;
        this.scheduleSnapshot(nextDelay);
      }
    }
  }

  private getOrCreateEngine(): WasmEngineProxy {
    if (this.engine === undefined) {
      this.engine = new WasmEngineProxy(`livemem-${++engineCounter}`);
    }
    return this.engine;
  }
}

function createMonitoringConfig(
  uniqueSessionName: string,
): protos.ITraceConfig {
  return {
    uniqueSessionName,
    buffers: [
      {
        name: 'initial_stats',
        sizeKb: 4 * 1024,
        fillPolicy: protos.TraceConfig.BufferConfig.FillPolicy.DISCARD,
      },
      {
        name: 'polled_stats',
        sizeKb: 4 * 1024,
        fillPolicy: protos.TraceConfig.BufferConfig.FillPolicy.RING_BUFFER,
      },
      {
        name: 'ftrace',
        sizeKb: 4 * 1024,
        fillPolicy: protos.TraceConfig.BufferConfig.FillPolicy.RING_BUFFER,
      },
    ],
    dataSources: [
      {
        config: {
          name: 'linux.process_stats',
          targetBufferName: 'initial_stats',
          processStatsConfig: {
            scanAllProcessesOnStart: true,
            recordProcessAge: true,
          },
        },
      },
      {
        config: {
          name: 'android.packages_list',
          targetBufferName: 'initial_stats',
        },
      },
      {
        config: {
          name: 'linux.process_stats',
          targetBufferName: 'polled_stats',
          processStatsConfig: {
            scanAllProcessesOnStart: false,
            procStatsPollMs: POLLING_INTERVAL_MS,
            recordProcessDmabufRss: true,
          },
        },
      },
      {
        config: {
          name: 'linux.sys_stats',
          targetBufferName: 'polled_stats',
          sysStatsConfig: {
            meminfoPeriodMs: POLLING_INTERVAL_MS,
            psiPeriodMs: POLLING_INTERVAL_MS,
            meminfoCounters: [
              protos.MeminfoCounters.MEMINFO_MEM_TOTAL,
              protos.MeminfoCounters.MEMINFO_MEM_FREE,
              protos.MeminfoCounters.MEMINFO_MEM_AVAILABLE,
              protos.MeminfoCounters.MEMINFO_BUFFERS,
              protos.MeminfoCounters.MEMINFO_CACHED,
              protos.MeminfoCounters.MEMINFO_SWAP_TOTAL,
              protos.MeminfoCounters.MEMINFO_SWAP_FREE,
              protos.MeminfoCounters.MEMINFO_SWAP_CACHED,
              protos.MeminfoCounters.MEMINFO_SHMEM,
              protos.MeminfoCounters.MEMINFO_ACTIVE_ANON,
              protos.MeminfoCounters.MEMINFO_INACTIVE_ANON,
              protos.MeminfoCounters.MEMINFO_ACTIVE_FILE,
              protos.MeminfoCounters.MEMINFO_INACTIVE_FILE,
              protos.MeminfoCounters.MEMINFO_ANON_PAGES,
              protos.MeminfoCounters.MEMINFO_SLAB,
              protos.MeminfoCounters.MEMINFO_KERNEL_STACK,
              protos.MeminfoCounters.MEMINFO_PAGE_TABLES,
              protos.MeminfoCounters.MEMINFO_ZRAM,
              protos.MeminfoCounters.MEMINFO_MAPPED,
              protos.MeminfoCounters.MEMINFO_DIRTY,
              protos.MeminfoCounters.MEMINFO_WRITEBACK,
            ],
            vmstatPeriodMs: POLLING_INTERVAL_MS,
            vmstatCounters: [
              protos.VmstatCounters.VMSTAT_PSWPIN,
              protos.VmstatCounters.VMSTAT_PSWPOUT,
              protos.VmstatCounters.VMSTAT_PGFAULT,
              protos.VmstatCounters.VMSTAT_PGMAJFAULT,
              protos.VmstatCounters.VMSTAT_WORKINGSET_REFAULT_FILE,
              protos.VmstatCounters.VMSTAT_PGSTEAL_FILE,
              protos.VmstatCounters.VMSTAT_PGSCAN_FILE,
            ],
          },
        },
      },
      {
        config: {
          name: 'linux.ftrace',
          targetBufferName: 'ftrace',
          ftraceConfig: {
            ftraceEvents: [
              'dmabuf_heap/dma_heap_stat',
              'sched/sched_process_exit',
              'sched/sched_process_free',
              'task/task_newtask',
              'task/task_rename',
              'lowmemorykiller/lowmemory_kill',
              'oom/oom_score_adj_update',
            ],
            atraceApps: ['lmkd'],
            disableGenericEvents: true,
          },
        },
      },
    ],
  };
}

interface ExtractResult {
  data: SnapshotData;
  queryMs: number;
  extractMs: number;
}

async function extractSnapshotData(
  engine: WasmEngineProxy,
): Promise<ExtractResult> {
  const tQueryStart = performance.now();
  const [
    ts0Result,
    sysTrackResult,
    procTrackResult,
    samplesResult,
    rateSamplesResult,
    metaResult,
    lmkResult,
    buildResult,
  ] = await Promise.all([
    engine.query(`
        SELECT
          (SELECT start_ts FROM trace_bounds) as ts0,
          MIN(c.ts) as min_ts,
          MAX(c.ts) as max_ts
        FROM counter c
        JOIN counter_track t ON c.track_id = t.id
        WHERE t.name = 'MemTotal'
      `),
    engine.query(`
        SELECT id, name
        FROM counter_track
        WHERE id NOT IN (SELECT id FROM process_counter_track)
      `),
    engine.query(`
        SELECT t.id, p.upid, p.name AS process_name, p.pid,
               t.name AS counter_name
        FROM process_counter_track t
        JOIN process p ON t.upid = p.upid
        WHERE p.name IS NOT NULL
      `),
    // Level counters (meminfo, RSS, oom_score_adj, etc). Pass through as-is.
    // Excludes psi/vmstat which are cumulative monotonic counters — those
    // are diffed into per-second rates by the next query.
    engine.query(`
        SELECT c.track_id AS trackId, c.ts, c.value
        FROM counter c
        JOIN track t ON c.track_id = t.id
        WHERE t.type NOT IN ('psi', 'vmstat')
        ORDER BY c.ts
      `),
    // Cumulative monotonic kernel counters (psi, vmstat). Diff each track in
    // SQL via LAG so the UI never sees the raw level. Both end up per-second:
    //   - psi:    ns_stall_delta / s_dt → divide by 1e6 to get ms/s.
    //   - vmstat: count_delta / s_dt    → events/s.
    // ts is in ns, so dt_s = (ts - prev_ts) / 1e9; the algebra simplifies to
    // the expressions below. First-of-track rows (prev_ts IS NULL) are
    // dropped.
    engine.query(`
        WITH diffed AS (
          SELECT
            c.track_id AS trackId,
            c.ts,
            t.type AS type,
            c.value - LAG(c.value) OVER w AS dvalue,
            c.ts - LAG(c.ts) OVER w AS dts
          FROM counter c
          JOIN track t ON c.track_id = t.id
          WHERE t.type IN ('psi', 'vmstat')
          WINDOW w AS (PARTITION BY c.track_id ORDER BY c.ts)
        )
        SELECT trackId, ts,
          MAX(0, CASE
            WHEN type = 'psi' THEN dvalue * 1e3 / dts
            ELSE dvalue * 1e9 / dts
          END) AS value
        FROM diffed
        WHERE dts > 0
        ORDER BY ts
      `),
    engine.query(`
        SELECT p.upid, p.name, p.pid, p.start_ts,
              COALESCE(pkg.profileable_from_shell, 0) AS debuggable
        FROM process p
        LEFT JOIN package_list pkg ON p.uid = pkg.uid
        WHERE p.name IS NOT NULL
      `),
    engine.query(`
        SELECT ts,
              CAST(str_split(s.name, ',', 1) AS INTEGER) AS pid,
              CAST(str_split(s.name, ',', 3) AS INTEGER) AS oom_score_adj,
              COALESCE(p.name, '') AS process_name
        FROM slice s
        LEFT JOIN process_track pt ON s.track_id = pt.id
        LEFT JOIN process p ON CAST(str_split(s.name, ',', 1) AS INTEGER) = p.pid
          AND s.ts >= COALESCE(p.start_ts, 0)
          AND s.ts <= COALESCE(p.end_ts, (SELECT MAX(ts) FROM counter))
        WHERE s.name GLOB 'lmk,*'
        UNION ALL
        SELECT c.ts,
              CAST(c.value AS INTEGER) AS pid,
              0 AS oom_score_adj,
              COALESCE(p.name, '') AS process_name
        FROM counter c
        JOIN counter_track ct ON c.track_id = ct.id
        LEFT JOIN process p ON CAST(c.value AS INTEGER) = p.pid
          AND c.ts >= COALESCE(p.start_ts, 0)
          AND c.ts <= COALESCE(p.end_ts, (SELECT MAX(ts) FROM counter))
        WHERE ct.name = 'kill_one_process' AND c.value > 0
        ORDER BY ts
      `),
    engine.query(`
        SELECT str_value AS fingerprint
        FROM metadata
        WHERE name = 'android_build_fingerprint'
        LIMIT 1
      `),
  ]);
  const tExtractStart = performance.now();
  const task = await deferChunkedTask({priority: 'background'});
  const YIELD_CHECK_INTERVAL = 64; // Num loops before yield check

  const boundsRow = ts0Result.firstRow({ts0: NUM, min_ts: NUM, max_ts: NUM});
  const ts0 = boundsRow?.ts0 ?? 0;
  const xMin = boundsRow !== undefined ? (boundsRow.min_ts - ts0) / 1e9 : 0;
  const xMax = boundsRow !== undefined ? (boundsRow.max_ts - ts0) / 1e9 : 0;

  // Build track id → counter name map for system counters.
  const sysTrackMap = new Map<number, string>();
  const sysTrackIter = sysTrackResult.iter({id: NUM, name: STR});
  for (; sysTrackIter.valid(); sysTrackIter.next()) {
    sysTrackMap.set(sysTrackIter.id, sysTrackIter.name);
  }

  // Build track id → {upid, processName, pid, counterName} for process counters.
  const procTrackMap = new Map<
    number,
    {upid: number; processName: string; pid: number; counterName: string}
  >();
  const procTrackIter = procTrackResult.iter({
    id: NUM,
    upid: NUM,
    process_name: STR,
    pid: NUM,
    counter_name: STR,
  });
  for (; procTrackIter.valid(); procTrackIter.next()) {
    procTrackMap.set(procTrackIter.id, {
      upid: procTrackIter.upid,
      processName: procTrackIter.process_name,
      pid: procTrackIter.pid,
      counterName: procTrackIter.counter_name,
    });
  }

  // Single pass over all counter samples, dispatching by track type.
  const systemCounters = new Map<string, TsValue[]>();
  const processCountersByUpid = new Map<number, Map<string, TsValue[]>>();
  const samplesIter = samplesResult.iter({trackId: NUM, ts: NUM, value: NUM});
  for (let i = 0; samplesIter.valid(); samplesIter.next(), ++i) {
    if (i % YIELD_CHECK_INTERVAL === 0 && task.shouldYield()) {
      await task.yield();
    }
    const {trackId, ts, value} = samplesIter;

    const sysName = sysTrackMap.get(trackId);
    if (sysName !== undefined) {
      let arr = systemCounters.get(sysName);
      if (arr === undefined) {
        arr = [];
        systemCounters.set(sysName, arr);
      }
      arr.push({ts, value});
      continue;
    }

    const proc = procTrackMap.get(trackId);
    if (proc !== undefined) {
      let counters = processCountersByUpid.get(proc.upid);
      if (counters === undefined) {
        counters = new Map();
        processCountersByUpid.set(proc.upid, counters);
      }
      let arr = counters.get(proc.counterName);
      if (arr === undefined) {
        arr = [];
        counters.set(proc.counterName, arr);
      }
      arr.push({ts, value});
    }
  }

  // Rate-converted psi/vmstat samples land in systemCounters under the same
  // counter names as the levels would have — the UI just sees per-second
  // rates (ms/s for psi, events/s for vmstat).
  const rateIter = rateSamplesResult.iter({trackId: NUM, ts: NUM, value: NUM});
  for (let i = 0; rateIter.valid(); rateIter.next(), ++i) {
    if (i % YIELD_CHECK_INTERVAL === 0 && task.shouldYield()) {
      await task.yield();
    }
    const {trackId, ts, value} = rateIter;
    const sysName = sysTrackMap.get(trackId);
    if (sysName === undefined) continue;
    let arr = systemCounters.get(sysName);
    if (arr === undefined) {
      arr = [];
      systemCounters.set(sysName, arr);
    }
    arr.push({ts, value});
  }

  // Build process metadata, keyed by upid.
  const processInfo = new Map<number, ProcessInfo>();
  const metaIter = metaResult.iter({
    upid: NUM,
    name: STR,
    pid: NUM,
    start_ts: NUM_NULL,
    debuggable: NUM,
  });
  for (let i = 0; metaIter.valid(); metaIter.next(), ++i) {
    if (i % YIELD_CHECK_INTERVAL === 0 && task.shouldYield()) {
      await task.yield();
    }
    processInfo.set(metaIter.upid, {
      upid: metaIter.upid,
      pid: metaIter.pid,
      processName: metaIter.name,
      startTs: metaIter.start_ts,
      debuggable: metaIter.debuggable !== 0,
    });
  }

  // Build LMK events.
  const lmkEvents: LmkEvent[] = [];
  const lmkIter = lmkResult.iter({
    ts: NUM,
    pid: NUM,
    oom_score_adj: NUM,
    process_name: STR,
  });
  for (let i = 0; lmkIter.valid(); lmkIter.next(), ++i) {
    if (i % YIELD_CHECK_INTERVAL === 0 && task.shouldYield()) {
      await task.yield();
    }
    lmkEvents.push({
      ts: lmkIter.ts,
      pid: lmkIter.pid,
      processName: lmkIter.process_name,
      oomScoreAdj: lmkIter.oom_score_adj,
    });
  }

  // Check if device is userdebug.
  const buildRow = buildResult.maybeFirstRow({fingerprint: STR});
  const isUserDebug = buildRow?.fingerprint?.includes('userdebug') ?? false;

  return {
    data: {
      systemCounters,
      processCountersByUpid,
      processInfo,
      lmkEvents,
      isUserDebug,
      ts0,
      xMin,
      xMax,
    },
    queryMs: Math.round(tExtractStart - tQueryStart),
    extractMs: Math.round(performance.now() - tExtractStart),
  };
}
