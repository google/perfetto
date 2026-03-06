// Copyright (C) 2024 The Android Open Source Project
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
import protos from '../../../protos';
import {RecordingManager} from '../recording_manager';
import {RecordSubpage} from '../config/config_interfaces';
import {RecordPluginSchema} from '../serialization_schema';
import {Button} from '../../../widgets/button';
import {Checkbox} from '../../../widgets/checkbox';
import {TracingSession} from '../interfaces/tracing_session';
import {uuidv4} from '../../../base/uuid';
import {WasmEngineProxy} from '../../../trace_processor/wasm_engine_proxy';
import {NUM, STR} from '../../../trace_processor/query_result';
import {
  DataGrid,
  renderCell,
} from '../../../components/widgets/datagrid/datagrid';
import {SchemaRegistry} from '../../../components/widgets/datagrid/datagrid_schema';
import {Row, SqlValue} from '../../../trace_processor/query_result';
import {
  LineChart,
  LineChartData,
  LineChartSeries,
} from '../../../components/widgets/charts/line_chart';

const snapshotIntervalMs = 3_000;
const snapshotInitialMs = 3_000;

const MEMORY_COUNTER_NAMES = [
  'mem.rss',
  'mem.rss.anon',
  'mem.rss.file',
  'mem.rss.shmem',
  'mem.swap',
] as const;

type MemoryCounterName = (typeof MEMORY_COUNTER_NAMES)[number];

const COUNTER_LABELS: Record<MemoryCounterName, string> = {
  'mem.rss': 'RSS (total)',
  'mem.rss.anon': 'RSS anon',
  'mem.rss.file': 'RSS file',
  'mem.rss.shmem': 'RSS shmem',
  'mem.swap': 'Swap',
};

interface MemoryDataPoint {
  elapsedSec: number;
  counters: Map<MemoryCounterName, number>;
}

export function snapshotPage(recMgr: RecordingManager): RecordSubpage {
  return {
    kind: 'GLOBAL_PAGE',
    id: 'snapshots',
    icon: 'memory',
    title: 'Memory Monitor',
    subtitle: 'Track per-process memory usage',
    render() {
      return m(SnapshotPage, {recMgr});
    },
    serialize(_state: RecordPluginSchema) {},
    deserialize(_state: RecordPluginSchema) {},
  };
}

interface SnapshotPageAttrs {
  recMgr: RecordingManager;
}

let snapshotEngineCounter = 0;

// Creates the "heavy" trace config: heap profiling only.
// This is the trace that gets opened in the UI when recording stops.
function createHeavyTraceConfig(
  uniqueSessionName: string,
  processName: string,
): protos.ITraceConfig {
  return {
    uniqueSessionName,
    durationMs: 0, // No timeout - run until stopped
    buffers: [
      {
        sizeKb: 64 * 1024, // 64MB ring buffer
        fillPolicy: protos.TraceConfig.BufferConfig.FillPolicy.RING_BUFFER,
      },
    ],
    dataSources: [
      {
        config: {
          name: 'android.heapprofd',
          heapprofdConfig: {
            samplingIntervalBytes: 4096,
            processCmdline: [processName],
            shmemSizeBytes: 8 * 1024 * 1024, // 8MB
            blockClient: true,
            allHeaps: false,
          },
        },
      },
      {
        config: {
          name: 'android.java_hprof',
          javaHprofConfig: {
            processCmdline: [processName],
          },
        },
      },
    ],
  };
}

// Creates the lightweight monitoring config: just process_stats counters.
// This session is cloned periodically for the live memory chart. Clones are
// small because there's no heap profiling data in this session.
function createMonitoringConfig(
  uniqueSessionName: string,
): protos.ITraceConfig {
  return {
    uniqueSessionName,
    durationMs: 0,
    buffers: [
      {
        sizeKb: 4 * 1024, // 4MB - counters are tiny
        fillPolicy: protos.TraceConfig.BufferConfig.FillPolicy.RING_BUFFER,
      },
    ],
    dataSources: [
      {
        config: {
          name: 'linux.process_stats',
          processStatsConfig: {
            scanAllProcessesOnStart: true,
            procStatsPollMs: 1000,
          },
        },
      },
    ],
  };
}

