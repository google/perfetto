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
import {MementoSession, type SnapshotData} from './memento_session';
import {formatKb, panel} from './utils';

function buildPsiTimeSeries(
  data: SnapshotData,
  t0: number,
): LineChartData | undefined {
  const someRaw = data.systemCounters.get('psi.mem.some');
  if (someRaw === undefined || someRaw.length < 2) return undefined;
  const toRatePoints = (
    samples: {ts: number; value: number}[],
  ): {x: number; y: number}[] => {
    const points: {x: number; y: number}[] = [];
    for (let i = 1; i < samples.length; i++) {
      const dtS = (samples[i].ts - samples[i - 1].ts) / 1e9;
      if (dtS <= 0) continue;
      const deltaNs = samples[i].value - samples[i - 1].value;
      points.push({
        x: (samples[i].ts - t0) / 1e9,
        y: Math.max(0, deltaNs / (dtS * 1e6)),
      });
    }
    return points;
  };
  const series: LineChartSeries[] = [
    {
      name: 'some (any task stalled)',
      points: toRatePoints(someRaw),
      color: '#f39c12',
    },
  ];
  const fullRaw = data.systemCounters.get('psi.mem.full');
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

function buildPageFaultTimeSeries(
  data: SnapshotData,
  t0: number,
): LineChartData | undefined {
  const pgfaultRaw = data.systemCounters.get('pgfault');
  if (pgfaultRaw === undefined || pgfaultRaw.length < 2) return undefined;
  const toRatePoints = (
    samples: {ts: number; value: number}[],
  ): {x: number; y: number}[] => {
    const points: {x: number; y: number}[] = [];
    for (let i = 1; i < samples.length; i++) {
      const dtS = (samples[i].ts - samples[i - 1].ts) / 1e9;
      if (dtS <= 0) continue;
      points.push({
        x: (samples[i].ts - t0) / 1e9,
        y: Math.max(0, (samples[i].value - samples[i - 1].value) / dtS),
      });
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
  const pgmajfaultRaw = data.systemCounters.get('pgmajfault');
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
      {name: 'Swap dirty', points: dirtyPts, color: '#e74c3c'},
      {name: 'SwapCached', points: cachedPts, color: '#f39c12'},
      {name: 'SwapFree', points: freePts, color: '#2ecc71'},
    ],
  };
}

function buildVmstatTimeSeries(
  data: SnapshotData,
  t0: number,
): LineChartData | undefined {
  const pswpinRaw = data.systemCounters.get('pswpin');
  if (pswpinRaw === undefined || pswpinRaw.length < 2) return undefined;
  const toRatePoints = (
    samples: {ts: number; value: number}[],
  ): {x: number; y: number}[] => {
    const points: {x: number; y: number}[] = [];
    for (let i = 1; i < samples.length; i++) {
      const dtS = (samples[i].ts - samples[i - 1].ts) / 1e9;
      if (dtS <= 0) continue;
      points.push({
        x: (samples[i].ts - t0) / 1e9,
        y: Math.max(0, (samples[i].value - samples[i - 1].value) / dtS),
      });
    }
    return points;
  };
  const series: LineChartSeries[] = [
    {name: 'pswpin', points: toRatePoints(pswpinRaw), color: '#3498db'},
  ];
  const pswpoutRaw = data.systemCounters.get('pswpout');
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

function renderLmkPanel(
  events: {ts: number; pid: number; processName: string; oomScoreAdj: number}[],
  t0: number,
): m.Children {
  return panel(
    `LMK Kills (${events.length})`,
    'Low Memory Killer events recorded during this session. ' +
      'Source: lmkd atrace / lowmemorykiller ftrace.',
    m(
      'table.pf-memento-table',
      m(
        'tr',
        m('th', 'Time'),
        m('th', 'PID'),
        m('th', 'Process'),
        m('th', 'OOM Adj'),
      ),
      events.map((ev) =>
        m(
          'tr',
          m('td', `${((ev.ts - t0) / 1e9).toFixed(1)}s`),
          m('td', String(ev.pid)),
          m('td', ev.processName || '(unknown)'),
          m('td', String(ev.oomScoreAdj)),
        ),
      ),
    ),
  );
}

export function renderPressureSwapTab(session: MementoSession): m.Children {
  const data = session.data;
  if (!data) return null;

  const t0 = data.ts0;
  const psiChartData = buildPsiTimeSeries(data, t0);
  const pageFaultChartData = buildPageFaultTimeSeries(data, t0);
  const swapChartData = buildSwapTimeSeries(data, t0);
  const vmstatChartData = buildVmstatTimeSeries(data, t0);

  return [
    panel(
      'Memory Pressure (PSI)',
      'Source: /proc/pressure/memory (psi.mem.some, psi.mem.full). ' +
        'Derived: cumulative \u00b5s converted to ms/s rate. ' +
        '"some" = at least one task stalled, "full" = all tasks stalled.',
      psiChartData
        ? m(LineChart, {
            data: psiChartData,
            height: 200,
            xAxisLabel: 'Time (s)',
            yAxisLabel: 'Stall (ms/s)',
            showLegend: true,
            showPoints: false,
            gridLines: 'horizontal',
            xAxisMin: data.xMin,
            xAxisMax: data.xMax,
            formatXValue: (v: number) => `${v.toFixed(0)}s`,
            formatYValue: (v: number) => `${v.toFixed(1)} ms/s`,
          })
        : m('.pf-memento-placeholder', 'Waiting for data\u2026'),
    ),

    panel(
      'Page Faults',
      'Source: /proc/vmstat counters pgfault, pgmajfault. ' +
        'Derived: cumulative counts converted to faults/s rate. ' +
        'Minor (pgfault) = page in RAM but not in TLB. Major (pgmajfault) = page must be read from disk.',
      pageFaultChartData
        ? m(LineChart, {
            data: pageFaultChartData,
            height: 200,
            xAxisLabel: 'Time (s)',
            yAxisLabel: 'Faults/s',
            showLegend: true,
            showPoints: false,
            gridLines: 'horizontal',
            xAxisMin: data.xMin,
            xAxisMax: data.xMax,
            formatXValue: (v: number) => `${v.toFixed(0)}s`,
            formatYValue: (v: number) => `${v.toFixed(0)} f/s`,
          })
        : m('.pf-memento-placeholder', 'Waiting for data\u2026'),
    ),

    swapChartData &&
      panel(
        'Swap Usage',
        'Source: /proc/meminfo counters SwapTotal, SwapFree, SwapCached. ' +
          'Derived: Swap dirty = (SwapTotal \u2212 SwapFree) \u2212 SwapCached.',
        m(LineChart, {
          data: swapChartData,
          height: 200,
          xAxisMin: data.xMin,
          xAxisMax: data.xMax,
          xAxisLabel: 'Time (s)',
          yAxisLabel: 'Swap',
          showLegend: true,
          showPoints: false,
          stacked: true,
          gridLines: 'horizontal',
          formatXValue: (v: number) => `${v.toFixed(0)}s`,
          formatYValue: (v: number) => formatKb(v),
        }),
      ),

    vmstatChartData &&
      panel(
        'Swap I/O (pswpin / pswpout)',
        'Source: /proc/vmstat counters pswpin, pswpout. ' +
          'Derived: cumulative page counts converted to pages/s rate.',
        m(LineChart, {
          data: vmstatChartData,
          xAxisMin: data.xMin,
          xAxisMax: data.xMax,
          height: 200,
          xAxisLabel: 'Time (s)',
          yAxisLabel: 'Pages/s',
          showLegend: true,
          showPoints: false,
          gridLines: 'horizontal',
          formatXValue: (v: number) => `${v.toFixed(0)}s`,
          formatYValue: (v: number) => `${v.toFixed(0)} pg/s`,
        }),
      ),

    renderLmkPanel(data.lmkEvents, t0),
  ];
}
