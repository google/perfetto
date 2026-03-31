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
import {
  LineChart,
  LineChartData,
} from '../../components/widgets/charts/line_chart';
import {DataGrid} from '../../components/widgets/datagrid/datagrid';
import type {SchemaRegistry} from '../../components/widgets/datagrid/datagrid_schema';
import type {Row} from '../../trace_processor/query_result';
import {Button, ButtonVariant} from '../../widgets/button';
import {Intent} from '../../widgets/common';
import {SegmentedButtons} from '../../widgets/segmented_buttons';
import {
  categorizeProcess,
  CATEGORIES,
  type CategoryId,
} from './process_categories';
import {formatKb} from './utils';

export type ProcessGrouping = 'category' | 'oom_score';

export type ProcessMetric = 'rss' | 'anon_swap' | 'file' | 'dmabuf';

export const PROCESS_METRIC_OPTIONS: ReadonlyArray<{
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

export interface OomScoreBucket {
  readonly name: string;
  readonly color: string;
  readonly minScore: number;
  readonly maxScore: number;
}

export const OOM_SCORE_BUCKETS: readonly OomScoreBucket[] = [
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

// Build a lookup from category name -> color for the cell renderer.
const CATEGORY_COLOR_MAP = new Map<string, string>(
  (Object.values(CATEGORIES) as readonly {name: string; color: string}[]).map(
    (c) => [c.name, c.color],
  ),
);

export const PROCESS_TABLE_SCHEMA: SchemaRegistry = {
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
    debuggable: {
      title: 'Debuggable',
      columnType: 'text',
      cellRenderer: (v) => (v as string) || '',
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
export interface ProcessMemoryRow {
  processName: string;
  pid: number;
  rssKb: number;
  anonKb: number;
  fileKb: number;
  shmemKb: number;
  swapKb: number;
  dmabufKb: number;
  oomScore: number;
  debuggable: boolean;
  ageSeconds: number | null;
}

export interface ProcessesTabData {
  processGrouping: ProcessGrouping;
  processMetric: ProcessMetric;
  selectedCategory?: CategoryId;
  selectedOomBucket?: number;
  categoryChartData?: LineChartData;
  drilldownChartData?: LineChartData;
  latestProcesses?: ProcessMemoryRow[];
  xAxisMin?: number;
  xAxisMax?: number;
  heapProfilePid?: number;
  heapProfileProcessName?: string;
  heapProfileStopping: boolean;
  isUserDebug: boolean;
}

export interface ProcessesTabCallbacks {
  onGroupingChange: (grouping: ProcessGrouping) => void;
  onMetricChange: (metric: ProcessMetric) => void;
  onClearDrilldown: () => void;
  onSeriesClick: (seriesName: string) => void;
  onStartProfile: (pid: number, processName: string) => void;
  onStopProfile: () => void;
  onCancelProfile: () => void;
}

export function renderProcessesTab(
  data: ProcessesTabData,
  callbacks: ProcessesTabCallbacks,
): m.Children {
  const isDrilledDown =
    data.processGrouping === 'category'
      ? data.selectedCategory !== undefined
      : data.selectedOomBucket !== undefined;

  const cat = data.selectedCategory
    ? CATEGORIES[data.selectedCategory]
    : undefined;
  const oomBucket =
    data.selectedOomBucket !== undefined
      ? OOM_SCORE_BUCKETS[data.selectedOomBucket]
      : undefined;

  const chartData = isDrilledDown
    ? data.drilldownChartData
    : data.categoryChartData;

  const processes = data.latestProcesses
    ? data.processGrouping === 'category' && data.selectedCategory
      ? data.latestProcesses.filter(
          (p) => categorizeProcess(p.processName).name === cat!.name,
        )
      : data.processGrouping === 'oom_score' && oomBucket
        ? data.latestProcesses.filter(
            (p) =>
              p.oomScore >= oomBucket.minScore &&
              p.oomScore <= oomBucket.maxScore,
          )
        : data.latestProcesses
    : undefined;

  const metricInfo = PROCESS_METRIC_OPTIONS.find(
    (o) => o.key === data.processMetric,
  )!;
  const drillName = cat?.name ?? oomBucket?.name;
  const groupLabel =
    data.processGrouping === 'category' ? 'Category' : 'OOM Score';
  const title = drillName
    ? `Process Memory: ${drillName}`
    : `Process Memory by ${groupLabel}`;
  const subtitle = drillName
    ? `Stacked ${metricInfo.label} per process. Totals may exceed actual memory usage because RSS counts shared pages (COW, mmap) in every process that maps them.`
    : `Stacked ${metricInfo.label} per ${groupLabel.toLowerCase()}. Click a ${groupLabel.toLowerCase()} to drill into individual processes.`;

  // Compute billboard totals from all processes.
  const allProcs = data.latestProcesses ?? [];
  const totalAnonSwapKb = allProcs.reduce((s, p) => s + p.anonKb + p.swapKb, 0);
  const totalFileKb = allProcs.reduce((s, p) => s + p.fileKb, 0);
  const totalDmabufKb = allProcs.reduce((s, p) => s + p.dmabufKb, 0);
  const hasBillboards = allProcs.length > 0;

  const bilboards =
    hasBillboards &&
    m(
      '.pf-memento-billboards',
      m(
        '.pf-memento-billboard',
        m('.pf-memento-billboard__value', formatKb(totalAnonSwapKb)),
        m('.pf-memento-billboard__label', 'Anon + Swap'),
        m(
          '.pf-memento-billboard__desc',
          'Sum of anonymous RSS + swap across all processes',
        ),
      ),
      m(
        '.pf-memento-billboard',
        m('.pf-memento-billboard__value', formatKb(totalFileKb)),
        m('.pf-memento-billboard__label', 'File'),
        m(
          '.pf-memento-billboard__desc',
          'Sum of file-backed RSS across all processes',
        ),
      ),
      m(
        '.pf-memento-billboard',
        m('.pf-memento-billboard__value', formatKb(totalDmabufKb)),
        m('.pf-memento-billboard__label', 'DMA-BUF'),
        m(
          '.pf-memento-billboard__desc',
          'Sum of DMA-BUF heap RSS across all processes',
        ),
      ),
    );

  return [
    bilboards,
    m(
      '.pf-memento-panel',
      m(
        '.pf-memento-panel__header',
        m(
          '.pf-memento-panel__title-row',
          isDrilledDown &&
            m(Button, {
              icon: 'arrow_back',
              label:
                data.processGrouping === 'category'
                  ? 'All categories'
                  : 'All OOM buckets',
              minimal: true,
              onclick: () => callbacks.onClearDrilldown(),
            }),
          m('h2', title),
        ),
        m('p', subtitle),
        m(
          '.pf-memento-panel__controls',
          m(SegmentedButtons, {
            options: [{label: 'By Category'}, {label: 'By OOM Score'}],
            selectedOption: data.processGrouping === 'category' ? 0 : 1,
            onOptionSelected: (i: number) => {
              const newGrouping: ProcessGrouping =
                i === 0 ? 'category' : 'oom_score';
              if (newGrouping === data.processGrouping) return;
              callbacks.onGroupingChange(newGrouping);
            },
          }),
          m(SegmentedButtons, {
            options: PROCESS_METRIC_OPTIONS.map((o) => ({label: o.label})),
            selectedOption: PROCESS_METRIC_OPTIONS.findIndex(
              (o) => o.key === data.processMetric,
            ),
            onOptionSelected: (i: number) => {
              const newMetric = PROCESS_METRIC_OPTIONS[i].key;
              if (newMetric === data.processMetric) return;
              callbacks.onMetricChange(newMetric);
            },
          }),
        ),
      ),
      m(
        '.pf-memento-panel__body',
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
              xAxisMin: data.xAxisMin,
              xAxisMax: data.xAxisMax,
              onSeriesClick: isDrilledDown
                ? undefined
                : (seriesName: string) => callbacks.onSeriesClick(seriesName),
            })
          : m('.pf-memento-placeholder', 'Waiting for data\u2026'),
        processes &&
          renderProcessTable(
            processes,
            data.heapProfilePid,
            data.heapProfileProcessName,
            data.heapProfileStopping,
            data.isUserDebug,
            callbacks,
          ),
      ),
    ),
  ];
}

function renderProcessTable(
  processes: ProcessMemoryRow[],
  heapProfilePid: number | undefined,
  heapProfileProcessName: string | undefined,
  heapProfileStopping: boolean,
  isUserDebug: boolean,
  callbacks: ProcessesTabCallbacks,
): m.Children {
  const rows: Row[] = processes.map((p) => {
    const cat = categorizeProcess(p.processName);
    const debugLabel = p.debuggable
      ? 'Yes'
      : isUserDebug
        ? 'Yes (userdebug)'
        : '';
    return {
      process: p.processName,
      category: cat.name,
      pid: p.pid,
      oom_score: p.oomScore,
      debuggable: debugLabel,
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
        columnType: 'text',
        cellRenderer: (_v, row) => {
          const pid = row.pid as number;
          return m(Button, {
            label: 'Profile',
            icon: 'science',
            minimal: true,
            intent: Intent.Primary,
            variant: ButtonVariant.Filled,
            className: 'pf-memento-profile-btn',
            onclick: () => {
              callbacks.onStartProfile(pid, row.process as string);
            },
          });
        },
      },
    },
  };

  return [
    heapProfilePid !== undefined &&
      m(
        '.pf-memento-status-bar',
        m('.pf-memento-status-bar__dot'),
        heapProfileStopping
          ? `Stopping and reading trace for ${heapProfileProcessName}\u2026`
          : `Recording heap profile for ${heapProfileProcessName} (PID ${heapProfilePid})`,
        !heapProfileStopping && [
          m(Button, {
            label: 'Stop & Open',
            icon: 'stop',
            minimal: true,
            intent: Intent.Danger,
            onclick: () => callbacks.onStopProfile(),
          }),
          m(Button, {
            label: 'Cancel',
            icon: 'close',
            minimal: true,
            onclick: () => callbacks.onCancelProfile(),
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
        {id: 'debuggable', field: 'debuggable', sort: undefined},
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