class SnapshotPage implements m.ClassComponent<SnapshotPageAttrs> {
  // Heavy session - heap profiling + stats. Opened in UI when stopped.
  private heavySessionName: string = '';
  private heavySession?: TracingSession;

  // Lightweight monitoring session - just counters. Cloned for live chart.
  private monitorSessionName: string = '';
  private monitorSession?: TracingSession;

  private isRecording = false;
  private isStopping = false;
  private openTraceOnStop = true;
  private error?: string;

  // Process selection state
  private processRows: Row[] = [];
  private selectedProcess?: {name: string; pid: number};
  private pollTimer?: number;
  private schema: SchemaRegistry;

  // Auto-clone state
  private dataPoints: MemoryDataPoint[] = [];
  private activeClones = 0;
  private activeQueries = 0;
  private cloneTimer?: number;
  private recordingStartTime?: number;

  constructor() {
    // Build schema with action column
    this.schema = {
      process: {
        process: {title: 'Process', columnType: 'text'},
        pid: {title: 'PID', columnType: 'quantitative'},
        pss_kb: {
          title: 'PSS',
          columnType: 'quantitative',
          cellRenderer: (value: SqlValue) => {
            if (typeof value === 'number') {
              return this.formatKb(value);
            }
            return renderCell(value);
          },
        },
        action: {
          title: '',
          columnType: 'text',
          cellRenderer: (value: SqlValue) => {
            if (typeof value !== 'string') return '';
            const {name, pid} = JSON.parse(value);
            return m(Button, {
              label: 'Choose',
              compact: true,
              onclick: () => {
                this.selectedProcess = {name, pid};
                m.redraw();
              },
            });
          },
        },
      },
    };
  }

  oninit({attrs}: m.CVnode<SnapshotPageAttrs>) {
    this.startPolling(attrs.recMgr);
  }

  onremove() {
    if (this.pollTimer) {
      window.clearInterval(this.pollTimer);
    }
    if (this.cloneTimer !== undefined) {
      window.clearTimeout(this.cloneTimer);
    }
  }

  private startPolling(recMgr: RecordingManager) {
    const poll = async () => {
      const target = recMgr.currentTarget;
      if (target?.pollMemoryStats) {
        const result = await target.pollMemoryStats();
        if (result !== undefined) {
          this.processRows = result.map((s) => ({
            process: s.processName,
            pid: s.pid,
            pss_kb: s.pssKb,
            action: JSON.stringify({name: s.processName, pid: s.pid}),
          }));
          m.redraw();
        }
      }
    };
    poll();
    this.pollTimer = window.setInterval(poll, 3000);
  }

