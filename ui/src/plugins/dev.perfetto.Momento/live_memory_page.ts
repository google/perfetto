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
import protos from '../../protos';
import {exists} from '../../base/utils';
import {uuidv4} from '../../base/uuid';
import {NUM, NUM_NULL, STR} from '../../trace_processor/query_result';
import {WasmEngineProxy} from '../../trace_processor/wasm_engine_proxy';
import {AdbKeyManager} from '../dev.perfetto.RecordTraceV2/adb/webusb/adb_key_manager';
import {AdbWebusbDevice} from '../dev.perfetto.RecordTraceV2/adb/webusb/adb_webusb_device';
import {
  ADB_DEVICE_FILTER,
  getAdbWebUsbInterface,
} from '../dev.perfetto.RecordTraceV2/adb/webusb/adb_webusb_utils';
import {AdbDevice} from '../dev.perfetto.RecordTraceV2/adb/adb_device';
import {AdbWebsocketDevice} from '../dev.perfetto.RecordTraceV2/adb/websocket/adb_websocket_device';
import {adbCmdAndWait} from '../dev.perfetto.RecordTraceV2/adb/websocket/adb_websocket_utils';
import {AsyncWebsocket} from '../dev.perfetto.RecordTraceV2/websocket/async_websocket';
import {
  WDP_TRACK_DEVICES_SCHEMA,
  type WdpDevice,
} from '../dev.perfetto.RecordTraceV2/adb/web_device_proxy/wdp_schema';
import {showPopupWindow} from '../../base/popup_window';
import {
  cloneAdbTracingSession,
  createAdbTracingSession,
} from '../dev.perfetto.RecordTraceV2/adb/adb_tracing_session';
import {TracingSession} from '../dev.perfetto.RecordTraceV2/interfaces/tracing_session';
import {Button, ButtonVariant} from '../../widgets/button';
import {Icon} from '../../widgets/icon';
import {SegmentedButtons} from '../../widgets/segmented_buttons';
import {
  LineChart,
  LineChartData,
  LineChartSeries,
} from '../../components/widgets/charts/line_chart';
import {Sankey, SankeyData} from '../../components/widgets/charts/sankey';
import {DataGrid} from '../../components/widgets/datagrid/datagrid';
import type {SchemaRegistry} from '../../components/widgets/datagrid/datagrid_schema';
import type {Row} from '../../trace_processor/query_result';
import {
  categorizeProcess,
  CATEGORIES,
  type CategoryId,
} from './process_categories';
import {Intent} from '../../widgets/common';
import {App} from '../../public/app';
import {TracedWebsocketTarget} from '../dev.perfetto.RecordTraceV2/traced_over_websocket/traced_websocket_target';

type ProcessGrouping = 'category' | 'oom_score';

interface OomScoreBucket {
  readonly name: string;
  readonly color: string;
  readonly minScore: number;
  readonly maxScore: number;
}

const OOM_SCORE_BUCKETS: readonly OomScoreBucket[] = [
  {name: 'Native (< 0)', color: '#1565c0', minScore: -1000, maxScore: -1},
  {name: 'Foreground (0)', color: '#4caf50', minScore: 0, maxScore: 0},
  {name: 'Visible (1-99)', color: '#8bc34a', minScore: 1, maxScore: 99},
  {
    name: 'Perceptible (100-299)',
    color: '#ff9800',
    minScore: 100,
    maxScore: 299,
  },
  {name: 'Service (300-599)', color: '#ff5722', minScore: 300, maxScore: 599},
  {name: 'Cached (600-899)', color: '#9c27b0', minScore: 600, maxScore: 899},
  {name: 'Cached (900+)', color: '#f44336', minScore: 900, maxScore: 1001},
];

const CLONE_INTERVAL_MS = 3_000;
const CLONE_INITIAL_DELAY_MS = 3_000;

let engineCounter = 0;

// System-level meminfo counters we want to collect.
const SYSTEM_MEMINFO_COUNTERS = [
  'MemTotal',
  'MemFree',
  'MemAvailable',
  'Buffers',
  'Cached',
  'SwapTotal',
  'SwapFree',
  'SwapCached',
  'Shmem',
  'Active(anon)',
  'Inactive(anon)',
  'Active(file)',
  'Inactive(file)',
  'AnonPages',
  'Slab',
  'KernelStack',
  'PageTables',
  'Zram',
  'Mlocked',
  'Mapped',
  'Dirty',
  'Writeback',
] as const;

// Build a lookup from category name → color for the cell renderer.
const CATEGORY_COLOR_MAP = new Map<string, string>(
  (Object.values(CATEGORIES) as readonly {name: string; color: string}[]).map(
    (c) => [c.name, c.color],
  ),
);

const PROCESS_TABLE_SCHEMA: SchemaRegistry = {
  process: {
    process: {title: 'Process', columnType: 'text'},
    category: {
      title: 'Category',
      columnType: 'text',
      cellRenderer: (v) => {
        const color = CATEGORY_COLOR_MAP.get(v as string);
        return color !== undefined
          ? m('span', {style: {color}}, v as string)
          : (v as string);
      },
    },
    pid: {title: 'PID', columnType: 'quantitative'},
    oom_score: {
      title: 'OOM Adj',
      columnType: 'quantitative',
      cellRenderer: (v) => {
        const score = v as number;
        const bucket = OOM_SCORE_BUCKETS.find(
          (b) => score >= b.minScore && score <= b.maxScore,
        );
        if (bucket === undefined) return `${score}`;
        // Extract just the category label (strip the range in parens).
        const label = bucket.name.replace(/ \(.*\)$/, '');
        return m('span', {style: {color: bucket.color}}, `${score} (${label})`);
      },
    },
    rss_kb: {
      title: 'RSS',
      columnType: 'quantitative',
      cellRenderer: (v) => formatKb(v as number),
    },
    anon_swap_kb: {
      title: 'Anon + Swap',
      columnType: 'quantitative',
      cellRenderer: (v) => ((v as number) > 0 ? formatKb(v as number) : '-'),
    },
    file_kb: {
      title: 'File',
      columnType: 'quantitative',
      cellRenderer: (v) => ((v as number) > 0 ? formatKb(v as number) : '-'),
    },
    shmem_kb: {
      title: 'Shmem',
      columnType: 'quantitative',
      cellRenderer: (v) => ((v as number) > 0 ? formatKb(v as number) : '-'),
    },
    age: {
      title: 'Age',
      columnType: 'quantitative',
      cellRenderer: (v) => {
        const secs = v as number | null;
        if (secs === null || secs < 0) return '-';
        const d = Math.floor(secs / 86400);
        const h = Math.floor((secs % 86400) / 3600);
        const m = Math.floor((secs % 3600) / 60);
        const s = Math.floor(secs % 60);
        if (d > 0) return `${d}d ${h}h`;
        if (h > 0) return `${h}h ${m}m`;
        if (m > 0) return `${m}m ${s}s`;
        return `${s}s`;
      },
    },
  },
};

// Per-process memory row (latest value only, for the table).
interface ProcessMemoryRow {
  processName: string;
  pid: number;
  rssKb: number;
  anonKb: number;
  fileKb: number;
  shmemKb: number;
  swapKb: number;
  oomScore: number;
  ageSeconds: number | null;
}

function createMonitoringConfig(
  uniqueSessionName: string,
): protos.ITraceConfig {
  return {
    uniqueSessionName,
    buffers: [
      // Small buffer to store the process metadata
      {
        sizeKb: 4 * 1024,
        fillPolicy: protos.TraceConfig.BufferConfig.FillPolicy.DISCARD,
      },
      {
        sizeKb: 8 * 1024,
        fillPolicy: protos.TraceConfig.BufferConfig.FillPolicy.RING_BUFFER,
      },
      // Ftrace events can be very verbose, so we put them in a separate buffer to avoid
      // them causing other important data (e.g. meminfo counters) to be dropped.
      {
        sizeKb: 8 * 1024,
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
          name: 'linux.ftrace',
          targetBuffer: 2,
          ftraceConfig: {
            ftraceEvents: ['dmabuf_heap/dma_heap_stat'],
            symbolizeKsyms: true,
            disableGenericEvents: true,
          },
        },
      },
    ],
  };
}

interface LiveMemoryPageAttrs {
  app: App;
}

type ConnectionMethod = 'usb' | 'websocket' | 'web_proxy' | 'linux';

type ProcessMetric = 'rss' | 'anon_swap' | 'file' | 'dmabuf';

const PROCESS_METRIC_OPTIONS: ReadonlyArray<{
  key: ProcessMetric;
  label: string;
  counters: readonly string[];
}> = [
  {key: 'rss', label: 'Total RSS', counters: ['mem.rss']},
  {
    key: 'anon_swap',
    label: 'Anon + Swap',
    counters: ['mem.rss.anon', 'mem.swap'],
  },
  {key: 'file', label: 'File', counters: ['mem.rss.file']},
  {key: 'dmabuf', label: 'DMA-BUF', counters: ['mem.dmabuf_rss']},
];

interface WsDevice {
  serial: string;
  model: string;
}

export class LiveMemoryPage implements m.ClassComponent<LiveMemoryPageAttrs> {
  private adbKeyMgr = new AdbKeyManager();
  private device?: AdbDevice;
  private deviceName?: string;
  private connecting = false;
  private error?: string;
  private connectionMethod: ConnectionMethod = 'usb';

  // Linux traced WebSocket target (direct connection to traced).
  private linuxTarget?: TracedWebsocketTarget;

  // WebSocket bridge state.
  private wsDevices: WsDevice[] = [];
  private wsConnecting = false;
  private wsConnected = false;

  // WDP (Web Device Proxy) state.
  private wdpDevices: WdpDevice[] = [];
  private wdpSocket?: WebSocket;
  private wdpConnecting = false;
  private wdpConnected = false;

  // Tracing session state.
  private sessionName = '';
  private session?: TracingSession;
  private isRecording = false;
  private isPaused = false;
  private cloneTimer?: number;
  private snapshotCount = 0;

  // Heap profile stopping state.
  private heapProfileStopping = false;

  // Heap profile session state.
  private heapProfileSession?: TracingSession;
  private heapProfilePid?: number;
  private heapProfileProcessName?: string;

  // Per-process memory breakdown chart shown during profiling.
  private heapProfileChartData?: LineChartData;

  // Reusable TP instance for processing cloned traces.
  private engine?: WasmEngineProxy;

  // Chart data extracted from the latest cloned trace (full time-series).
  private systemChartData?: LineChartData;
  private pageCacheChartData?: LineChartData;
  private fileCacheBreakdownData?: LineChartData;
  private fileCacheActivityData?: LineChartData;
  private categoryChartData?: LineChartData;
  private sankeyData?: SankeyData;
  private psiChartData?: LineChartData;
  private swapChartData?: LineChartData;
  private vmstatChartData?: LineChartData;
  private pageFaultChartData?: LineChartData;
  // Latest process table (most recent values only).
  private latestProcesses?: ProcessMemoryRow[];

  // Top-level tab selection.
  private activeTab: 'system' | 'file_cache' | 'pressure_swap' | 'processes' =
    'processes';

  // Category drill-down state.
  private selectedCategory?: CategoryId;

  // Process metric selector (which counter to chart in the process tab).
  private processMetric: ProcessMetric = 'rss';
  private processGrouping: ProcessGrouping = 'category';
  private selectedOomBucket?: number;
  private drilldownChartData?: LineChartData;

  // Shared time origin (ns) across all charts for synced x-axes.
  private traceT0?: number;

  // Shared x-axis range (seconds relative to t0) across all charts.
  private xAxisMin?: number;
  private xAxisMax?: number;

  // Last cloned trace buffer for "Stop and open trace".
  private lastTraceBuffer?: ArrayBuffer;
  private app!: App;

  onremove() {
    this.stopSession();
    this.heapProfileSession?.cancel();
    this.disposeEngine();
    this.disconnectWdp();
    this.linuxTarget?.disconnect();
  }

  view({attrs}: m.CVnode<LiveMemoryPageAttrs>) {
    this.app = attrs.app;
    return m('.pf-live-memory-page__container', this.renderPageContent());
  }

  private renderPageContent() {
    if (!this.device && !this.linuxTarget) {
      return this.renderDisconnected();
    }
    if (!this.isRecording) {
      return this.renderConnectedIdle();
    }
    if (this.heapProfilePid !== undefined) {
      return this.renderProfiling();
    }
    return this.renderRecording();
  }

  // ---------------------------------------------------------------------------
  // Disconnected — show connect button.
  // ---------------------------------------------------------------------------

  private renderDisconnected(): m.Children {
    return m(
      '.pf-live-memory-page',
      m('.pf-live-memory-title-bar', m('h1', 'Memento')),
      m(
        '.pf-live-memory-hero',
        m(Icon, {icon: 'memory', className: 'pf-live-memory-hero__icon'}),
        m(
          '.pf-live-memory-hero__text',
          'Connect to an Android device or Linux host to monitor ' +
            'per-process memory usage in real time via traced.',
        ),
        m(SegmentedButtons, {
          options: [
            {label: 'USB', icon: 'usb'},
            {label: 'WebSocket', icon: 'lan'},
            {label: 'Web Proxy', icon: 'corporate_fare'},
            {label: 'Linux', icon: 'computer'},
          ],
          selectedOption: (
            ['usb', 'websocket', 'web_proxy', 'linux'] as const
          ).indexOf(this.connectionMethod),
          onOptionSelected: (i: number) => {
            const methods: ConnectionMethod[] = [
              'usb',
              'websocket',
              'web_proxy',
              'linux',
            ];
            this.connectionMethod = methods[i];
            this.error = undefined;
          },
        }),
        this.connectionMethod === 'usb'
          ? this.renderUsbConnect()
          : this.connectionMethod === 'websocket'
            ? this.renderWsConnect()
            : this.connectionMethod === 'linux'
              ? this.renderLinuxConnect()
              : this.renderWdpConnect(),
        this.error && m('.pf-live-memory-error', this.error),
      ),
    );
  }

