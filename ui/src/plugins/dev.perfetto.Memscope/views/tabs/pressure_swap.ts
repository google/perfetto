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
import {LiveSession, type SnapshotData} from '../../sessions/live_session';
import {
  counterPoints,
  formatKb,
  maxSeriesKb,
  niceKbInterval,
} from '../../utils';
import {Panel} from '../../components/panel';
import {Grid, GridCell, GridHeaderCell} from '../../../../widgets/grid';

// Note: the cumulative→per-second-rate conversion for psi/vmstat counters
// happens in SQL (see extractSnapshotData in live_session.ts), so samples
// arrive in their plotted units (psi: ms/s, vmstat: events/s).

function buildPsiTimeSeries(
  data: SnapshotData,
  t0: number,
): LineChartData | undefined {
  const some = counterPoints(data.systemCounters.get('psi.mem.some'), t0);
  if (some === undefined) return undefined;
  const series: LineChartSeries[] = [
    {
      name: 'some (any task stalled)',
      points: some,
      color: 'var(--pf-color-warning)',
    },
  ];
  const full = counterPoints(data.systemCounters.get('psi.mem.full'), t0);
  if (full !== undefined) {
    series.push({
      name: 'full (all tasks stalled)',
      points: full,
      color: 'var(--pf-color-danger)',
    });
  }
  return {series};
}

function buildPageFaultTimeSeries(
  data: SnapshotData,
  t0: number,
): LineChartData | undefined {
  const minor = counterPoints(data.systemCounters.get('pgfault'), t0);
  if (minor === undefined) return undefined;
  const series: LineChartSeries[] = [
    {
      name: 'pgfault (minor)',
      points: minor,
      color: 'var(--pf-color-warning)',
    },
  ];
  const major = counterPoints(data.systemCounters.get('pgmajfault'), t0);
  if (major !== undefined) {
    series.push({
      name: 'pgmajfault (major)',
      points: major,
      color: 'var(--pf-color-danger)',
    });
  }
  return {series};
}

