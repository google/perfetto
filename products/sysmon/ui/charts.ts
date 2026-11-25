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
  coreHeatmap,
  stackedBarChart,
  timeSeriesChart,
  type LineSeries,
  type LiveStripContext,
} from '../../common/ui/kit/charts';
import {OOM_BUCKETS} from '../../common/ui/kit/sparkline';
import {
  fmtPowerMw,
  fmtRateMb,
  fmtTempC,
} from '../../common/ui/kit/format';
import type {
  CpuCoreRow,
  CpuCoreSeriesRow,
  CpuFreqSeriesRow,
  CpuSeriesRow,
  LmkdRow,
  MemPressureSeriesRow,
  MemSeriesRow,
  OomBucketProcSeriesRow,
  OomMemSeriesRow,
  PowerSeriesRow,
  ProcDetailSeriesRow,
  RunnableHistRow,
  StorageSeriesRow,
  ThermalSeriesRow,
} from './queries';

const PALETTE = [
  '#2563eb',
  '#7c3aed',
  '#0891b2',
  '#ea580c',
  '#16a34a',
  '#ec4899',
  '#d97706',
  '#64748b',
];

export function chartCard(title: string, body: m.Child): m.Child {
  if (!body) return undefined;
  return m('.pf-sysmon-chart-card', [
    m('.pf-sysmon-chart-card__title', title),
    body,
  ]);
}

function fmtGibFromMib(mib: number): string {
  if (mib >= 1024) return `${(mib / 1024).toFixed(1)} GiB`;
  return `${Math.round(mib)} MiB`;
}

function fmtMhz(mhz: number): string {
  if (mhz >= 1000) return `${(mhz / 1000).toFixed(2)} GHz`;
  return `${Math.round(mhz)} MHz`;
}

// Pivot long-form (relS, key, value) rows into aligned LineSeries so stacked
// ECharts area charts always share identical X coordinates across all series.
function pivotSeries<T extends {readonly relS: number}>(
  rows: ReadonlyArray<T>,
  getKey: (r: T) => string,
  getVal: (r: T) => number,
  alignZeros: boolean,
  stripPrefix?: string,
): LineSeries[] {
  if (rows.length === 0) return [];
  const totals = new Map<string, number>();
  const byKey = new Map<string, Map<number, number>>();
  const allTimes = new Set<number>();
  for (const r of rows) {
    const k = getKey(r);
    const v = getVal(r);
    allTimes.add(r.relS);
    totals.set(k, (totals.get(k) ?? 0) + v);
    let tm = byKey.get(k);
    if (!tm) {
      tm = new Map();
      byKey.set(k, tm);
    }
    tm.set(r.relS, v);
  }
  const times = [...allTimes].sort((a, b) => a - b);
  const keys = [...byKey.keys()].sort((a, b) => {
    if (a === 'Other rails') return 1;
    if (b === 'Other rails') return -1;
    return (totals.get(b) ?? 0) - (totals.get(a) ?? 0);
  });

  return keys.map((k, idx) => {
    const tm = byKey.get(k)!;
    const points = alignZeros
      ? times.map((t) => ({t, v: tm.get(t) ?? 0}))
      : times
          .filter((t) => tm.has(t))
          .map((t) => ({t, v: tm.get(t)!}));
    const label =
      stripPrefix && k.startsWith(stripPrefix) ? k.slice(stripPrefix.length) : k;
    return {
      name: label,
      color: k === 'Other rails' ? '#94a3b8' : PALETTE[idx % PALETTE.length]!,
      points,
    };
  });
}