  private renderUsbConnect(): m.Children {
    return [
      m(Button, {
        label: this.connecting ? 'Connecting...' : 'Connect USB device',
        icon: 'usb',
        variant: ButtonVariant.Filled,
        intent: Intent.Primary,
        disabled: this.connecting || !exists(navigator.usb),
        onclick: () => this.connectDevice(),
      }),
      !exists(navigator.usb) &&
        m('.pf-live-memory-error', 'WebUSB is not available in this browser.'),
    ];
  }

  private renderWsConnect(): m.Children {
    if (!this.wsConnected) {
      return m(Button, {
        label: this.wsConnecting
          ? 'Connecting...'
          : 'Connect to WebSocket bridge',
        icon: 'lan',
        variant: ButtonVariant.Filled,
        intent: Intent.Primary,
        disabled: this.wsConnecting,
        onclick: () => this.connectWebsocket(),
      });
    }

    if (this.wsDevices.length === 0) {
      return m(
        '.pf-live-memory-hero__text',
        'No devices found. Connect an Android device via ADB.',
      );
    }

    return m(
      '.pf-live-memory-device-list',
      this.wsDevices.map((dev) =>
        m(Button, {
          key: dev.serial,
          label: this.connecting
            ? 'Connecting...'
            : `${dev.model} [${dev.serial}]`,
          icon: 'smartphone',
          variant: ButtonVariant.Outlined,
          disabled: this.connecting,
          onclick: () => this.connectWsDevice(dev),
        }),
      ),
    );
  }

  private renderWdpConnect(): m.Children {
    if (!this.wdpConnected) {
      return m(Button, {
        label: this.wdpConnecting
          ? 'Connecting to proxy...'
          : 'Connect to Web Device Proxy',
        icon: 'corporate_fare',
        variant: ButtonVariant.Filled,
        intent: Intent.Primary,
        disabled: this.wdpConnecting,
        onclick: () => this.connectWdp(),
      });
    }

    if (this.wdpDevices.length === 0) {
      return m(
        '.pf-live-memory-hero__text',
        'No devices found. Connect an Android device and authorize it.',
      );
    }

    return m(
      '.pf-live-memory-device-list',
      this.wdpDevices.map((dev) => {
        const ready = dev.proxyStatus === 'ADB' && dev.adbStatus === 'DEVICE';
        const model =
          dev.proxyStatus === 'ADB' ? dev.adbProps?.model ?? '?' : '?';
        const label = ready
          ? `${model} [${dev.serialNumber}]`
          : `${dev.proxyStatus}/${dev.adbStatus} [${dev.serialNumber}]`;
        return m(Button, {
          key: dev.serialNumber,
          label: this.connecting ? 'Connecting...' : label,
          icon: ready ? 'smartphone' : 'lock',
          variant: ButtonVariant.Outlined,
          disabled: this.connecting,
          onclick: () => this.connectWdpDevice(dev),
        });
      }),
    );
  }

  private renderLinuxConnect(): m.Children {
    return m(Button, {
      label: this.connecting ? 'Connecting...' : 'Connect to local traced',
      icon: 'computer',
      variant: ButtonVariant.Filled,
      intent: Intent.Primary,
      disabled: this.connecting,
      onclick: () => this.connectLinux(),
    });
  }

  private async connectLinux() {
    this.connecting = true;
    this.error = undefined;
    m.redraw();

    const wsUrl = 'ws://127.0.0.1:8037/traced';
    const target = new TracedWebsocketTarget(wsUrl);

    // Run preflight checks to verify connection.
    try {
      for await (const check of target.runPreflightChecks()) {
        if (!check.status.ok) {
          this.error = `${check.name}: ${check.status.error}`;
          this.connecting = false;
          m.redraw();
          return;
        }
      }
    } catch (e) {
      this.error = `Connection failed: ${e}`;
      this.connecting = false;
      m.redraw();
      return;
    }

    this.linuxTarget = target;
    this.deviceName = 'Linux (localhost)';
    this.connecting = false;
    m.redraw();
    this.startSession();
  }

  // ---------------------------------------------------------------------------
  // Connected but not recording — show start button.
  // ---------------------------------------------------------------------------

  private renderConnectedIdle(): m.Children {
    return m(
      '.pf-live-memory-page',
      m('.pf-live-memory-title-bar', m('h1', 'Memory Monitor')),
      m(
        '.pf-live-memory-hero',
        m(Icon, {icon: 'usb', className: 'pf-live-memory-hero__icon'}),
        m('.pf-live-memory-hero__text', `Connected to ${this.deviceName}`),
        this.error && m('.pf-live-memory-error', this.error),
        m(
          '.pf-live-memory-hero__actions',
          m(Button, {
            label: 'Start monitoring',
            icon: 'play_arrow',
            variant: ButtonVariant.Filled,
            intent: Intent.Primary,
            onclick: () => this.startSession(),
          }),
          m(Button, {
            label: 'Disconnect',
            icon: 'usb_off',
            onclick: () => this.disconnect(),
          }),
        ),
      ),
    );
  }

  // ---------------------------------------------------------------------------
  // Profiling — show per-process memory breakdown while heap profiling.
  // ---------------------------------------------------------------------------

  private renderProfiling(): m.Children {
    const processName = this.heapProfileProcessName ?? 'unknown';
    const pid = this.heapProfilePid!;
    return m(
      '.pf-live-memory-page',
      m(
        '.pf-live-memory-title-bar',
        m('h1', `Profiling: ${processName} (PID ${pid})`),
        m(
          '.pf-live-memory-title-bar__actions',
          !this.heapProfileStopping &&
            m(Button, {
              label: 'Stop & Open',
              icon: 'stop',
              variant: ButtonVariant.Filled,
              intent: Intent.Danger,
              onclick: () => this.stopHeapProfile(),
            }),
          !this.heapProfileStopping &&
            m(Button, {
              label: 'Cancel',
              icon: 'close',
              onclick: () => this.cancelHeapProfile(),
            }),
        ),
      ),
      m(
        '.pf-live-memory-status-bar',
        m('.pf-live-memory-status-bar__dot'),
        this.heapProfileStopping
          ? `Stopping and reading trace\u2026`
          : `Recording heap profile`,
        '\u00b7',
        `${this.deviceName}`,
        '\u00b7',
        `${this.snapshotCount} snapshots`,
      ),
      this.error && m('.pf-live-memory-error', this.error),

      panel(
        'Process Memory Breakdown',
        `Stacked area chart of memory usage for ${processName}. ` +
          'Anon + Swap = anonymous resident + swapped pages. ' +
          'File = file-backed resident pages. DMA-BUF = GPU/DMA buffer RSS.',
        this.heapProfileChartData
          ? m(LineChart, {
              data: this.heapProfileChartData,
              height: 350,
              xAxisLabel: 'Time (s)',
              yAxisLabel: 'Memory',
              showLegend: true,
              showPoints: false,
              stacked: true,
              gridLines: 'horizontal',
              formatXValue: (v: number) => `${v.toFixed(0)}s`,
              formatYValue: (v: number) => formatKb(v),
              xAxisMin: this.xAxisMin,
              xAxisMax: this.xAxisMax,
            })
          : m('.pf-live-memory-placeholder', 'Waiting for data\u2026'),
      ),
    );
  }

  // ---------------------------------------------------------------------------
  // Recording — show charts + process table.
  // ---------------------------------------------------------------------------

  private renderRecording(): m.Children {
    return m(
      '.pf-live-memory-page',
      m(
        '.pf-live-memory-title-bar',
        m('h1', 'Memory Monitor'),
        m(
          '.pf-live-memory-title-bar__actions',
          m(Button, {
            label: this.isPaused ? 'Resume' : 'Pause',
            icon: this.isPaused ? 'play_arrow' : 'pause',
            variant: ButtonVariant.Filled,
            onclick: () => this.togglePause(),
          }),
          m(Button, {
            label: 'Stop & Open Trace',
            icon: 'open_in_new',
            variant: ButtonVariant.Filled,
            intent: Intent.Primary,
            disabled: this.lastTraceBuffer === undefined,
            onclick: () => this.stopAndOpenTrace(),
          }),
          m(Button, {
            label: 'Disconnect',
            icon: 'usb_off',
            onclick: () => this.disconnect(),
          }),
        ),
      ),
      m(
        '.pf-live-memory-status-bar',
        m('.pf-live-memory-status-bar__dot', {
          class: this.isPaused ? 'pf-live-memory-status-bar__dot--paused' : '',
        }),
        `${this.deviceName}`,
        '\u00b7',
        `${this.snapshotCount} snapshots`,
      ),
      this.error && m('.pf-live-memory-error', this.error),

      m(
        '.pf-live-memory-tabs',
        m(SegmentedButtons, {
          options: [
            {label: 'Processes', icon: 'apps'},
            {label: 'System', icon: 'monitoring'},
            {label: 'Page Cache', icon: 'file_copy'},
            {label: 'Pressure, Faults & Swap', icon: 'speed'},
          ],
          selectedOption: (
            ['processes', 'system', 'file_cache', 'pressure_swap'] as const
          ).indexOf(this.activeTab),
          onOptionSelected: (i: number) => {
            const tabs = [
              'processes',
              'system',
              'file_cache',
              'pressure_swap',
            ] as const;
            this.activeTab = tabs[i];
          },
        }),
      ),

      this.activeTab === 'processes' && this.renderProcessSection(),
      this.activeTab === 'system' && this.renderSystemMetricsTab(),
      this.activeTab === 'file_cache' && this.renderPageCacheTab(),
      this.activeTab === 'pressure_swap' && this.renderPressureSwapTab(),
    );
  }

  private renderSystemMetricsTab(): m.Children {
    return [
      panel(
        'System Memory Overview',
        'Physical RAM breakdown by category. Source: /proc/meminfo. Unaccounted = MemTotal minus all named categories.',
        this.sankeyData
          ? m(Sankey, {
              data: this.sankeyData,
              height: 350,
              formatValue: (v: number) => formatKb(v),
            })
          : m('.pf-live-memory-placeholder', 'Waiting for data\u2026'),
      ),

      panel(
        'System Memory',
        'Stacked areas partition MemTotal. Source: /proc/meminfo. ' +
          'Anon = Active(anon) + Inactive(anon). Page cache = Active(file) + Inactive(file) \u2212 Shmem. ' +
          'Unaccounted = MemTotal \u2212 sum of all other categories.',
        this.systemChartData
          ? m(LineChart, {
              data: this.systemChartData,
              height: 400,
              xAxisLabel: 'Time (s)',
              yAxisLabel: 'Memory',
              showLegend: true,
              showPoints: false,
              stacked: true,
              gridLines: 'horizontal',
              formatXValue: (v: number) => `${v.toFixed(0)}s`,
              formatYValue: (v: number) => formatKb(v),
              xAxisMin: this.xAxisMin,
              xAxisMax: this.xAxisMax,
            })
          : m('.pf-live-memory-placeholder', 'Waiting for data\u2026'),
      ),
    ];
  }

  private getPageCacheBillboards():
    | {total: number; dirty: number; mapped: number}
    | undefined {
    const data = this.fileCacheBreakdownData;
    if (data === undefined || data.series.length < 4) return undefined;
    // Series order: Mapped+Dirty, Mapped+Clean, Unmapped+Dirty, Unmapped+Clean
    const last = (idx: number) => {
      const pts = data.series[idx].points;
      return pts.length > 0 ? pts[pts.length - 1].y : 0;
    };
    const mappedDirty = last(0);
    const mappedClean = last(1);
    const unmappedDirty = last(2);
    const unmappedClean = last(3);
    return {
      total: mappedDirty + mappedClean + unmappedDirty + unmappedClean,
      dirty: mappedDirty + unmappedDirty,
      mapped: mappedDirty + mappedClean,
    };
  }

  private renderPageCacheTab(): m.Children {
    const billboards = this.getPageCacheBillboards();

    return [
      billboards !== undefined &&
        m(
          '.pf-live-memory-billboards',
          m(
            '.pf-live-memory-billboard',
            m('.pf-live-memory-billboard__value', formatKb(billboards.total)),
            m('.pf-live-memory-billboard__label', 'Total Page Cache'),
            m(
              '.pf-live-memory-billboard__desc',
              'Derived: Active(file) + Inactive(file) from /proc/meminfo',
            ),
          ),
          m(
            '.pf-live-memory-billboard',
            m('.pf-live-memory-billboard__value', formatKb(billboards.dirty)),
            m('.pf-live-memory-billboard__label', 'Dirty'),
            m(
              '.pf-live-memory-billboard__desc',
              'Source: Dirty from /proc/meminfo',
            ),
          ),
          m(
            '.pf-live-memory-billboard',
            m('.pf-live-memory-billboard__value', formatKb(billboards.mapped)),
            m('.pf-live-memory-billboard__label', 'Mapped'),
            m(
              '.pf-live-memory-billboard__desc',
              'Source: Mapped from /proc/meminfo',
            ),
          ),
        ),

      panel(
        'Page Cache',
        'Source: /proc/meminfo counters Active(file), Inactive(file), Shmem. ' +
          'Stacked: Active(file) + Inactive(file) + Shmem \u2248 Cached.',
        this.pageCacheChartData
          ? m(LineChart, {
              data: this.pageCacheChartData,
              height: 250,
              xAxisLabel: 'Time (s)',
              yAxisLabel: 'Cache',
              showLegend: true,
              showPoints: false,
              stacked: true,
              gridLines: 'horizontal',
              formatXValue: (v: number) => `${v.toFixed(0)}s`,
              formatYValue: (v: number) => formatKb(v),
              xAxisMin: this.xAxisMin,
              xAxisMax: this.xAxisMax,
            })
          : m('.pf-live-memory-placeholder', 'Waiting for data\u2026'),
      ),

      panel(
        'Page Cache Activity',
        'Source: /proc/vmstat counters, shown as rates (delta/s). ' +
          'Refaults = workingset_refault_file (evicted pages needed again). ' +
          'Stolen = pgsteal_file (pages reclaimed). ' +
          'Scanned = pgscan_file (pages considered for reclaim).',
        this.fileCacheActivityData
          ? m(LineChart, {
              data: this.fileCacheActivityData,
              height: 200,
              xAxisLabel: 'Time (s)',
              yAxisLabel: 'Pages/s',
              showLegend: true,
              showPoints: false,
              gridLines: 'horizontal',
              formatXValue: (v: number) => `${v.toFixed(0)}s`,
              formatYValue: (v: number) => v.toLocaleString(),
              xAxisMin: this.xAxisMin,
              xAxisMax: this.xAxisMax,
            })
          : m('.pf-live-memory-placeholder', 'Waiting for data\u2026'),
      ),
    ];
  }