  view({attrs}: m.CVnode<SnapshotPageAttrs>) {
    const recMgr = attrs.recMgr;
    const target = recMgr.currentTarget;
    const supportsClone = target?.cloneSession !== undefined;
    const supportsPoll = target?.pollMemoryStats !== undefined;

    if (!target) {
      return m('.snapshot-page', m('.note', 'Please select a target first.'));
    }

    if (!supportsClone) {
      return m(
        '.snapshot-page',
        m(
          '.note',
          'The current target does not support live snapshots. ',
          'Try using a WebSocket or ADB connection.',
        ),
      );
    }

    if (!supportsPoll) {
      return m(
        '.snapshot-page',
        m(
          '.note',
          'The current target does not support memory polling. ',
          'Try using an ADB connection to an Android device.',
        ),
      );
    }

    // Not recording - show process selection
    if (!this.isRecording) {
      return m(
        '.snapshot-page',
        m('header', 'Select Process'),
        m(
          'p',
          'Click a process row, then click Start Recording to begin tracking.',
        ),

        // Process list datagrid
        m(DataGrid, {
          className: 'pf-device-memory-table',
          schema: this.schema,
          rootSchema: 'process',
          data: this.processRows,
          initialColumns: [
            {id: 'process', field: 'process'},
            {id: 'pid', field: 'pid'},
            {id: 'pss_kb', field: 'pss_kb', sort: 'DESC'},
            {id: 'action', field: 'action'},
          ],
          canAddColumns: false,
          canRemoveColumns: false,
          enablePivotControls: false,
        }),

        // Selected process and start button
        m(
          '.snapshot-controls',
          this.selectedProcess &&
            m(
              'span.selected-process',
              `Selected: ${this.selectedProcess.name} (PID ${this.selectedProcess.pid})`,
            ),
          m(Button, {
            label: 'Start Recording',
            icon: 'fiber_manual_record',
            disabled: !this.selectedProcess,
            onclick: () => this.startRecording(recMgr),
          }),
        ),

        // Error display
        this.error && m('.snapshot-error', this.error),
      );
    }

    // Recording - show memory chart and controls
    return m(
      '.snapshot-page',
      m('header', 'Memory Monitor'),
      m(
        'p',
        `Tracking: ${this.selectedProcess?.name} (PID ${this.selectedProcess?.pid})`,
      ),

      // Session controls - only a Stop button
      m(
        '.snapshot-controls',
        m(Button, {
          label: this.isStopping ? 'Stopping...' : 'Stop',
          icon: 'stop',
          disabled: this.isStopping,
          onclick: () => this.stopRecording(recMgr),
        }),
        m(Checkbox, {
          label: 'Open trace when done',
          checked: this.openTraceOnStop,
          onchange: (e) => {
            this.openTraceOnStop = Boolean(
              (e.target as HTMLInputElement).checked,
            );
          },
        }),
      ),

      // Status
      m(
        '.snapshot-status',
        m('span.recording-indicator', '● Recording'),
        m(
          'span',
          ` Heavy: ${this.heavySessionName} | Monitor: ${this.monitorSessionName}`,
        ),
        m(
          'span',
          ` | ${this.dataPoints.length} snapshots` +
            ` (${this.dataPointsWithData()} with data)`,
        ),
        m(
          'span',
          ` | Clones: ${this.activeClones} | Queries: ${this.activeQueries}`,
        ),
      ),

      // Error display
      this.error && m('.snapshot-error', this.error),

      // Memory history chart
      this.buildChartData() !== undefined
        ? m(LineChart, {
            data: this.buildChartData(),
            height: 300,
            xAxisLabel: 'Time (seconds)',
            yAxisLabel: 'Memory',
            showLegend: true,
            showPoints: true,
            gridLines: 'horizontal',
            formatXValue: (v: number) => `${v.toFixed(0)}s`,
            formatYValue: (v: number) => this.formatBytes(v),
          })
        : m('p', 'Waiting for memory data...'),
    );
  }

  private buildChartData(): LineChartData | undefined {
    if (this.dataPoints.length === 0) return undefined;

    // Determine which counters actually appear in any data point.
    const presentCounters = new Set<MemoryCounterName>();
    for (const dp of this.dataPoints) {
      for (const name of dp.counters.keys()) {
        presentCounters.add(name);
      }
    }

    const series: LineChartSeries[] = [];
    for (const counterName of MEMORY_COUNTER_NAMES) {
      if (!presentCounters.has(counterName)) continue;

      const points = this.dataPoints
        .filter((dp) => dp.counters.has(counterName))
        .map((dp) => ({
          x: dp.elapsedSec,
          y: dp.counters.get(counterName)!,
        }));

      if (points.length > 0) {
        series.push({
          name: COUNTER_LABELS[counterName],
          points,
        });
      }
    }

    return {series};
  }

