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
import {MementoSession, SnapshotData} from './memento_session';
import {billboardKb, formatKb, panel} from './utils';

/** System memory breakdown from /proc/meminfo, partitioning MemTotal. */
function buildSystemTimeSeries(
  data: SnapshotData,
  t0: number,
): LineChartData | undefined {
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
  const hasDmaHeap = dmaByTs.size > 0;

  const tsSet = new Set<number>();
  for (const tsMap of byName.values()) {
    for (const ts of tsMap.keys()) tsSet.add(ts);
  }
  for (const ts of dmaByTs.keys()) tsSet.add(ts);
  const timestamps = [...tsSet].sort((a, b) => a - b);
  if (timestamps.length < 2) return undefined;

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
    for (const name of counterNames) {
      const tsMap = byName.get(name);
      if (tsMap !== undefined && tsMap.has(ts)) {
        lastKnown.set(name, tsMap.get(ts)!);
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
    series.splice(series.length - 2, 0, {
      name: 'DMA-BUF',
      points: dmaHeapPts,
      color: '#00acc1',
    });
  }
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
  const getLatest = (name: string): number => {
    const arr = data.systemCounters.get(name);
    if (arr === undefined || arr.length === 0) return 0;
    return Math.round(arr[arr.length - 1].value / 1024);
  };
  const totalKb = getLatest('MemTotal');
  if (totalKb === 0) return undefined;
  const anonKb = getLatest('Active(anon)') + getLatest('Inactive(anon)');
  const fileLruKb = getLatest('Active(file)') + getLatest('Inactive(file)');
  const shmemKb = getLatest('Shmem');
  return {
    totalKb,
    availableKb: getLatest('MemAvailable'),
    anonKb,
    fileCacheKb: Math.max(0, fileLruKb - shmemKb),
    freeKb: getLatest('MemFree'),
  };
}

export function renderSystemTab(session: MementoSession): m.Children {
  const data = session.data;
  if (!data) return null;

  const t0 = data.ts0;
  const bb = buildSystemBillboards(data);
  const chartData = buildSystemTimeSeries(data, t0);

  return [
    bb !== undefined &&
      m(
        '.pf-memento-billboards',
        m(
          '.pf-memento-billboard',
          m('.pf-memento-billboard__value', billboardKb(bb.totalKb)),
          m('.pf-memento-billboard__label', 'MemTotal'),
          m('.pf-memento-billboard__desc', 'Total physical RAM'),
        ),
        m(
          '.pf-memento-billboard',
          m('.pf-memento-billboard__value', billboardKb(bb.availableKb)),
          m('.pf-memento-billboard__label', 'MemAvailable'),
          m('.pf-memento-billboard__desc', 'Available without swapping'),
        ),
        m(
          '.pf-memento-billboard',
          m('.pf-memento-billboard__value', billboardKb(bb.anonKb)),
          m('.pf-memento-billboard__label', 'Anon'),
          m('.pf-memento-billboard__desc', 'Active(anon) + Inactive(anon)'),
        ),
        m(
          '.pf-memento-billboard',
          m('.pf-memento-billboard__value', billboardKb(bb.fileCacheKb)),
          m('.pf-memento-billboard__label', 'Page Cache'),
          m('.pf-memento-billboard__desc', 'File LRU \u2212 Shmem'),
        ),
        m(
          '.pf-memento-billboard',
          m('.pf-memento-billboard__value', billboardKb(bb.freeKb)),
          m('.pf-memento-billboard__label', 'MemFree'),
          m('.pf-memento-billboard__desc', 'Completely unused RAM'),
        ),
      ),

    panel(
      'System Memory',
      'Stacked areas partition MemTotal. Source: /proc/meminfo. ' +
        'Anon = Active(anon) + Inactive(anon). Page cache = Active(file) + Inactive(file) \u2212 Shmem. ' +
        'Unaccounted = MemTotal \u2212 sum of all other categories.',
      chartData
        ? m(LineChart, {
            data: chartData,
            height: 400,
            xAxisLabel: 'Time (s)',
            yAxisLabel: 'Memory',
            showLegend: true,
            showPoints: false,
            stacked: true,
            gridLines: 'horizontal',
            xAxisMin: data.xMin,
            xAxisMax: data.xMax,
            formatXValue: (v: number) => `${v.toFixed(0)}s`,
            formatYValue: (v: number) => formatKb(v),
          })
        : m('.pf-memento-placeholder', 'Waiting for data\u2026'),
    ),
  ];
}