  private renderPressureSwapTab(): m.Children {
    return [
      panel(
        'Memory Pressure (PSI)',
        'Source: /proc/pressure/memory (psi.mem.some, psi.mem.full). ' +
          'Derived: cumulative \u00b5s converted to ms/s rate. ' +
          '"some" = at least one task stalled, "full" = all tasks stalled.',
        this.psiChartData
          ? m(LineChart, {
              data: this.psiChartData,
              height: 200,
              xAxisLabel: 'Time (s)',
              yAxisLabel: 'Stall (ms/s)',
              showLegend: true,
              showPoints: false,
              gridLines: 'horizontal',
              formatXValue: (v: number) => `${v.toFixed(0)}s`,
              formatYValue: (v: number) => `${v.toFixed(1)} ms/s`,
              xAxisMin: this.xAxisMin,
              xAxisMax: this.xAxisMax,
            })
          : m('.pf-live-memory-placeholder', 'Waiting for data\u2026'),
      ),

      panel(
        'Page Faults',
        'Source: /proc/vmstat counters pgfault, pgmajfault. ' +
          'Derived: cumulative counts converted to faults/s rate. ' +
          'Minor (pgfault) = page in RAM but not in TLB. Major (pgmajfault) = page must be read from disk.',
        this.pageFaultChartData
          ? m(LineChart, {
              data: this.pageFaultChartData,
              height: 200,
              xAxisLabel: 'Time (s)',
              yAxisLabel: 'Faults/s',
              showLegend: true,
              showPoints: false,
              gridLines: 'horizontal',
              formatXValue: (v: number) => `${v.toFixed(0)}s`,
              formatYValue: (v: number) => `${v.toFixed(0)} f/s`,
              xAxisMin: this.xAxisMin,
              xAxisMax: this.xAxisMax,
            })
          : m('.pf-live-memory-placeholder', 'Waiting for data\u2026'),
      ),

      this.swapChartData &&
        panel(
          'Swap Usage',
          'Source: /proc/meminfo counters SwapTotal, SwapFree, SwapCached. ' +
            'Derived: Swap dirty = (SwapTotal \u2212 SwapFree) \u2212 SwapCached.',
          m(LineChart, {
            data: this.swapChartData,
            height: 200,
            xAxisLabel: 'Time (s)',
            yAxisLabel: 'Swap',
            showLegend: true,
            showPoints: false,
            stacked: true,
            gridLines: 'horizontal',
            formatXValue: (v: number) => `${v.toFixed(0)}s`,
            formatYValue: (v: number) => formatKb(v),
            xAxisMin: this.xAxisMin,
            xAxisMax: this.xAxisMax,
          }),
        ),

      this.vmstatChartData &&
        panel(
          'Swap I/O (pswpin / pswpout)',
          'Source: /proc/vmstat counters pswpin, pswpout. ' +
            'Derived: cumulative page counts converted to pages/s rate.',
          m(LineChart, {
            data: this.vmstatChartData,
            height: 200,
            xAxisLabel: 'Time (s)',
            yAxisLabel: 'Pages/s',
            showLegend: true,
            showPoints: false,
            gridLines: 'horizontal',
            formatXValue: (v: number) => `${v.toFixed(0)}s`,
            formatYValue: (v: number) => `${v.toFixed(0)} pg/s`,
            xAxisMin: this.xAxisMin,
            xAxisMax: this.xAxisMax,
          }),
        ),
    ];
  }

  private computeSharedXRange() {
    let min = Infinity;
    let max = -Infinity;
    const allData: Array<LineChartData | undefined> = [
      this.systemChartData,
      this.pageCacheChartData,
      this.fileCacheBreakdownData,
      this.fileCacheActivityData,
      this.categoryChartData,
      this.psiChartData,
      this.swapChartData,
      this.vmstatChartData,
      this.pageFaultChartData,
      this.drilldownChartData,
      this.heapProfileChartData,
    ];
    for (const data of allData) {
      if (data === undefined) continue;
      for (const s of data.series) {
        for (const p of s.points) {
          if (p.x < min) min = p.x;
          if (p.x > max) max = p.x;
        }
      }
    }
    if (min < max) {
      this.xAxisMin = min;
      this.xAxisMax = max;
    }
  }

  private renderProcessSection(): m.Children {
    const isDrilledDown =
      this.processGrouping === 'category'
        ? this.selectedCategory !== undefined
        : this.selectedOomBucket !== undefined;

    const cat = this.selectedCategory
      ? CATEGORIES[this.selectedCategory]
      : undefined;
    const oomBucket =
      this.selectedOomBucket !== undefined
        ? OOM_SCORE_BUCKETS[this.selectedOomBucket]
        : undefined;

    const chartData = isDrilledDown
      ? this.drilldownChartData
      : this.categoryChartData;

    const processes = this.latestProcesses
      ? this.processGrouping === 'category' && this.selectedCategory
        ? this.latestProcesses.filter(
            (p) => categorizeProcess(p.processName).name === cat!.name,
          )
        : this.processGrouping === 'oom_score' && oomBucket
          ? this.latestProcesses.filter(
              (p) =>
                p.oomScore >= oomBucket.minScore &&
                p.oomScore <= oomBucket.maxScore,
            )
          : this.latestProcesses
      : undefined;

    const metricInfo = PROCESS_METRIC_OPTIONS.find(
      (o) => o.key === this.processMetric,
    )!;
    const drillName = cat?.name ?? oomBucket?.name;
    const groupLabel =
      this.processGrouping === 'category' ? 'Category' : 'OOM Score';
    const title = drillName
      ? `Process Memory: ${drillName}`
      : `Process Memory by ${groupLabel}`;
    const subtitle = drillName
      ? `Stacked ${metricInfo.label} per process. Totals may exceed actual memory usage because RSS counts shared pages (COW, mmap) in every process that maps them.`
      : `Stacked ${metricInfo.label} per ${groupLabel.toLowerCase()}. Click a ${groupLabel.toLowerCase()} to drill into individual processes.`;

    return m(
      '.pf-live-memory-panel',
      m(
        '.pf-live-memory-panel__header',
        isDrilledDown &&
          m(Button, {
            icon: 'arrow_back',
            label:
              this.processGrouping === 'category'
                ? 'All categories'
                : 'All OOM buckets',
            minimal: true,
            onclick: () => {
              this.selectedCategory = undefined;
              this.selectedOomBucket = undefined;
              this.drilldownChartData = undefined;
            },
          }),
        m('h2', title),
        m('p', subtitle),
        m(SegmentedButtons, {
          options: [{label: 'By Category'}, {label: 'By OOM Score'}],
          selectedOption: this.processGrouping === 'category' ? 0 : 1,
          onOptionSelected: (i: number) => {
            const newGrouping: ProcessGrouping =
              i === 0 ? 'category' : 'oom_score';
            if (newGrouping === this.processGrouping) return;
            this.processGrouping = newGrouping;
            this.selectedCategory = undefined;
            this.selectedOomBucket = undefined;
            this.categoryChartData = undefined;
            this.drilldownChartData = undefined;
            if (this.engine) {
              this.requeryGroupingChart();
            }
          },
        }),
        m(SegmentedButtons, {
          options: PROCESS_METRIC_OPTIONS.map((o) => ({label: o.label})),
          selectedOption: PROCESS_METRIC_OPTIONS.findIndex(
            (o) => o.key === this.processMetric,
          ),
          onOptionSelected: (i: number) => {
            const newMetric = PROCESS_METRIC_OPTIONS[i].key;
            if (newMetric === this.processMetric) return;
            this.processMetric = newMetric;
            this.categoryChartData = undefined;
            this.drilldownChartData = undefined;
            if (this.engine) {
              this.requeryGroupingChart();
            }
          },
        }),
      ),
      m(
        '.pf-live-memory-panel__body',
        chartData
          ? m(LineChart, {
              data: chartData,
              height: 350,
              xAxisLabel: 'Time (s)',
              yAxisLabel: 'RSS',
              showLegend: true,
              showPoints: false,
              stacked: true,
              gridLines: 'horizontal',
              formatXValue: (v: number) => `${v.toFixed(0)}s`,
              formatYValue: (v: number) => formatKb(v),
              xAxisMin: this.xAxisMin,
              xAxisMax: this.xAxisMax,
              onSeriesClick: isDrilledDown
                ? undefined
                : (seriesName: string) => {
                    if (this.processGrouping === 'category') {
                      this.drillDownToCategory(seriesName);
                    } else {
                      this.drillDownToOomBucket(seriesName);
                    }
                  },
            })
          : m('.pf-live-memory-placeholder', 'Waiting for data\u2026'),
        processes && this.renderProcessTable(processes),
      ),
    );
  }

  private activeProcessCounters(): readonly string[] {
    return PROCESS_METRIC_OPTIONS.find((o) => o.key === this.processMetric)!
      .counters;
  }

  private requeryGroupingChart() {
    if (!this.engine) return;
    const t0 = this.traceT0 ?? 0;
    const counter = this.activeProcessCounters();
    const groupingPromise =
      this.processGrouping === 'category'
        ? this.queryCategoryTimeSeries(this.engine, t0, counter)
        : this.queryOomScoreTimeSeries(this.engine, t0, counter);
    groupingPromise.then((data) => {
      this.categoryChartData = data;
      m.redraw();
    });
    if (this.processGrouping === 'category' && this.selectedCategory) {
      this.queryDrilldownTimeSeries(
        this.engine,
        this.selectedCategory,
        t0,
        counter,
      ).then((data) => {
        this.drilldownChartData = data;
        m.redraw();
      });
    } else if (
      this.processGrouping === 'oom_score' &&
      this.selectedOomBucket !== undefined
    ) {
      this.queryOomDrilldownTimeSeries(
        this.engine,
        this.selectedOomBucket,
        t0,
        counter,
      ).then((data) => {
        this.drilldownChartData = data;
        m.redraw();
      });
    }
  }

  private drillDownToCategory(seriesName: string) {
    const catIds = Object.keys(CATEGORIES) as CategoryId[];
    const id = catIds.find((k) => CATEGORIES[k].name === seriesName);
    if (id === undefined) return;
    this.selectedCategory = id;
    this.drilldownChartData = undefined;
    if (this.engine) {
      const counter = this.activeProcessCounters();
      this.queryDrilldownTimeSeries(
        this.engine,
        id,
        this.traceT0 ?? 0,
        counter,
      ).then((data) => {
        this.drilldownChartData = data;
        m.redraw();
      });
    }
  }

  private drillDownToOomBucket(seriesName: string) {
    const idx = OOM_SCORE_BUCKETS.findIndex((b) => b.name === seriesName);
    if (idx === -1) return;
    this.selectedOomBucket = idx;
    this.drilldownChartData = undefined;
    if (this.engine) {
      const counter = this.activeProcessCounters();
      this.queryOomDrilldownTimeSeries(
        this.engine,
        idx,
        this.traceT0 ?? 0,
        counter,
      ).then((data) => {
        this.drilldownChartData = data;
        m.redraw();
      });
    }
  }

  // ---------------------------------------------------------------------------
  // Process table
  // ---------------------------------------------------------------------------

  private renderProcessTable(processes: ProcessMemoryRow[]): m.Children {
    const rows: Row[] = processes.map((p) => {
      const cat = categorizeProcess(p.processName);
      return {
        process: p.processName,
        category: cat.name,
        pid: p.pid,
        oom_score: p.oomScore,
        rss_kb: p.rssKb,
        anon_swap_kb: p.anonKb + p.swapKb,
        file_kb: p.fileKb,
        shmem_kb: p.shmemKb,
        age: p.ageSeconds,
        actions: '',
      };
    });

    const schema: SchemaRegistry = {
      process: {
        ...PROCESS_TABLE_SCHEMA.process,
        actions: {
          title: '',
          columnType: 'text' as const,
          cellRenderer: (_v, row) => {
            const pid = row.pid as number;
            const isProfiling = this.heapProfilePid === pid;
            return m(Button, {
              label: isProfiling ? 'Stop' : 'Profile',
              icon: isProfiling ? 'stop' : 'science',
              minimal: true,
              className: isProfiling ? '' : 'pf-live-memory-profile-btn',
              onclick: (e: Event) => {
                e.stopPropagation();
                if (isProfiling) {
                  this.stopHeapProfile();
                } else {
                  this.startHeapProfile(pid, row.process as string);
                }
              },
            });
          },
        },
      },
    };

    return [
      this.heapProfilePid !== undefined &&
        m(
          '.pf-live-memory-status-bar',
          m('.pf-live-memory-status-bar__dot'),
          this.heapProfileStopping
            ? `Stopping and reading trace for ${this.heapProfileProcessName}\u2026`
            : `Recording heap profile for ${this.heapProfileProcessName} (PID ${this.heapProfilePid})`,
          !this.heapProfileStopping && [
            m(Button, {
              label: 'Stop & Open',
              icon: 'stop',
              minimal: true,
              intent: Intent.Danger,
              onclick: () => this.stopHeapProfile(),
            }),
            m(Button, {
              label: 'Cancel',
              icon: 'close',
              minimal: true,
              onclick: () => this.cancelHeapProfile(),
            }),
          ],
        ),
      m(DataGrid, {
        schema,
        rootSchema: 'process',
        data: rows,
        initialColumns: [
          {id: 'actions', field: 'actions', sort: undefined},
          {id: 'process', field: 'process', sort: undefined},
          {id: 'category', field: 'category', sort: undefined},
          {id: 'pid', field: 'pid', sort: undefined},
          {id: 'oom_score', field: 'oom_score', sort: undefined},
          {id: 'rss_kb', field: 'rss_kb', sort: 'DESC'},
          {id: 'anon_swap_kb', field: 'anon_swap_kb', sort: undefined},
          {id: 'file_kb', field: 'file_kb', sort: undefined},
          {id: 'shmem_kb', field: 'shmem_kb', sort: undefined},
          {id: 'age', field: 'age', sort: undefined},
        ],
        fillHeight: false,
      }),
    ];
  }

