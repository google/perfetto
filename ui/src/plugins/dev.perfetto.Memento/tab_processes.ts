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
  type LineChartData,
  type LineChartSeries,
} from '../../components/widgets/charts/line_chart';
import {DataGrid} from '../../components/widgets/datagrid/datagrid';
import type {SchemaRegistry} from '../../components/widgets/datagrid/datagrid_schema';
import type {Row} from '../../trace_processor/query_result';
import {Button, ButtonVariant} from '../../widgets/button';
import {Intent} from '../../widgets/common';
import {SegmentedButtons} from '../../widgets/segmented_buttons';
import {MementoSession, type SnapshotData} from './memento_session';
import {
  categorizeProcess,
  CATEGORIES,
  type CategoryId,
} from './process_categories';
import {computeT0, formatKb} from './utils';
import {
  type ProcessGrouping,
  type ProcessMetric,
  type ProcessMemoryRow,
  PROCESS_METRIC_OPTIONS,
  OOM_SCORE_BUCKETS,
} from './process_data';

export type {ProcessGrouping, ProcessMetric, ProcessMemoryRow};
export {PROCESS_METRIC_OPTIONS, OOM_SCORE_BUCKETS};

// ---------------------------------------------------------------------------
// Chart builders (inlined from chart_builders.ts)
// ---------------------------------------------------------------------------

