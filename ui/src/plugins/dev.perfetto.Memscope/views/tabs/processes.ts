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
  LineChartSvg,
  type LineChartData,
  type LineChartSeries,
} from '../../../../components/widgets/charts_svg/line_chart_svg';
import {Button, ButtonVariant} from '../../../../widgets/button';
import {Intent} from '../../../../widgets/common';
import {MenuDivider, MenuItem, PopupMenu} from '../../../../widgets/menu';
import {PopupPosition} from '../../../../widgets/popup';
import {
  Grid,
  GridCell,
  type GridColumn,
  GridHeaderCell,
  renderSortMenuItems,
  type SortDirection,
} from '../../../../widgets/grid';
import {TextInput} from '../../../../widgets/text_input';
import {RadioGroup} from '../../../../widgets/radio_group';
import {LiveSession, type SnapshotData} from '../../sessions/live_session';
import {
  categorizeProcess,
  CATEGORIES,
  type CategoryId,
} from '../../process_categories';
import {Billboard} from '../../components/billboard';
import {ColorChip, chipColor} from '../../components/color_chip';
import {billboardKb, formatKb, maxSeriesKb, niceKbInterval} from '../../utils';
import {
  type ProcessGrouping,
  type ProcessMetric,
  type ProcessMemoryRow,
  PROCESS_METRIC_OPTIONS,
  OOM_SCORE_BUCKETS,
} from '../../process_data';
import {Stack} from '../../../../widgets/stack';

export type {ProcessGrouping, ProcessMetric, ProcessMemoryRow};
export {PROCESS_METRIC_OPTIONS, OOM_SCORE_BUCKETS};

// Tiny SVG sparkline for the trend column. Min/max are derived from the
// values themselves (so a flat series renders as a flat line at mid-height).
function sparkline(
  values: ReadonlyArray<number>,
  width = 64,
  height = 16,
): m.Children {
  if (values.length < 2) {
    return m('span.pf-memscope-sparkline-empty', '—');
  }
  let min = values[0];
  let max = values[0];
  for (const v of values) {
    if (v < min) min = v;
    if (v > max) max = v;
  }
  const range = max - min || 1;
  const stepX = width / (values.length - 1);
  const padY = 2;
  const drawH = height - padY * 2;
  const points = values
    .map((v, i) => {
      const x = (i * stepX).toFixed(1);
      const y = (padY + drawH - ((v - min) / range) * drawH).toFixed(1);
      return `${x},${y}`;
    })
    .join(' ');
  const last = values[values.length - 1];
  const first = values[0];
  const stroke =
    last > first
      ? 'var(--pf-color-danger)'
      : last < first
        ? 'var(--pf-color-success)'
        : 'var(--pf-color-text-muted)';
  return m(
    'svg.pf-memscope-sparkline',
    {
      width,
      height,
      viewBox: `0 0 ${width} ${height}`,
      preserveAspectRatio: 'none',
    },
    m('polyline', {
      points,
      'fill': 'none',
      stroke,
      'stroke-width': 1.25,
      'stroke-linejoin': 'round',
      'stroke-linecap': 'round',
    }),
  );
}

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
  for (const [upid, counterMap] of data.processCountersByUpid) {
    const info = data.processInfo.get(upid);
    if (info === undefined) continue;
    const cat = categorizeProcess(info.processName);
    const id = catIds.find((k) => CATEGORIES[k].name === cat.name)!;
    for (const counterName of counters) {
      const samples = counterMap.get(counterName);
      if (samples === undefined) continue;
      for (const {ts, value} of samples) {
        tsSet.add(ts);
        let catMap = byCatTs.get(ts);
        if (catMap === undefined) {
          catMap = new Map();
          byCatTs.set(ts, catMap);
        }
        catMap.set(id, (catMap.get(id) ?? 0) + Math.round(value / 1024));
      }
    }
  }
  const timestamps = [...tsSet].sort((a, b) => a - b);
  if (timestamps.length < 2) return undefined;
  const pointsByCategory = new Map<CategoryId, {x: number; y: number}[]>();
  for (const id of catIds) pointsByCategory.set(id, []);
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
      series.push({name: cat.name, points, color: chipColor(cat.color)});
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
  // Latest oom_score_adj per upid.
  const oomByUpid = new Map<number, number>();
  for (const [upid, counterMap] of data.processCountersByUpid) {
    const samples = counterMap.get('oom_score_adj');
    if (samples === undefined || samples.length === 0) continue;
    let latestTs = -1;
    let latestVal = 0;
    for (const {ts, value} of samples) {
      if (ts >= latestTs) {
        latestTs = ts;
        latestVal = value;
      }
    }
    oomByUpid.set(upid, latestVal);
  }
  const tsSet = new Set<number>();
  const byBucketTs = new Map<number, Map<number, number>>();
  for (const [upid, counterMap] of data.processCountersByUpid) {
    const oomScore = oomByUpid.get(upid) ?? 0;
    const bucketIdx = OOM_SCORE_BUCKETS.findIndex(
      (b) => oomScore >= b.minScore && oomScore <= b.maxScore,
    );
    const idx = bucketIdx !== -1 ? bucketIdx : OOM_SCORE_BUCKETS.length - 1;
    for (const counterName of counters) {
      const samples = counterMap.get(counterName);
      if (samples === undefined) continue;
      for (const {ts, value} of samples) {
        tsSet.add(ts);
        let bucketMap = byBucketTs.get(ts);
        if (bucketMap === undefined) {
          bucketMap = new Map();
          byBucketTs.set(ts, bucketMap);
        }
        bucketMap.set(
          idx,
          (bucketMap.get(idx) ?? 0) + Math.round(value / 1024),
        );
      }
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
      series.push({name: bucket.name, points, color: chipColor(bucket.color)});
    }
  }
  if (series.length === 0) return undefined;
  return {series};
}