  private dataPointsWithData(): number {
    return this.dataPoints.filter((dp) => dp.counters.size > 0).length;
  }

  private async startRecording(recMgr: RecordingManager) {
    const target = recMgr.currentTarget;
    if (!target || !this.selectedProcess) return;

    this.error = undefined;
    this.dataPoints = [];
    const uid = uuidv4().substring(0, 8);
    this.heavySessionName = `mem-heavy-${uid}`;
    this.monitorSessionName = `mem-monitor-${uid}`;

    // Start the heavy session (heap profiling + stats) — this is the trace
    // the user opens at the end.
    const heavyConfig = createHeavyTraceConfig(
      this.heavySessionName,
      this.selectedProcess.name,
    );
    console.log('Heavy trace config:', heavyConfig);

    const heavyResult = await target.startTracing(heavyConfig);
    if (!heavyResult.ok) {
      this.error = `Failed to start heavy session: ${heavyResult.error}`;
      m.redraw();
      return;
    }
    this.heavySession = heavyResult.value;

    // Start the lightweight monitoring session (just counters) — this is the
    // one we clone periodically for the live chart.
    const monitorConfig = createMonitoringConfig(this.monitorSessionName);
    console.log('Monitor trace config:', monitorConfig);

    const monitorResult = await target.startTracing(monitorConfig);
    if (!monitorResult.ok) {
      this.error = `Failed to start monitor session: ${monitorResult.error}`;
      // Stop the heavy session since we can't monitor.
      await this.heavySession.stop();
      this.heavySession = undefined;
      m.redraw();
      return;
    }
    this.monitorSession = monitorResult.value;

    this.isRecording = true;
    this.recordingStartTime = Date.now();

    // Listen for heavy session errors.
    this.heavySession.onSessionUpdate.addListener(() => {
      if (this.heavySession?.state === 'ERRORED') {
        this.error = 'Heavy recording session errored';
        this.isRecording = false;
        if (this.cloneTimer !== undefined) {
          window.clearTimeout(this.cloneTimer);
          this.cloneTimer = undefined;
        }
      }
      m.redraw();
    });

    // Take an initial snapshot after a short delay, then schedule the next
    // snapshot only after the current one completes to avoid piling up.
    this.cloneTimer = window.setTimeout(() => {
      this.autoClone(recMgr);
    }, snapshotInitialMs);

    m.redraw();
  }

  private async autoClone(recMgr: RecordingManager): Promise<void> {
    const target = recMgr.currentTarget;
    if (
      !this.isRecording ||
      !target?.cloneSession ||
      !this.monitorSessionName ||
      !this.selectedProcess
    ) {
      return;
    }

    this.activeClones++;
    m.redraw();

    try {
      // Clone the lightweight monitoring session, not the heavy one.
      const result = await target.cloneSession(this.monitorSessionName);
      if (!result.ok) {
        this.error = `Clone failed: ${result.error}`;
        return;
      }

      const traceData = result.value;
      const engineId = `snapshot-${++snapshotEngineCounter}`;
      const engine = new WasmEngineProxy(engineId);

      try {
        await engine.resetTraceProcessor({
          tokenizeOnly: false,
          cropTrackEvents: false,
          ingestFtraceInRawTable: true,
          analyzeTraceProtoContent: false,
          ftraceDropUntilAllCpusValid: true,
          forceFullSort: false,
        });
        await engine.parse(traceData);
        await engine.notifyEof();

        this.activeQueries++;
        m.redraw();

        try {
          const counters = await this.extractMemoryCounters(
            engine,
            this.selectedProcess.pid,
          );

          const elapsedSec =
            (Date.now() - (this.recordingStartTime ?? Date.now())) / 1000;

          this.dataPoints.push({elapsedSec, counters});
          this.error = undefined; // Clear any previous error on success.
        } finally {
          this.activeQueries--;
        }
      } finally {
        engine[Symbol.dispose]();
      }
    } catch (e) {
      this.error = `Snapshot error: ${e}`;
    } finally {
      this.activeClones--;
      // Schedule the next snapshot after this one completes.
      if (this.isRecording) {
        this.cloneTimer = window.setTimeout(() => {
          this.autoClone(recMgr);
        }, snapshotIntervalMs);
      }
      m.redraw();
    }
  }