  // ---------------------------------------------------------------------------
  // Heap profiling
  // ---------------------------------------------------------------------------

  private async startHeapProfile(pid: number, processName: string) {
    if (!this.device || this.heapProfileSession) return;

    this.heapProfilePid = pid;
    this.heapProfileProcessName = processName;
    m.redraw();

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

    const result = await createAdbTracingSession(this.device, config);
    if (!result.ok) {
      this.error = `Heap profile failed: ${result.error}`;
      this.heapProfilePid = undefined;
      this.heapProfileProcessName = undefined;
      m.redraw();
      return;
    }

    this.heapProfileSession = result.value;
    this.heapProfileSession.onSessionUpdate.addListener(() => {
      if (this.heapProfileSession?.state === 'FINISHED') {
        this.onHeapProfileFinished();
      }
      m.redraw();
    });
    m.redraw();
  }

  private async stopHeapProfile() {
    if (!this.heapProfileSession) return;
    this.heapProfileStopping = true;
    m.redraw();
    await this.heapProfileSession.stop();
  }

  private cancelHeapProfile() {
    if (!this.heapProfileSession) return;
    this.heapProfileSession.cancel();
    this.heapProfileSession = undefined;
    this.heapProfilePid = undefined;
    this.heapProfileProcessName = undefined;
    this.heapProfileStopping = false;
    this.heapProfileChartData = undefined;
    m.redraw();
  }

  private onHeapProfileFinished() {
    const traceData = this.heapProfileSession?.getTraceData();
    const processName = this.heapProfileProcessName ?? 'unknown';
    this.heapProfileSession = undefined;
    this.heapProfilePid = undefined;
    this.heapProfileProcessName = undefined;
    this.heapProfileStopping = false;
    this.heapProfileChartData = undefined;

    if (traceData !== undefined) {
      const fileName = `heap-${processName}-${Date.now()}.perfetto-trace`;
      this.app.openTraceFromBuffer({
        buffer: traceData.buffer as ArrayBuffer,
        title: fileName,
        fileName,
      });
    }
    m.redraw();
  }

  // ---------------------------------------------------------------------------
  // USB connection
  // ---------------------------------------------------------------------------

  private async connectDevice() {
    this.connecting = true;
    this.error = undefined;
    m.redraw();

    try {
      const usbdev = await navigator.usb.requestDevice({
        filters: [ADB_DEVICE_FILTER],
      });

      const usbiface = getAdbWebUsbInterface(usbdev);
      if (!usbiface) {
        this.error = 'Could not find ADB interface on selected device.';
        this.connecting = false;
        m.redraw();
        return;
      }

      const result = await AdbWebusbDevice.connect(usbdev, this.adbKeyMgr);
      if (!result.ok) {
        this.error = result.error;
        this.connecting = false;
        m.redraw();
        return;
      }

      this.device = result.value;
      this.deviceName = `${usbdev.productName} [${usbdev.serialNumber}]`;
      this.connecting = false;
      m.redraw();
      this.startSession();
    } catch (e) {
      if (`${(e as {name?: string}).name}` === 'NotFoundError') {
        this.connecting = false;
        m.redraw();
        return;
      }
      this.error = `Connection failed: ${e}`;
      this.connecting = false;
      m.redraw();
    }
  }

  private disconnect() {
    this.stopSession();
    this.device?.close();
    this.device = undefined;
    this.linuxTarget?.disconnect();
    this.linuxTarget = undefined;
    this.deviceName = undefined;
    this.error = undefined;
    this.wsDevices = [];
    this.wsConnected = false;
    this.disconnectWdp();
  }

  // ---------------------------------------------------------------------------
  // WebSocket bridge connection
  // ---------------------------------------------------------------------------

  private async connectWebsocket() {
    this.wsConnecting = true;
    this.error = undefined;
    this.wsDevices = [];
    m.redraw();

    const wsUrl = 'ws://127.0.0.1:8037/adb';
    using sock = await AsyncWebsocket.connect(wsUrl);
    if (!sock) {
      this.error =
        'Failed to connect to websocket_bridge at ' +
        wsUrl +
        '. Make sure websocket_bridge is running.';
      this.wsConnecting = false;
      m.redraw();
      return;
    }

    const status = await adbCmdAndWait(sock, 'host:devices-l', true);
    if (!status.ok) {
      this.error = `Failed to list devices: ${status.error}`;
      this.wsConnecting = false;
      m.redraw();
      return;
    }

    const devices: WsDevice[] = [];
    for (const line of status.value.trimEnd().split('\n')) {
      if (line === '') continue;
      const match = line.match(/^([^\s]+)\s+.*model:([^ ]+)/);
      if (!match) continue;
      devices.push({serial: match[1], model: match[2]});
    }

    this.wsDevices = devices;
    this.wsConnected = true;
    this.wsConnecting = false;
    m.redraw();
  }

  private async connectWsDevice(dev: WsDevice) {
    this.connecting = true;
    this.error = undefined;
    m.redraw();

    try {
      const wsUrl = 'ws://127.0.0.1:8037/adb';
      const result = await AdbWebsocketDevice.connect(
        wsUrl,
        dev.serial,
        'WEBSOCKET_BRIDGE',
      );
      if (!result.ok) {
        this.error = result.error;
        this.connecting = false;
        m.redraw();
        return;
      }

      this.device = result.value;
      this.deviceName = `${dev.model} [${dev.serial}]`;
      this.connecting = false;
      m.redraw();
      this.startSession();
    } catch (e) {
      this.error = `WebSocket connection failed: ${e}`;
      this.connecting = false;
      m.redraw();
    }
  }

  // ---------------------------------------------------------------------------
  // Web Device Proxy connection
  // ---------------------------------------------------------------------------

  private async connectWdp() {
    this.wdpConnecting = true;
    this.error = undefined;
    this.wdpDevices = [];
    m.redraw();

    const wsUrl = 'ws://127.0.0.1:9167/track-devices-json';

    for (let attempt = 0; attempt < 2; attempt++) {
      const aws = await AsyncWebsocket.connect(wsUrl);
      if (aws === undefined) {
        this.error =
          'Failed to connect to Web Device Proxy. ' +
          'Make sure it is running (see go/web-device-proxy).';
        this.wdpConnecting = false;
        m.redraw();
        return;
      }

      const respStr = await aws.waitForString();
      const respJson = JSON.parse(respStr);
      const respSchema = WDP_TRACK_DEVICES_SCHEMA.safeParse(respJson);
      if (!respSchema.success) {
        this.error = `Invalid WDP response: ${respSchema.error}`;
        this.wdpConnecting = false;
        m.redraw();
        return;
      }
      const resp = respSchema.data;

      if (
        resp.error?.type === 'ORIGIN_NOT_ALLOWLISTED' &&
        resp.error.approveUrl !== undefined
      ) {
        const popup = await showPopupWindow({url: resp.error.approveUrl});
        if (popup === false) {
          this.error = 'You need to enable popups and try again.';
          this.wdpConnecting = false;
          m.redraw();
          return;
        }
        continue; // Retry after user approved.
      } else if (resp.error !== undefined) {
        this.error = resp.error.message ?? 'Unknown WDP error';
        this.wdpConnecting = false;
        m.redraw();
        return;
      }

      // Success — listen for device updates.
      const ws = aws.release();
      this.wdpSocket = ws;
      this.wdpConnected = true;
      this.wdpConnecting = false;
      this.wdpDevices = resp.device ?? [];

      ws.onmessage = (e: MessageEvent<string>) => {
        const parsed = WDP_TRACK_DEVICES_SCHEMA.safeParse(JSON.parse(e.data));
        if (parsed.success && parsed.data.error === undefined) {
          this.wdpDevices = parsed.data.device ?? [];
        }
        m.redraw();
      };
      ws.onclose = () => {
        this.wdpConnected = false;
        this.wdpSocket = undefined;
        m.redraw();
      };
      ws.onerror = () => {
        this.wdpConnected = false;
        this.wdpSocket = undefined;
        m.redraw();
      };

      m.redraw();
      return;
    }

    this.error =
      'Failed to authenticate with WDP. ' +
      'Click allow on the popup and try again.';
    this.wdpConnecting = false;
    m.redraw();
  }

  private async connectWdpDevice(dev: WdpDevice) {
    this.connecting = true;
    this.error = undefined;
    m.redraw();

    try {
      if (dev.proxyStatus === 'PROXY_UNAUTHORIZED') {
        const res = await showPopupWindow({url: dev.approveUrl});
        if (!res) {
          this.error = 'Enable popups and try again.';
          this.connecting = false;
          m.redraw();
          return;
        }
      }

      if (dev.proxyStatus !== 'ADB' || dev.adbStatus !== 'DEVICE') {
        this.error =
          `Device not ready: proxyStatus=${dev.proxyStatus}` +
          ` adbStatus=${dev.adbStatus}`;
        this.connecting = false;
        m.redraw();
        return;
      }

      const wsUrl = 'ws://127.0.0.1:9167/adb-json';
      const result = await AdbWebsocketDevice.connect(
        wsUrl,
        dev.serialNumber,
        'WEB_DEVICE_PROXY',
      );
      if (!result.ok) {
        this.error = result.error;
        this.connecting = false;
        m.redraw();
        return;
      }

      this.device = result.value;
      const model =
        dev.proxyStatus === 'ADB' ? dev.adbProps?.model ?? '?' : '?';
      this.deviceName = `${model} [${dev.serialNumber}]`;
      this.connecting = false;
      m.redraw();
      this.startSession();
    } catch (e) {
      this.error = `WDP connection failed: ${e}`;
      this.connecting = false;
      m.redraw();
    }
  }

  private disconnectWdp() {
    if (this.wdpSocket) {
      this.wdpSocket.close();
      this.wdpSocket = undefined;
    }
    this.wdpConnected = false;
    this.wdpDevices = [];
  }

  // ---------------------------------------------------------------------------
  // Tracing session lifecycle
  // ---------------------------------------------------------------------------

  private async startSession() {
    if (!this.device && !this.linuxTarget) return;

    this.error = undefined;
    this.snapshotCount = 0;
    this.systemChartData = undefined;
    this.pageCacheChartData = undefined;
    this.fileCacheBreakdownData = undefined;
    this.fileCacheActivityData = undefined;
    this.categoryChartData = undefined;
    this.sankeyData = undefined;
    this.psiChartData = undefined;
    this.swapChartData = undefined;
    this.vmstatChartData = undefined;
    this.latestProcesses = undefined;
    this.selectedCategory = undefined;
    this.selectedOomBucket = undefined;
    this.drilldownChartData = undefined;
    this.sessionName = `livemem-${uuidv4().substring(0, 8)}`;

    const config = createMonitoringConfig(this.sessionName);
    const result = this.linuxTarget
      ? await this.linuxTarget.startTracing(config)
      : await createAdbTracingSession(this.device!, config);
    if (!result.ok) {
      this.error = `Failed to start tracing: ${result.error}`;
      m.redraw();
      return;
    }

    this.session = result.value;
    this.isRecording = true;

    this.session.onSessionUpdate.addListener(() => {
      if (this.session?.state === 'ERRORED') {
        this.error = 'Tracing session errored';
        console.error('[LiveMem] session ERRORED, stopping clone timer');
        this.stopCloneTimer();
        this.isRecording = false;
      }
      m.redraw();
    });

    // First clone after a short delay, then chain subsequent clones.
    this.cloneTimer = window.setTimeout(() => {
      this.cloneAndQuery();
    }, CLONE_INITIAL_DELAY_MS);

    m.redraw();
  }

  private async stopAndOpenTrace() {
    const buffer = this.lastTraceBuffer;
    if (buffer === undefined) return;
    const fileName = `live-memory-${Date.now()}.perfetto-trace`;
    await this.stopSession();
    this.app.openTraceFromBuffer({buffer, title: fileName, fileName});
  }

  private async stopSession() {
    this.stopCloneTimer();
    if (this.session) {
      await this.session.stop();
      this.session = undefined;
    }
    this.disposeEngine();
    this.isRecording = false;
    this.isPaused = false;
    this.snapshotCount = 0;
    this.systemChartData = undefined;
    this.pageCacheChartData = undefined;
    this.fileCacheBreakdownData = undefined;
    this.fileCacheActivityData = undefined;
    this.categoryChartData = undefined;
    this.sankeyData = undefined;
    this.psiChartData = undefined;
    this.swapChartData = undefined;
    this.vmstatChartData = undefined;
    this.latestProcesses = undefined;
    this.selectedCategory = undefined;
    this.selectedOomBucket = undefined;
    this.drilldownChartData = undefined;
    this.lastTraceBuffer = undefined;
    this.traceT0 = undefined;
    m.redraw();
  }

  private togglePause() {
    this.isPaused = !this.isPaused;
    if (!this.isPaused) {
      // Resume: kick off the next clone immediately.
      this.cloneTimer = window.setTimeout(() => {
        this.cloneAndQuery();
      }, 0);
    } else {
      // Pause: stop the pending clone timer.
      this.stopCloneTimer();
    }
    m.redraw();
  }

  private stopCloneTimer() {
    if (this.cloneTimer !== undefined) {
      window.clearTimeout(this.cloneTimer);
      this.cloneTimer = undefined;
    }
  }

  // ---------------------------------------------------------------------------
  // Clone + TraceProcessor query loop
  // ---------------------------------------------------------------------------

  private getOrCreateEngine(): WasmEngineProxy {
    if (this.engine === undefined) {
      this.engine = new WasmEngineProxy(`livemem-${++engineCounter}`);
    }
    return this.engine;
  }

  private disposeEngine() {
    if (this.engine !== undefined) {
      this.engine[Symbol.dispose]();
      this.engine = undefined;
    }
  }

