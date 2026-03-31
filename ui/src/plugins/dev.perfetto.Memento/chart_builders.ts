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

import {
  LineChartData,
  LineChartSeries,
} from '../../components/widgets/charts/line_chart';
import {
  categorizeProcess,
  CATEGORIES,
  type CategoryId,
} from './process_categories';
import {type ProcessMemoryRow, OOM_SCORE_BUCKETS} from './tab_processes';
import {SnapshotData} from './memento_session';

/** Compute the earliest timestamp across all data. */
export function computeT0(data: SnapshotData): number {
  let minTs = Infinity;
  for (const arr of data.systemCounters.values()) {
    if (arr.length > 0 && arr[0].ts < minTs) minTs = arr[0].ts;
  }
  for (const counterMap of data.processCountersByName.values()) {
    for (const byTs of counterMap.values()) {
      const firstTs = byTs.keys().next().value;
      if (firstTs !== undefined && firstTs < minTs) minTs = firstTs;
    }
  }
  return minTs < Infinity ? minTs : 0;
}

/** Per-process RSS grouped by category. */
export function buildCategoryTimeSeries(
  data: SnapshotData,
  t0: number,
  counters: readonly string[] = ['mem.rss'],
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

/** Per-process RSS grouped by OOM score bucket. */
export function buildOomScoreTimeSeries(
  data: SnapshotData,
  t0: number,
  counters: readonly string[] = ['mem.rss'],
): LineChartData | undefined {
  const oomByName = new Map<string, number>();
  for (const [processName, counterMap] of data.processCountersByName) {
    const oomTs = counterMap.get('oom_score_adj');
    if (oomTs === undefined || oomTs.size === 0) continue;
    let lastVal = 0;
    for (const val of oomTs.values()) {
      lastVal = val;
    }
    oomByName.set(processName, lastVal);
  }

  const tsSet = new Set<number>();
  const byBucketTs = new Map<number, Map<number, number>>();

  for (const [processName, counterMap] of data.processCountersByName) {
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

/** Drill-down: per-process time-series for a single category. */
export function buildCategoryDrilldown(
  data: SnapshotData,
  categoryId: CategoryId,
  t0: number,
  counters: readonly string[] = ['mem.rss'],
): LineChartData | undefined {
  const targetCat = CATEGORIES[categoryId];
  return buildProcessDrilldown(
    data,
    t0,
    counters,
    (name) => categorizeProcess(name).name === targetCat.name,
  );
}

/** Drill-down: per-process time-series for a single OOM score bucket. */
export function buildOomDrilldown(
  data: SnapshotData,
  bucketIdx: number,
  t0: number,
  counters: readonly string[] = ['mem.rss'],
): LineChartData | undefined {
  const bucket = OOM_SCORE_BUCKETS[bucketIdx];

  const oomByName = new Map<string, number>();
  for (const [processName, counterMap] of data.processCountersByName) {
    const oomTs = counterMap.get('oom_score_adj');
    let oomScore = 0;
    if (oomTs !== undefined && oomTs.size > 0) {
      for (const val of oomTs.values()) {
        oomScore = val;
      }
    }
    oomByName.set(processName, oomScore);
  }

  return buildProcessDrilldown(data, t0, counters, (name) => {
    const score = oomByName.get(name) ?? 0;
    return score >= bucket.minScore && score <= bucket.maxScore;
  });
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

/** System memory breakdown from /proc/meminfo, partitioning MemTotal. */
export function buildSystemTimeSeries(
  data: SnapshotData,
  t0: number,
): LineChartData | undefined {
  // Collect all meminfo counters keyed by counter name → sorted {ts, value}.
  const counterNames = [
    'MemTotal',
    'MemFree',
    'Buffers',
    'Shmem',
    'Active(anon)',
    'Inactive(anon)',
    'Active(file)',
    'Inactive(file)',
    'Slab',
    'KernelStack',
    'PageTables',
    'Zram',
  ];
  const byName = new Map<string, Map<number, number>>();
  for (const name of counterNames) {
    const arr = data.systemCounters.get(name);
    if (arr === undefined) continue;
    const m = new Map<number, number>();
    for (const {ts, value} of arr) {
      m.set(ts, value / 1024); // bytes → KB
    }
    byName.set(name, m);
  }

  // Also check for DMA-BUF heap counter.
  const dmaArr = data.systemCounters.get('mem.dma_heap');
  const dmaByTs = new Map<number, number>();
  if (dmaArr !== undefined) {
    for (const {ts, value} of dmaArr) {
      dmaByTs.set(ts, value / 1024);
    }
  }
  const hasDmaHeap = dmaByTs.size > 0;

  // Collect all timestamps.
  const tsSet = new Set<number>();
  for (const m of byName.values()) {
    for (const ts of m.keys()) tsSet.add(ts);
  }
  for (const ts of dmaByTs.keys()) tsSet.add(ts);
  const timestamps = [...tsSet].sort((a, b) => a - b);
  if (timestamps.length < 2) return undefined;

  // Sample-and-hold: at each timestamp, get the latest known value.
  const lastKnown = new Map<string, number>();
  let lastDmaKb = 0;

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

  for (const ts of timestamps) {
    const x = (ts - t0) / 1e9;

    // Update sample-and-hold values.
    for (const name of counterNames) {
      const m = byName.get(name);
      if (m !== undefined && m.has(ts)) {
        lastKnown.set(name, m.get(ts)!);
      }
    }
    if (dmaByTs.has(ts)) lastDmaKb = dmaByTs.get(ts)!;

    const get = (name: string) => lastKnown.get(name) ?? 0;
    const total = get('MemTotal');
    const free = get('MemFree');
    const anon = get('Active(anon)') + get('Inactive(anon)');
    const fileLru = get('Active(file)') + get('Inactive(file)');
    const shmem = get('Shmem');
    const fileCache = Math.max(0, fileLru - shmem);
    const buffers = get('Buffers');
    const slab = get('Slab');
    const pageTables = get('PageTables');
    const kernelStack = get('KernelStack');
    const zram = get('Zram');
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

/** Page cache stacked chart: Active(file) + Inactive(file) + Shmem. */
export function buildPageCacheTimeSeries(
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

/** File cache 4-way breakdown: Mapped/Unmapped × Dirty/Clean. */
export function buildFileCacheBreakdownTimeSeries(
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

/** File cache activity rates from vmstat counters (events/second). */
export function buildFileCacheActivityTimeSeries(
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
      points.push({
        x: (samples[i].ts - t0) / 1e9,
        y: Math.max(0, delta / dtS),
      });
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

/** PSI memory pressure: cumulative µs → ms/s rate. */
export function buildPsiTimeSeries(
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
      const msPerSec = deltaNs / (dtS * 1e6);
      points.push({x: (samples[i].ts - t0) / 1e9, y: Math.max(0, msPerSec)});
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

/** Page faults: cumulative counts → faults/s rate. */
export function buildPageFaultTimeSeries(
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
      const delta = samples[i].value - samples[i - 1].value;
      points.push({x: (samples[i].ts - t0) / 1e9, y: Math.max(0, delta / dtS)});
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

/** Swap usage: SwapTotal partitioned into dirty, cached, free. */
export function buildSwapTimeSeries(
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

  const firstRow = byTs.get(timestamps[0]);
  if ((firstRow?.get('SwapTotal') ?? 0) === 0) return undefined;

  const dirtyPts: {x: number; y: number}[] = [];
  const cachedPts: {x: number; y: number}[] = [];
  const freePts: {x: number; y: number}[] = [];

  for (const ts of timestamps) {
    const row = byTs.get(ts)!;
    const total = row.get('SwapTotal') ?? 0;
    const free = row.get('SwapFree') ?? 0;
    const cached = row.get('SwapCached') ?? 0;
    const x = (ts - t0) / 1e9;
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

/** Swap I/O: pswpin/pswpout cumulative → pages/s rate. */
export function buildVmstatTimeSeries(
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
      const delta = samples[i].value - samples[i - 1].value;
      points.push({x: (samples[i].ts - t0) / 1e9, y: Math.max(0, delta / dtS)});
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

/** Per-process memory breakdown: Anon+Swap, File, DMA-BUF for a single PID. */
export function buildProcessMemoryBreakdown(
  data: SnapshotData,
  pid: number,
  t0: number,
): LineChartData | undefined {
  const pidCounters = data.processCountersByPid.get(pid);
  if (pidCounters === undefined) return undefined;

  const SERIES_NAMES = ['Anon + Swap', 'File', 'DMA-BUF'] as const;
  const counterMapping: Record<string, string> = {
    'mem.rss.anon': 'Anon + Swap',
    'mem.swap': 'Anon + Swap',
    'mem.rss.file': 'File',
    'mem.dmabuf_rss': 'DMA-BUF',
  };

  const tsSet = new Set<number>();
  const bySeriesTs = new Map<number, Map<string, number>>();

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

/** Build latest per-process memory rows for the table. */
export function buildLatestProcessMemory(
  data: SnapshotData,
): ProcessMemoryRow[] {
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