// Stacked physical RAM partition (Anon + File Cache + Kernel & Slab + DMA-BUF
// & GPU + ZRAM + Free + Other) summing cleanly to MemTotal.
export function memoryCompositionChart(
  series: ReadonlyArray<MemSeriesRow>,
  lmkd: ReadonlyArray<LmkdRow>,
  height = 220,
  live?: LiveStripContext,
): m.Child {
  if (series.length < 2) return undefined;
  const defs: Array<{
    name: string;
    color: string;
    pick: (r: MemSeriesRow) => number;
  }> = [
    {name: 'Anon', color: '#7c3aed', pick: (r) => r.anonMib},
    {name: 'File Cache', color: '#0891b2', pick: (r) => r.cacheMib},
    {name: 'Kernel & Slab', color: '#64748b', pick: (r) => r.kernelSlabMib},
    {name: 'DMA-BUF & GPU', color: '#ec4899', pick: (r) => r.dmabufGpuMib},
    {name: 'ZRAM', color: '#f59e0b', pick: (r) => r.zramMib},
    {name: 'Free', color: '#16a34a', pick: (r) => r.freeMib},
    {name: 'Other', color: '#cbd5e1', pick: (r) => r.otherMib},
  ];
  const active = defs.filter((d) => series.some((r) => d.pick(r) > 0.5));
  return timeSeriesChart({
    height,
    stacked: true,
    live,
    formatY: fmtGibFromMib,
    markers: lmkd.map((k) => ({
      t: k.relS,
      label: `LMKD killed ${k.processName}`,
      color: '#dc2626',
    })),
    series: active.map((d) => ({
      name: d.name,
      color: d.color,
      points: series.map((r) => ({t: r.relS, v: d.pick(r)})),
    })),
  });
}

export function memoryPsiChart(
  series: ReadonlyArray<MemPressureSeriesRow>,
  height = 185,
  lmkd?: ReadonlyArray<LmkdRow>,
  live?: LiveStripContext,
): m.Child {
  if (series.length < 2) return undefined;
  return timeSeriesChart({
    height,
    live,
    yMaxFloor: 5,
    formatY: (v) => `${v.toFixed(1)}%`,
    markers: lmkd?.map((k) => ({
      t: k.relS,
      label: `LMK: ${k.processName}`,
      color: '#dc2626',
    })),
    series: [
      {
        name: 'PSI Some %',
        color: '#d97706',
        points: series.map((r) => ({t: r.relS, v: r.psiMemSomePct})),
      },
      {
        name: 'PSI Full %',
        color: '#dc2626',
        points: series.map((r) => ({t: r.relS, v: r.psiMemFullPct})),
      },
    ],
  });
}

export function memoryPagingChart(
  series: ReadonlyArray<MemPressureSeriesRow>,
  height = 185,
  live?: LiveStripContext,
): m.Child {
  if (series.length < 2) return undefined;
  return timeSeriesChart({
    height,
    live,
    yMaxFloor: 10,
    formatY: (v) => `${v >= 1000 ? `${(v / 1000).toFixed(1)}k` : Math.round(v)}/s`,
    series: [
      {
        name: 'Major faults/s',
        color: '#dc2626',
        points: series.map((r) => ({t: r.relS, v: r.pgmajfaultS})),
      },
      {
        name: 'Swap in/s',
        color: '#7c3aed',
        points: series.map((r) => ({t: r.relS, v: r.pswpinS})),
      },
      {
        name: 'Swap out/s',
        color: '#d97706',
        points: series.map((r) => ({t: r.relS, v: r.pswpoutS})),
      },
      {
        name: 'Reclaim scan/s',
        color: '#0891b2',
        points: series.map((r) => ({t: r.relS, v: r.reclaimScanS})),
      },
    ],
  });
}