  private async cloneAndQuery(): Promise<void> {
    const snapNum = this.snapshotCount + 1;
    if (!this.isRecording || (!this.device && !this.linuxTarget)) {
      return;
    }

    try {
      // Clone the running session by name.
      const cloneResult = this.linuxTarget
        ? await this.linuxTarget.cloneSession(this.sessionName)
        : await cloneAdbTracingSession(this.device!, this.sessionName);
      if (!cloneResult.ok) {
        this.error = `Clone failed: ${cloneResult.error}`;
        console.error(
          `[LiveMem] #${snapNum} clone failed: ${cloneResult.error}`,
        );
        return;
      }
      this.lastTraceBuffer = cloneResult.value.buffer as ArrayBuffer;

      // Reset the reusable TP instance and load the cloned trace.
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

      // Compute shared time origin on first snapshot, reuse for all subsequent
      // snapshots so x-axes stay synced across charts.
      if (this.traceT0 === undefined) {
        const t0Result = await engine.query(
          'SELECT MIN(ts) AS t0 FROM counter',
        );
        const t0Iter = t0Result.iter({t0: NUM});
        if (t0Iter.valid()) {
          this.traceT0 = t0Iter.t0;
        }
      }
      const t0 = this.traceT0 ?? 0;

      // Query full time-series from the ring buffer and replace chart data.
      const [
        systemData,
        pageCacheData,
        fileCacheBreakdownData,
        fileCacheActivityData,
        categoryData,
        sankeyData,
        psiData,
        swapData,
        vmstatData,
        pageFaultData,
        drilldownData,
        latestProcesses,
        profileChartData,
      ] = await Promise.all([
        this.querySystemTimeSeries(engine, t0),
        this.queryPageCacheTimeSeries(engine, t0),
        this.queryFileCacheBreakdownTimeSeries(engine, t0),
        this.queryFileCacheActivityTimeSeries(engine, t0),
        this.processGrouping === 'category'
          ? this.queryCategoryTimeSeries(
              engine,
              t0,
              this.activeProcessCounters(),
            )
          : this.queryOomScoreTimeSeries(
              engine,
              t0,
              this.activeProcessCounters(),
            ),
        this.querySankeyData(engine),
        this.queryPsiTimeSeries(engine, t0),
        this.querySwapTimeSeries(engine, t0),
        this.queryVmstatTimeSeries(engine, t0),
        this.queryPageFaultTimeSeries(engine, t0),
        this.selectedCategory
          ? this.queryDrilldownTimeSeries(
              engine,
              this.selectedCategory,
              t0,
              this.activeProcessCounters(),
            )
          : this.selectedOomBucket !== undefined
            ? this.queryOomDrilldownTimeSeries(
                engine,
                this.selectedOomBucket,
                t0,
                this.activeProcessCounters(),
              )
            : Promise.resolve(undefined),
        this.queryLatestProcessMemory(engine),
        this.heapProfilePid !== undefined
          ? this.queryProcessMemoryBreakdown(engine, this.heapProfilePid, t0)
          : Promise.resolve(undefined),
      ]);

      this.systemChartData = systemData;
      this.pageCacheChartData = pageCacheData;
      this.fileCacheBreakdownData = fileCacheBreakdownData;
      this.fileCacheActivityData = fileCacheActivityData;
      this.categoryChartData = categoryData;
      this.sankeyData = sankeyData;
      this.psiChartData = psiData;
      this.swapChartData = swapData;
      this.vmstatChartData = vmstatData;
      this.pageFaultChartData = pageFaultData;

      // Compute shared x-axis range across all line chart data.
      this.computeSharedXRange();

      if (this.selectedCategory || this.selectedOomBucket !== undefined) {
        this.drilldownChartData = drilldownData;
      }
      this.latestProcesses = latestProcesses;
      this.heapProfileChartData = profileChartData;
      this.snapshotCount++;
      this.error = undefined;
    } catch (e) {
      this.error = `Snapshot error: ${e}`;
    } finally {
      // Schedule next clone after this one finishes.
      if (this.isRecording) {
        this.cloneTimer = window.setTimeout(() => {
          this.cloneAndQuery();
        }, CLONE_INTERVAL_MS);
      }
      m.redraw();
    }
  }

  // ---------------------------------------------------------------------------
  // System memory: full time-series from TP, decomposed for stacking.
  // ---------------------------------------------------------------------------

  private async querySystemTimeSeries(
    engine: WasmEngineProxy,
    t0: number,
  ): Promise<LineChartData | undefined> {
    const names = SYSTEM_MEMINFO_COUNTERS.map((n) => `'${n}'`).join(',');
    const queryResult = await engine.query(`
      SELECT
        t.name AS counter_name,
        c.ts AS ts,
        c.value AS value_bytes
      FROM counter c
      JOIN counter_track t ON c.track_id = t.id
      WHERE t.name IN (${names})
      ORDER BY c.ts
    `);

    // Group meminfo values by timestamp. All meminfo counters share the
    // same poll interval so they arrive at the same ts.
    // TP stores meminfo values in bytes; convert to KB here.
    const meminfoByTs = new Map<number, Map<string, number>>();
    const iter = queryResult.iter({
      counter_name: STR,
      ts: NUM,
      value_bytes: NUM,
    });
    for (; iter.valid(); iter.next()) {
      let row = meminfoByTs.get(iter.ts);
      if (row === undefined) {
        row = new Map();
        meminfoByTs.set(iter.ts, row);
      }
      row.set(iter.counter_name, Math.round(iter.value_bytes / 1024));
    }

    if (meminfoByTs.size < 2) return undefined;

    // Query DMA-BUF heap total separately (event-driven timestamps).
    const dmaResult = await engine.query(`
      SELECT c.ts AS ts, c.value AS value_bytes
      FROM counter c
      JOIN counter_track t ON c.track_id = t.id
      WHERE t.name = 'mem.dma_heap'
      ORDER BY c.ts
    `);
    const dmaSamples: {ts: number; kb: number}[] = [];
    const dmaIter = dmaResult.iter({ts: NUM, value_bytes: NUM});
    for (; dmaIter.valid(); dmaIter.next()) {
      dmaSamples.push({
        ts: dmaIter.ts,
        kb: Math.round(dmaIter.value_bytes / 1024),
      });
    }
    const hasDmaHeap = dmaSamples.length > 0;

    // Union all timestamps from meminfo and DMA-BUF, then sample-and-hold
    // both sources across the combined timeline.
    const allTs = new Set<number>(meminfoByTs.keys());
    for (const s of dmaSamples) {
      allTs.add(s.ts);
    }
    const timestamps = [...allTs].sort((a, b) => a - b);

    // Partition MemTotal into non-overlapping bands:
    //   Anon + FileCache + Shmem + Buffers + Slab + PageTables +
    //   KernelStack + Zram + DMA-BUF + MemFree + Unaccounted = MemTotal
    // where Anon = Active(anon)+Inactive(anon),
    //       FileCache = Active(file)+Inactive(file) - Shmem.
    // Both meminfo and DMA-BUF are forward-filled (sample-and-hold) across
    // the union of timestamps so every point has values for all counters.
    const anonPts: {x: number; y: number}[] = [];
    const filePts: {x: number; y: number}[] = [];
    const shmemPts: {x: number; y: number}[] = [];
    const buffersPts: {x: number; y: number}[] = [];
    const slabPts: {x: number; y: number}[] = [];
    const pageTablesPts: {x: number; y: number}[] = [];
    const kernelStackPts: {x: number; y: number}[] = [];
    const zramPts: {x: number; y: number}[] = [];
    const dmaHeapPts: {x: number; y: number}[] = [];
    const freePts: {x: number; y: number}[] = [];
    const unaccountedPts: {x: number; y: number}[] = [];

    let lastMeminfo: Map<string, number> | undefined;
    let dmaIdx = 0;
    let lastDmaKb = 0;

    for (const ts of timestamps) {
      // Sample-and-hold: use this ts's meminfo if available, else last known.
      const meminfo = meminfoByTs.get(ts) ?? lastMeminfo;
      if (meminfo !== undefined) {
        lastMeminfo = meminfo;
      }

      // Advance DMA-BUF sample-and-hold cursor.
      while (dmaIdx < dmaSamples.length && dmaSamples[dmaIdx].ts <= ts) {
        lastDmaKb = dmaSamples[dmaIdx].kb;
        dmaIdx++;
      }

      // Skip timestamps before we have meminfo data.
      if (meminfo === undefined) continue;

      const total = meminfo.get('MemTotal');
      const free = meminfo.get('MemFree');
      if (total === undefined || free === undefined) continue;

      const x = (ts - t0) / 1e9;
      const anon =
        (meminfo.get('Active(anon)') ?? 0) +
        (meminfo.get('Inactive(anon)') ?? 0);
      const fileLru =
        (meminfo.get('Active(file)') ?? 0) +
        (meminfo.get('Inactive(file)') ?? 0);
      const shmem = meminfo.get('Shmem') ?? 0;
      const fileCache = Math.max(0, fileLru - shmem);
      const buffers = meminfo.get('Buffers') ?? 0;
      const slab = meminfo.get('Slab') ?? 0;
      const pageTables = meminfo.get('PageTables') ?? 0;
      const kernelStack = meminfo.get('KernelStack') ?? 0;
      const zram = meminfo.get('Zram') ?? 0;
      const dmaHeap = hasDmaHeap ? lastDmaKb : 0;

      const accounted =
        anon +
        fileCache +
        shmem +
        buffers +
        slab +
        pageTables +
        kernelStack +
        zram +
        dmaHeap +
        free;
      const unaccounted = Math.max(0, total - accounted);

      anonPts.push({x, y: anon});
      filePts.push({x, y: fileCache});
      shmemPts.push({x, y: shmem});
      buffersPts.push({x, y: buffers});
      slabPts.push({x, y: slab});
      pageTablesPts.push({x, y: pageTables});
      kernelStackPts.push({x, y: kernelStack});
      zramPts.push({x, y: zram});
      dmaHeapPts.push({x, y: dmaHeap});
      freePts.push({x, y: free});
      unaccountedPts.push({x, y: unaccounted});
    }

    if (anonPts.length < 2) return undefined;

    const series: LineChartSeries[] = [
      {name: 'Anon', points: anonPts, color: '#e74c3c'},
      {name: 'Page cache', points: filePts, color: '#f39c12'},
      {name: 'Shmem', points: shmemPts, color: '#ab47bc'},
      {name: 'Buffers', points: buffersPts, color: '#3498db'},
      {name: 'Slab', points: slabPts, color: '#9c27b0'},
      {name: 'PageTables', points: pageTablesPts, color: '#4a148c'},
      {name: 'KernelStack', points: kernelStackPts, color: '#7b1fa2'},
      {name: 'Zram', points: zramPts, color: '#00897b'},
      {name: 'MemFree', points: freePts, color: '#2ecc71'},
      {name: 'Unaccounted', points: unaccountedPts, color: '#78909c'},
    ];

    if (hasDmaHeap) {
      // Insert before MemFree so stacking order makes sense.
      series.splice(series.length - 2, 0, {
        name: 'DMA-BUF',
        points: dmaHeapPts,
        color: '#00acc1',
      });
    }

    return {series};
  }

  // ---------------------------------------------------------------------------
  // Page cache: stacked breakdown of Cached into page cache + shmem.
  // ---------------------------------------------------------------------------

  private async queryPageCacheTimeSeries(
    engine: WasmEngineProxy,
    t0: number,
  ): Promise<LineChartData | undefined> {
    const counters = ['Cached', 'Shmem', 'Active(file)', 'Inactive(file)'];
    const names = counters.map((n) => `'${n}'`).join(',');
    const queryResult = await engine.query(`
      SELECT
        t.name AS counter_name,
        c.ts AS ts,
        c.value AS value_bytes
      FROM counter c
      JOIN counter_track t ON c.track_id = t.id
      WHERE t.name IN (${names})
      ORDER BY c.ts
    `);

    // TP stores meminfo values in bytes; convert to KB here.
    const byTs = new Map<number, Map<string, number>>();
    const iter = queryResult.iter({
      counter_name: STR,
      ts: NUM,
      value_bytes: NUM,
    });
    for (; iter.valid(); iter.next()) {
      let row = byTs.get(iter.ts);
      if (row === undefined) {
        row = new Map();
        byTs.set(iter.ts, row);
      }
      row.set(iter.counter_name, Math.round(iter.value_bytes / 1024));
    }

    if (byTs.size < 2) return undefined;

    const timestamps = [...byTs.keys()].sort((a, b) => a - b);

    // Decompose Cached into non-overlapping slices:
    //   Cached (from meminfo) = page cache + shmem (tmpfs/ashmem)
    //   Page cache = Cached - Shmem
    //   Page cache = Active(file) + Inactive(file) (alternative breakdown)
    const activeFilePoints: {x: number; y: number}[] = [];
    const inactiveFilePoints: {x: number; y: number}[] = [];
    const shmemPoints: {x: number; y: number}[] = [];

    for (const ts of timestamps) {
      const row = byTs.get(ts)!;
      const shmem = row.get('Shmem');
      const activeFile = row.get('Active(file)');
      const inactiveFile = row.get('Inactive(file)');
      if (
        shmem === undefined ||
        activeFile === undefined ||
        inactiveFile === undefined
      ) {
        continue;
      }
      const x = (ts - t0) / 1e9;
      activeFilePoints.push({x, y: activeFile});
      inactiveFilePoints.push({x, y: inactiveFile});
      shmemPoints.push({x, y: shmem});
    }

    if (activeFilePoints.length < 2) return undefined;

    // Stack: Active(file) + Inactive(file) + Shmem ≈ Cached
    return {
      series: [
        {name: 'Active(file)', points: activeFilePoints, color: '#2ecc71'},
        {name: 'Inactive(file)', points: inactiveFilePoints, color: '#f39c12'},
        {name: 'Shmem (tmpfs/ashmem)', points: shmemPoints, color: '#9b59b6'},
      ],
    };
  }

  // ---------------------------------------------------------------------------
  // Page cache breakdown: Mapped/Unmapped and Dirty/Clean.
  // ---------------------------------------------------------------------------