function buildSwapTimeSeries(
  data: SnapshotData,
  t0: number,
): LineChartData | undefined {
  const byTs = new Map<number, Map<string, number>>();
  for (const name of ['SwapTotal', 'SwapFree', 'SwapCached']) {
    const samples = data.systemCounters.get(name);
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
  if ((byTs.get(timestamps[0])?.get('SwapTotal') ?? 0) === 0) return undefined;
  const dirtyPts: {x: number; y: number}[] = [];
  const cachedPts: {x: number; y: number}[] = [];
  const freePts: {x: number; y: number}[] = [];
  for (const ts of timestamps) {
    const row = byTs.get(ts)!;
    const total = row.get('SwapTotal') ?? 0;
    const free = row.get('SwapFree') ?? 0;
    const cached = row.get('SwapCached') ?? 0;
    const x = (ts - t0) / 1e9;
    const dirty = Math.max(0, Math.max(0, total - free) - cached);
    dirtyPts.push({x, y: dirty});
    cachedPts.push({x, y: cached});
    freePts.push({x, y: free});
  }
  if (dirtyPts.length < 2) return undefined;
  return {
    series: [
      {
        name: 'Swap dirty',
        points: dirtyPts,
        color: 'var(--pf-color-danger)',
      },
      {
        name: 'SwapCached',
        points: cachedPts,
        color: 'var(--pf-color-warning)',
      },
      {
        name: 'SwapFree',
        points: freePts,
        color: 'var(--pf-color-success)',
      },
    ],
  };
}

function buildVmstatTimeSeries(
  data: SnapshotData,
  t0: number,
): LineChartData | undefined {
  const pin = counterPoints(data.systemCounters.get('pswpin'), t0);
  if (pin === undefined) return undefined;
  const series: LineChartSeries[] = [
    {name: 'pswpin', points: pin, color: 'var(--pf-color-warning)'},
  ];
  const out = counterPoints(data.systemCounters.get('pswpout'), t0);
  if (out !== undefined) {
    series.push({
      name: 'pswpout',
      points: out,
      color: 'var(--pf-color-danger)',
    });
  }
  return {series};
}

function renderLmkPanel(
  events: {ts: number; pid: number; processName: string; oomScoreAdj: number}[],
  t0: number,
): m.Children {
  return m(
    Panel,
    {
      title: `LMK Kills (${events.length})`,
      subtitle:
        'Low Memory Killer events recorded during this session. ' +
        'Source: lmkd atrace / lowmemorykiller ftrace.',
    },
    events.length > 0 &&
      m(Grid, {
        columns: [
          {key: 'time', header: m(GridHeaderCell, 'Time')},
          {key: 'pid', header: m(GridHeaderCell, 'PID')},
          {
            key: 'process',
            header: m(GridHeaderCell, 'Process'),
            maxInitialWidthPx: 400,
          },
          {key: 'oom', header: m(GridHeaderCell, 'OOM Adj')},
        ],
        rowData: events.map((ev) => [
          m(GridCell, {align: 'right'}, `${((ev.ts - t0) / 1e9).toFixed(1)}s`),
          m(GridCell, {align: 'right'}, String(ev.pid)),
          m(GridCell, ev.processName || '(unknown)'),
          m(GridCell, {align: 'right'}, String(ev.oomScoreAdj)),
        ]),
        fillHeight: false,
      }),
  );
}

export function renderPressureSwapTab(session: LiveSession): m.Children {
  const data = session.data;
  if (!data) return null;

  const t0 = data.ts0;
  const psiChartData = buildPsiTimeSeries(data, t0);
  const pageFaultChartData = buildPageFaultTimeSeries(data, t0);
  const swapChartData = buildSwapTimeSeries(data, t0);
  const vmstatChartData = buildVmstatTimeSeries(data, t0);

  return [
    m(
      Panel,
      {
        title: 'Memory Pressure (PSI)',
        subtitle:
          'Source: /proc/pressure/memory (psi.mem.some, psi.mem.full). ' +
          'Derived: cumulative \u00b5s converted to ms/s rate. ' +
          '"some" = at least one task stalled, "full" = all tasks stalled.',
      },
      psiChartData
        ? m(LineChartSvg, {
            data: psiChartData,
            height: 200,
            xAxisLabel: 'Time (s)',
            yAxisLabel: 'Stall (ms/s)',
            showLegend: true,
            showPoints: false,
            gridLines: 'both',
            xAxisMin: data.xMin,
            xAxisMax: data.xMax,
            formatXValue: (v: number) => `${v.toFixed(0)}s`,
            formatYValue: (v: number) => `${v.toFixed(1)} ms/s`,
            // TODO(stevegolton): Add markers back in when we support them in LineChartSvg.
            //   x: (ev.ts - t0) / 1e9,
            // })),
          })
        : m('.pf-memscope-placeholder', 'Waiting for data\u2026'),
    ),

    m(
      Panel,
      {
        title: 'Page Faults',
        subtitle:
          'Source: /proc/vmstat counters pgfault, pgmajfault. ' +
          'Derived: cumulative counts converted to faults/s rate. ' +
          'Minor (pgfault) = page in RAM but not in TLB. Major (pgmajfault) = page must be read from disk.',
      },
      pageFaultChartData
        ? m(LineChartSvg, {
            data: pageFaultChartData,
            height: 200,
            xAxisLabel: 'Time (s)',
            yAxisLabel: 'Faults/s',
            showLegend: true,
            showPoints: false,
            gridLines: 'both',
            xAxisMin: data.xMin,
            xAxisMax: data.xMax,
            formatXValue: (v: number) => `${v.toFixed(0)}s`,
            formatYValue: (v: number) => `${v.toFixed(0)} f/s`,
          })
        : m('.pf-memscope-placeholder', 'Waiting for data\u2026'),
    ),

    swapChartData &&
      m(
        Panel,
        {
          title: 'Swap Usage',
          subtitle:
            'Source: /proc/meminfo counters SwapTotal, SwapFree, SwapCached. ' +
            'Derived: Swap dirty = (SwapTotal \u2212 SwapFree) \u2212 SwapCached.',
        },
        m(LineChartSvg, {
          data: swapChartData,
          height: 200,
          xAxisMin: data.xMin,
          xAxisMax: data.xMax,
          xAxisLabel: 'Time (s)',
          yAxisLabel: 'Swap',
          showLegend: true,
          showPoints: false,
          stacked: true,
          gridLines: 'both',
          formatXValue: (v: number) => `${v.toFixed(0)}s`,
          formatYValue: (v: number) => formatKb(v),
          yAxisMinInterval: niceKbInterval(
            maxSeriesKb(swapChartData?.series ?? []),
          ),
        }),
      ),

    vmstatChartData &&
      m(
        Panel,
        {
          title: 'Swap I/O (pswpin / pswpout)',
          subtitle:
            'Source: /proc/vmstat counters pswpin, pswpout. ' +
            'Derived: cumulative page counts converted to pages/s rate.',
        },
        m(LineChartSvg, {
          data: vmstatChartData,
          xAxisMin: data.xMin,
          xAxisMax: data.xMax,
          height: 200,
          xAxisLabel: 'Time (s)',
          yAxisLabel: 'Pages/s',
          showLegend: true,
          showPoints: false,
          gridLines: 'both',
          formatXValue: (v: number) => `${v.toFixed(0)}s`,
          formatYValue: (v: number) => `${v.toFixed(0)} pg/s`,
        }),
      ),

    renderLmkPanel(data.lmkEvents, t0),
  ];
}
