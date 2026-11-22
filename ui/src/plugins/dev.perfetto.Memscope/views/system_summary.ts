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
import {noData, xAxisFor} from './chart_axis';
import {
  LiveStripChart,
  type StripSeries,
} from '../../../components/widgets/charts/live_strip_chart';
import {
  LineChartSvg,
  type LineChartData,
  type LineChartSeries,
} from '../../../components/widgets/charts_svg/line_chart_svg';
import type {SnapshotData} from '../sessions/live_session';
import {billboardBytes, maxSeriesKb, niceKbInterval} from '../utils';
import {formatBytesIec} from '../../../base/bytes_format';
import {Billboard, BillboardStrip} from '../components/billboard';
import {Panel} from '../components/panel';

/**
 * System memory breakdown from /proc/meminfo (+ ftrace DMA-BUF / GPU counters),
 * partitioning MemTotal without double-counting:
 *   - Anon: AnonPages (covers Active + Inactive + Unevictable anon; falls back
 *     to Active(anon) + Inactive(anon) - Shmem when AnonPages is absent).
 *   - Page cache: Cached - Shmem (covers Active + Inactive + Unevictable/pinned
 *     file pages; falls back to Active(file) + Inactive(file) + Unevictable).
 *   - Shmem, Buffers, SwapCached, Slab, PageTables, KernelStack, VmallocUsed,
 *     Zram, DMA-BUF (max of mem.dma_heap and ION_heap + ION_heap_pool),
 *     GPU (meminfo Gpu, falling back to global GPU Memory counter), Kernel Misc
 *     (meminfo Misc), MemFree, and Unaccounted (remaining MemTotal).
 */