  private async queryFileCacheBreakdownTimeSeries(
    engine: WasmEngineProxy,
    t0: number,
  ): Promise<LineChartData | undefined> {
    const counters = ['Active(file)', 'Inactive(file)', 'Mapped', 'Dirty'];
    const names = counters.map((n) => `'${n}'`).join(',');
    const queryResult = await engine.query(`
      SELECT
        t.name AS counter_name,
        c.ts AS ts,
        c.value AS value_bytes
      FROM counter c
      JOIN counter_track t ON c.track_id = t.id
      WHERE t.name IN (${names})
      ORDER BY c.ts
    `);

    const byTs = new Map<number, Map<string, number>>();
    const iter = queryResult.iter({
      counter_name: STR,
      ts: NUM,
      value_bytes: NUM,
    });
    for (; iter.valid(); iter.next()) {
      let row = byTs.get(iter.ts);
      if (row === undefined) {
        row = new Map();
        byTs.set(iter.ts, row);
      }
      row.set(iter.counter_name, Math.round(iter.value_bytes / 1024));
    }

    if (byTs.size < 2) return undefined;

    const timestamps = [...byTs.keys()].sort((a, b) => a - b);
    // Approximate the 4-way split assuming independence between Mapped and
    // Dirty (the kernel doesn't expose the cross-tabulation directly).
    //   mapped_dirty   = mapped * dirty / fileCache
    //   mapped_clean   = mapped - mapped_dirty
    //   unmapped_dirty = dirty  - mapped_dirty
    //   unmapped_clean = fileCache - mapped - dirty + mapped_dirty
    const mappedDirtyPts: {x: number; y: number}[] = [];
    const mappedCleanPts: {x: number; y: number}[] = [];
    const unmappedDirtyPts: {x: number; y: number}[] = [];
    const unmappedCleanPts: {x: number; y: number}[] = [];

    for (const ts of timestamps) {
      const row = byTs.get(ts)!;
      const activeFile = row.get('Active(file)');
      const inactiveFile = row.get('Inactive(file)');
      const mapped = row.get('Mapped');
      const dirty = row.get('Dirty');
      if (
        activeFile === undefined ||
        inactiveFile === undefined ||
        mapped === undefined ||
        dirty === undefined
      ) {
        continue;
      }
      const fileCache = activeFile + inactiveFile;
      if (fileCache === 0) continue;
      const mappedDirty = (mapped * dirty) / fileCache;
      const x = (ts - t0) / 1e9;
      mappedDirtyPts.push({x, y: Math.round(mappedDirty)});
      mappedCleanPts.push({x, y: Math.round(mapped - mappedDirty)});
      unmappedDirtyPts.push({x, y: Math.round(dirty - mappedDirty)});
      unmappedCleanPts.push({
        x,
        y: Math.max(0, Math.round(fileCache - mapped - dirty + mappedDirty)),
      });
    }

    if (mappedDirtyPts.length < 2) return undefined;

    return {
      series: [
        {name: 'Mapped + Dirty', points: mappedDirtyPts, color: '#e74c3c'},
        {name: 'Mapped + Clean', points: mappedCleanPts, color: '#3498db'},
        {name: 'Unmapped + Dirty', points: unmappedDirtyPts, color: '#f39c12'},
        {name: 'Unmapped + Clean', points: unmappedCleanPts, color: '#2ecc71'},
      ],
    };
  }

  // ---------------------------------------------------------------------------
  // Per-process RSS by category: full time-series from TP.
  // ---------------------------------------------------------------------------

  private async queryCategoryTimeSeries(
    engine: WasmEngineProxy,
    t0: number,
    counters: readonly string[] = ['mem.rss'],
  ): Promise<LineChartData | undefined> {
    const counterList = counters.map((c) => `'${c}'`).join(', ');
    const queryResult = await engine.query(`
      SELECT
        p.name AS process_name,
        c.ts AS ts,
        SUM(c.value) AS rss_bytes
      FROM counter c
      JOIN process_counter_track t ON c.track_id = t.id
      JOIN process p ON t.upid = p.upid
      WHERE t.name IN (${counterList})
        AND p.name IS NOT NULL
      GROUP BY p.name, c.ts
      ORDER BY c.ts
    `);

    // Collect all unique timestamps and sum RSS per category at each ts.
    const catIds = Object.keys(CATEGORIES) as CategoryId[];
    const tsSet = new Set<number>();
    // Map<ts, Map<CategoryId, sumKb>>
    const byCatTs = new Map<number, Map<CategoryId, number>>();

    const iter = queryResult.iter({
      process_name: STR,
      ts: NUM,
      rss_bytes: NUM,
    });
    for (; iter.valid(); iter.next()) {
      const ts = iter.ts;
      tsSet.add(ts);
      let catMap = byCatTs.get(ts);
      if (catMap === undefined) {
        catMap = new Map();
        byCatTs.set(ts, catMap);
      }
      const cat = categorizeProcess(iter.process_name);
      const id = catIds.find((k) => CATEGORIES[k].name === cat.name)!;
      catMap.set(id, (catMap.get(id) ?? 0) + Math.round(iter.rss_bytes / 1024));
    }

    const timestamps = [...tsSet].sort((a, b) => a - b);
    if (timestamps.length < 2) return undefined;

    // Build one series per category.
    const pointsByCategory = new Map<CategoryId, {x: number; y: number}[]>();
    for (const id of catIds) {
      pointsByCategory.set(id, []);
    }
    for (const ts of timestamps) {
      const x = (ts - t0) / 1e9;
      const catMap = byCatTs.get(ts)!;
      for (const id of catIds) {
        pointsByCategory.get(id)!.push({x, y: catMap.get(id) ?? 0});
      }
    }

    const series: LineChartSeries[] = [];
    for (const id of catIds) {
      const points = pointsByCategory.get(id)!;
      if (points.some((p) => p.y > 0)) {
        const cat = CATEGORIES[id];
        series.push({name: cat.name, points, color: cat.color});
      }
    }
    if (series.length === 0) return undefined;
    return {series};
  }

  // ---------------------------------------------------------------------------
  // Drill-down: per-process RSS time-series for a single category.
  // ---------------------------------------------------------------------------

  private async queryDrilldownTimeSeries(
    engine: WasmEngineProxy,
    categoryId: CategoryId,
    t0: number = 0,
    counters: readonly string[] = ['mem.rss'],
  ): Promise<LineChartData | undefined> {
    const counterList = counters.map((c) => `'${c}'`).join(', ');
    const queryResult = await engine.query(`
      SELECT
        p.name AS process_name,
        c.ts AS ts,
        SUM(c.value) AS rss_bytes
      FROM counter c
      JOIN process_counter_track t ON c.track_id = t.id
      JOIN process p ON t.upid = p.upid
      WHERE t.name IN (${counterList})
        AND p.name IS NOT NULL
      GROUP BY p.name, c.ts
      ORDER BY c.ts
    `);

    const targetCat = CATEGORIES[categoryId];

    // Collect timestamps and per-process RSS for matching processes.
    const tsSet = new Set<number>();
    // Map<ts, Map<processName, sumKb>> (sum handles multiple PIDs per name)
    const byProcTs = new Map<number, Map<string, number>>();

    const iter = queryResult.iter({
      process_name: STR,
      ts: NUM,
      rss_bytes: NUM,
    });
    for (; iter.valid(); iter.next()) {
      const cat = categorizeProcess(iter.process_name);
      if (cat.name !== targetCat.name) continue;

      const ts = iter.ts;
      tsSet.add(ts);
      let procMap = byProcTs.get(ts);
      if (procMap === undefined) {
        procMap = new Map();
        byProcTs.set(ts, procMap);
      }
      procMap.set(
        iter.process_name,
        (procMap.get(iter.process_name) ?? 0) +
          Math.round(iter.rss_bytes / 1024),
      );
    }

    const timestamps = [...tsSet].sort((a, b) => a - b);
    if (timestamps.length < 2) return undefined;

    // Discover all process names that appear.
    const allNames = new Set<string>();
    for (const procMap of byProcTs.values()) {
      for (const name of procMap.keys()) {
        allNames.add(name);
      }
    }

    // Build one series per process, aligned to all timestamps.
    const pointsByProc = new Map<string, {x: number; y: number}[]>();
    for (const name of allNames) {
      pointsByProc.set(name, []);
    }
    for (const ts of timestamps) {
      const x = (ts - t0) / 1e9;
      const procMap = byProcTs.get(ts)!;
      for (const name of allNames) {
        pointsByProc.get(name)!.push({x, y: procMap.get(name) ?? 0});
      }
    }

    // Sort series by total RSS descending, keep top 15.
    const ranked = [...allNames]
      .map((name) => {
        const points = pointsByProc.get(name)!;
        const total = points.reduce((s, p) => s + p.y, 0);
        return {name, points, total};
      })
      .sort((a, b) => b.total - a.total);

    const TOP_N = 15;
    const top = ranked.slice(0, TOP_N);
    const rest = ranked.slice(TOP_N);

    const series: LineChartSeries[] = top.map((r) => ({
      name: r.name,
      points: r.points,
    }));

    if (rest.length > 0) {
      const otherPoints = timestamps.map((ts, i) => {
        const x = (ts - t0) / 1e9;
        const y = rest.reduce((sum, r) => sum + r.points[i].y, 0);
        return {x, y};
      });
      series.push({
        name: `Other (${rest.length} processes)`,
        points: otherPoints,
        color: '#999',
      });
    }

    if (series.length === 0) return undefined;
    return {series};
  }

  // ---------------------------------------------------------------------------
  // Per-process RSS by OOM score bucket: full time-series from TP.
  // ---------------------------------------------------------------------------

  private async queryOomScoreTimeSeries(
    engine: WasmEngineProxy,
    t0: number,
    counters: readonly string[] = ['mem.rss'],
  ): Promise<LineChartData | undefined> {
    // 1. Get latest OOM score per process name.
    const oomResult = await engine.query(`
      SELECT p.name AS process_name, MIN(latest.value) AS oom_score
      FROM (
        SELECT c.track_id, c.value,
          ROW_NUMBER() OVER (PARTITION BY c.track_id ORDER BY c.ts DESC) AS rn
        FROM counter c
        JOIN process_counter_track t ON c.track_id = t.id
        WHERE t.name = 'oom_score_adj'
      ) latest
      JOIN process_counter_track t ON latest.track_id = t.id
      JOIN process p ON t.upid = p.upid
      WHERE latest.rn = 1 AND p.name IS NOT NULL
      GROUP BY p.name
    `);

    const oomByName = new Map<string, number>();
    const oomIter = oomResult.iter({process_name: STR, oom_score: NUM});
    for (; oomIter.valid(); oomIter.next()) {
      oomByName.set(oomIter.process_name, oomIter.oom_score);
    }

    // 2. Get RSS time series.
    const counterList = counters.map((c) => `'${c}'`).join(', ');
    const queryResult = await engine.query(`
      SELECT
        p.name AS process_name,
        c.ts AS ts,
        SUM(c.value) AS rss_bytes
      FROM counter c
      JOIN process_counter_track t ON c.track_id = t.id
      JOIN process p ON t.upid = p.upid
      WHERE t.name IN (${counterList})
        AND p.name IS NOT NULL
      GROUP BY p.name, c.ts
      ORDER BY c.ts
    `);

    // 3. Group by OOM bucket at each timestamp.
    const tsSet = new Set<number>();
    const byBucketTs = new Map<number, Map<number, number>>();

    const iter = queryResult.iter({
      process_name: STR,
      ts: NUM,
      rss_bytes: NUM,
    });
    for (; iter.valid(); iter.next()) {
      const ts = iter.ts;
      tsSet.add(ts);
      let bucketMap = byBucketTs.get(ts);
      if (bucketMap === undefined) {
        bucketMap = new Map();
        byBucketTs.set(ts, bucketMap);
      }
      const oomScore = oomByName.get(iter.process_name) ?? 0;
      const bucketIdx = OOM_SCORE_BUCKETS.findIndex(
        (b) => oomScore >= b.minScore && oomScore <= b.maxScore,
      );
      const idx = bucketIdx !== -1 ? bucketIdx : OOM_SCORE_BUCKETS.length - 1;
      bucketMap.set(
        idx,
        (bucketMap.get(idx) ?? 0) + Math.round(iter.rss_bytes / 1024),
      );
    }

    const timestamps = [...tsSet].sort((a, b) => a - b);
    if (timestamps.length < 2) return undefined;

    // Build one series per bucket.
    const pointsByBucket = new Map<number, {x: number; y: number}[]>();
    for (let i = 0; i < OOM_SCORE_BUCKETS.length; i++) {
      pointsByBucket.set(i, []);
    }
    for (const ts of timestamps) {
      const x = (ts - t0) / 1e9;
      const bucketMap = byBucketTs.get(ts)!;
      for (let i = 0; i < OOM_SCORE_BUCKETS.length; i++) {
        pointsByBucket.get(i)!.push({x, y: bucketMap.get(i) ?? 0});
      }
    }

    const series: LineChartSeries[] = [];
    for (let i = 0; i < OOM_SCORE_BUCKETS.length; i++) {
      const points = pointsByBucket.get(i)!;
      if (points.some((p) => p.y > 0)) {
        const bucket = OOM_SCORE_BUCKETS[i];
        series.push({name: bucket.name, points, color: bucket.color});
      }
    }
    if (series.length === 0) return undefined;
    return {series};
  }

  // ---------------------------------------------------------------------------
  // Drill-down: per-process RSS time-series for a single OOM score bucket.
  // ---------------------------------------------------------------------------