// Stacked CPU utilization breakdown: User + Kernel (Sys) + I/O Wait + IRQ.
export function cpuBreakdownChart(
  series: ReadonlyArray<CpuSeriesRow>,
  height = 195,
  live?: LiveStripContext,
): m.Child {
  if (series.length < 2) return undefined;
  const hasSplit = series.some(
    (r) => r.sysPct > 0 || r.iowaitPct > 0 || r.irqPct > 0,
  );
  if (!hasSplit) {
    return timeSeriesChart({
      height,
      live,
      yMaxFixed: 100,
      formatY: (v) => `${v.toFixed(0)}%`,
      series: [
        {
          name: 'CPU Busy %',
          color: '#2563eb',
          points: series.map((r) => ({t: r.relS, v: r.busyPct})),
        },
        {
          name: 'CPU PSI %',
          color: '#d97706',
          points: series.map((r) => ({t: r.relS, v: r.psiCpuPct})),
        },
      ],
    });
  }
  return timeSeriesChart({
    height,
    stacked: true,
    live,
    yMaxFixed: 100,
    formatY: (v) => `${v.toFixed(0)}%`,
    series: [
      {
        name: 'User %',
        color: '#2563eb',
        points: series.map((r) => ({t: r.relS, v: r.userPct})),
      },
      {
        name: 'Kernel %',
        color: '#dc2626',
        points: series.map((r) => ({t: r.relS, v: r.sysPct})),
      },
      {
        name: 'I/O Wait %',
        color: '#0891b2',
        points: series.map((r) => ({t: r.relS, v: r.iowaitPct})),
      },
      {
        name: 'IRQ %',
        color: '#d97706',
        points: series.map((r) => ({t: r.relS, v: r.irqPct})),
      },
    ],
  });
}

export function cpuPressureChart(
  series: ReadonlyArray<CpuSeriesRow>,
  height = 195,
  live?: LiveStripContext,
): m.Child {
  if (series.length < 2) return undefined;
  return timeSeriesChart({
    height,
    live,
    yMaxFixed: 100,
    formatY: (v) => `${v.toFixed(0)}%`,
    series: [
      {
        name: 'Busy %',
        color: '#2563eb',
        points: series.map((r) => ({t: r.relS, v: r.busyPct})),
      },
      {
        name: 'CPU PSI %',
        color: '#d97706',
        points: series.map((r) => ({t: r.relS, v: r.psiCpuPct})),
      },
      {
        name: 'I/O Wait %',
        color: '#0891b2',
        points: series.map((r) => ({t: r.relS, v: r.iowaitPct})),
      },
    ],
  });
}

export function cpuFreqChart(
  freqSeries: ReadonlyArray<CpuFreqSeriesRow>,
  height = 195,
  live?: LiveStripContext,
): m.Child {
  if (freqSeries.length < 2) return undefined;
  const series = pivotSeries(
    freqSeries,
    (r) => r.cluster,
    (r) => r.mhz,
    false,
  );
  if (series.length === 0) return undefined;
  return timeSeriesChart({
    height,
    live,
    yMaxFloor: 1000,
    formatY: fmtMhz,
    series,
  });
}

export function cpuCoresBarChart(
  cores: ReadonlyArray<CpuCoreRow>,
  height = 195,
): m.Child {
  if (cores.length === 0) return undefined;
  const hasSplit = cores.some((c) => c.userPct !== null);
  if (!hasSplit) {
    return stackedBarChart({
      height,
      measureLabel: 'CPU %',
      formatMeasure: (v) => `${v.toFixed(0)}%`,
      series: [
        {
          name: 'Busy %',
          items: cores.map((c) => ({
            label: `CPU ${c.cpu}`,
            value: c.busyPct ?? 0,
          })),
        },
      ],
    });
  }
  return stackedBarChart({
    height,
    measureLabel: 'CPU %',
    formatMeasure: (v) => `${v.toFixed(0)}%`,
    series: [
      {
        name: 'User %',
        items: cores.map((c) => ({label: `CPU ${c.cpu}`, value: c.userPct ?? 0})),
      },
      {
        name: 'Kernel %',
        items: cores.map((c) => ({label: `CPU ${c.cpu}`, value: c.sysPct ?? 0})),
      },
      {
        name: 'I/O Wait %',
        items: cores.map((c) => ({
          label: `CPU ${c.cpu}`,
          value: c.iowaitPct ?? 0,
        })),
      },
      {
        name: 'IRQ %',
        items: cores.map((c) => ({label: `CPU ${c.cpu}`, value: c.irqPct ?? 0})),
      },
    ],
  });
}

