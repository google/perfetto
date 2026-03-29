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
import {uuidv4} from '../../base/uuid';
import {NUM, NUM_NULL, STR, STR_NULL} from '../../trace_processor/query_result';
import {WasmEngineProxy} from '../../trace_processor/wasm_engine_proxy';
import {AdbDevice} from '../dev.perfetto.RecordTraceV2/adb/adb_device';
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
  LineChartMarker,
  LineChartSeries,
} from '../../components/widgets/charts/line_chart';
import {
  categorizeProcess,
  CATEGORIES,
  type CategoryId,
} from './process_categories';
import {Intent} from '../../widgets/common';
import {App} from '../../public/app';
import {TracedWebsocketTarget} from '../dev.perfetto.RecordTraceV2/traced_over_websocket/traced_websocket_target';
import {formatKb, panel} from './utils';
import {renderSystemTab} from './tab_system';
import {renderPageCacheTab} from './tab_page_cache';
import {renderPressureSwapTab} from './tab_pressure_swap';
import {
  renderProcessesTab,
  type ProcessGrouping,
  type ProcessMetric,
  type ProcessMemoryRow,
  OOM_SCORE_BUCKETS,
  PROCESS_METRIC_OPTIONS,
} from './tab_processes';

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

// Raw data extracted from a trace snapshot, stored in JS for instant access.
interface SnapshotRawData {
  // System-level counter time series (meminfo, vmstat, PSI, dma_heap, etc.)
  // counterName → sorted array of {ts, value} (value in raw TP units: bytes
  // for meminfo, ns for PSI, counts for vmstat)
  systemCounters: Map<string, {ts: number; value: number}[]>;

  // Per-process counter time series, aggregated by process name (summing
  // across PIDs).
  // processName → counterName → Map<ts, summedValueInBytes>
  processCountersByName: Map<string, Map<string, Map<number, number>>>;

  // Per-PID counter time series (needed for profiling breakdown of a specific
  // PID).
  // pid → counterName → sorted array of {ts, value}
  processCountersByPid: Map<number, Map<string, {ts: number; value: number}[]>>;

  // Process metadata: processName → {pid, startTs, debuggable}
  processInfo: Map<
    string,
    {pid: number; startTs: number | null; debuggable: boolean}
  >;

  // LMK (Low Memory Killer) events.
  lmkEvents: LmkEvent[];

  // Whether the device is a userdebug build.
  isUserDebug: boolean;

  // The Android build fingerprint string, if available.
  androidBuildFingerprint?: string;
}