function buildProcessDrilldown(
  data: SnapshotData,
  t0: number,
  counters: readonly string[],
  filter: (info: import('../../sessions/live_session').ProcessInfo) => boolean,
): LineChartData | undefined {
  const tsSet = new Set<number>();
  const byProcTs = new Map<number, Map<string, number>>();
  for (const [upid, counterMap] of data.processCountersByUpid) {
    const info = data.processInfo.get(upid);
    if (info === undefined || !filter(info)) continue;
    // Disambiguate processes that share a name by appending the PID.
    const seriesKey = `${info.processName} (${info.pid})`;
    for (const counterName of counters) {
      const samples = counterMap.get(counterName);
      if (samples === undefined) continue;
      for (const {ts, value} of samples) {
        tsSet.add(ts);
        let procMap = byProcTs.get(ts);
        if (procMap === undefined) {
          procMap = new Map();
          byProcTs.set(ts, procMap);
        }
        procMap.set(
          seriesKey,
          (procMap.get(seriesKey) ?? 0) + Math.round(value / 1024),
        );
      }
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
    for (const name of allNames) {
      pointsByProc.get(name)!.push({x, y: procMap.get(name) ?? 0});
    }
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
      color: 'var(--pf-chart-color-neutral)',
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
    (info) => categorizeProcess(info.processName).name === targetCat.name,
  );
}

function buildOomDrilldown(
  data: SnapshotData,
  bucketIdx: number,
  t0: number,
  counters: readonly string[],
): LineChartData | undefined {
  const bucket = OOM_SCORE_BUCKETS[bucketIdx];
  const oomByUpid = new Map<number, number>();
  for (const [upid, counterMap] of data.processCountersByUpid) {
    const samples = counterMap.get('oom_score_adj');
    if (samples === undefined || samples.length === 0) continue;
    let latestTs = -1;
    let latestVal = 0;
    for (const {ts, value} of samples) {
      if (ts >= latestTs) {
        latestTs = ts;
        latestVal = value;
      }
    }
    oomByUpid.set(upid, latestVal);
  }
  return buildProcessDrilldown(data, t0, counters, (info) => {
    const score = oomByUpid.get(info.upid) ?? 0;
    return score >= bucket.minScore && score <= bucket.maxScore;
  });
}

function buildLatestProcessMemory(data: SnapshotData): ProcessMemoryRow[] {
  const rows: ProcessMemoryRow[] = [];
  let maxTs = 0;
  for (const counterMap of data.processCountersByUpid.values()) {
    for (const samples of counterMap.values()) {
      for (const {ts} of samples) {
        if (ts > maxTs) maxTs = ts;
      }
    }
  }
  for (const [upid, counterMap] of data.processCountersByUpid) {
    const info = data.processInfo.get(upid);
    if (info === undefined) continue;
    const getLatestRaw = (counterName: string): number => {
      const samples = counterMap.get(counterName);
      if (samples === undefined || samples.length === 0) return 0;
      let latestTs = -1;
      let latestValue = 0;
      for (const {ts, value} of samples) {
        if (ts >= latestTs) {
          latestTs = ts;
          latestValue = value;
        }
      }
      return latestValue;
    };
    const rssKb = Math.round(getLatestRaw('mem.rss') / 1024);
    if (rssKb === 0) continue;
    const rssSamples = counterMap.get('mem.rss');
    const rssTrendKb =
      rssSamples !== undefined
        ? [...rssSamples]
            .sort((a, b) => a.ts - b.ts)
            .map(({value}) => Math.round(value / 1024))
        : [];
    rows.push({
      processName: info.processName,
      pid: info.pid,
      rssKb,
      anonKb: Math.round(getLatestRaw('mem.rss.anon') / 1024),
      fileKb: Math.round(getLatestRaw('mem.rss.file') / 1024),
      shmemKb: Math.round(getLatestRaw('mem.rss.shmem') / 1024),
      swapKb: Math.round(getLatestRaw('mem.swap') / 1024),
      dmabufKb: Math.round(getLatestRaw('mem.dmabuf_rss') / 1024),
      oomScore: getLatestRaw('oom_score_adj'),
      debuggable: info.debuggable,
      ageSeconds: info.startTs !== null ? (maxTs - info.startTs) / 1e9 : null,
      rssTrendKb,
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

export interface ProcessesTabAttrs {
  readonly session: LiveSession;
}

export class ProcessesTab implements m.ClassComponent<ProcessesTabAttrs> {
  private grouping: ProcessGrouping = 'category';
  private metric: ProcessMetric = 'anon_swap';
  private selectedCategory?: CategoryId;
  private selectedOomBucket?: number;
  private processSearch: string = '';

  view({attrs}: m.CVnode<ProcessesTabAttrs>): m.Children {
    const data = attrs.session.data;
    if (!data) return null;

    const t0 = data.ts0;
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

    // Pin x-axis to the exact data range so the plot edges line up with the
    // first and last sample.
    let chartXMin: number | undefined;
    let chartXMax: number | undefined;
    if (chartData !== undefined) {
      for (const s of chartData.series) {
        for (const p of s.points) {
          if (chartXMin === undefined || p.x < chartXMin) chartXMin = p.x;
          if (chartXMax === undefined || p.x > chartXMax) chartXMax = p.x;
        }
      }
    }

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

    const filteredProcesses =
      this.processSearch.trim() === ''
        ? processes
        : processes.filter((p) =>
            p.processName
              .toLowerCase()
              .includes(this.processSearch.toLowerCase()),
          );

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

    return m(Stack, {spacing: 'large'}, [
      latestProcesses.length > 0 &&
        m(
          Stack,
          {orientation: 'horizontal', spacing: 'large'},
          m(Billboard, {
            ...billboardKb(totalAnonSwapKb),
            label: 'Anon + Swap',
            desc: 'Sum of anonymous RSS + swap across all processes',
          }),
          m(Billboard, {
            ...billboardKb(totalFileKb),
            label: 'File',
            desc: 'Sum of file-backed RSS across all processes',
          }),
          m(Billboard, {
            ...billboardKb(totalDmabufKb),
            label: 'DMA-BUF',
            desc: 'Sum of DMA-BUF heap RSS across all processes',
          }),
        ),
      m(
        '.pf-memscope-panel',
        m(
          '.pf-memscope-panel__header',
          m(
            '.pf-memscope-panel__title-row',
            isDrilledDown &&
              m(Button, {
                variant: ButtonVariant.Filled,
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
              '.pf-memscope-panel__controls',
              m(
                RadioGroup,
                {
                  intent: Intent.Primary,
                  selectedValue: this.grouping,
                  onValueChange: (value) => {
                    const g = value as ProcessGrouping;
                    if (g === this.grouping) return;
                    this.grouping = g;
                    this.selectedCategory = undefined;
                    this.selectedOomBucket = undefined;
                  },
                },
                m(RadioGroup.Button, {value: 'category'}, 'By Category'),
                m(RadioGroup.Button, {value: 'oom_score'}, 'By OOM Score'),
              ),
              m(
                RadioGroup,
                {
                  intent: Intent.Primary,
                  selectedValue: this.metric,
                  onValueChange: (value) => {
                    const newMetric = value as ProcessMetric;
                    if (newMetric === this.metric) return;
                    this.metric = newMetric;
                    this.selectedCategory = undefined;
                    this.selectedOomBucket = undefined;
                  },
                },
                PROCESS_METRIC_OPTIONS.map((o) =>
                  m(RadioGroup.Button, {value: o.key}, o.label),
                ),
              ),
            ),
          ),
          m('p', subtitle),
        ),
        m(
          '.pf-memscope-panel__body',
          chartData
            ? m(LineChartSvg, {
                data: chartData,
                height: 350,
                xAxisLabel: 'Time (s)',
                yAxisLabel: 'RSS',
                showLegend: true,
                showPoints: false,
                stacked: true,
                gridLines: 'both',
                xAxisMin: chartXMin,
                xAxisMax: chartXMax,
                formatXValue: (v: number) => `${v.toFixed(0)}s`,
                formatYValue: (v: number) => formatKb(v),
                yAxisMinInterval: niceKbInterval(maxSeriesKb(chartData.series)),
                onSeriesClick: isDrilledDown
                  ? undefined
                  : (seriesName: string) => this.onSeriesClick(seriesName),
              })
            : m('.pf-memscope-placeholder', 'Waiting for data\u2026'),
        ),
      ),
      m(
        '.pf-memscope-panel',
        m(ProcessTable, {
          processes: filteredProcesses,
          isUserDebug: data.isUserDebug,
          session: attrs.session,
          searchQuery: this.processSearch,
          onSearchChange: (q) => {
            this.processSearch = q;
          },
        }),
      ),
    ]);
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

interface ProcessTableAttrs {
  readonly processes: ProcessMemoryRow[];
  readonly isUserDebug: boolean;
  readonly session: LiveSession;
  readonly searchQuery: string;
  readonly onSearchChange: (q: string) => void;
}

class ProcessTable implements m.ClassComponent<ProcessTableAttrs> {
  private sortKey: string = 'rss_kb';
  private sortDir: SortDirection = 'DESC';
  private showDebuggableOnly: boolean = false;
  private oomBucketFilter: Set<number> = new Set();
  private categoryFilter: Set<CategoryId> = new Set();

  private headerCell(
    key: string,
    label: string,
    hint: SortDirection = 'DESC',
  ): m.Children {
    const current = this.sortKey === key ? this.sortDir : undefined;
    const onSort = (dir: SortDirection | undefined) => {
      this.sortKey = dir !== undefined ? key : 'rss_kb';
      this.sortDir = dir ?? 'DESC';
    };
    return m(
      GridHeaderCell,
      {
        sort: current,
        onSort,
        hintSortDirection: hint,
        menuItems: renderSortMenuItems(current, onSort),
      },
      label,
    );
  }

  view({attrs}: m.CVnode<ProcessTableAttrs>): m.Children {
    const {processes, isUserDebug, searchQuery, onSearchChange} = attrs;

    const visible = processes.filter((p) => {
      if (this.showDebuggableOnly && !p.debuggable && !isUserDebug) {
        return false;
      }
      if (this.oomBucketFilter.size > 0) {
        const idx = OOM_SCORE_BUCKETS.findIndex(
          (b) => p.oomScore >= b.minScore && p.oomScore <= b.maxScore,
        );
        if (!this.oomBucketFilter.has(idx)) return false;
      }
      if (this.categoryFilter.size > 0) {
        const catIds = Object.keys(CATEGORIES) as CategoryId[];
        const id = catIds.find(
          (k) => CATEGORIES[k].name === categorizeProcess(p.processName).name,
        );
        if (id === undefined || !this.categoryFilter.has(id)) return false;
      }
      return true;
    });

    const mul = this.sortDir === 'ASC' ? 1 : -1;
    const sorted = [...visible].sort((a, b) => {
      switch (this.sortKey) {
        case 'rss_kb':
          return mul * (a.rssKb - b.rssKb);
        case 'anon_swap_kb':
          return mul * (a.anonKb + a.swapKb - b.anonKb - b.swapKb);
        case 'file_kb':
          return mul * (a.fileKb - b.fileKb);
        case 'shmem_kb':
          return mul * (a.shmemKb - b.shmemKb);
        case 'pid':
          return mul * (a.pid - b.pid);
        case 'oom_score':
          return mul * (a.oomScore - b.oomScore);
        case 'age':
          return mul * ((a.ageSeconds ?? -1) - (b.ageSeconds ?? -1));
        case 'process':
          return mul * a.processName.localeCompare(b.processName);
        case 'category':
          return (
            mul *
            categorizeProcess(a.processName).name.localeCompare(
              categorizeProcess(b.processName).name,
            )
          );
        case 'debuggable': {
          const toNum = (p: ProcessMemoryRow) =>
            p.debuggable ? 2 : isUserDebug ? 1 : 0;
          return mul * (toNum(a) - toNum(b));
        }
        default:
          return 0;
      }
    });

    const columns: GridColumn[] = [
      {
        key: 'process',
        header: this.headerCell('process', 'Process', 'ASC'),
        maxInitialWidthPx: 400,
      },
      {key: 'category', header: this.headerCell('category', 'Category', 'ASC')},
      {key: 'pid', header: this.headerCell('pid', 'PID')},
      {key: 'oom_score', header: this.headerCell('oom_score', 'OOM Adj')},
      {key: 'age', header: this.headerCell('age', 'Age')},
      {key: 'rss_kb', header: this.headerCell('rss_kb', 'RSS')},
      {key: 'trend', header: m(GridCell, 'RSS trend')},
      {
        key: 'anon_swap_kb',
        header: this.headerCell('anon_swap_kb', 'Anon + Swap'),
      },
      {key: 'file_kb', header: this.headerCell('file_kb', 'File')},
      {key: 'shmem_kb', header: this.headerCell('shmem_kb', 'Shmem')},
    ];

    const rowData = sorted.map((p) => {
      const cat = categorizeProcess(p.processName);
      const color = CATEGORY_COLOR_MAP.get(cat.name);
      const oomBucket = OOM_SCORE_BUCKETS.find(
        (b) => p.oomScore >= b.minScore && p.oomScore <= b.maxScore,
      );
      const oomLabel = oomBucket
        ? `${p.oomScore} (${oomBucket.name.replace(/ \(.*\)$/, '')})`
        : `${p.oomScore}`;
      const secs = p.ageSeconds;
      const ageStr = (() => {
        if (secs === null || secs < 0) return '-';
        const d = Math.floor(secs / 86400);
        const h = Math.floor((secs % 86400) / 3600);
        const mn = Math.floor((secs % 3600) / 60);
        const s = Math.floor(secs % 60);
        if (d > 0) return `${d}d ${h}h`;
        if (h > 0) return `${h}h ${mn}m`;
        if (mn > 0) return `${mn}m ${s}s`;
        return `${s}s`;
      })();
      return [
        m(GridCell, p.processName),
        m(GridCell, m(ColorChip, {color}, cat.name)),
        m(GridCell, {align: 'right'}, `${p.pid}`),
        m(
          GridCell,
          {align: 'right'},
          oomBucket
            ? m(ColorChip, {color: oomBucket.color}, oomLabel)
            : oomLabel,
        ),
        m(GridCell, {align: 'right'}, ageStr),
        m(GridCell, {align: 'right'}, formatKb(p.rssKb)),
        m(GridCell, sparkline(p.rssTrendKb)),
        m(
          GridCell,
          {align: 'right'},
          p.anonKb + p.swapKb > 0 ? formatKb(p.anonKb + p.swapKb) : '-',
        ),
        m(GridCell, {align: 'right'}, p.fileKb > 0 ? formatKb(p.fileKb) : '-'),
        m(
          GridCell,
          {align: 'right'},
          p.shmemKb > 0 ? formatKb(p.shmemKb) : '-',
        ),
      ];
    });

    return [
      m(
        '.pf-memscope-panel__header.pf-memscope-search-row',
        m(TextInput, {
          leftIcon: 'search',
          placeholder: 'Filter processes\u2026',
          value: searchQuery,
          onInput: (v) => {
            onSearchChange(v);
          },
        }),
        m(Button, {
          label: 'Debuggable only',
          icon: 'bug_report',
          variant: ButtonVariant.Filled,
          intent: this.showDebuggableOnly ? Intent.Primary : Intent.None,
          onclick: () => {
            this.showDebuggableOnly = !this.showDebuggableOnly;
          },
        }),
        m(
          PopupMenu,
          {
            position: PopupPosition.Bottom,
            trigger: m(Button, {
              label: 'OOM Score',
              icon: 'filter_list',
              variant: ButtonVariant.Filled,
              intent:
                this.oomBucketFilter.size > 0 ? Intent.Primary : Intent.None,
            }),
          },
          m(MenuItem, {
            label: 'Clear all',
            icon: 'close',
            disabled: this.oomBucketFilter.size === 0,
            onclick: () => this.oomBucketFilter.clear(),
          }),
          m(MenuDivider),
          OOM_SCORE_BUCKETS.map((bucket, idx) =>
            m(MenuItem, {
              label: m('span.pf-memscope-oom-item', [
                m(ColorChip, {color: bucket.color}),
                bucket.name,
              ]),
              active: this.oomBucketFilter.has(idx),
              closePopupOnClick: false,
              onclick: () => {
                if (this.oomBucketFilter.has(idx)) {
                  this.oomBucketFilter.delete(idx);
                } else {
                  this.oomBucketFilter.add(idx);
                }
              },
            }),
          ),
        ),
        m(
          PopupMenu,
          {
            position: PopupPosition.Bottom,
            trigger: m(Button, {
              label: 'Category',
              icon: 'filter_list',
              variant: ButtonVariant.Filled,
              intent:
                this.categoryFilter.size > 0 ? Intent.Primary : Intent.None,
            }),
          },
          m(MenuItem, {
            label: 'Clear all',
            icon: 'close',
            disabled: this.categoryFilter.size === 0,
            onclick: () => this.categoryFilter.clear(),
          }),
          m(MenuDivider),
          (Object.keys(CATEGORIES) as CategoryId[]).map((id) => {
            const cat = CATEGORIES[id];
            return m(MenuItem, {
              label: m('span.pf-memscope-oom-item', [
                m(ColorChip, {color: cat.color}),
                cat.name,
              ]),
              active: this.categoryFilter.has(id),
              closePopupOnClick: false,
              onclick: () => {
                if (this.categoryFilter.has(id)) {
                  this.categoryFilter.delete(id);
                } else {
                  this.categoryFilter.add(id);
                }
              },
            });
          }),
        ),
      ),
      m(Grid, {columns, rowData, fillHeight: false}),
    ];
  }
}