  private async queryOomDrilldownTimeSeries(
    engine: WasmEngineProxy,
    bucketIdx: number,
    t0: number = 0,
    counters: readonly string[] = ['mem.rss'],
  ): Promise<LineChartData | undefined> {
    const bucket = OOM_SCORE_BUCKETS[bucketIdx];

    // 1. Get latest OOM score per process name.
    const oomResult = await engine.query(`
      SELECT p.name AS process_name, MIN(latest.value) AS oom_score
      FROM (
        SELECT c.track_id, c.value,
          ROW_NUMBER() OVER (PARTITION BY c.track_id ORDER BY c.ts DESC) AS rn
        FROM counter c
        JOIN process_counter_track t ON c.track_id = t.id
        WHERE t.name = 'oom_score_adj'
      ) latest
      JOIN process_counter_track t ON latest.track_id = t.id
      JOIN process p ON t.upid = p.upid
      WHERE latest.rn = 1 AND p.name IS NOT NULL
      GROUP BY p.name
    `);

    const matchingNames = new Set<string>();
    const oomIter = oomResult.iter({process_name: STR, oom_score: NUM});
    for (; oomIter.valid(); oomIter.next()) {
      if (
        oomIter.oom_score >= bucket.minScore &&
        oomIter.oom_score <= bucket.maxScore
      ) {
        matchingNames.add(oomIter.process_name);
      }
    }

    // 2. Get RSS time series.
    const counterList = counters.map((c) => `'${c}'`).join(', ');
    const queryResult = await engine.query(`
      SELECT
        p.name AS process_name,
        c.ts AS ts,
        SUM(c.value) AS rss_bytes
      FROM counter c
      JOIN process_counter_track t ON c.track_id = t.id
      JOIN process p ON t.upid = p.upid
      WHERE t.name IN (${counterList})
        AND p.name IS NOT NULL
      GROUP BY p.name, c.ts
      ORDER BY c.ts
    `);

    // 3. Collect per-process data for matching processes.
    const tsSet = new Set<number>();
    const byProcTs = new Map<number, Map<string, number>>();

    const iter = queryResult.iter({
      process_name: STR,
      ts: NUM,
      rss_bytes: NUM,
    });
    for (; iter.valid(); iter.next()) {
      if (!matchingNames.has(iter.process_name)) continue;
      const ts = iter.ts;
      tsSet.add(ts);
      let procMap = byProcTs.get(ts);
      if (procMap === undefined) {
        procMap = new Map();
        byProcTs.set(ts, procMap);
      }
      procMap.set(
        iter.process_name,
        (procMap.get(iter.process_name) ?? 0) +
          Math.round(iter.rss_bytes / 1024),
      );
    }

    const timestamps = [...tsSet].sort((a, b) => a - b);
    if (timestamps.length < 2) return undefined;

    const allNames = new Set<string>();
    for (const procMap of byProcTs.values()) {
      for (const name of procMap.keys()) {
        allNames.add(name);
      }
    }

    const pointsByProc = new Map<string, {x: number; y: number}[]>();
    for (const name of allNames) {
      pointsByProc.set(name, []);
    }
    for (const ts of timestamps) {
      const x = (ts - t0) / 1e9;
      const procMap = byProcTs.get(ts)!;
      for (const name of allNames) {
        pointsByProc.get(name)!.push({x, y: procMap.get(name) ?? 0});
      }
    }

    const ranked = [...allNames]
      .map((name) => {
        const points = pointsByProc.get(name)!;
        const total = points.reduce((s, p) => s + p.y, 0);
        return {name, points, total};
      })
      .sort((a, b) => b.total - a.total);

    const TOP_N = 15;
    const top = ranked.slice(0, TOP_N);
    const rest = ranked.slice(TOP_N);

    const series: LineChartSeries[] = top.map((r) => ({
      name: r.name,
      points: r.points,
    }));

    if (rest.length > 0) {
      const otherPoints = timestamps.map((ts, i) => {
        const x = (ts - t0) / 1e9;
        const y = rest.reduce((sum, r) => sum + r.points[i].y, 0);
        return {x, y};
      });
      series.push({
        name: `Other (${rest.length} processes)`,
        points: otherPoints,
        color: '#999',
      });
    }

    if (series.length === 0) return undefined;
    return {series};
  }

  // ---------------------------------------------------------------------------
  // PSI: memory pressure stall time over time (rate of change).
  // ---------------------------------------------------------------------------

  private async queryPsiTimeSeries(
    engine: WasmEngineProxy,
    t0: number,
  ): Promise<LineChartData | undefined> {
    const queryResult = await engine.query(`
      SELECT
        t.name AS counter_name,
        c.ts AS ts,
        c.value AS value_ns
      FROM counter c
      JOIN counter_track t ON c.track_id = t.id
      WHERE t.name IN ('psi.mem.some', 'psi.mem.full')
      ORDER BY c.ts
    `);

    // Group by counter name, then compute rate (delta_ns / delta_s → ms/s).
    const byName = new Map<string, {ts: number; value: number}[]>();
    const iter = queryResult.iter({
      counter_name: STR,
      ts: NUM,
      value_ns: NUM,
    });
    for (; iter.valid(); iter.next()) {
      let arr = byName.get(iter.counter_name);
      if (arr === undefined) {
        arr = [];
        byName.set(iter.counter_name, arr);
      }
      arr.push({ts: iter.ts, value: iter.value_ns});
    }

    const someRaw = byName.get('psi.mem.some');
    if (someRaw === undefined || someRaw.length < 2) return undefined;

    const toRatePoints = (
      raw: {ts: number; value: number}[],
    ): {x: number; y: number}[] => {
      const points: {x: number; y: number}[] = [];
      for (let i = 1; i < raw.length; i++) {
        const dtS = (raw[i].ts - raw[i - 1].ts) / 1e9;
        if (dtS <= 0) continue;
        const deltaNs = raw[i].value - raw[i - 1].value;
        // Convert ns stalled per second → ms stalled per second.
        const msPerSec = deltaNs / (dtS * 1e6);
        points.push({x: (raw[i].ts - t0) / 1e9, y: Math.max(0, msPerSec)});
      }
      return points;
    };

    const series: LineChartSeries[] = [];
    if (someRaw.length >= 2) {
      series.push({
        name: 'some (any task stalled)',
        points: toRatePoints(someRaw),
        color: '#f39c12',
      });
    }
    const fullRaw = byName.get('psi.mem.full');
    if (fullRaw !== undefined && fullRaw.length >= 2) {
      series.push({
        name: 'full (all tasks stalled)',
        points: toRatePoints(fullRaw),
        color: '#e74c3c',
      });
    }

    if (series.length === 0) return undefined;
    return {series};
  }

  // ---------------------------------------------------------------------------
  // Swap usage time-series.
  // ---------------------------------------------------------------------------

  private async querySwapTimeSeries(
    engine: WasmEngineProxy,
    t0: number,
  ): Promise<LineChartData | undefined> {
    const queryResult = await engine.query(`
      SELECT
        t.name AS counter_name,
        c.ts AS ts,
        c.value AS value_bytes
      FROM counter c
      JOIN counter_track t ON c.track_id = t.id
      WHERE t.name IN ('SwapTotal', 'SwapFree', 'SwapCached')
      ORDER BY c.ts
    `);

    const byTs = new Map<number, Map<string, number>>();
    const iter = queryResult.iter({
      counter_name: STR,
      ts: NUM,
      value_bytes: NUM,
    });
    for (; iter.valid(); iter.next()) {
      let row = byTs.get(iter.ts);
      if (row === undefined) {
        row = new Map();
        byTs.set(iter.ts, row);
      }
      row.set(iter.counter_name, Math.round(iter.value_bytes / 1024));
    }

    if (byTs.size < 2) return undefined;

    const timestamps = [...byTs.keys()].sort((a, b) => a - b);

    // Check if swap is enabled (SwapTotal > 0).
    const firstRow = byTs.get(timestamps[0]);
    const swapTotal = firstRow?.get('SwapTotal') ?? 0;
    if (swapTotal === 0) return undefined;

    const dirtyPts: {x: number; y: number}[] = [];
    const cachedPts: {x: number; y: number}[] = [];
    const freePts: {x: number; y: number}[] = [];

    for (const ts of timestamps) {
      const row = byTs.get(ts)!;
      const total = row.get('SwapTotal') ?? 0;
      const free = row.get('SwapFree') ?? 0;
      const cached = row.get('SwapCached') ?? 0;
      const x = (ts - t0) / 1e9;
      // SwapUsed = SwapTotal - SwapFree; split into cached (clean) and dirty.
      const used = Math.max(0, total - free);
      const dirty = Math.max(0, used - cached);
      dirtyPts.push({x, y: dirty});
      cachedPts.push({x, y: cached});
      freePts.push({x, y: free});
    }

    if (dirtyPts.length < 2) return undefined;

    return {
      series: [
        {name: 'Swap dirty', points: dirtyPts, color: '#e74c3c'},
        {name: 'SwapCached', points: cachedPts, color: '#f39c12'},
        {name: 'SwapFree', points: freePts, color: '#2ecc71'},
      ],
    };
  }

  // ---------------------------------------------------------------------------
  // vmstat: pswpin/pswpout rate (pages/s).
  // ---------------------------------------------------------------------------

  private async queryVmstatTimeSeries(
    engine: WasmEngineProxy,
    t0: number,
  ): Promise<LineChartData | undefined> {
    const queryResult = await engine.query(`
      SELECT
        t.name AS counter_name,
        c.ts AS ts,
        c.value AS value
      FROM counter c
      JOIN counter_track t ON c.track_id = t.id
      WHERE t.name IN ('pswpin', 'pswpout')
      ORDER BY c.ts
    `);

    // Group raw cumulative values by counter name.
    const byName = new Map<string, {ts: number; value: number}[]>();
    const iter = queryResult.iter({
      counter_name: STR,
      ts: NUM,
      value: NUM,
    });
    for (; iter.valid(); iter.next()) {
      let arr = byName.get(iter.counter_name);
      if (arr === undefined) {
        arr = [];
        byName.set(iter.counter_name, arr);
      }
      arr.push({ts: iter.ts, value: iter.value});
    }

    const pswpinRaw = byName.get('pswpin');
    if (pswpinRaw === undefined || pswpinRaw.length < 2) return undefined;

    // Convert cumulative page counts to rate (pages/second).
    const toRatePoints = (
      raw: {ts: number; value: number}[],
    ): {x: number; y: number}[] => {
      const points: {x: number; y: number}[] = [];
      for (let i = 1; i < raw.length; i++) {
        const dtS = (raw[i].ts - raw[i - 1].ts) / 1e9;
        if (dtS <= 0) continue;
        const delta = raw[i].value - raw[i - 1].value;
        const rate = delta / dtS;
        points.push({x: (raw[i].ts - t0) / 1e9, y: Math.max(0, rate)});
      }
      return points;
    };

    const series: LineChartSeries[] = [
      {name: 'pswpin', points: toRatePoints(pswpinRaw), color: '#3498db'},
    ];
    const pswpoutRaw = byName.get('pswpout');
    if (pswpoutRaw !== undefined && pswpoutRaw.length >= 2) {
      series.push({
        name: 'pswpout',
        points: toRatePoints(pswpoutRaw),
        color: '#e74c3c',
      });
    }

    if (series.every((s) => s.points.length === 0)) return undefined;
    return {series};
  }

  private async queryPageFaultTimeSeries(
    engine: WasmEngineProxy,
    t0: number,
  ): Promise<LineChartData | undefined> {
    const queryResult = await engine.query(`
      SELECT
        t.name AS counter_name,
        c.ts AS ts,
        c.value AS value
      FROM counter c
      JOIN counter_track t ON c.track_id = t.id
      WHERE t.name IN ('pgfault', 'pgmajfault')
      ORDER BY c.ts
    `);

    const byName = new Map<string, {ts: number; value: number}[]>();
    const iter = queryResult.iter({
      counter_name: STR,
      ts: NUM,
      value: NUM,
    });
    for (; iter.valid(); iter.next()) {
      let arr = byName.get(iter.counter_name);
      if (arr === undefined) {
        arr = [];
        byName.set(iter.counter_name, arr);
      }
      arr.push({ts: iter.ts, value: iter.value});
    }

    const pgfaultRaw = byName.get('pgfault');
    if (pgfaultRaw === undefined || pgfaultRaw.length < 2) return undefined;

    // Convert cumulative counts to rate (faults/second).
    const toRatePoints = (
      raw: {ts: number; value: number}[],
    ): {x: number; y: number}[] => {
      const points: {x: number; y: number}[] = [];
      for (let i = 1; i < raw.length; i++) {
        const dtS = (raw[i].ts - raw[i - 1].ts) / 1e9;
        if (dtS <= 0) continue;
        const delta = raw[i].value - raw[i - 1].value;
        const rate = delta / dtS;
        points.push({x: (raw[i].ts - t0) / 1e9, y: Math.max(0, rate)});
      }
      return points;
    };

    const series: LineChartSeries[] = [
      {
        name: 'pgfault (minor)',
        points: toRatePoints(pgfaultRaw),
        color: '#3498db',
      },
    ];
    const pgmajfaultRaw = byName.get('pgmajfault');
    if (pgmajfaultRaw !== undefined && pgmajfaultRaw.length >= 2) {
      series.push({
        name: 'pgmajfault (major)',
        points: toRatePoints(pgmajfaultRaw),
        color: '#e74c3c',
      });
    }

    if (series.every((s) => s.points.length === 0)) return undefined;
    return {series};
  }

  // ---------------------------------------------------------------------------
  // Page cache activity: reclamation and thrashing rates from vmstat.
  // ---------------------------------------------------------------------------

  private async queryFileCacheActivityTimeSeries(
    engine: WasmEngineProxy,
    t0: number,
  ): Promise<LineChartData | undefined> {
    const counters = ['workingset_refault_file', 'pgsteal_file', 'pgscan_file'];
    const names = counters.map((n) => `'${n}'`).join(',');
    const queryResult = await engine.query(`
      SELECT
        t.name AS counter_name,
        c.ts AS ts,
        c.value AS value
      FROM counter c
      JOIN counter_track t ON c.track_id = t.id
      WHERE t.name IN (${names})
      ORDER BY c.ts
    `);

    const byName = new Map<string, {ts: number; value: number}[]>();
    const iter = queryResult.iter({
      counter_name: STR,
      ts: NUM,
      value: NUM,
    });
    for (; iter.valid(); iter.next()) {
      let arr = byName.get(iter.counter_name);
      if (arr === undefined) {
        arr = [];
        byName.set(iter.counter_name, arr);
      }
      arr.push({ts: iter.ts, value: iter.value});
    }

    // Convert cumulative counts to rate (events/second).
    const toRatePoints = (
      raw: {ts: number; value: number}[],
    ): {x: number; y: number}[] => {
      const points: {x: number; y: number}[] = [];
      for (let i = 1; i < raw.length; i++) {
        const dtS = (raw[i].ts - raw[i - 1].ts) / 1e9;
        if (dtS <= 0) continue;
        const delta = raw[i].value - raw[i - 1].value;
        points.push({x: (raw[i].ts - t0) / 1e9, y: Math.max(0, delta / dtS)});
      }
      return points;
    };

    const series: LineChartSeries[] = [];
    const refaultRaw = byName.get('workingset_refault_file');
    if (refaultRaw !== undefined && refaultRaw.length >= 2) {
      series.push({
        name: 'Refaults (thrashing)',
        points: toRatePoints(refaultRaw),
        color: '#e74c3c',
      });
    }
    const stealRaw = byName.get('pgsteal_file');
    if (stealRaw !== undefined && stealRaw.length >= 2) {
      series.push({
        name: 'Stolen (reclaimed)',
        points: toRatePoints(stealRaw),
        color: '#f39c12',
      });
    }
    const scanRaw = byName.get('pgscan_file');
    if (scanRaw !== undefined && scanRaw.length >= 2) {
      series.push({
        name: 'Scanned',
        points: toRatePoints(scanRaw),
        color: '#95a5a6',
      });
    }

    if (series.length === 0 || series.every((s) => s.points.length === 0)) {
      return undefined;
    }
    return {series};
  }