interface LmkEvent {
  ts: number;
  pid: number;
  processName: string;
  oomScoreAdj: number;
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

export interface LiveMemoryDashboardAttrs {
  app: App;
  device?: AdbDevice;
  deviceName: string;
  linuxTarget?: TracedWebsocketTarget;
  onDisconnect: () => void;
}

export class LiveMemoryDashboard
  implements m.ClassComponent<LiveMemoryDashboardAttrs>
{
  private device?: AdbDevice;
  private deviceName?: string;
  private linuxTarget?: TracedWebsocketTarget;
  private error?: string;

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
  private heapProfileStartTime?: number; // Date.now() when profiling started
  // Baseline values (KB) captured from first chart data after profiling starts.
  private heapProfileBaseline?: {
    anonSwap: number;
    file: number;
    dmabuf: number;
  };

  // Reusable TP instance for processing cloned traces.
  private engine?: WasmEngineProxy;

  // Raw data extracted once per snapshot; all charts are derived from this.
  private rawData?: SnapshotRawData;

  // Chart data extracted from the latest cloned trace (full time-series).
  private systemChartData?: LineChartData;
  private pageCacheChartData?: LineChartData;
  private fileCacheBreakdownData?: LineChartData;
  private fileCacheActivityData?: LineChartData;
  private categoryChartData?: LineChartData;
  private psiChartData?: LineChartData;
  private swapChartData?: LineChartData;
  private vmstatChartData?: LineChartData;
  private pageFaultChartData?: LineChartData;
  // LMK event markers to overlay on charts.
  private lmkMarkers: LineChartMarker[] = [];
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

  private onDisconnect?: () => void;

  oncreate({attrs}: m.CVnodeDOM<LiveMemoryDashboardAttrs>) {
    this.app = attrs.app;
    this.device = attrs.device;
    this.deviceName = attrs.deviceName;
    this.linuxTarget = attrs.linuxTarget;
    this.onDisconnect = attrs.onDisconnect;
    this.startSession();
  }

  onremove() {
    this.stopSession();
    this.heapProfileSession?.cancel();
    this.disposeEngine();
  }

  view({attrs}: m.CVnode<LiveMemoryDashboardAttrs>) {
    this.app = attrs.app;
    this.onDisconnect = attrs.onDisconnect;
    return m('.pf-live-memory-page__container', this.renderPageContent());
  }

  private renderPageContent() {
    if (!this.isRecording) {
      return this.renderConnectedIdle();
    }
    if (this.heapProfilePid !== undefined) {
      return this.renderProfiling();
    }
    return this.renderRecording();
  }

  // ---------------------------------------------------------------------------
  // Connected but not recording — show start button.
  // ---------------------------------------------------------------------------

  private renderConnectedIdle(): m.Children {
    return m(
      '.pf-live-memory-page',
      m('.pf-live-memory-title-bar', m('h1', 'Memento')),
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

  private formatProfilingDuration(): string {
    if (this.heapProfileStartTime === undefined) return '';
    const elapsed = Math.floor((Date.now() - this.heapProfileStartTime) / 1000);
    const m = Math.floor(elapsed / 60);
    const s = elapsed % 60;
    if (m > 0) return `${m}m ${s}s`;
    return `${s}s`;
  }

  private renderProfiling(): m.Children {
    const processName = this.heapProfileProcessName ?? 'unknown';
    const pid = this.heapProfilePid!;
    const duration = this.formatProfilingDuration();
    return m(
      '.pf-live-memory-page',
      m(
        '.pf-live-memory-title-bar',
        m('h1', `Profiling: ${processName} (PID ${pid})`),
        duration && m('span.pf-live-memory-title-bar__duration', duration),
        m(
          '.pf-live-memory-title-bar__actions',
          !this.heapProfileStopping &&
            m(Button, {
              label: 'Stop & Open Trace',
              icon: 'stop',
              variant: ButtonVariant.Filled,
              intent: Intent.Primary,
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

      this.renderProcessBillboards(),

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

      m(
        '.pf-live-memory-profiling-status',
        m(
          '.pf-live-memory-profiling-status__line',
          m(Icon, {icon: 'memory'}),
          'Heap profiling (heapprofd) ',
          m('span.pf-live-memory-profiling-status__active', 'recording'),
        ),
        m(
          '.pf-live-memory-profiling-status__line',
          m(Icon, {icon: 'coffee'}),
          'Java heap profiling (java_hprof) ',
          m('span.pf-live-memory-profiling-status__active', 'recording'),
        ),
      ),
    );
  }

  private renderProcessBillboards(): m.Children {
    const data = this.heapProfileChartData;
    if (data === undefined || data.series.length === 0) return null;
    const latest = (name: string): number => {
      const s = data.series.find((sr) => sr.name === name);
      if (s === undefined || s.points.length === 0) return 0;
      return s.points[s.points.length - 1].y;
    };
    const base = this.heapProfileBaseline;

    const billboard = (
      current: number,
      baseline: number | undefined,
      label: string,
      desc: string,
    ) => {
      const delta = baseline !== undefined ? current - baseline : undefined;
      const deltaStr =
        delta !== undefined
          ? `${delta >= 0 ? '+' : ''}${formatKb(delta)}`
          : undefined;
      return m(
        '.pf-live-memory-billboard',
        m('.pf-live-memory-billboard__value', formatKb(current)),
        deltaStr !== undefined &&
          m(
            '.pf-live-memory-billboard__delta',
            {
              class:
                delta! > 0
                  ? 'pf-live-memory-billboard__delta--up'
                  : delta! < 0
                    ? 'pf-live-memory-billboard__delta--down'
                    : '',
            },
            deltaStr,
          ),
        m('.pf-live-memory-billboard__label', label),
        m('.pf-live-memory-billboard__desc', desc),
      );
    };

    return m(
      '.pf-live-memory-billboards',
      billboard(
        latest('Anon + Swap'),
        base?.anonSwap,
        'Anon + Swap',
        'Anonymous resident + swapped pages',
      ),
      billboard(
        latest('File'),
        base?.file,
        'File',
        'File-backed resident pages',
      ),
      billboard(
        latest('DMA-BUF'),
        base?.dmabuf,
        'DMA-BUF',
        'GPU/DMA buffer RSS',
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
        m('h1', 'Memento'),
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
        this.rawData?.androidBuildFingerprint &&
          m('', '\u00b7', this.rawData.androidBuildFingerprint),
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

      this.activeTab === 'processes' && this.renderProcessesTab(),
      this.activeTab === 'system' &&
        renderSystemTab({
          systemChartData: this.systemChartData,
          xAxisMin: this.xAxisMin,
          xAxisMax: this.xAxisMax,
        }),
      this.activeTab === 'file_cache' &&
        renderPageCacheTab({
          pageCacheChartData: this.pageCacheChartData,
          fileCacheBreakdownData: this.fileCacheBreakdownData,
          fileCacheActivityData: this.fileCacheActivityData,
          xAxisMin: this.xAxisMin,
          xAxisMax: this.xAxisMax,
        }),
      this.activeTab === 'pressure_swap' &&
        renderPressureSwapTab({
          psiChartData: this.psiChartData,
          pageFaultChartData: this.pageFaultChartData,
          swapChartData: this.swapChartData,
          vmstatChartData: this.vmstatChartData,
          lmkEvents: this.rawData?.lmkEvents ?? [],
          traceT0: this.traceT0 ?? 0,
          xAxisMin: this.xAxisMin,
          xAxisMax: this.xAxisMax,
        }),
    );
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

  private renderProcessesTab(): m.Children {
    return renderProcessesTab(
      {
        processGrouping: this.processGrouping,
        processMetric: this.processMetric,
        selectedCategory: this.selectedCategory,
        selectedOomBucket: this.selectedOomBucket,
        categoryChartData: this.categoryChartData,
        drilldownChartData: this.drilldownChartData,
        latestProcesses: this.latestProcesses,
        xAxisMin: this.xAxisMin,
        xAxisMax: this.xAxisMax,
        heapProfilePid: this.heapProfilePid,
        heapProfileProcessName: this.heapProfileProcessName,
        heapProfileStopping: this.heapProfileStopping,
        isUserDebug: this.rawData?.isUserDebug ?? false,
      },
      {
        onGroupingChange: (grouping) => {
          this.processGrouping = grouping;
          this.selectedCategory = undefined;
          this.selectedOomBucket = undefined;
          this.categoryChartData = undefined;
          this.drilldownChartData = undefined;
          if (this.rawData) {
            this.requeryGroupingChart();
          }
        },
        onMetricChange: (metric) => {
          this.processMetric = metric;
          this.categoryChartData = undefined;
          this.drilldownChartData = undefined;
          if (this.rawData) {
            this.requeryGroupingChart();
          }
        },
        onClearDrilldown: () => {
          this.selectedCategory = undefined;
          this.selectedOomBucket = undefined;
          this.drilldownChartData = undefined;
        },
        onSeriesClick: (seriesName) => {
          if (this.processGrouping === 'category') {
            this.drillDownToCategory(seriesName);
          } else {
            this.drillDownToOomBucket(seriesName);
          }
        },
        onStartProfile: (pid, processName) => {
          this.startHeapProfile(pid, processName);
        },
        onStopProfile: () => this.stopHeapProfile(),
        onCancelProfile: () => this.cancelHeapProfile(),
      },
    );
  }

  private activeProcessCounters(): readonly string[] {
    return PROCESS_METRIC_OPTIONS.find((o) => o.key === this.processMetric)!
      .counters;
  }

  private requeryGroupingChart() {
    if (this.rawData === undefined) return;
    const t0 = this.traceT0 ?? 0;
    const counter = this.activeProcessCounters();
    this.categoryChartData =
      this.processGrouping === 'category'
        ? this.buildCategoryTimeSeries(t0, counter)
        : this.buildOomScoreTimeSeries(t0, counter);
    if (this.processGrouping === 'category' && this.selectedCategory) {
      this.drilldownChartData = this.buildDrilldownTimeSeries(
        this.selectedCategory,
        t0,
        counter,
      );
    } else if (
      this.processGrouping === 'oom_score' &&
      this.selectedOomBucket !== undefined
    ) {
      this.drilldownChartData = this.buildOomDrilldownTimeSeries(
        this.selectedOomBucket,
        t0,
        counter,
      );
    }
    m.redraw();
  }

  private drillDownToCategory(seriesName: string) {
    const catIds = Object.keys(CATEGORIES) as CategoryId[];
    const id = catIds.find((k) => CATEGORIES[k].name === seriesName);
    if (id === undefined) return;
    this.selectedCategory = id;
    this.drilldownChartData = this.rawData
      ? this.buildDrilldownTimeSeries(
          id,
          this.traceT0 ?? 0,
          this.activeProcessCounters(),
        )
      : undefined;
    m.redraw();
  }

  private drillDownToOomBucket(seriesName: string) {
    const idx = OOM_SCORE_BUCKETS.findIndex((b) => b.name === seriesName);
    if (idx === -1) return;
    this.selectedOomBucket = idx;
    this.drilldownChartData = this.rawData
      ? this.buildOomDrilldownTimeSeries(
          idx,
          this.traceT0 ?? 0,
          this.activeProcessCounters(),
        )
      : undefined;
    m.redraw();
  }

  // ---------------------------------------------------------------------------
  // Heap profiling
  // ---------------------------------------------------------------------------

  private async startHeapProfile(pid: number, processName: string) {
    if (!this.device || this.heapProfileSession) return;

    this.heapProfilePid = pid;
    this.heapProfileProcessName = processName;
    this.heapProfileStartTime = Date.now();
    this.heapProfileBaseline = undefined;
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
    this.heapProfileStartTime = undefined;
    this.heapProfileBaseline = undefined;
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
    this.heapProfileStartTime = undefined;
    this.heapProfileBaseline = undefined;

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

  private disconnect() {
    this.stopSession();
    this.onDisconnect?.();
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

    this.psiChartData = undefined;
    this.swapChartData = undefined;
    this.vmstatChartData = undefined;
    this.latestProcesses = undefined;
    this.selectedCategory = undefined;
    this.selectedOomBucket = undefined;
    this.drilldownChartData = undefined;
    this.lmkMarkers = [];
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

    this.psiChartData = undefined;
    this.swapChartData = undefined;
    this.vmstatChartData = undefined;
    this.latestProcesses = undefined;
    this.selectedCategory = undefined;
    this.selectedOomBucket = undefined;
    this.drilldownChartData = undefined;
    this.lmkMarkers = [];
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

  private async extractRawData(
    engine: WasmEngineProxy,
  ): Promise<SnapshotRawData> {
    const [sysResult, procResult, metaResult, lmkResult, metadata] =
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
        // LMK events: lmkd writes atrace slices named "lmk,<pid>,<reason>,<oom>".
        // Also check the legacy kill_one_process counter.
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
        SELECT extract_metadata('android_build_fingerprint') AS androidBuildFingerprint
      `),
      ]);

    // Build system counters map.
    const systemCounters = new Map<string, {ts: number; value: number}[]>();
    const sysIter = sysResult.iter({
      counter_name: STR,
      ts: NUM,
      value: NUM,
    });
    for (; sysIter.valid(); sysIter.next()) {
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
    const processCountersByPid = new Map<
      number,
      Map<string, {ts: number; value: number}[]>
    >();
    const procIter = procResult.iter({
      process_name: STR,
      pid: NUM,
      counter_name: STR,
      ts: NUM,
      value: NUM,
    });
    for (; procIter.valid(); procIter.next()) {
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
    const processInfo = new Map<
      string,
      {pid: number; startTs: number | null; debuggable: boolean}
    >();
    const metaIter = metaResult.iter({
      name: STR,
      pid: NUM,
      start_ts: NUM_NULL,
      debuggable: NUM,
    });
    for (; metaIter.valid(); metaIter.next()) {
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
    for (; lmkIter.valid(); lmkIter.next()) {
      lmkEvents.push({
        ts: lmkIter.ts,
        pid: lmkIter.pid,
        processName: lmkIter.process_name,
        oomScoreAdj: lmkIter.oom_score_adj,
      });
    }

    const androidBuildFingerprint = metadata.maybeFirstRow({
      androidBuildFingerprint: STR_NULL,
    })?.androidBuildFingerprint;

    const isUserDebug = androidBuildFingerprint?.includes('userdebug');

    return {
      systemCounters,
      processCountersByName,
      processCountersByPid,
      processInfo,
      lmkEvents,
      isUserDebug: isUserDebug ?? false,
      androidBuildFingerprint: androidBuildFingerprint ?? undefined,
    };
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

      // Extract all raw data from SQL into JS data structures.
      this.rawData = await this.extractRawData(engine);

      // Compute shared time origin on first snapshot, reuse for all
      // subsequent snapshots so x-axes stay synced across charts.
      if (this.traceT0 === undefined) {
        let minTs = Infinity;
        for (const arr of this.rawData.systemCounters.values()) {
          if (arr.length > 0 && arr[0].ts < minTs) minTs = arr[0].ts;
        }
        for (const counterMap of this.rawData.processCountersByName.values()) {
          for (const byTs of counterMap.values()) {
            const firstTs = byTs.keys().next().value;
            if (firstTs !== undefined && firstTs < minTs) minTs = firstTs;
          }
        }
        if (minTs < Infinity) this.traceT0 = minTs;
      }
      const t0 = this.traceT0 ?? 0;

      // Build LMK markers from raw events.
      this.lmkMarkers = this.rawData.lmkEvents.map((ev) => {
        const label = ev.processName
          ? `LMK: ${ev.processName} (${ev.pid})`
          : `LMK: pid ${ev.pid}`;
        return {x: (ev.ts - t0) / 1e9, label, color: '#e53935'};
      });

      // Build all chart data from raw data (synchronous).
      this.systemChartData = this.addLmkMarkers(this.buildSystemTimeSeries(t0));
      this.pageCacheChartData = this.buildPageCacheTimeSeries(t0);
      this.fileCacheBreakdownData = this.buildFileCacheBreakdownTimeSeries(t0);
      this.fileCacheActivityData = this.buildFileCacheActivityTimeSeries(t0);
      this.categoryChartData =
        this.processGrouping === 'category'
          ? this.buildCategoryTimeSeries(t0, this.activeProcessCounters())
          : this.buildOomScoreTimeSeries(t0, this.activeProcessCounters());
      this.psiChartData = this.addLmkMarkers(this.buildPsiTimeSeries(t0));
      this.swapChartData = this.buildSwapTimeSeries(t0);
      this.vmstatChartData = this.buildVmstatTimeSeries(t0);
      this.pageFaultChartData = this.buildPageFaultTimeSeries(t0);

      if (this.selectedCategory) {
        this.drilldownChartData = this.buildDrilldownTimeSeries(
          this.selectedCategory,
          t0,
          this.activeProcessCounters(),
        );
      } else if (this.selectedOomBucket !== undefined) {
        this.drilldownChartData = this.buildOomDrilldownTimeSeries(
          this.selectedOomBucket,
          t0,
          this.activeProcessCounters(),
        );
      }

      this.latestProcesses = this.buildLatestProcessMemory();

      if (this.heapProfilePid !== undefined) {
        this.heapProfileChartData = this.buildProcessMemoryBreakdown(
          this.heapProfilePid,
          t0,
        );
        // Capture baseline from the first data point on the first snapshot.
        if (
          this.heapProfileBaseline === undefined &&
          this.heapProfileChartData !== undefined
        ) {
          const first = (name: string): number => {
            const s = this.heapProfileChartData!.series.find(
              (sr) => sr.name === name,
            );
            return s !== undefined && s.points.length > 0 ? s.points[0].y : 0;
          };
          this.heapProfileBaseline = {
            anonSwap: first('Anon + Swap'),
            file: first('File'),
            dmabuf: first('DMA-BUF'),
          };
        }
      }

      this.computeSharedXRange();
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
  // LMK marker injection helper.
  // ---------------------------------------------------------------------------

  private addLmkMarkers(
    data: LineChartData | undefined,
  ): LineChartData | undefined {
    if (data === undefined || this.lmkMarkers.length === 0) return data;
    return {...data, markers: this.lmkMarkers};
  }

  // ---------------------------------------------------------------------------
  // System memory: full time-series from TP, decomposed for stacking.
  // ---------------------------------------------------------------------------

  private buildSystemTimeSeries(t0: number): LineChartData | undefined {
    const raw = this.rawData!;
    // Build Map<ts, Map<counterName, valueKb>> from raw system counters.
    const meminfoByTs = new Map<number, Map<string, number>>();
    for (const name of SYSTEM_MEMINFO_COUNTERS) {
      const samples = raw.systemCounters.get(name);
      if (samples === undefined) continue;
      for (const {ts, value} of samples) {
        let row = meminfoByTs.get(ts);
        if (row === undefined) {
          row = new Map();
          meminfoByTs.set(ts, row);
        }
        row.set(name, Math.round(value / 1024));
      }
    }

    if (meminfoByTs.size < 2) return undefined;

    // DMA-BUF heap (from system counters).
    const dmaSamplesRaw = raw.systemCounters.get('mem.dma_heap') ?? [];
    const dmaSamples = dmaSamplesRaw.map((s) => ({
      ts: s.ts,
      kb: Math.round(s.value / 1024),
    }));
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

  private buildPageCacheTimeSeries(t0: number): LineChartData | undefined {
    const raw = this.rawData!;
    const counterNames = ['Cached', 'Shmem', 'Active(file)', 'Inactive(file)'];
    const byTs = new Map<number, Map<string, number>>();
    for (const name of counterNames) {
      const samples = raw.systemCounters.get(name);
      if (samples === undefined) continue;
      for (const {ts, value} of samples) {
        let row = byTs.get(ts);
        if (row === undefined) {
          row = new Map();
          byTs.set(ts, row);
        }
        row.set(name, Math.round(value / 1024));
      }
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

  private buildFileCacheBreakdownTimeSeries(
    t0: number,
  ): LineChartData | undefined {
    const raw = this.rawData!;
    const counterNames = ['Active(file)', 'Inactive(file)', 'Mapped', 'Dirty'];
    const byTs = new Map<number, Map<string, number>>();
    for (const name of counterNames) {
      const samples = raw.systemCounters.get(name);
      if (samples === undefined) continue;
      for (const {ts, value} of samples) {
        let row = byTs.get(ts);
        if (row === undefined) {
          row = new Map();
          byTs.set(ts, row);
        }
        row.set(name, Math.round(value / 1024));
      }
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

  private buildCategoryTimeSeries(
    t0: number,
    counters: readonly string[] = ['mem.rss'],
  ): LineChartData | undefined {
    const raw = this.rawData!;
    const catIds = Object.keys(CATEGORIES) as CategoryId[];
    const tsSet = new Set<number>();
    // Map<ts, Map<CategoryId, sumKb>>
    const byCatTs = new Map<number, Map<CategoryId, number>>();

    for (const [processName, counterMap] of raw.processCountersByName) {
      const cat = categorizeProcess(processName);
      const id = catIds.find((k) => CATEGORIES[k].name === cat.name)!;

      // Collect all timestamps for this process across the specified
      // counters, summing counter values at each ts.
      const tsSums = new Map<number, number>();
      for (const counterName of counters) {
        const byTs = counterMap.get(counterName);
        if (byTs === undefined) continue;
        for (const [ts, value] of byTs) {
          tsSums.set(ts, (tsSums.get(ts) ?? 0) + value);
        }
      }

      for (const [ts, sumBytes] of tsSums) {
        tsSet.add(ts);
        let catMap = byCatTs.get(ts);
        if (catMap === undefined) {
          catMap = new Map();
          byCatTs.set(ts, catMap);
        }
        catMap.set(id, (catMap.get(id) ?? 0) + Math.round(sumBytes / 1024));
      }
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

  private buildDrilldownTimeSeries(
    categoryId: CategoryId,
    t0: number = 0,
    counters: readonly string[] = ['mem.rss'],
  ): LineChartData | undefined {
    const raw = this.rawData!;
    const targetCat = CATEGORIES[categoryId];

    // Collect timestamps and per-process RSS for matching processes.
    const tsSet = new Set<number>();
    // Map<ts, Map<processName, sumKb>> (sum handles multiple PIDs per name)
    const byProcTs = new Map<number, Map<string, number>>();

    for (const [processName, counterMap] of raw.processCountersByName) {
      const cat = categorizeProcess(processName);
      if (cat.name !== targetCat.name) continue;

      // Sum counter values at each ts.
      const tsSums = new Map<number, number>();
      for (const counterName of counters) {
        const byTs = counterMap.get(counterName);
        if (byTs === undefined) continue;
        for (const [ts, value] of byTs) {
          tsSums.set(ts, (tsSums.get(ts) ?? 0) + value);
        }
      }

      for (const [ts, sumBytes] of tsSums) {
        tsSet.add(ts);
        let procMap = byProcTs.get(ts);
        if (procMap === undefined) {
          procMap = new Map();
          byProcTs.set(ts, procMap);
        }
        procMap.set(
          processName,
          (procMap.get(processName) ?? 0) + Math.round(sumBytes / 1024),
        );
      }
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

  private buildOomScoreTimeSeries(
    t0: number,
    counters: readonly string[] = ['mem.rss'],
  ): LineChartData | undefined {
    const raw = this.rawData!;

    // 1. Get latest OOM score per process name from raw data.
    const oomByName = new Map<string, number>();
    for (const [processName, counterMap] of raw.processCountersByName) {
      const oomTs = counterMap.get('oom_score_adj');
      if (oomTs === undefined || oomTs.size === 0) continue;
      // Get latest value (last entry in the Map iteration order, which is
      // insertion order = sorted by ts since the SQL was ORDER BY c.ts).
      let lastVal = 0;
      for (const val of oomTs.values()) {
        lastVal = val;
      }
      oomByName.set(processName, lastVal);
    }

    // 2. Group by OOM bucket at each timestamp.
    const tsSet = new Set<number>();
    const byBucketTs = new Map<number, Map<number, number>>();

    for (const [processName, counterMap] of raw.processCountersByName) {
      // Sum counter values at each ts.
      const tsSums = new Map<number, number>();
      for (const counterName of counters) {
        const byTs = counterMap.get(counterName);
        if (byTs === undefined) continue;
        for (const [ts, value] of byTs) {
          tsSums.set(ts, (tsSums.get(ts) ?? 0) + value);
        }
      }

      const oomScore = oomByName.get(processName) ?? 0;
      const bucketIdx = OOM_SCORE_BUCKETS.findIndex(
        (b) => oomScore >= b.minScore && oomScore <= b.maxScore,
      );
      const idx = bucketIdx !== -1 ? bucketIdx : OOM_SCORE_BUCKETS.length - 1;

      for (const [ts, sumBytes] of tsSums) {
        tsSet.add(ts);
        let bucketMap = byBucketTs.get(ts);
        if (bucketMap === undefined) {
          bucketMap = new Map();
          byBucketTs.set(ts, bucketMap);
        }
        bucketMap.set(
          idx,
          (bucketMap.get(idx) ?? 0) + Math.round(sumBytes / 1024),
        );
      }
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

  private buildOomDrilldownTimeSeries(
    bucketIdx: number,
    t0: number = 0,
    counters: readonly string[] = ['mem.rss'],
  ): LineChartData | undefined {
    const raw = this.rawData!;
    const bucket = OOM_SCORE_BUCKETS[bucketIdx];

    // 1. Get latest OOM score per process name and find matching names.
    const matchingNames = new Set<string>();
    for (const [processName, counterMap] of raw.processCountersByName) {
      const oomTs = counterMap.get('oom_score_adj');
      let oomScore = 0;
      if (oomTs !== undefined && oomTs.size > 0) {
        for (const val of oomTs.values()) {
          oomScore = val;
        }
      }
      if (oomScore >= bucket.minScore && oomScore <= bucket.maxScore) {
        matchingNames.add(processName);
      }
    }

    // 2. Collect per-process data for matching processes.
    const tsSet = new Set<number>();
    const byProcTs = new Map<number, Map<string, number>>();

    for (const [processName, counterMap] of raw.processCountersByName) {
      if (!matchingNames.has(processName)) continue;

      const tsSums = new Map<number, number>();
      for (const counterName of counters) {
        const byTs = counterMap.get(counterName);
        if (byTs === undefined) continue;
        for (const [ts, value] of byTs) {
          tsSums.set(ts, (tsSums.get(ts) ?? 0) + value);
        }
      }

      for (const [ts, sumBytes] of tsSums) {
        tsSet.add(ts);
        let procMap = byProcTs.get(ts);
        if (procMap === undefined) {
          procMap = new Map();
          byProcTs.set(ts, procMap);
        }
        procMap.set(
          processName,
          (procMap.get(processName) ?? 0) + Math.round(sumBytes / 1024),
        );
      }
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

  private buildPsiTimeSeries(t0: number): LineChartData | undefined {
    const raw = this.rawData!;
    const someRaw = raw.systemCounters.get('psi.mem.some');
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
    const fullRaw = raw.systemCounters.get('psi.mem.full');
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

  private buildSwapTimeSeries(t0: number): LineChartData | undefined {
    const raw = this.rawData!;
    const byTs = new Map<number, Map<string, number>>();
    for (const name of ['SwapTotal', 'SwapFree', 'SwapCached']) {
      const samples = raw.systemCounters.get(name);
      if (samples === undefined) continue;
      for (const {ts, value} of samples) {
        let row = byTs.get(ts);
        if (row === undefined) {
          row = new Map();
          byTs.set(ts, row);
        }
        row.set(name, Math.round(value / 1024));
      }
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

  private buildVmstatTimeSeries(t0: number): LineChartData | undefined {
    const raw = this.rawData!;
    const pswpinRaw = raw.systemCounters.get('pswpin');
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
    const pswpoutRaw = raw.systemCounters.get('pswpout');
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

  private buildPageFaultTimeSeries(t0: number): LineChartData | undefined {
    const raw = this.rawData!;
    const pgfaultRaw = raw.systemCounters.get('pgfault');
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
    const pgmajfaultRaw = raw.systemCounters.get('pgmajfault');
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

  private buildFileCacheActivityTimeSeries(
    t0: number,
  ): LineChartData | undefined {
    const raw = this.rawData!;

    // Convert cumulative counts to rate (events/second).
    const toRatePoints = (
      samples: {ts: number; value: number}[],
    ): {x: number; y: number}[] => {
      const points: {x: number; y: number}[] = [];
      for (let i = 1; i < samples.length; i++) {
        const dtS = (samples[i].ts - samples[i - 1].ts) / 1e9;
        if (dtS <= 0) continue;
        const delta = samples[i].value - samples[i - 1].value;
        points.push({
          x: (samples[i].ts - t0) / 1e9,
          y: Math.max(0, delta / dtS),
        });
      }
      return points;
    };

    const series: LineChartSeries[] = [];
    const refaultRaw = raw.systemCounters.get('workingset_refault_file');
    if (refaultRaw !== undefined && refaultRaw.length >= 2) {
      series.push({
        name: 'Refaults (thrashing)',
        points: toRatePoints(refaultRaw),
        color: '#e74c3c',
      });
    }
    const stealRaw = raw.systemCounters.get('pgsteal_file');
    if (stealRaw !== undefined && stealRaw.length >= 2) {
      series.push({
        name: 'Stolen (reclaimed)',
        points: toRatePoints(stealRaw),
        color: '#f39c12',
      });
    }
    const scanRaw = raw.systemCounters.get('pgscan_file');
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
  // Per-process memory breakdown (for profiling view).
  // ---------------------------------------------------------------------------

  private buildProcessMemoryBreakdown(
    pid: number,
    t0: number,
  ): LineChartData | undefined {
    const raw = this.rawData!;
    const pidCounters = raw.processCountersByPid.get(pid);
    if (pidCounters === undefined) return undefined;

    const tsSet = new Set<number>();
    const bySeriesTs = new Map<number, Map<string, number>>();
    const SERIES_NAMES = ['Anon + Swap', 'File', 'DMA-BUF'] as const;
    const counterMapping: Record<string, string> = {
      'mem.rss.anon': 'Anon + Swap',
      'mem.swap': 'Anon + Swap',
      'mem.rss.file': 'File',
      'mem.dmabuf_rss': 'DMA-BUF',
    };

    for (const [counterName, samples] of pidCounters) {
      const seriesName = counterMapping[counterName];
      if (seriesName === undefined) continue;
      for (const {ts, value} of samples) {
        tsSet.add(ts);
        let seriesMap = bySeriesTs.get(ts);
        if (seriesMap === undefined) {
          seriesMap = new Map();
          bySeriesTs.set(ts, seriesMap);
        }
        const kb = Math.round(value / 1024);
        seriesMap.set(seriesName, (seriesMap.get(seriesName) ?? 0) + kb);
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

  private buildLatestProcessMemory(): ProcessMemoryRow[] {
    const raw = this.rawData!;
    const rows: ProcessMemoryRow[] = [];

    // Find max ts across all process counters (for age computation).
    let maxTs = 0;
    for (const counterMap of raw.processCountersByName.values()) {
      for (const byTs of counterMap.values()) {
        for (const ts of byTs.keys()) {
          if (ts > maxTs) maxTs = ts;
        }
      }
    }

    for (const [processName, counterMap] of raw.processCountersByName) {
      const info = raw.processInfo.get(processName);
      const pid = info?.pid ?? 0;

      const getLatestRaw = (counterName: string): number => {
        const byTs = counterMap.get(counterName);
        if (byTs === undefined || byTs.size === 0) return 0;
        let latestTs = 0;
        let latestValue = 0;
        for (const [ts, value] of byTs) {
          if (ts >= latestTs) {
            latestTs = ts;
            latestValue = value;
          }
        }
        return latestValue;
      };

      const rssKb = Math.round(getLatestRaw('mem.rss') / 1024);
      if (rssKb === 0) continue; // Skip processes with no RSS data

      rows.push({
        processName,
        pid,
        rssKb,
        anonKb: Math.round(getLatestRaw('mem.rss.anon') / 1024),
        fileKb: Math.round(getLatestRaw('mem.rss.file') / 1024),
        shmemKb: Math.round(getLatestRaw('mem.rss.shmem') / 1024),
        swapKb: Math.round(getLatestRaw('mem.swap') / 1024),
        dmabufKb: Math.round(getLatestRaw('mem.dmabuf_rss') / 1024),
        oomScore: getLatestRaw('oom_score_adj'),
        debuggable: info?.debuggable ?? false,
        ageSeconds:
          info?.startTs !== null && info?.startTs !== undefined
            ? (maxTs - info.startTs) / 1e9
            : null,
      });
    }

    // Sort by RSS descending.
    rows.sort((a, b) => b.rssKb - a.rssKb);
    return rows;
  }
}