  private async extractMemoryCounters(
    engine: WasmEngineProxy,
    pid: number,
  ): Promise<Map<MemoryCounterName, number>> {
    const counters = new Map<MemoryCounterName, number>();

    const queryResult = await engine.query(`
      SELECT
        t.name AS counter_name,
        c.value AS counter_value
      FROM counter c
      JOIN process_counter_track t ON c.track_id = t.id
      JOIN process p ON t.upid = p.upid
      WHERE t.name IN (
        'mem.rss',
        'mem.rss.anon',
        'mem.rss.file',
        'mem.rss.shmem',
        'mem.swap'
      )
      AND p.pid = ${pid}
      AND c.ts = (
        SELECT MAX(c2.ts)
        FROM counter c2
        WHERE c2.track_id = c.track_id
      )
    `);

    const iter = queryResult.iter({
      counter_name: STR,
      counter_value: NUM,
    });

    for (; iter.valid(); iter.next()) {
      const name = iter.counter_name as MemoryCounterName;
      if ((MEMORY_COUNTER_NAMES as readonly string[]).includes(name)) {
        counters.set(name, iter.counter_value as number);
      }
    }

    return counters;
  }

  private async stopRecording(recMgr: RecordingManager) {
    if (!this.heavySession) return;

    this.isStopping = true;

    // Stop the auto-clone timer.
    if (this.cloneTimer !== undefined) {
      window.clearTimeout(this.cloneTimer);
      this.cloneTimer = undefined;
    }

    m.redraw();

    // Stop the monitoring session first (we don't need its data).
    if (this.monitorSession) {
      await this.monitorSession.stop();
      this.monitorSession = undefined;
    }

    // Stop the heavy session and wait for it to finish.
    await this.heavySession.stop();

    await new Promise<void>((resolve) => {
      const checkState = () => {
        if (this.heavySession?.state === 'FINISHED') {
          resolve();
        } else if (this.heavySession?.state === 'ERRORED') {
          this.error = 'Heavy session ended with error';
          resolve();
        } else {
          setTimeout(checkState, 100);
        }
      };
      checkState();
    });

    // Get the trace data from the heavy session and open it if requested.
    const traceData = this.heavySession.getTraceData();
    if (traceData && this.openTraceOnStop) {
      const fileName = `memory-${this.selectedProcess?.name ?? 'trace'}-${Date.now()}.perfetto-trace`;
      recMgr.app.openTraceFromBuffer({
        buffer: traceData.buffer as ArrayBuffer,
        title: fileName,
        fileName,
      });
    }

    this.heavySession = undefined;
    this.monitorSession = undefined;
    this.isRecording = false;
    this.isStopping = false;
    this.selectedProcess = undefined;
    this.dataPoints = [];
    this.recordingStartTime = undefined;
    m.redraw();
  }

  private formatBytes(bytes: number): string {
    if (bytes === 0) return '0 B';
    const k = 1024;
    const sizes = ['B', 'KB', 'MB', 'GB'];
    const i = Math.floor(Math.log(bytes) / Math.log(k));
    return `${(bytes / Math.pow(k, i)).toFixed(1)} ${sizes[i]}`;
  }

  private formatKb(kb: number): string {
    if (kb < 1024) return `${kb.toLocaleString()} KB`;
    if (kb < 1024 * 1024) return `${(kb / 1024).toFixed(1)} MB`;
    return `${(kb / (1024 * 1024)).toFixed(1)} GB`;
  }
}