export function buildSystemTimeSeries(
  data: SnapshotData,
  t0: number,
): LineChartData | undefined {
  const counterNames = [
    'MemTotal',
    'MemFree',
    'Buffers',
    'Cached',
    'SwapCached',
    'Shmem',
    'AnonPages',
    'Active(anon)',
    'Inactive(anon)',
    'Active(file)',
    'Inactive(file)',
    'Unevictable',
    'Slab',
    'KernelStack',
    'PageTables',
    'VmallocUsed',
    'Zram',
    'ION_heap',
    'ION_heap_pool',
    'Gpu',
    'Misc',
  ];
  const byName = new Map<string, Map<number, number>>();
  for (const name of counterNames) {
    const arr = data.systemCounters.get(name);
    if (arr === undefined) continue;
    const tsMap = new Map<number, number>();
    for (const {ts, value} of arr) {
      tsMap.set(ts, value / 1024);
    }
    byName.set(name, tsMap);
  }

  const dmaArr = data.systemCounters.get('mem.dma_heap');
  const dmaByTs = new Map<number, number>();
  if (dmaArr !== undefined) {
    for (const {ts, value} of dmaArr) {
      dmaByTs.set(ts, value / 1024);
    }
  }

  const gpuMemArr = data.systemCounters.get('GPU Memory');
  const gpuMemByTs = new Map<number, number>();
  if (gpuMemArr !== undefined) {
    for (const {ts, value} of gpuMemArr) {
      gpuMemByTs.set(ts, value / 1024);
    }
  }

  const tsSet = new Set<number>();
  for (const tsMap of byName.values()) {
    for (const ts of tsMap.keys()) tsSet.add(ts);
  }
  for (const ts of dmaByTs.keys()) tsSet.add(ts);
  for (const ts of gpuMemByTs.keys()) tsSet.add(ts);
  const timestamps = [...tsSet].sort((a, b) => a - b);
  if (timestamps.length < 2) return undefined;

  const hasAnonPages = byName.has('AnonPages');
  const hasCached = byName.has('Cached');

  const lastKnown = new Map<string, number>();
  let lastDmaKb = 0;
  let lastGpuMemKb = 0;

  const anonPts: {x: number; y: number}[] = [];
  const filePts: {x: number; y: number}[] = [];
  const shmemPts: {x: number; y: number}[] = [];
  const buffersPts: {x: number; y: number}[] = [];
  const swapCachedPts: {x: number; y: number}[] = [];
  const slabPts: {x: number; y: number}[] = [];
  const pageTablesPts: {x: number; y: number}[] = [];
  const kernelStackPts: {x: number; y: number}[] = [];
  const vmallocPts: {x: number; y: number}[] = [];
  const zramPts: {x: number; y: number}[] = [];
  const dmaHeapPts: {x: number; y: number}[] = [];
  const gpuPts: {x: number; y: number}[] = [];
  const miscPts: {x: number; y: number}[] = [];
  const freePts: {x: number; y: number}[] = [];
  const unaccountedPts: {x: number; y: number}[] = [];

  for (const ts of timestamps) {
    const x = (ts - t0) / 1e9;
    for (const name of counterNames) {
      const tsMap = byName.get(name);
      if (tsMap !== undefined && tsMap.has(ts)) {
        lastKnown.set(name, tsMap.get(ts)!);
      }
    }
    if (dmaByTs.has(ts)) lastDmaKb = dmaByTs.get(ts)!;
    if (gpuMemByTs.has(ts)) lastGpuMemKb = gpuMemByTs.get(ts)!;

    const get = (name: string) => lastKnown.get(name) ?? 0;
    const total = get('MemTotal');
    // Skip ftrace events that fired before the first /proc/meminfo poll.
    if (total <= 0) continue;

    const free = get('MemFree');
    const shmem = get('Shmem');
    const anon = hasAnonPages
      ? get('AnonPages')
      : Math.max(0, get('Active(anon)') + get('Inactive(anon)') - shmem);
    const fileCache = hasCached
      ? Math.max(0, get('Cached') - shmem)
      : get('Active(file)') + get('Inactive(file)') + get('Unevictable');
    const buffers = get('Buffers');
    const swapCached = get('SwapCached');
    const slab = get('Slab');
    const pageTables = get('PageTables');
    const kernelStack = get('KernelStack');
    const vmalloc = get('VmallocUsed');
    const zram = get('Zram');
    const ionTotal = get('ION_heap') + get('ION_heap_pool');
    const dmaHeap = Math.max(lastDmaKb, ionTotal);
    const meminfoGpu = get('Gpu');
    const gpu = meminfoGpu > 0 ? meminfoGpu : lastGpuMemKb;
    const misc = get('Misc');
    const accounted =
      anon +
      fileCache +
      shmem +
      buffers +
      swapCached +
      slab +
      pageTables +
      kernelStack +
      vmalloc +
      zram +
      dmaHeap +
      gpu +
      misc +
      free;
    const unaccounted = Math.max(0, total - accounted);

    anonPts.push({x, y: anon});
    filePts.push({x, y: fileCache});
    shmemPts.push({x, y: shmem});
    buffersPts.push({x, y: buffers});
    swapCachedPts.push({x, y: swapCached});
    slabPts.push({x, y: slab});
    pageTablesPts.push({x, y: pageTables});
    kernelStackPts.push({x, y: kernelStack});
    vmallocPts.push({x, y: vmalloc});
    zramPts.push({x, y: zram});
    dmaHeapPts.push({x, y: dmaHeap});
    gpuPts.push({x, y: gpu});
    miscPts.push({x, y: misc});
    freePts.push({x, y: free});
    unaccountedPts.push({x, y: unaccounted});
  }

  if (anonPts.length < 2) return undefined;

  const maybeAdd = (
    list: LineChartSeries[],
    name: string,
    points: {x: number; y: number}[],
    alwaysInclude = false,
  ) => {
    if (alwaysInclude || points.some((p) => p.y > 0)) {
      list.push({name, points});
    }
  };

  const series: LineChartSeries[] = [];
  maybeAdd(series, 'Anon', anonPts, true);
  maybeAdd(series, 'Page cache', filePts, true);
  maybeAdd(series, 'Shmem', shmemPts);
  maybeAdd(series, 'Buffers', buffersPts);
  maybeAdd(series, 'SwapCached', swapCachedPts);
  maybeAdd(series, 'Slab', slabPts, true);
  maybeAdd(series, 'PageTables', pageTablesPts);
  maybeAdd(series, 'KernelStack', kernelStackPts);
  maybeAdd(series, 'VmallocUsed', vmallocPts);
  maybeAdd(series, 'Zram', zramPts);
  maybeAdd(series, 'DMA-BUF', dmaHeapPts);
  maybeAdd(series, 'GPU', gpuPts);
  maybeAdd(series, 'Kernel Misc', miscPts);
  maybeAdd(series, 'MemFree', freePts, true);
  maybeAdd(series, 'Unaccounted', unaccountedPts);
  return {series};
}

interface SystemBillboards {
  totalKb: number;
  availableKb: number;
  anonKb: number;
  fileCacheKb: number;
  freeKb: number;
}

/** Latest system memory values for billboard display. */
function buildSystemBillboards(
  data: SnapshotData,
): SystemBillboards | undefined {
  const hasCounter = (name: string): boolean => {
    const arr = data.systemCounters.get(name);
    return arr !== undefined && arr.length > 0;
  };
  const getLatest = (name: string): number => {
    const arr = data.systemCounters.get(name);
    if (arr === undefined || arr.length === 0) return 0;
    return Math.round(arr[arr.length - 1]!.value / 1024);
  };
  const totalKb = getLatest('MemTotal');
  if (totalKb === 0) return undefined;
  const shmemKb = getLatest('Shmem');
  const anonKb = hasCounter('AnonPages')
    ? getLatest('AnonPages')
    : Math.max(
        0,
        getLatest('Active(anon)') + getLatest('Inactive(anon)') - shmemKb,
      );
  const fileCacheKb = hasCounter('Cached')
    ? Math.max(0, getLatest('Cached') - shmemKb)
    : getLatest('Active(file)') +
      getLatest('Inactive(file)') +
      getLatest('Unevictable');
  return {
    totalKb,
    availableKb: getLatest('MemAvailable'),
    anonKb,
    fileCacheKb,
    freeKb: getLatest('MemFree'),
  };
}