export function storageThroughputChart(
  series: ReadonlyArray<StorageSeriesRow>,
  height = 195,
  live?: LiveStripContext,
): m.Child {
  if (series.length < 2) return undefined;
  return timeSeriesChart({
    height,
    live,
    yMaxFloor: 5,
    formatY: (v) => fmtRateMb(v),
    series: [
      {
        name: 'Read',
        color: '#0284c7',
        points: series.map((r) => ({t: r.relS, v: r.readMbS})),
      },
      {
        name: 'Write',
        color: '#ea580c',
        points: series.map((r) => ({t: r.relS, v: r.writeMbS})),
      },
    ],
  });
}

export function storageDirtyChart(
  series: ReadonlyArray<StorageSeriesRow>,
  height = 195,
  live?: LiveStripContext,
): m.Child {
  if (series.length < 2) return undefined;
  return timeSeriesChart({
    height,
    live,
    yMaxFloor: 8,
    formatY: (v) => `${v.toFixed(1)} MiB`,
    series: [
      {
        name: 'Dirty',
        color: '#d97706',
        points: series.map((r) => ({t: r.relS, v: r.dirtyMib})),
      },
      {
        name: 'Writeback',
        color: '#dc2626',
        points: series.map((r) => ({t: r.relS, v: r.writebackMib})),
      },
    ],
  });
}

export function storagePsiChart(
  series: ReadonlyArray<StorageSeriesRow>,
  height = 185,
  live?: LiveStripContext,
): m.Child {
  if (series.length < 2) return undefined;
  return timeSeriesChart({
    height,
    live,
    yMaxFloor: 5,
    formatY: (v) => `${v.toFixed(1)}%`,
    series: [
      {
        name: 'IO PSI Some %',
        color: '#d97706',
        points: series.map((r) => ({t: r.relS, v: r.ioPsiPct})),
      },
      {
        name: 'IO PSI Full %',
        color: '#dc2626',
        points: series.map((r) => ({t: r.relS, v: r.ioPsiFullPct})),
      },
    ],
  });
}

export function powerRailsChart(
  powerSeries: ReadonlyArray<PowerSeriesRow>,
  height = 210,
  live?: LiveStripContext,
): m.Child {
  if (powerSeries.length < 2) return undefined;
  const series = pivotSeries(
    powerSeries,
    (r) => r.rail,
    (r) => r.mw,
    true,
    'power.rails.',
  );
  if (series.length === 0) return undefined;
  return timeSeriesChart({
    height,
    stacked: true,
    live,
    yMaxFloor: 200,
    formatY: (v) => fmtPowerMw(v),
    series,
  });
}

export function thermalZonesChart(
  zoneSeries: ReadonlyArray<ThermalSeriesRow>,
  height = 200,
  live?: LiveStripContext,
): m.Child {
  if (zoneSeries.length < 2) return undefined;
  const series = pivotSeries(
    zoneSeries,
    (r) => r.zone,
    (r) => r.tempC,
    false,
  );
  if (series.length === 0) return undefined;
  return timeSeriesChart({
    height,
    scaleAxes: true,
    live,
    formatY: (v) => fmtTempC(v),
    series,
  });
}

export function cpuCoreHeatmapChart(
  coreSeries: ReadonlyArray<CpuCoreSeriesRow>,
  height?: number,
): m.Child {
  return coreHeatmap({rows: coreSeries, height});
}

export type OomMemMetric = 'anon_swap' | 'file' | 'dmabuf';

