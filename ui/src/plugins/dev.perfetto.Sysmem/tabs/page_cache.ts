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
import {noData, xAxisFor} from '../chart_axis';
import {
  LineChartSvg,
  type LineChartData,
  type LineChartSeries,
} from '../../../components/widgets/charts_svg/line_chart_svg';
import type {SnapshotData} from '../../dev.perfetto.Memscope/live';
import {
  billboardBytes,
  counterPoints,
  maxSeriesKb,
  niceKbInterval,
} from '../../dev.perfetto.Memscope/live';
import {formatBytesIec} from '../../../base/bytes_format';
import {Billboard, BillboardStrip} from '../../dev.perfetto.Memscope/live';
import {Panel} from '../../dev.perfetto.Memscope/live';

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
    const pts = data.series[idx]!.points;
    return pts.length > 0 ? pts[pts.length - 1]!.y : 0;
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

export function renderPageCacheTab(
  data: SnapshotData | undefined,
  live: boolean,
): m.Children {
  if (!data) return null;

  const t0 = data.ts0;
  const pageCacheChartData = buildPageCacheTimeSeries(data, t0);
  const fileCacheBreakdownData = buildFileCacheBreakdownTimeSeries(data, t0);
  const fileCacheActivityData = buildFileCacheActivityTimeSeries(data, t0);
  const bb = getPageCacheBillboards(fileCacheBreakdownData);
  const xWindow = xAxisFor(data, live);
  const formatX = xWindow.format;

  return m.fragment({}, [
    bb !== undefined &&
      m(
        BillboardStrip,
        m(Billboard, {
          ...billboardBytes(bb.total * 1024),
          label: 'Total Page Cache',
          desc: 'Derived: Active(file) + Inactive(file) from /proc/meminfo',
        }),
        m(Billboard, {
          ...billboardBytes(bb.dirty * 1024),
          label: 'Dirty',
          desc: 'Source: Dirty from /proc/meminfo',
        }),
        m(Billboard, {
          ...billboardBytes(bb.mapped * 1024),
          label: 'Mapped',
          desc: 'Source: Mapped from /proc/meminfo',
        }),
      ),

    m(
      Panel,
      m(Panel.Header, {
        title: 'Page Cache',
        help: m.fragment({}, [
          m('div', [
            'Stacked: Active(file) + Inactive(file) + Shmem \u2248 Cached.',
          ]),
          m('div', [
            'Source: /proc/meminfo counters Active(file), Inactive(file), Shmem ' +
              'via the linux.sys_stats data source (traced_probes).',
          ]),
        ]),
      }),
      m(
        Panel.Body,
        pageCacheChartData
          ? m(LineChartSvg, {
              data: pageCacheChartData,
              height: 250,
              showLegend: true,
              showPoints: false,
              stacked: true,
              gridLines: 'both',
              xAxisMin: xWindow.min,
              xAxisMax: xWindow.max,
              formatXValue: formatX,
              formatYValue: (v: number) => formatBytesIec(v * 1024),
              yAxisMinInterval: niceKbInterval(
                maxSeriesKb(pageCacheChartData.series),
              ),
            })
          : noData(live),
      ),
    ),

    m(
      Panel,
      m(Panel.Header, {
        title: 'Page Cache Activity',
        help: m.fragment({}, [
          m('div', [
            'Shown as rates (delta/s). Refaults = workingset_refault_file ' +
              '(evicted pages needed again). Stolen = pgsteal_file (pages reclaimed). ' +
              'Scanned = pgscan_file (pages considered for reclaim).',
          ]),
          m('div', [
            'Source: /proc/vmstat counters via the linux.sys_stats data source ' +
              '(traced_probes).',
          ]),
        ]),
      }),
      m(
        Panel.Body,
        fileCacheActivityData
          ? m(LineChartSvg, {
              data: fileCacheActivityData,
              height: 200,
              showLegend: true,
              showPoints: false,
              gridLines: 'both',
              xAxisMin: xWindow.min,
              xAxisMax: xWindow.max,
              formatXValue: formatX,
              formatYValue: (v: number) => v.toLocaleString(),
            })
          : noData(live),
      ),
    ),
  ]);
}
