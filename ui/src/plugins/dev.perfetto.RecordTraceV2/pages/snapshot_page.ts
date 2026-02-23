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
import {TracingSession} from '../interfaces/tracing_session';
import {uuidv4} from '../../../base/uuid';
import {WasmEngineProxy} from '../../../trace_processor/wasm_engine_proxy';
import {EngineBase} from '../../../trace_processor/engine';
import {NUM} from '../../../trace_processor/query_result';
import {
  DataGrid,
  renderCell,
} from '../../../components/widgets/datagrid/datagrid';
import {SchemaRegistry} from '../../../components/widgets/datagrid/datagrid_schema';
import {Row, SqlValue} from '../../../trace_processor/query_result';

interface SnapshotEntry {
  timestamp: Date;
  sizeBytes: number;
  data?: Uint8Array;
  engine?: EngineBase;
  loading?: boolean;
  error?: string;
  rssBytes?: number;
}

export function snapshotPage(recMgr: RecordingManager): RecordSubpage {
  return {
    kind: 'GLOBAL_PAGE',
    id: 'snapshots',
    icon: 'memory',
    title: 'Memory Snapshots',
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

// Creates a minimal trace config focused on per-process memory counters.
function createMemorySnapshotConfig(
  uniqueSessionName: string,
): protos.ITraceConfig {
  return {
    uniqueSessionName,
    durationMs: 0, // No timeout - run until stopped
    buffers: [
      {
        sizeKb: 32 * 1024, // 32MB ring buffer
        fillPolicy: protos.TraceConfig.BufferConfig.FillPolicy.RING_BUFFER,
      },
    ],
    dataSources: [
      // Per-process memory stats (RSS, swap, etc.)
      {
        config: {
          name: 'linux.process_stats',
          targetBuffer: 0,
          processStatsConfig: {
            scanAllProcessesOnStart: true,
            procStatsPollMs: 1000, // Poll every second
          },
        },
      },
      // Package list for mapping PIDs to package names (Android)
      {
        config: {
          name: 'android.packages_list',
          targetBuffer: 0,
        },
      },
      // Ftrace events for process/thread association
      {
        config: {
          name: 'linux.ftrace',
          targetBuffer: 0,
          ftraceConfig: {
            ftraceEvents: [
              'sched/sched_process_exit',
              'sched/sched_process_free',
              'task/task_newtask',
              'task/task_rename',
            ],
          },
        },
      },
    ],
  };
}

class SnapshotPage implements m.ClassComponent<SnapshotPageAttrs> {
  private sessionName: string = '';
  private session?: TracingSession;
  private isRecording = false;
  private snapshots: SnapshotEntry[] = [];
  private isTakingSnapshot = false;
  private error?: string;

  // Process selection state
  private processRows: Row[] = [];
  private selectedProcess?: {name: string; pid: number};
  private pollTimer?: number;
  private schema: SchemaRegistry;

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
        this.processRows.length > 0 &&
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

    // Recording - show snapshot controls
    return m(
      '.snapshot-page',
      m('header', 'Memory Snapshots'),
      m(
        'p',
        `Tracking: ${this.selectedProcess?.name} (PID ${this.selectedProcess?.pid})`,
      ),

      // Session controls
      m(
        '.snapshot-controls',
        m(Button, {
          label: this.isTakingSnapshot ? 'Taking...' : 'Take Snapshot',
          icon: 'photo_camera',
          disabled: this.isTakingSnapshot,
          onclick: () => this.takeSnapshot(recMgr),
        }),
        m(Button, {
          label: 'Stop Recording',
          icon: 'stop',
          onclick: () => this.stopRecording(),
        }),
      ),

      // Status
      m(
        '.snapshot-status',
        m('span.recording-indicator', '● Recording'),
        m('span', ` Session: ${this.sessionName}`),
      ),

      // Error display
      this.error && m('.snapshot-error', this.error),

      // Snapshots list
      this.snapshots.length > 0 && [
        m('header', 'Snapshots'),
        m(
          '.snapshot-list',
          this.snapshots.map((snap, i) =>
            m(
              '.snapshot-entry',
              {key: i},
              m('span.snapshot-num', `#${i + 1}`),
              m('span.snapshot-time', snap.timestamp.toLocaleTimeString()),
              snap.error
                ? m('span.snapshot-error', snap.error)
                : snap.loading
                  ? m(
                      'span.snapshot-loading',
                      'Loading into trace processor...',
                    )
                  : [
                      m('span.snapshot-size', this.formatBytes(snap.sizeBytes)),
                      snap.rssBytes !== undefined &&
                        m(
                          'span.snapshot-rss',
                          ` | RSS: ${this.formatBytes(snap.rssBytes)}`,
                        ),
                    ],
            ),
          ),
        ),
      ],
    );
  }

  private async startRecording(recMgr: RecordingManager) {
    const target = recMgr.currentTarget;
    if (!target || !this.selectedProcess) return;

    this.error = undefined;
    this.snapshots = [];
    this.sessionName = `snapshot-session-${uuidv4().substring(0, 8)}`;

    // Create a minimal config focused on memory counters
    const traceConfig = createMemorySnapshotConfig(this.sessionName);

    const result = await target.startTracing(traceConfig);
    if (!result.ok) {
      this.error = `Failed to start recording: ${result.error}`;
      m.redraw();
      return;
    }

    this.session = result.value;
    this.isRecording = true;

    // Listen for session state changes
    this.session.onSessionUpdate.addListener(() => {
      if (this.session?.state === 'ERRORED') {
        this.error = 'Recording session errored';
        this.isRecording = false;
      }
      m.redraw();
    });

    m.redraw();
  }

  private async takeSnapshot(recMgr: RecordingManager) {
    const target = recMgr.currentTarget;
    if (!target?.cloneSession || !this.sessionName || !this.selectedProcess)
      return;

    this.isTakingSnapshot = true;
    this.error = undefined;
    m.redraw();

    const result = await target.cloneSession(this.sessionName);

    const entry: SnapshotEntry = {
      timestamp: new Date(),
      sizeBytes: 0,
    };

    if (result.ok) {
      entry.sizeBytes = result.value.length;
      entry.data = result.value;
      entry.loading = true;

      // Add entry immediately to show progress
      this.snapshots.push(entry);
      this.isTakingSnapshot = false;
      m.redraw();

      // Create a new trace processor instance and load the snapshot
      try {
        const engine = await this.createEngineAndLoadTrace(entry.data);
        entry.engine = engine;
        entry.loading = false;

        // Get latest RSS for the selected process
        const processName = this.selectedProcess.name;
        const memResult = await engine.query(`
          SELECT
            c.value as rss_bytes
          FROM counter c
          JOIN process_counter_track t ON c.track_id = t.id
          JOIN process p ON t.upid = p.upid
          WHERE t.name = 'mem.rss'
            AND p.name GLOB '*${processName}*'
          ORDER BY c.ts DESC
          LIMIT 1
        `);
        const iter = memResult.iter({rss_bytes: NUM});
        if (iter.valid()) {
          entry.rssBytes = iter.rss_bytes as number;
        }
      } catch (e) {
        entry.error = `Failed to load into trace processor: ${e}`;
        entry.loading = false;
      }
      m.redraw();
    } else {
      entry.error = result.error;
      this.snapshots.push(entry);
      this.isTakingSnapshot = false;
      m.redraw();
    }
  }

  private async createEngineAndLoadTrace(
    traceData: Uint8Array,
  ): Promise<EngineBase> {
    const engineId = `snapshot-${++snapshotEngineCounter}`;
    const engine = new WasmEngineProxy(engineId);

    // Initialize the trace processor
    await engine.resetTraceProcessor({
      tokenizeOnly: false,
      cropTrackEvents: false,
      ingestFtraceInRawTable: true,
      analyzeTraceProtoContent: false,
      ftraceDropUntilAllCpusValid: true,
      forceFullSort: false,
    });

    // Load the trace data
    await engine.parse(traceData);
    await engine.notifyEof();

    return engine;
  }

  private stopRecording() {
    this.session?.cancel();
    this.session = undefined;
    this.isRecording = false;
    this.selectedProcess = undefined;
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