export function oomMemoryStackedChart(
  rows: ReadonlyArray<OomMemSeriesRow>,
  metric: OomMemMetric,
  onBucketClick?: (bucketIdx: number) => void,
  height = 230,
  live?: LiveStripContext,
): m.Child {
  if (rows.length < 2) return undefined;
  const pick = (r: OomMemSeriesRow) =>
    metric === 'file'
      ? r.fileMib
      : metric === 'dmabuf'
        ? r.dmabufMib
        : r.anonSwapMib;

  const times = [...new Set(rows.map((r) => r.relS))].sort((a, b) => a - b);
  if (times.length < 2) return undefined;

  const byIdx = new Map<number, Map<number, number>>();
  for (const r of rows) {
    let tm = byIdx.get(r.bucketIdx);
    if (!tm) {
      tm = new Map();
      byIdx.set(r.bucketIdx, tm);
    }
    tm.set(r.relS, pick(r));
  }

  const series: LineSeries[] = [];
  for (const b of OOM_BUCKETS) {
    const tm = byIdx.get(b.idx);
    if (!tm) continue;
    const pts = times.map((t) => ({t, v: tm.get(t) ?? 0}));
    if (pts.some((p) => p.v > 0.1)) {
      series.push({name: b.name, color: b.color, points: pts});
    }
  }
  if (series.length === 0) return undefined;

  return timeSeriesChart({
    height,
    stacked: true,
    live,
    chartKey: `oom:${metric}`,
    formatY: fmtGibFromMib,
    series,
    onSeriesClick: onBucketClick
      ? (name) => {
          const found = OOM_BUCKETS.find((b) => b.name === name);
          if (found) onBucketClick(found.idx);
        }
      : undefined,
  });
}

export function oomBucketDrilldownChart(
  rows: ReadonlyArray<OomBucketProcSeriesRow>,
  metric: OomMemMetric,
  height = 230,
  live?: LiveStripContext,
  bucketIdx?: number,
): m.Child {
  if (rows.length < 2) return undefined;
  const pick = (r: OomBucketProcSeriesRow) =>
    metric === 'file'
      ? r.fileMib
      : metric === 'dmabuf'
        ? r.dmabufMib
        : r.anonSwapMib;
  const series = pivotSeries(rows, (r) => r.processName, pick, true);
  if (series.length === 0) return undefined;
  return timeSeriesChart({
    height,
    stacked: true,
    live,
    chartKey: `oom-drill:${bucketIdx ?? 'x'}:${metric}`,
    formatY: fmtGibFromMib,
    series,
  });
}

export function processCpuChart(
  series: ReadonlyArray<ProcDetailSeriesRow>,
  height = 200,
  live?: LiveStripContext,
): m.Child {
  if (series.length < 2) return undefined;
  return timeSeriesChart({
    height,
    live,
    yMaxFloor: 10,
    formatY: (v) => `${v.toFixed(1)}%`,
    series: [
      {
        name: 'CPU %',
        color: '#2563eb',
        points: series.map((r) => ({t: r.relS, v: r.cpuPct})),
      },
    ],
  });
}

export function processMemoryChart(
  series: ReadonlyArray<ProcDetailSeriesRow>,
  height = 200,
  live?: LiveStripContext,
): m.Child {
  // Carry forward last known value per counter because traced_probes only
  // emits changed counters and polls can straddle integer-second boundaries.
  let anon = 0;
  let swap = 0;
  let file = 0;
  let dmabuf = 0;
  let shmem = 0;
  let seen = false;
  const carried: Array<{
    t: number;
    anon: number;
    swap: number;
    file: number;
    dmabuf: number;
    shmem: number;
  }> = [];
  for (const r of series) {
    const hasAny =
      r.anonMib !== null ||
      r.swapMib !== null ||
      r.fileMib !== null ||
      r.dmabufMib !== null ||
      r.shmemMib !== null;
    if (!hasAny) continue;
    if (r.anonMib !== null) anon = r.anonMib;
    if (r.swapMib !== null) swap = r.swapMib;
    if (r.fileMib !== null) file = r.fileMib;
    if (r.dmabufMib !== null) dmabuf = r.dmabufMib;
    if (r.shmemMib !== null) shmem = r.shmemMib;
    seen = true;
    carried.push({t: r.relS, anon, swap, file, dmabuf, shmem});
  }
  if (!seen || carried.length < 2) return undefined;
  const defs: Array<{
    name: string;
    color: string;
    pick: (r: (typeof carried)[number]) => number;
  }> = [
    {name: 'Anon', color: '#7c3aed', pick: (r) => r.anon},
    {name: 'Swap', color: '#f59e0b', pick: (r) => r.swap},
    {name: 'File RSS', color: '#0891b2', pick: (r) => r.file},
    {name: 'DMA-BUF', color: '#ec4899', pick: (r) => r.dmabuf},
    {name: 'Shmem', color: '#64748b', pick: (r) => r.shmem},
  ];
  const active = defs.filter((d) => carried.some((r) => d.pick(r) > 0.1));
  if (active.length === 0) return undefined;
  return timeSeriesChart({
    height,
    stacked: true,
    live,
    formatY: fmtGibFromMib,
    series: active.map((d) => ({
      name: d.name,
      color: d.color,
      points: carried.map((r) => ({t: r.t, v: d.pick(r)})),
    })),
  });
}

