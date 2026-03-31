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

import protos from '../../protos';
import {uuidv4} from '../../base/uuid';
import {deferChunkedTask} from '../../base/chunked_task';
import {NUM, NUM_NULL, STR} from '../../trace_processor/query_result';
import {WasmEngineProxy} from '../../trace_processor/wasm_engine_proxy';
import {AdbDevice} from '../dev.perfetto.RecordTraceV2/adb/adb_device';
import {
  createAdbTracingSession,
  cloneAdbTracingSession,
} from '../dev.perfetto.RecordTraceV2/adb/adb_tracing_session';
import {TracingSession} from '../dev.perfetto.RecordTraceV2/interfaces/tracing_session';
import {TracedWebsocketTarget} from '../dev.perfetto.RecordTraceV2/traced_over_websocket/traced_websocket_target';
import {ConnectionResult} from './connection_page';
import {ProcessProfile} from './process_profile';

let engineCounter = 0;

const DEFAULT_SNAPSHOT_INTERVAL_MS = 3_000;

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
  pid: number;
  startTs: number | null;
  debuggable: boolean;
}

export interface SnapshotData {
  // counterName → sorted array of {ts, value}
  systemCounters: Map<string, TsValue[]>;

  // processName → counterName → Map<ts, summedValue>
  processCountersByName: Map<string, Map<string, Map<number, number>>>;

  // pid → counterName → sorted array of {ts, value}
  processCountersByPid: Map<number, Map<string, TsValue[]>>;

  // processName → metadata
  processInfo: Map<string, ProcessInfo>;

  // LMK events sorted by ts.
  lmkEvents: LmkEvent[];

  // Whether the device is a userdebug build.
  isUserDebug: boolean;
}

export type OnSnapshotCallback = (data: SnapshotData) => void;

/**
 * Connects to a device or Linux host, starts a memory tracing session, and
 * polls snapshots on a timer. Each snapshot clones the running trace, loads
 * it into a reusable TraceProcessor engine, extracts structured data, and
 * notifies registered callbacks.
 */
export class MementoSession {
  private session?: TracingSession;
  private engine?: WasmEngineProxy;
  private readonly sessionName: string;
  private readonly device?: AdbDevice;
  private readonly linuxTarget?: TracedWebsocketTarget;
  private timer?: ReturnType<typeof setTimeout>;
  private snapshotInFlight = false;
  private readonly onSnapshotCallbacks: OnSnapshotCallback[] = [];
  snapshotIntervalMs = DEFAULT_SNAPSHOT_INTERVAL_MS;

  readonly deviceName: string;
  snapshotCount = 0;
  status = 'Starting tracing session...';
  isPaused = false;
  data?: SnapshotData;
  lastTraceBuffer?: ArrayBuffer;

  // Timing breakdown for the last snapshot (ms).
  lastSnapshotMs = 0;
  lastCloneMs = 0;
  lastParseMs = 0;
  lastQueryMs = 0;
  lastExtractMs = 0;

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

  /** Changes the snapshot interval and restarts the timer. */
  setSnapshotInterval(ms: number): void {
    this.snapshotIntervalMs = ms;
    clearTimeout(this.timer);
    if (!this.isPaused && this.session && !this.snapshotInFlight) {
      this.scheduleSnapshot();
    }
  }

  /** Pauses or resumes snapshot polling. */
  togglePause(): void {
    this.isPaused = !this.isPaused;
    if (!this.isPaused) {
      this.scheduleSnapshot();
    } else {
      clearTimeout(this.timer);
    }
  }

  /** Starts a heap profiling session for a single process. */
  async startProcessProfile(
    pid: number,
    processName: string,
  ): Promise<ProcessProfile> {
    const config: protos.ITraceConfig = {
      buffers: [
        {
          sizeKb: 64 * 1024,
          fillPolicy: protos.TraceConfig.BufferConfig.FillPolicy.RING_BUFFER,
        },
      ],
      dataSources: [
        {
          config: {
            name: 'android.heapprofd',
            heapprofdConfig: {
              pid: [pid],
              samplingIntervalBytes: 4096,
              allHeaps: true,
              shmemSizeBytes: 8 * 1024 * 1024,
            },
          },
        },
        {
          config: {
            name: 'android.java_hprof',
            javaHprofConfig: {
              pid: [pid],
              continuousDumpConfig: {
                dumpIntervalMs: 1000,
              },
            },
          },
        },
        {
          config: {
            name: 'linux.process_stats',
            processStatsConfig: {
              scanAllProcessesOnStart: true,
            },
          },
        },
      ],
    };

    const result = this.linuxTarget
      ? await this.linuxTarget.startTracing(config)
      : await createAdbTracingSession(this.device!, config);
    if (!result.ok) {
      throw new Error(`Failed to start profile: ${result.error}`);
    }
    return new ProcessProfile(result.value, pid, processName);
  }

  /** Stops the tracing session, polling, and disposes of the engine. */
  async dispose(): Promise<void> {
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
    this.scheduleSnapshot();
  }

  private scheduleSnapshot(delayMs = this.snapshotIntervalMs) {
    this.timer = setTimeout(() => this.poll(), Math.max(0, delayMs));
  }

