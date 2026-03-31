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
import {computeT0, formatKb, panel} from './utils';

function buildPageCacheTimeSeries(
  data: SnapshotData,
  t0: number,
): LineChartData | undefined {
  const counterNames = ['Cached', 'Shmem', 'Active(file)', 'Inactive(file)'];
  const byTs = new Map<number, Map<string, number>>();
  for (const name of counterNames) {
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
  return {
    series: [
      {name: 'Active(file)', points: activeFilePoints, color: '#2ecc71'},
      {name: 'Inactive(file)', points: inactiveFilePoints, color: '#f39c12'},
      {name: 'Shmem (tmpfs/ashmem)', points: shmemPoints, color: '#9b59b6'},
    ],
  };
}

function buildFileCacheBreakdownTimeSeries(
  data: SnapshotData,
  t0: number,
): LineChartData | undefined {
  const counterNames = ['Active(file)', 'Inactive(file)', 'Mapped', 'Dirty'];
  const byTs = new Map<number, Map<string, number>>();
  for (const name of counterNames) {
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

function buildFileCacheActivityTimeSeries(
  data: SnapshotData,
  t0: number,
): LineChartData | undefined {
  const toRatePoints = (
    samples: {ts: number; value: number}[],
  ): {x: number; y: number}[] => {
    const points: {x: number; y: number}[] = [];
    for (let i = 1; i < samples.length; i++) {
      const dtS = (samples[i].ts - samples[i - 1].ts) / 1e9;
      if (dtS <= 0) continue;
      const delta = samples[i].value - samples[i - 1].value;
      points.push({x: (samples[i].ts - t0) / 1e9, y: Math.max(0, delta / dtS)});
    }
    return points;
  };
  const series: LineChartSeries[] = [];
  const refaultRaw = data.systemCounters.get('workingset_refault_file');
  if (refaultRaw !== undefined && refaultRaw.length >= 2) {
    series.push({
      name: 'Refaults (thrashing)',
      points: toRatePoints(refaultRaw),
      color: '#e74c3c',
    });
  }
  const stealRaw = data.systemCounters.get('pgsteal_file');
  if (stealRaw !== undefined && stealRaw.length >= 2) {
    series.push({
      name: 'Stolen (reclaimed)',
      points: toRatePoints(stealRaw),
      color: '#f39c12',
    });
  }
  const scanRaw = data.systemCounters.get('pgscan_file');
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

function getPageCacheBillboards(
  fileCacheBreakdownData?: LineChartData,
): {total: number; dirty: number; mapped: number} | undefined {
  const data = fileCacheBreakdownData;
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

export function renderPageCacheTab(session: MementoSession): m.Children {
  const data = session.data;
  if (!data) return null;

  const t0 = computeT0(data);
  const pageCacheChartData = buildPageCacheTimeSeries(data, t0);
  const fileCacheBreakdownData = buildFileCacheBreakdownTimeSeries(data, t0);
  const fileCacheActivityData = buildFileCacheActivityTimeSeries(data, t0);
  const billboards = getPageCacheBillboards(fileCacheBreakdownData);

  return [
    billboards !== undefined &&
      m(
        '.pf-memento-billboards',
        m(
          '.pf-memento-billboard',
          m('.pf-memento-billboard__value', formatKb(billboards.total)),
          m('.pf-memento-billboard__label', 'Total Page Cache'),
          m(
            '.pf-memento-billboard__desc',
            'Derived: Active(file) + Inactive(file) from /proc/meminfo',
          ),
        ),
        m(
          '.pf-memento-billboard',
          m('.pf-memento-billboard__value', formatKb(billboards.dirty)),
          m('.pf-memento-billboard__label', 'Dirty'),
          m('.pf-memento-billboard__desc', 'Source: Dirty from /proc/meminfo'),
        ),
        m(
          '.pf-memento-billboard',
          m('.pf-memento-billboard__value', formatKb(billboards.mapped)),
          m('.pf-memento-billboard__label', 'Mapped'),
          m('.pf-memento-billboard__desc', 'Source: Mapped from /proc/meminfo'),
        ),
      ),

    panel(
      'Page Cache',
      'Source: /proc/meminfo counters Active(file), Inactive(file), Shmem. ' +
        'Stacked: Active(file) + Inactive(file) + Shmem \u2248 Cached.',
      pageCacheChartData
        ? m(LineChart, {
            data: pageCacheChartData,
            height: 250,
            xAxisLabel: 'Time (s)',
            yAxisLabel: 'Cache',
            showLegend: true,
            showPoints: false,
            stacked: true,
            gridLines: 'horizontal',
            formatXValue: (v: number) => `${v.toFixed(0)}s`,
            formatYValue: (v: number) => formatKb(v),
          })
        : m('.pf-memento-placeholder', 'Waiting for data\u2026'),
    ),

    panel(
      'Page Cache Activity',
      'Source: /proc/vmstat counters, shown as rates (delta/s). ' +
        'Refaults = workingset_refault_file (evicted pages needed again). ' +
        'Stolen = pgsteal_file (pages reclaimed). ' +
        'Scanned = pgscan_file (pages considered for reclaim).',
      fileCacheActivityData
        ? m(LineChart, {
            data: fileCacheActivityData,
            height: 200,
            xAxisLabel: 'Time (s)',
            yAxisLabel: 'Pages/s',
            showLegend: true,
            showPoints: false,
            gridLines: 'horizontal',
            formatXValue: (v: number) => `${v.toFixed(0)}s`,
            formatYValue: (v: number) => v.toLocaleString(),
          })
        : m('.pf-memento-placeholder', 'Waiting for data\u2026'),
    ),
  ];
}
