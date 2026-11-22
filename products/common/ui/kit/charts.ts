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
  BarChart,
  type BarChartItem,
  type BarChartSeries,
} from '../../../../ui/src/components/widgets/charts/bar_chart';
import {
  HeatmapChart,
  type HeatmapData,
} from '../../../../ui/src/components/widgets/charts/heatmap';
import {
  LineChart,
  type LineChartSeries,
} from '../../../../ui/src/components/widgets/charts/line_chart';
import {
  LiveStripChart,
  type StripSeries,
} from '../../../../ui/src/components/widgets/charts/live_strip_chart';
import {
  ProportionBar,
  type ProportionBarSegment,
} from '../../../../ui/src/components/widgets/charts/proportion_bar';

export interface LiveStripContext {
  // Monotonic boot-clock second corresponding to point t === 0.
  readonly baseSec: number;
  // Latest monotonic trace timestamp (seconds).
  readonly latestSec: number;
  // True when live stream is running; false when paused or replaying.
  readonly isLive: boolean;
  readonly windowSec?: number;
  readonly lagSec?: number;
}

export interface SeriesPoint {
  readonly t: number; // relative seconds from window start
  readonly v: number;
}

export interface LineSeries {
  readonly name: string;
  readonly color: string;
  readonly points: ReadonlyArray<SeriesPoint>;
  readonly fill?: boolean;
  readonly dashed?: boolean;
}

export interface EventMarker {
  readonly t: number;
  readonly label: string;
  readonly color: string;
}

export interface TimeSeriesChartAttrs {
  readonly series: ReadonlyArray<LineSeries>;
  readonly markers?: ReadonlyArray<EventMarker>;
  readonly height?: number;
  readonly yAxisLabel?: string;
  readonly xAxisLabel?: string;
  readonly formatY?: (v: number) => string;
  readonly formatX?: (v: number) => string;
  readonly scaleAxes?: boolean;
  readonly showLegend?: boolean;
  readonly stacked?: boolean;
  readonly xAxisMin?: number;
  readonly xAxisMax?: number;
  readonly onSeriesClick?: (seriesName: string) => void;
  // Optional Mithril key to reset LiveStripChart history on metric/grouping switch.
  readonly chartKey?: string;
  // When provided, renders a 60fps jitter-buffered Canvas rolling strip chart
  // instead of a static ECharts redraw so live streams scroll smoothly.
  readonly live?: LiveStripContext;
  readonly yMaxFixed?: number;
  readonly yMaxFloor?: number;
}

// Thin adapter over Perfetto's LineChart widget (ECharts) for static snapshots,
// or 60fps LiveStripChart when `attrs.live` is supplied.
export function timeSeriesChart(attrs: TimeSeriesChartAttrs): m.Child {
  const nonEmpty = attrs.series.filter((s) => s.points.length > 0);
  if (nonEmpty.length === 0) return undefined;

  if (attrs.live !== undefined) {
    const live = attrs.live;
    let maxPointT = -Infinity;
    const stripSeries: StripSeries[] = nonEmpty.map((s) => ({
      name: s.name,
      color: s.color,
      points: s.points.map((p) => {
        const t = live.baseSec + p.t;
        if (t > maxPointT) maxPointT = t;
        return {t, v: p.v};
      }),
    }));
    const latestSec = Number.isFinite(maxPointT) ? maxPointT : live.latestSec;
    return m(LiveStripChart, {
      resetKey: attrs.chartKey,
      series: stripSeries,
      latestSec,
      windowSec: live.windowSec ?? 60,
      lagSec: live.lagSec ?? 2.0,
      isLive: live.isLive,
      stacked: attrs.stacked,
      scaleAxes: attrs.scaleAxes,
      height: attrs.height ?? 190,
      yMaxFixed: attrs.yMaxFixed,
      yMaxFloor: attrs.yMaxFloor,
      formatY: attrs.formatY ?? ((v) => `${v.toFixed(1)}`),
      showLegend: attrs.showLegend ?? stripSeries.length > 1,
      onSeriesClick: attrs.onSeriesClick,
      markers: attrs.markers?.map((mk) => ({
        t: live.baseSec + mk.t,
        label: mk.label,
        color: mk.color,
      })),
    });
  }

  const series: LineChartSeries[] = nonEmpty.map((s) => ({
    name: s.name,
    color: s.color,
    points: s.points.map((p) => ({x: p.t, y: p.v})),
  }));

  return m(LineChart, {
    data: {series},
    height: attrs.height ?? 190,
    showPoints: false,
    gridLines: 'horizontal',
    legendPosition: 'bottom',
    showLegend: attrs.showLegend ?? series.length > 1,
    stacked: attrs.stacked,
    xAxisMin: attrs.xAxisMin,
    xAxisMax: attrs.xAxisMax,
    xAxisLabel: attrs.xAxisLabel,
    yAxisLabel: attrs.yAxisLabel,
    scaleAxes: attrs.scaleAxes,
    formatXValue: attrs.formatX ?? ((v) => `${Math.round(v)}s`),
    formatYValue: attrs.formatY,
    onSeriesClick: attrs.onSeriesClick,
    markers: attrs.markers?.map((mk) => ({
      x: mk.t,
      color: mk.color,
      label: mk.label,
    })),
  });
}