function buildCategoryTimeSeries(
  data: SnapshotData,
  t0: number,
  counters: readonly string[],
): LineChartData | undefined {
  const catIds = Object.keys(CATEGORIES) as CategoryId[];
  const tsSet = new Set<number>();
  const byCatTs = new Map<number, Map<CategoryId, number>>();
  for (const [processName, counterMap] of data.processCountersByName) {
    const cat = categorizeProcess(processName);
    const id = catIds.find((k) => CATEGORIES[k].name === cat.name)!;
    const tsSums = new Map<number, number>();
    for (const counterName of counters) {
      const byTs = counterMap.get(counterName);
      if (byTs === undefined) continue;
      for (const [ts, value] of byTs)
        tsSums.set(ts, (tsSums.get(ts) ?? 0) + value);
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
  const pointsByCategory = new Map<CategoryId, {x: number; y: number}[]>();
  for (const id of catIds) pointsByCategory.set(id, []);
  for (const ts of timestamps) {
    const x = (ts - t0) / 1e9;
    const catMap = byCatTs.get(ts)!;
    for (const id of catIds)
      pointsByCategory.get(id)!.push({x, y: catMap.get(id) ?? 0});
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

function buildOomScoreTimeSeries(
  data: SnapshotData,
  t0: number,
  counters: readonly string[],
): LineChartData | undefined {
  const oomByName = new Map<string, number>();
  for (const [processName, counterMap] of data.processCountersByName) {
    const oomTs = counterMap.get('oom_score_adj');
    if (oomTs === undefined || oomTs.size === 0) continue;
    let lastVal = 0;
    for (const val of oomTs.values()) lastVal = val;
    oomByName.set(processName, lastVal);
  }
  const tsSet = new Set<number>();
  const byBucketTs = new Map<number, Map<number, number>>();
  for (const [processName, counterMap] of data.processCountersByName) {
    const tsSums = new Map<number, number>();
    for (const counterName of counters) {
      const byTs = counterMap.get(counterName);
      if (byTs === undefined) continue;
      for (const [ts, value] of byTs)
        tsSums.set(ts, (tsSums.get(ts) ?? 0) + value);
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
  const pointsByBucket = new Map<number, {x: number; y: number}[]>();
  for (let i = 0; i < OOM_SCORE_BUCKETS.length; i++) pointsByBucket.set(i, []);
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

function buildProcessDrilldown(
  data: SnapshotData,
  t0: number,
  counters: readonly string[],
  filter: (processName: string) => boolean,
): LineChartData | undefined {
  const tsSet = new Set<number>();
  const byProcTs = new Map<number, Map<string, number>>();
  for (const [processName, counterMap] of data.processCountersByName) {
    if (!filter(processName)) continue;
    const tsSums = new Map<number, number>();
    for (const counterName of counters) {
      const byTs = counterMap.get(counterName);
      if (byTs === undefined) continue;
      for (const [ts, value] of byTs)
        tsSums.set(ts, (tsSums.get(ts) ?? 0) + value);
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
    for (const name of procMap.keys()) allNames.add(name);
  }
  const pointsByProc = new Map<string, {x: number; y: number}[]>();
  for (const name of allNames) pointsByProc.set(name, []);
  for (const ts of timestamps) {
    const x = (ts - t0) / 1e9;
    const procMap = byProcTs.get(ts)!;
    for (const name of allNames)
      pointsByProc.get(name)!.push({x, y: procMap.get(name) ?? 0});
  }
  const ranked = [...allNames]
    .map((name) => ({
      name,
      points: pointsByProc.get(name)!,
      total: pointsByProc.get(name)!.reduce((s, p) => s + p.y, 0),
    }))
    .sort((a, b) => b.total - a.total);
  const TOP_N = 15;
  const top = ranked.slice(0, TOP_N);
  const rest = ranked.slice(TOP_N);
  const series: LineChartSeries[] = top.map((r) => ({
    name: r.name,
    points: r.points,
  }));
  if (rest.length > 0) {
    const otherPoints = timestamps.map((ts, i) => ({
      x: (ts - t0) / 1e9,
      y: rest.reduce((sum, r) => sum + r.points[i].y, 0),
    }));
    series.push({
      name: `Other (${rest.length} processes)`,
      points: otherPoints,
      color: '#999',
    });
  }
  if (series.length === 0) return undefined;
  return {series};
}

function buildCategoryDrilldown(
  data: SnapshotData,
  categoryId: CategoryId,
  t0: number,
  counters: readonly string[],
): LineChartData | undefined {
  const targetCat = CATEGORIES[categoryId];
  return buildProcessDrilldown(
    data,
    t0,
    counters,
    (name) => categorizeProcess(name).name === targetCat.name,
  );
}

function buildOomDrilldown(
  data: SnapshotData,
  bucketIdx: number,
  t0: number,
  counters: readonly string[],
): LineChartData | undefined {
  const bucket = OOM_SCORE_BUCKETS[bucketIdx];
  const oomByName = new Map<string, number>();
  for (const [processName, counterMap] of data.processCountersByName) {
    const oomTs = counterMap.get('oom_score_adj');
    let oomScore = 0;
    if (oomTs !== undefined && oomTs.size > 0) {
      for (const val of oomTs.values()) oomScore = val;
    }
    oomByName.set(processName, oomScore);
  }
  return buildProcessDrilldown(data, t0, counters, (name) => {
    const score = oomByName.get(name) ?? 0;
    return score >= bucket.minScore && score <= bucket.maxScore;
  });
}

function buildLatestProcessMemory(data: SnapshotData): ProcessMemoryRow[] {
  const rows: ProcessMemoryRow[] = [];
  let maxTs = 0;
  for (const counterMap of data.processCountersByName.values()) {
    for (const byTs of counterMap.values()) {
      for (const ts of byTs.keys()) {
        if (ts > maxTs) maxTs = ts;
      }
    }
  }
  for (const [processName, counterMap] of data.processCountersByName) {
    const info = data.processInfo.get(processName);
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
    if (rssKb === 0) continue;
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
  rows.sort((a, b) => b.rssKb - a.rssKb);
  return rows;
}

// Build a lookup from category name -> color for the cell renderer.
const CATEGORY_COLOR_MAP = new Map<string, string>(
  (Object.values(CATEGORIES) as readonly {name: string; color: string}[]).map(
    (c) => [c.name, c.color],
  ),
);

export const PROCESS_TABLE_SCHEMA: SchemaRegistry = {
  process: {
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

export interface ProcessesTabAttrs {
  readonly session: MementoSession;
}

export class ProcessesTab implements m.ClassComponent<ProcessesTabAttrs> {
  private grouping: ProcessGrouping = 'category';
  private metric: ProcessMetric = 'rss';
  private selectedCategory?: CategoryId;
  private selectedOomBucket?: number;

  view({attrs}: m.CVnode<ProcessesTabAttrs>): m.Children {
    const data = attrs.session.data;
    if (!data) return null;

    const t0 = computeT0(data);
    const counters = PROCESS_METRIC_OPTIONS.find(
      (o) => o.key === this.metric,
    )!.counters;

    const isDrilledDown =
      this.grouping === 'category'
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
      ? this.selectedCategory !== undefined
        ? buildCategoryDrilldown(data, this.selectedCategory, t0, counters)
        : this.selectedOomBucket !== undefined
          ? buildOomDrilldown(data, this.selectedOomBucket, t0, counters)
          : undefined
      : this.grouping === 'category'
        ? buildCategoryTimeSeries(data, t0, counters)
        : buildOomScoreTimeSeries(data, t0, counters);

    const latestProcesses = buildLatestProcessMemory(data);

    const processes =
      this.grouping === 'category' && this.selectedCategory
        ? latestProcesses.filter(
            (p: ProcessMemoryRow) =>
              categorizeProcess(p.processName).name === cat!.name,
          )
        : this.grouping === 'oom_score' && oomBucket
          ? latestProcesses.filter(
              (p: ProcessMemoryRow) =>
                p.oomScore >= oomBucket.minScore &&
                p.oomScore <= oomBucket.maxScore,
            )
          : latestProcesses;

    const metricInfo = PROCESS_METRIC_OPTIONS.find(
      (o) => o.key === this.metric,
    )!;
    const drillName = cat?.name ?? oomBucket?.name;
    const groupLabel = this.grouping === 'category' ? 'Category' : 'OOM Score';
    const title = drillName
      ? `Process Memory: ${drillName}`
      : `Process Memory by ${groupLabel}`;
    const subtitle = drillName
      ? `Stacked ${metricInfo.label} per process. Totals may exceed actual memory usage because RSS counts shared pages (COW, mmap) in every process that maps them.`
      : `Stacked ${metricInfo.label} per ${groupLabel.toLowerCase()}. Click a ${groupLabel.toLowerCase()} to drill into individual processes.`;

    // Compute billboard totals from all processes.
    const totalAnonSwapKb = latestProcesses.reduce(
      (s, p) => s + p.anonKb + p.swapKb,
      0,
    );
    const totalFileKb = latestProcesses.reduce((s, p) => s + p.fileKb, 0);
    const totalDmabufKb = latestProcesses.reduce((s, p) => s + p.dmabufKb, 0);

    return [
      latestProcesses.length > 0 &&
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
        ),
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
                  this.grouping === 'category'
                    ? 'All categories'
                    : 'All OOM buckets',
                minimal: true,
                onclick: () => {
                  this.selectedCategory = undefined;
                  this.selectedOomBucket = undefined;
                },
              }),
            m('h2', title),
            m(
              '.pf-memento-panel__controls',
              m(SegmentedButtons, {
                options: [{label: 'By Category'}, {label: 'By OOM Score'}],
                selectedOption: this.grouping === 'category' ? 0 : 1,
                onOptionSelected: (i: number) => {
                  const g: ProcessGrouping =
                    i === 0 ? 'category' : 'oom_score';
                  if (g === this.grouping) return;
                  this.grouping = g;
                  this.selectedCategory = undefined;
                  this.selectedOomBucket = undefined;
                },
              }),
              m(SegmentedButtons, {
                options: PROCESS_METRIC_OPTIONS.map((o) => ({label: o.label})),
                selectedOption: PROCESS_METRIC_OPTIONS.findIndex(
                  (o) => o.key === this.metric,
                ),
                onOptionSelected: (i: number) => {
                  const newMetric = PROCESS_METRIC_OPTIONS[i].key;
                  if (newMetric === this.metric) return;
                  this.metric = newMetric;
                  this.selectedCategory = undefined;
                  this.selectedOomBucket = undefined;
                },
              }),
            ),
          ),
          m('p', subtitle),
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
                onSeriesClick: isDrilledDown
                  ? undefined
                  : (seriesName: string) => this.onSeriesClick(seriesName),
              })
            : m('.pf-memento-placeholder', 'Waiting for data\u2026'),
        ),
      ),
      processes &&
        renderProcessTable(processes, data.isUserDebug, attrs.session),
    ];
  }

  private onSeriesClick(seriesName: string) {
    if (this.grouping === 'category') {
      const catIds = Object.keys(CATEGORIES) as CategoryId[];
      const id = catIds.find((k) => CATEGORIES[k].name === seriesName);
      if (id) this.selectedCategory = id;
    } else {
      const idx = OOM_SCORE_BUCKETS.findIndex((b) => b.name === seriesName);
      if (idx !== -1) this.selectedOomBucket = idx;
    }
  }
}

function renderProcessTable(
  processes: ProcessMemoryRow[],
  isUserDebug: boolean,
  session: MementoSession,
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
      process: {
        title: 'Process',
        columnType: 'text',
        actions: (value, row) => {
          return m(Button, {
            label: 'Profile',
            rightIcon: 'arrow_forward',
            rounded: true,
            variant: ButtonVariant.Filled,
            intent: Intent.Primary,
            onclick: () => {
              const pid = row.pid as number;
              const processName = value as string;
              session.startProfile(pid, processName).then(() => m.redraw());
            },
          });
        },
      },
    },
  };

  const isStopping = session.profileState === 'stopping';

  return [
    session.isProfiling &&
      m(
        '.pf-memento-status-bar',
        m('.pf-memento-status-bar__dot'),
        isStopping
          ? `Stopping and reading trace for ${session.profileProcessName}\u2026`
          : `Recording heap profile for ${session.profileProcessName} (PID ${session.profilePid})`,
        !isStopping && [
          m(Button, {
            label: 'Stop & Open',
            icon: 'stop',
            minimal: true,
            intent: Intent.Danger,
            onclick: () => {
              session.stopAndOpenProfile().then(() => m.redraw());
            },
          }),
          m(Button, {
            label: 'Cancel',
            icon: 'close',
            minimal: true,
            onclick: () => {
              session.cancelProfile().then(() => m.redraw());
            },
          }),
        ],
      ),
    m(DataGrid, {
      schema,
      rootSchema: 'process',
      data: rows,
      initialColumns: [
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