  private async poll(): Promise<void> {
    if (!this.session || this.snapshotInFlight) return;
    this.snapshotInFlight = true;

    try {
      const t0 = performance.now();

      const cloneResult = this.linuxTarget
        ? await this.linuxTarget.cloneSession(this.sessionName)
        : await cloneAdbTracingSession(this.device!, this.sessionName);
      if (!cloneResult.ok) {
        this.status = `Snapshot failed: ${cloneResult.error}`;
        return;
      }
      this.lastTraceBuffer = cloneResult.value.buffer as ArrayBuffer;
      const tClone = performance.now();

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
      this.snapshotCount++;
      this.status = `Snapshot #${this.snapshotCount} OK (${this.lastSnapshotMs}ms)`;
    } catch (e) {
      this.status = `Snapshot error: ${e}`;
    } finally {
      this.snapshotInFlight = false;
      if (!this.isPaused) {
        const nextDelay = this.snapshotIntervalMs - this.lastSnapshotMs;
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
        sizeKb: 4 * 1024,
        fillPolicy: protos.TraceConfig.BufferConfig.FillPolicy.DISCARD,
      },
      {
        sizeKb: 4 * 1024,
        fillPolicy: protos.TraceConfig.BufferConfig.FillPolicy.RING_BUFFER,
      },
      {
        sizeKb: 4 * 1024,
        fillPolicy: protos.TraceConfig.BufferConfig.FillPolicy.RING_BUFFER,
      },
    ],
    dataSources: [
      {
        config: {
          name: 'linux.process_stats',
          targetBuffer: 0,
          processStatsConfig: {
            scanAllProcessesOnStart: true,
            recordProcessAge: true,
          },
        },
      },
      {
        config: {
          name: 'linux.process_stats',
          targetBuffer: 1,
          processStatsConfig: {
            scanAllProcessesOnStart: false,
            procStatsPollMs: 250,
            recordProcessDmabufRss: true,
          },
        },
      },
      {
        config: {
          name: 'linux.sys_stats',
          targetBuffer: 1,
          sysStatsConfig: {
            meminfoPeriodMs: 250,
            psiPeriodMs: 250,
            vmstatPeriodMs: 250,
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
          name: 'android.packages_list',
          targetBuffer: 0,
        },
      },
      {
        config: {
          name: 'linux.ftrace',
          targetBuffer: 2,
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
            symbolizeKsyms: true,
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
  const [sysResult, procResult, metaResult, lmkResult, buildResult] =
    await Promise.all([
      engine.query(`
        SELECT t.name AS counter_name, c.ts, c.value
        FROM counter c
        JOIN counter_track t ON c.track_id = t.id
        ORDER BY c.ts
      `),
      engine.query(`
        SELECT p.name AS process_name, p.pid, t.name AS counter_name,
              c.ts, c.value
        FROM counter c
        JOIN process_counter_track t ON c.track_id = t.id
        JOIN process p ON t.upid = p.upid
        WHERE p.name IS NOT NULL
        ORDER BY c.ts
      `),
      engine.query(`
        SELECT p.name, p.pid, p.start_ts,
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

  // Build system counters map.
  const systemCounters = new Map<string, TsValue[]>();
  const sysIter = sysResult.iter({
    counter_name: STR,
    ts: NUM,
    value: NUM,
  });
  for (let i = 0; sysIter.valid(); sysIter.next(), ++i) {
    if (i % YIELD_CHECK_INTERVAL === 0 && task.shouldYield()) {
      await task.yield();
    }
    let arr = systemCounters.get(sysIter.counter_name);
    if (arr === undefined) {
      arr = [];
      systemCounters.set(sysIter.counter_name, arr);
    }
    arr.push({ts: sysIter.ts, value: sysIter.value});
  }

  // Build process counters by name (summing across PIDs) and by PID.
  const processCountersByName = new Map<
    string,
    Map<string, Map<number, number>>
  >();
  const processCountersByPid = new Map<number, Map<string, TsValue[]>>();
  const procIter = procResult.iter({
    process_name: STR,
    pid: NUM,
    counter_name: STR,
    ts: NUM,
    value: NUM,
  });
  for (let i = 0; procIter.valid(); procIter.next(), ++i) {
    if (i % YIELD_CHECK_INTERVAL === 0 && task.shouldYield()) {
      await task.yield();
    }

    // By name (summing across PIDs at each ts).
    let byCounter = processCountersByName.get(procIter.process_name);
    if (byCounter === undefined) {
      byCounter = new Map();
      processCountersByName.set(procIter.process_name, byCounter);
    }
    let byTs = byCounter.get(procIter.counter_name);
    if (byTs === undefined) {
      byTs = new Map();
      byCounter.set(procIter.counter_name, byTs);
    }
    byTs.set(procIter.ts, (byTs.get(procIter.ts) ?? 0) + procIter.value);

    // By PID.
    let pidCounters = processCountersByPid.get(procIter.pid);
    if (pidCounters === undefined) {
      pidCounters = new Map();
      processCountersByPid.set(procIter.pid, pidCounters);
    }
    let pidArr = pidCounters.get(procIter.counter_name);
    if (pidArr === undefined) {
      pidArr = [];
      pidCounters.set(procIter.counter_name, pidArr);
    }
    pidArr.push({ts: procIter.ts, value: procIter.value});
  }

  // Build process metadata.
  const processInfo = new Map<string, ProcessInfo>();
  const metaIter = metaResult.iter({
    name: STR,
    pid: NUM,
    start_ts: NUM_NULL,
    debuggable: NUM,
  });
  for (let i = 0; metaIter.valid(); metaIter.next(), ++i) {
    if (i % YIELD_CHECK_INTERVAL === 0 && task.shouldYield()) {
      await task.yield();
    }
    const existing = processInfo.get(metaIter.name);
    if (existing === undefined || metaIter.pid > existing.pid) {
      processInfo.set(metaIter.name, {
        pid: metaIter.pid,
        startTs: metaIter.start_ts,
        debuggable: metaIter.debuggable !== 0,
      });
    }
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
      processCountersByName,
      processCountersByPid,
      processInfo,
      lmkEvents,
      isUserDebug,
    },
    queryMs: Math.round(tExtractStart - tQueryStart),
    extractMs: Math.round(performance.now() - tExtractStart),
  };
}