// Per-core CPU utilization x Time heatmap using Perfetto's HeatmapChart widget.
export function coreHeatmap(attrs: {
  readonly rows: ReadonlyArray<{
    readonly relS: number;
    readonly cpu: number;
    readonly busyPct: number;
  }>;
  readonly height?: number;
}): m.Child {
  if (attrs.rows.length === 0) return undefined;
  const times = [...new Set(attrs.rows.map((r) => r.relS))].sort((a, b) => a - b);
  // Order cores so highest-numbered (big) cores sit at the top of the heatmap.
  const cpus = [...new Set(attrs.rows.map((r) => r.cpu))].sort((a, b) => a - b);
  if (times.length < 2 || cpus.length === 0) return undefined;

  const tIdx = new Map(times.map((t, i) => [t, i]));
  const cIdx = new Map(cpus.map((c, i) => [c, i]));
  const values: [number, number, number][] = [];
  for (const r of attrs.rows) {
    const x = tIdx.get(r.relS);
    const y = cIdx.get(r.cpu);
    if (x !== undefined && y !== undefined) {
      values.push([x, y, Math.round(Math.max(0, Math.min(100, r.busyPct)))]);
    }
  }
  const n = cpus.length;
  const cpuLabel = (c: number) => {
    if (n === 8) {
      if (c === 7) return `CPU ${c} (Big)`;
      if (c >= 4) return `CPU ${c} (Mid)`;
      return `CPU ${c} (Little)`;
    }
    return `CPU ${c}`;
  };
  const data: HeatmapData = {
    xLabels: times.map((t) => `${Math.round(t)}s`),
    yLabels: cpus.map(cpuLabel),
    values,
    min: 0,
    max: 100,
  };
  return m(HeatmapChart, {
    data,
    height: attrs.height ?? Math.max(195, 22 * cpus.length + 58),
    xAxisLabel: 'Time (s)',
    formatValue: (v) => `${Math.round(v)}% busy`,
  });
}

export interface StackedBarChartAttrs {
  readonly series: ReadonlyArray<{
    readonly name: string;
    readonly items: ReadonlyArray<BarChartItem>;
  }>;
  readonly height?: number;
  readonly orientation?: 'vertical' | 'horizontal';
  readonly formatMeasure?: (v: number) => string;
  readonly measureLabel?: string;
  readonly dimensionLabel?: string;
}