const SYSTEM_SERIES_COLORS: Record<string, string> = {
  'Anon': '#7c3aed',
  'Page cache': '#0891b2',
  'Shmem': '#2563eb',
  'Buffers': '#0284c7',
  'SwapCached': '#0ea5e9',
  'Slab': '#64748b',
  'PageTables': '#475569',
  'KernelStack': '#94a3b8',
  'VmallocUsed': '#6366f1',
  'Zram': '#f59e0b',
  'DMA-BUF': '#ec4899',
  'GPU': '#ea580c',
  'Kernel Misc': '#78716c',
  'MemFree': '#16a34a',
  'Unaccounted': '#cbd5e1',
};

// Device-wide memory: Sysmem's System tab, and context on Memscope's live
// page.
export function renderSystemSummary(
  data: SnapshotData | undefined,
  live: boolean,
  chartHeight = 400,
  isPlaying = live,
): m.Children {
  return [
    renderSystemBillboards(data),
    renderSystemChart(data, live, chartHeight, isPlaying),
  ];
}

export function renderSystemBillboards(
  data: SnapshotData | undefined,
): m.Children {
  const bb = data && buildSystemBillboards(data);
  return (
    bb !== undefined &&
    m(BillboardStrip, [
      m(Billboard, {
        ...billboardBytes(bb.totalKb * 1024),
        label: 'MemTotal',
        desc: 'Total physical RAM',
      }),
      m(Billboard, {
        ...billboardBytes(bb.availableKb * 1024),
        label: 'MemAvailable',
        desc: 'Available without swapping',
      }),
      m(Billboard, {
        ...billboardBytes(bb.anonKb * 1024),
        label: 'Anon',
        desc: 'AnonPages (mapped anonymous RAM)',
      }),
      m(Billboard, {
        ...billboardBytes(bb.fileCacheKb * 1024),
        label: 'Page Cache',
        desc: 'Cached \u2212 Shmem (includes pinned files)',
      }),
      m(Billboard, {
        ...billboardBytes(bb.freeKb * 1024),
        label: 'MemFree',
        desc: 'Completely unused RAM',
      }),
    ])
  );
}

export function renderSystemChart(
  data: SnapshotData | undefined,
  live: boolean,
  chartHeight = 400,
  isPlaying = live,
): m.Children {
  if (!data) return null;
  const chartData = buildSystemTimeSeries(data, data.ts0);
  const xWindow = xAxisFor(data, live);
  const formatX = xWindow.format;

  let body: m.Children;
  if (!chartData) {
    body = noData(live);
  } else if (live) {
    const baseSec = data.ts0 / 1e9;
    let maxPointT = -Infinity;
    const stripSeries: StripSeries[] = chartData.series
      .filter((s) => s.points.some((p) => p.y > 0))
      .map((s) => ({
        name: s.name,
        color: s.color ?? SYSTEM_SERIES_COLORS[s.name] ?? '#64748b',
        points: s.points.map((p) => {
          const t = baseSec + p.x;
          if (t > maxPointT) maxPointT = t;
          return {t, v: p.y};
        }),
      }));
    body = m(LiveStripChart, {
      series: stripSeries,
      latestSec: Number.isFinite(maxPointT) ? maxPointT : baseSec + data.xMax,
      windowSec: 60,
      lagSec: 2.0,
      isLive: isPlaying,
      stacked: true,
      height: chartHeight,
      formatY: (v: number) => formatBytesIec(v * 1024),
      showLegend: true,
    });
  } else {
    body = m(LineChartSvg, {
      data: chartData,
      height: chartHeight,
      showLegend: true,
      showPoints: false,
      stacked: true,
      gridLines: 'both',
      xAxisMin: xWindow.min,
      xAxisMax: xWindow.max,
      formatXValue: formatX,
      formatYValue: (v: number) => formatBytesIec(v * 1024),
      yAxisMinInterval: niceKbInterval(maxSeriesKb(chartData.series)),
    });
  }

  return m(
    Panel,
    m(Panel.Header, {
      title: 'System Memory',
      help: m.fragment({}, [
        m('div', [
          'Stacked areas partition MemTotal. Anon = AnonPages; ' +
            'Page cache = Cached \u2212 Shmem (including pinned/unevictable file pages); ' +
            'DMA-BUF combines ION_heap / ION_heap_pool and dma_heap_stat; ' +
            'GPU uses /proc/meminfo Gpu or android.gpu.memory. ' +
            'Unaccounted = remaining MemTotal (direct kernel/driver alloc_pages(), per-CPU areas, HugePages, or drivers without meminfo hooks).',
        ]),
        m('div', [
          'Source: linux.sys_stats (/proc/meminfo), android.gpu.memory, ' +
            'and ftrace dma_heap_stat / gpu_mem_total.',
        ]),
      ]),
    }),
    m(Panel.Body, body),
  );
}