export function runnableHistChart(
  hist: ReadonlyArray<RunnableHistRow>,
  height = 190,
): m.Child {
  if (hist.length === 0) return undefined;
  return stackedBarChart({
    height,
    measureLabel: 'Wakeups',
    formatMeasure: (v) => `${Math.round(v)}`,
    series: [
      {
        name: 'Runnable waits',
        items: hist.map((h) => ({label: h.bucketLabel, value: h.cnt})),
      },
    ],
  });
}

// Compact 3-line Memory chart for the Task Manager–style Overview grid:
// Anon working set, File Cache, and Available RAM on one clean axis.
export function memorySummaryLineChart(
  series: ReadonlyArray<MemSeriesRow>,
  lmkd: ReadonlyArray<LmkdRow>,
  height = 145,
  live?: LiveStripContext,
): m.Child {
  if (series.length < 2) return undefined;
  return timeSeriesChart({
    height,
    live,
    formatY: fmtGibFromMib,
    markers: lmkd.map((k) => ({
      t: k.relS,
      label: `LMK: ${k.processName}`,
      color: '#dc2626',
    })),
    series: [
      {
        name: 'Anon',
        color: '#7c3aed',
        points: series.map((r) => ({t: r.relS, v: r.anonMib})),
      },
      {
        name: 'File Cache',
        color: '#0891b2',
        points: series.map((r) => ({t: r.relS, v: r.cacheMib})),
      },
      {
        name: 'Available',
        color: '#16a34a',
        points: series.map((r) => ({t: r.relS, v: r.availMib})),
      },
    ],
  });
}

// Combined CPU / Memory / I/O Pressure Stall Information (PSI) line chart for
// the Task Manager–style Overview grid.
export function systemPsiChart(
  cpuSeries: ReadonlyArray<CpuSeriesRow>,
  memSeries: ReadonlyArray<MemPressureSeriesRow>,
  storageSeries: ReadonlyArray<StorageSeriesRow>,
  height = 145,
  live?: LiveStripContext,
): m.Child {
  const series: LineSeries[] = [];
  if (cpuSeries.length >= 2) {
    series.push({
      name: 'CPU PSI',
      color: '#2563eb',
      points: cpuSeries.map((r) => ({t: r.relS, v: r.psiCpuPct})),
    });
  }
  if (memSeries.length >= 2) {
    series.push({
      name: 'Mem PSI',
      color: '#7c3aed',
      points: memSeries.map((r) => ({t: r.relS, v: r.psiMemSomePct})),
    });
  }
  if (storageSeries.length >= 2) {
    series.push({
      name: 'I/O PSI',
      color: '#0284c7',
      points: storageSeries.map((r) => ({t: r.relS, v: r.ioPsiPct})),
    });
  }
  if (series.length === 0) return undefined;
  return timeSeriesChart({
    height,
    live,
    yMaxFloor: 5,
    formatY: (v) => `${v.toFixed(1)}%`,
    series,
  });
}