// Thin adapter over Perfetto's BarChart widget (ECharts) for stacked/grouped bars.
export function stackedBarChart(attrs: StackedBarChartAttrs): m.Child {
  const series: BarChartSeries[] = attrs.series.filter(
    (s) => s.items.length > 0,
  );
  if (series.length === 0) return undefined;

  return m(BarChart, {
    data: {items: [], series},
    height: attrs.height ?? 210,
    orientation: attrs.orientation ?? 'vertical',
    gridLines: attrs.orientation === 'horizontal' ? 'vertical' : 'horizontal',
    legendPosition: 'bottom',
    measureLabel: attrs.measureLabel,
    dimensionLabel: attrs.dimensionLabel,
    formatMeasure: attrs.formatMeasure,
  });
}

export interface StackedSegment {
  readonly label: string;
  readonly value: number;
  readonly display: string;
  readonly color?: string;
}

// Thin adapter over Perfetto's ProportionBar widget (same widget lat uses).
export function proportionBar(
  segments: ReadonlyArray<StackedSegment>,
): m.Child {
  const segs: ProportionBarSegment[] = segments
    .filter((s) => s.value > 0)
    .map((s) => ({
      label: s.label,
      weight: s.value,
      color: s.color,
      value: s.display,
    }));
  if (segs.length === 0) return undefined;
  return m(ProportionBar, {segments: segs});
}

export interface PressureLane {
  readonly id: string;
  readonly label: string;
  readonly buckets: ReadonlyArray<number>; // normalized 0..1 intensity per bin
}

export interface BrushRange {
  readonly startFrac: number; // 0..1
  readonly endFrac: number; // 0..1
}

// Synchronized multi-resource pressure heatmap using Perfetto's HeatmapChart
// widget with native brush selection (matching lat's stallHeatmap).
export function pressureHeatmap(attrs: {
  readonly lanes: ReadonlyArray<PressureLane>;
  readonly durationS: number;
  readonly selection?: BrushRange;
  readonly onChange: (sel: BrushRange | undefined) => void;
}): m.Child {
  const binCount = Math.max(
    1,
    ...attrs.lanes.map((l) => l.buckets.length),
  );
  const xLabels = Array.from({length: binCount}, (_, i) =>
    `${(((i + 0.5) / binCount) * attrs.durationS).toFixed(1)}s`,
  );
  // Render top-to-bottom visually by reversing yLabels (ECharts draws y=0 at bottom).
  const ordered = [...attrs.lanes].reverse();
  const yLabels = ordered.map((l) => l.label);

  const values: [number, number, number][] = [];
  let max = 0.01;
  ordered.forEach((lane, y) => {
    lane.buckets.forEach((v, x) => {
      const pct = Math.round(Math.max(0, Math.min(1, v)) * 100);
      values.push([x, y, pct]);
      if (pct > max) max = pct;
    });
  });

  const data: HeatmapData = {
    xLabels,
    yLabels,
    values,
    min: 0,
    max: Math.max(100, max),
  };

  let selection: {xLabels: string[]; yLabels: string[]} | undefined;
  if (attrs.selection) {
    const startIdx = Math.max(
      0,
      Math.floor(attrs.selection.startFrac * binCount),
    );
    const endIdx = Math.min(
      binCount - 1,
      Math.ceil(attrs.selection.endFrac * binCount) - 1,
    );
    if (endIdx >= startIdx) {
      selection = {
        xLabels: xLabels.slice(startIdx, endIdx + 1),
        yLabels,
      };
    }
  }

  return m(HeatmapChart, {
    data,
    height: Math.max(170, 28 * ordered.length + 68),
    xAxisLabel: 'Time (drag to focus a window)',
    formatValue: (v) => `${Math.round(v)}% pressure`,
    selection,
    onBrush: (b) => {
      if (b.xLabels.length === 0) {
        attrs.onChange(undefined);
        return;
      }
      const indices = b.xLabels
        .map((lbl) => xLabels.indexOf(lbl))
        .filter((i) => i >= 0);
      if (indices.length === 0) {
        attrs.onChange(undefined);
        return;
      }
      const minI = Math.min(...indices);
      const maxI = Math.max(...indices);
      attrs.onChange({
        startFrac: minI / binCount,
        endFrac: (maxI + 1) / binCount,
      });
    },
  });
}