  // ---------------------------------------------------------------------------
  // Sankey: snapshot of where MemTotal is going right now.
  // ---------------------------------------------------------------------------

  private async querySankeyData(
    engine: WasmEngineProxy,
  ): Promise<SankeyData | undefined> {
    // 1. Latest system meminfo values.
    const sysCounters = [
      'MemTotal',
      'MemFree',
      'Buffers',
      'Active(anon)',
      'Inactive(anon)',
      'Active(file)',
      'Inactive(file)',
      'Shmem',
      'Slab',
      'KernelStack',
      'PageTables',
      'Zram',
      'SwapTotal',
      'SwapFree',
    ];
    const sysNames = sysCounters.map((n) => `'${n}'`).join(',');
    const sysResult = await engine.query(`
      SELECT counter_name, value_bytes
      FROM (
        SELECT
          t.name AS counter_name,
          c.value AS value_bytes,
          ROW_NUMBER() OVER (PARTITION BY c.track_id ORDER BY c.ts DESC) AS rn
        FROM counter c
        JOIN counter_track t ON c.track_id = t.id
        WHERE t.name IN (${sysNames})
      )
      WHERE rn = 1
    `);

    const sys = new Map<string, number>();
    const sysIter = sysResult.iter({counter_name: STR, value_bytes: NUM});
    for (; sysIter.valid(); sysIter.next()) {
      sys.set(sysIter.counter_name, Math.round(sysIter.value_bytes / 1024));
    }

    const memTotal = sys.get('MemTotal');
    const memFree = sys.get('MemFree');
    const buffers = sys.get('Buffers') ?? 0;
    if (memTotal === undefined || memFree === undefined) {
      return undefined;
    }

    const anon =
      (sys.get('Active(anon)') ?? 0) + (sys.get('Inactive(anon)') ?? 0);
    const fileLru =
      (sys.get('Active(file)') ?? 0) + (sys.get('Inactive(file)') ?? 0);
    const shmem = sys.get('Shmem') ?? 0;
    const fileCache = Math.max(0, fileLru - shmem);
    const slab = sys.get('Slab') ?? 0;
    const kernelStack = sys.get('KernelStack') ?? 0;
    const pageTables = sys.get('PageTables') ?? 0;
    const zram = sys.get('Zram') ?? 0;

    // Query latest global DMA-BUF heap total (from dma_heap_stat ftrace).
    const dmaResult = await engine.query(`
      SELECT value_bytes
      FROM (
        SELECT
          c.value AS value_bytes,
          ROW_NUMBER() OVER (ORDER BY c.ts DESC) AS rn
        FROM counter c
        JOIN counter_track t ON c.track_id = t.id
        WHERE t.name = 'mem.dma_heap'
      )
      WHERE rn = 1
    `);
    const dmaIter = dmaResult.iter({value_bytes: NUM});
    const dmaHeap = dmaIter.valid()
      ? Math.round(dmaIter.value_bytes / 1024)
      : 0;

    const accounted =
      anon +
      fileCache +
      shmem +
      buffers +
      slab +
      pageTables +
      kernelStack +
      zram +
      dmaHeap +
      memFree;
    const unaccounted = Math.max(0, memTotal - accounted);

    // 2. Latest per-process RssAnon, grouped by category.
    const procResult = await engine.query(`
      SELECT process_name, rss_bytes
      FROM (
        SELECT
          p.name AS process_name,
          c.value AS rss_bytes,
          ROW_NUMBER() OVER (PARTITION BY c.track_id ORDER BY c.ts DESC) AS rn
        FROM counter c
        JOIN process_counter_track t ON c.track_id = t.id
        JOIN process p ON t.upid = p.upid
        WHERE t.name = 'mem.rss.anon'
          AND p.name IS NOT NULL
      )
      WHERE rn = 1
    `);

    const catIds = Object.keys(CATEGORIES) as CategoryId[];
    const catSums = new Map<CategoryId, number>();
    const procIter = procResult.iter({process_name: STR, rss_bytes: NUM});
    for (; procIter.valid(); procIter.next()) {
      const cat = categorizeProcess(procIter.process_name);
      const id = catIds.find((k) => CATEGORIES[k].name === cat.name)!;
      catSums.set(
        id,
        (catSums.get(id) ?? 0) + Math.round(procIter.rss_bytes / 1024),
      );
    }

    // 3. Build Sankey nodes and links.
    // Explicit depth: 0=MemTotal, 1=primary partitions, 2=detail breakdown.
    const nodes: {name: string; color?: string; depth?: number}[] = [
      {name: 'MemTotal', color: '#78909c', depth: 0},
    ];
    const links: {source: string; target: string; value: number}[] = [];

    // Level 1: MemTotal → primary partitions (matching the system chart).
    const addBucket = (name: string, value: number, color: string) => {
      if (value <= 0) return;
      nodes.push({name, color, depth: 1});
      links.push({source: 'MemTotal', target: name, value});
    };
    addBucket('Anon', anon, '#e74c3c');
    addBucket('Page cache', fileCache, '#f39c12');
    addBucket('Shmem', shmem, '#ab47bc');
    addBucket('Buffers', buffers, '#3498db');
    addBucket('Slab', slab, '#9c27b0');
    addBucket('PageTables', pageTables, '#4a148c');
    addBucket('KernelStack', kernelStack, '#7b1fa2');
    addBucket('DMA-BUF', dmaHeap, '#00acc1');
    addBucket('Zram', zram, '#00897b');
    addBucket('MemFree', memFree, '#2ecc71');
    addBucket('Unaccounted', unaccounted, '#78909c');

    // Level 2: Anon → per-process-category breakdown.
    let accountedAnon = 0;
    for (const id of catIds) {
      const sum = catSums.get(id);
      if (sum !== undefined && sum > 0) {
        const cat = CATEGORIES[id];
        nodes.push({name: cat.name, color: cat.color, depth: 2});
        links.push({source: 'Anon', target: cat.name, value: sum});
        accountedAnon += sum;
      }
    }
    const unaccountedAnon = anon - accountedAnon;
    if (unaccountedAnon > 0) {
      nodes.push({
        name: 'Other (untracked)',
        color: '#9e9e9e',
        depth: 2,
      });
      links.push({
        source: 'Anon',
        target: 'Other (untracked)',
        value: unaccountedAnon,
      });
    }

    // Level 2: Page cache → Active/Inactive breakdown.
    // Shmem is counted in Active(file) by the kernel but already has its own
    // node, so subtract it to keep the sub-links summing to fileCache.
    const activeFile = Math.max(0, (sys.get('Active(file)') ?? 0) - shmem);
    const inactiveFile = sys.get('Inactive(file)') ?? 0;
    if (activeFile > 0) {
      nodes.push({name: 'Active', color: '#e67e22', depth: 2});
      links.push({
        source: 'Page cache',
        target: 'Active',
        value: activeFile,
      });
    }
    if (inactiveFile > 0) {
      nodes.push({name: 'Inactive', color: '#f5b041', depth: 2});
      links.push({
        source: 'Page cache',
        target: 'Inactive',
        value: inactiveFile,
      });
    }

    if (links.length === 0) return undefined;
    return {nodes, links};
  }

  // ---------------------------------------------------------------------------
  // Per-process memory breakdown (for profiling view).
  // ---------------------------------------------------------------------------

  private async queryProcessMemoryBreakdown(
    engine: WasmEngineProxy,
    pid: number,
    t0: number,
  ): Promise<LineChartData | undefined> {
    const queryResult = await engine.query(`
      SELECT
        t.name AS counter_name,
        c.ts AS ts,
        c.value AS value
      FROM counter c
      JOIN process_counter_track t ON c.track_id = t.id
      JOIN process p ON t.upid = p.upid
      WHERE p.pid = ${pid}
        AND t.name IN ('mem.rss.anon', 'mem.swap', 'mem.rss.file', 'mem.dmabuf_rss')
      ORDER BY c.ts
    `);

    // Accumulate values per timestamp, summing anon + swap together.
    const tsSet = new Set<number>();
    const bySeriesTs = new Map<number, Map<string, number>>();
    const SERIES_NAMES = ['Anon + Swap', 'File', 'DMA-BUF'] as const;

    const iter = queryResult.iter({
      counter_name: STR,
      ts: NUM,
      value: NUM,
    });
    for (; iter.valid(); iter.next()) {
      const ts = iter.ts;
      tsSet.add(ts);
      let seriesMap = bySeriesTs.get(ts);
      if (seriesMap === undefined) {
        seriesMap = new Map();
        bySeriesTs.set(ts, seriesMap);
      }
      const kb = Math.round(iter.value / 1024);
      const name = iter.counter_name;
      if (name === 'mem.rss.anon' || name === 'mem.swap') {
        seriesMap.set('Anon + Swap', (seriesMap.get('Anon + Swap') ?? 0) + kb);
      } else if (name === 'mem.rss.file') {
        seriesMap.set('File', (seriesMap.get('File') ?? 0) + kb);
      } else if (name === 'mem.dmabuf_rss') {
        seriesMap.set('DMA-BUF', (seriesMap.get('DMA-BUF') ?? 0) + kb);
      }
    }

    const timestamps = [...tsSet].sort((a, b) => a - b);
    if (timestamps.length < 2) return undefined;

    const colors: Record<string, string> = {
      'Anon + Swap': '#ff9800',
      'File': '#4caf50',
      'DMA-BUF': '#2196f3',
    };

    const series: LineChartSeries[] = [];
    for (const name of SERIES_NAMES) {
      const points = timestamps.map((ts) => ({
        x: (ts - t0) / 1e9,
        y: bySeriesTs.get(ts)?.get(name) ?? 0,
      }));
      if (points.some((p) => p.y > 0)) {
        series.push({name, points, color: colors[name]});
      }
    }

    if (series.length === 0) return undefined;
    return {series};
  }

  // ---------------------------------------------------------------------------
  // Latest per-process memory (for the table).
  // ---------------------------------------------------------------------------

  private async queryLatestProcessMemory(
    engine: WasmEngineProxy,
  ): Promise<ProcessMemoryRow[]> {
    // Pivot latest counter values per process across multiple track names.
    // Also compute process age from start_ts (set by record_process_age).
    const queryResult = await engine.query(`
      SELECT
        p.name AS process_name,
        p.pid AS pid,
        MAX(CASE WHEN t.name = 'mem.rss' THEN latest.value END) AS rss_bytes,
        MAX(CASE WHEN t.name = 'mem.rss.anon' THEN latest.value END) AS anon_bytes,
        MAX(CASE WHEN t.name = 'mem.rss.file' THEN latest.value END) AS file_bytes,
        MAX(CASE WHEN t.name = 'mem.rss.shmem' THEN latest.value END) AS shmem_bytes,
        MAX(CASE WHEN t.name = 'mem.swap' THEN latest.value END) AS swap_bytes,
        MAX(CASE WHEN t.name = 'oom_score_adj' THEN latest.value END) AS oom_score,
        CASE WHEN p.start_ts IS NOT NULL
          THEN ((SELECT MAX(ts) FROM counter) - p.start_ts) / 1e9
          ELSE NULL
        END AS age_seconds
      FROM (
        SELECT
          c.track_id,
          c.value,
          ROW_NUMBER() OVER (PARTITION BY c.track_id ORDER BY c.ts DESC) AS rn
        FROM counter c
        JOIN process_counter_track t ON c.track_id = t.id
        WHERE t.name IN ('mem.rss', 'mem.rss.anon', 'mem.rss.file',
                         'mem.rss.shmem', 'mem.swap', 'oom_score_adj')
      ) latest
      JOIN process_counter_track t ON latest.track_id = t.id
      JOIN process p ON t.upid = p.upid
      WHERE latest.rn = 1
        AND p.name IS NOT NULL
      GROUP BY p.upid
      ORDER BY rss_bytes DESC
    `);

    const rows: ProcessMemoryRow[] = [];
    const iter = queryResult.iter({
      process_name: STR,
      pid: NUM,
      rss_bytes: NUM,
      anon_bytes: NUM,
      file_bytes: NUM,
      shmem_bytes: NUM,
      swap_bytes: NUM,
      oom_score: NUM,
      age_seconds: NUM_NULL,
    });

    for (; iter.valid(); iter.next()) {
      rows.push({
        processName: iter.process_name,
        pid: iter.pid,
        rssKb: Math.round(iter.rss_bytes / 1024),
        anonKb: Math.round(iter.anon_bytes / 1024),
        fileKb: Math.round(iter.file_bytes / 1024),
        shmemKb: Math.round(iter.shmem_bytes / 1024),
        swapKb: Math.round(iter.swap_bytes / 1024),
        oomScore: iter.oom_score,
        ageSeconds: iter.age_seconds,
      });
    }
    return rows;
  }
}

function panel(
  title: string,
  subtitle: string | undefined,
  body: m.Children,
): m.Children {
  return m(
    '.pf-live-memory-panel',
    m(
      '.pf-live-memory-panel__header',
      m('h2', title),
      subtitle !== undefined && m('p', subtitle),
    ),
    m('.pf-live-memory-panel__body', body),
  );
}

function formatKb(kb: number): string {
  if (kb < 1024) return `${kb.toLocaleString()} KB`;
  if (kb < 1024 * 1024) return `${(kb / 1024).toFixed(1)} MB`;
  return `${(kb / (1024 * 1024)).toFixed(1)} GB`;
}
