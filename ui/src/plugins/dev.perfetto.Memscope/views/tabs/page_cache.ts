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
  billboardKb,
  counterPoints,
  formatKb,
  maxSeriesKb,
  niceKbInterval,
} from '../../utils';
import {Billboard} from '../../components/billboard';
import {Panel} from '../../components/panel';
import {Stack} from '../../../../widgets/stack';

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
      {
        name: 'Active(file)',
        points: activeFilePoints,
        color: 'var(--pf-color-success)',
      },
      {
        name: 'Inactive(file)',
        points: inactiveFilePoints,
        color: 'var(--pf-color-warning)',
      },
      {
        name: 'Shmem (tmpfs/ashmem)',
        points: shmemPoints,
        color: 'var(--pf-color-danger)',
      },
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
      {name: 'Mapped + Dirty', points: mappedDirtyPts},
      {name: 'Mapped + Clean', points: mappedCleanPts},
      {name: 'Unmapped + Dirty', points: unmappedDirtyPts},
      {name: 'Unmapped + Clean', points: unmappedCleanPts},
    ],
  };
}

function buildFileCacheActivityTimeSeries(
  data: SnapshotData,
  t0: number,
): LineChartData | undefined {
  // Rates (events/s) are computed in SQL — see extractSnapshotData in
  // live_session.ts.
  const series: LineChartSeries[] = [];
  const refault = counterPoints(
    data.systemCounters.get('workingset_refault_file'),
    t0,
  );
  if (refault !== undefined) {
    series.push({
      name: 'Refaults (thrashing)',
      points: refault,
      color: 'var(--pf-color-danger)',
    });
  }
  const steal = counterPoints(data.systemCounters.get('pgsteal_file'), t0);
  if (steal !== undefined) {
    series.push({
      name: 'Stolen (reclaimed)',
      points: steal,
      color: 'var(--pf-color-warning)',
    });
  }
  const scan = counterPoints(data.systemCounters.get('pgscan_file'), t0);
  if (scan !== undefined) {
    series.push({
      name: 'Scanned',
      points: scan,
      color: 'var(--pf-color-success)',
    });
  }
  if (series.length === 0) return undefined;
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

export function renderPageCacheTab(session: LiveSession): m.Children {
  const data = session.data;
  if (!data) return null;

  const t0 = data.ts0;
  const pageCacheChartData = buildPageCacheTimeSeries(data, t0);
  const fileCacheBreakdownData = buildFileCacheBreakdownTimeSeries(data, t0);
  const fileCacheActivityData = buildFileCacheActivityTimeSeries(data, t0);
  const bb = getPageCacheBillboards(fileCacheBreakdownData);

  return m(Stack, {spacing: 'large'}, [
    bb !== undefined &&
      m(
        Stack,
        {orientation: 'horizontal', spacing: 'large'},
        m(Billboard, {
          ...billboardKb(bb.total),
          label: 'Total Page Cache',
          desc: 'Derived: Active(file) + Inactive(file) from /proc/meminfo',
        }),
        m(Billboard, {
          ...billboardKb(bb.dirty),
          label: 'Dirty',
          desc: 'Source: Dirty from /proc/meminfo',
        }),
        m(Billboard, {
          ...billboardKb(bb.mapped),
          label: 'Mapped',
          desc: 'Source: Mapped from /proc/meminfo',
        }),
      ),

    m(
      Panel,
      {
        title: 'Page Cache',
        subtitle:
          'Source: /proc/meminfo counters Active(file), Inactive(file), Shmem. ' +
          'Stacked: Active(file) + Inactive(file) + Shmem \u2248 Cached.',
      },
      pageCacheChartData
        ? m(LineChartSvg, {
            data: pageCacheChartData,
            height: 250,
            xAxisLabel: 'Time (s)',
            yAxisLabel: 'Cache',
            showLegend: true,
            showPoints: false,
            stacked: true,
            gridLines: 'both',
            xAxisMin: data.xMin,
            xAxisMax: data.xMax,
            formatXValue: (v: number) => `${v.toFixed(0)}s`,
            formatYValue: (v: number) => formatKb(v),
            yAxisMinInterval: niceKbInterval(
              maxSeriesKb(pageCacheChartData.series),
            ),
          })
        : m('.pf-memscope-placeholder', 'Waiting for data\u2026'),
    ),

    m(
      Panel,
      {
        title: 'Page Cache Activity',
        subtitle:
          'Source: /proc/vmstat counters, shown as rates (delta/s). ' +
          'Refaults = workingset_refault_file (evicted pages needed again). ' +
          'Stolen = pgsteal_file (pages reclaimed). ' +
          'Scanned = pgscan_file (pages considered for reclaim).',
      },
      fileCacheActivityData
        ? m(LineChartSvg, {
            data: fileCacheActivityData,
            height: 200,
            xAxisLabel: 'Time (s)',
            yAxisLabel: 'Pages/s',
            showLegend: true,
            showPoints: false,
            gridLines: 'both',
            xAxisMin: data.xMin,
            xAxisMax: data.xMax,
            formatXValue: (v: number) => `${v.toFixed(0)}s`,
            formatYValue: (v: number) => v.toLocaleString(),
          })
        : m('.pf-memscope-placeholder', 'Waiting for data\u2026'),
    ),
  ]);
}
