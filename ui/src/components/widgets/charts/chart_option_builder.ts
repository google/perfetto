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

/**
 * Chart option builder utilities.
 *
 * These functions build ECharts option objects with a consistent structure.
 * Theme colors are NOT included here - EChartView applies theme colors
 * automatically by reading CSS variables from the DOM element.
 */

import type {EChartsCoreOption} from 'echarts/core';

/** Font size used for axis tick labels across all charts. */
export const AXIS_LABEL_FONT_SIZE = 10;

/**
 * Configuration for an axis in a chart.
 */
export interface AxisConfig {
  readonly type: 'value' | 'category' | 'log' | 'time';
  readonly name?: string;
  readonly nameGap?: number;
  readonly data?: readonly string[];
  readonly minInterval?: number;
  readonly min?: number;
  readonly max?: number;
  readonly formatter?: (v: number | string) => string;
  readonly labelOverflow?: 'truncate';
  readonly labelWidth?: number;
  readonly showSplitLine?: boolean;
  /**
   * When true, axis range is computed from data min/max instead of
   *  always including zero. Maps to ECharts `scale: true`.
   */
  readonly scale?: boolean;
}

/**
 * Brush configuration for interactive selection.
 */
export interface BrushConfig {
  readonly xAxisIndex?: number;
  readonly yAxisIndex?: number;
  readonly brushType: 'lineX' | 'lineY' | 'rect';
}

/**
 * Build an axis option from config.
 * Theme colors are applied by EChartView, so we omit color settings here.
 *
 * Only X-axis gets a default `nameGap`. Y-axis `nameGap` is intentionally
 * left unset when the caller doesn't provide one, so that EChartView's
 * auto-spacing can measure tick-label width and set it dynamically.
 */
export function buildAxisOption(
  config: AxisConfig,
  isXAxis: boolean,
): Record<string, unknown> {
  const nameGap = config.nameGap ?? (isXAxis ? 25 : undefined);
  const axis: Record<string, unknown> = {
    type: config.type,
    name: config.name,
    nameLocation: 'middle' as const,
    ...(nameGap !== undefined && {nameGap}),
    nameTextStyle: {fontSize: 11},
    axisLabel: {
      fontSize: AXIS_LABEL_FONT_SIZE,
      ...(config.formatter !== undefined && {formatter: config.formatter}),
      ...(config.labelOverflow !== undefined && {
        overflow: config.labelOverflow,
      }),
      ...(config.labelWidth !== undefined && {width: config.labelWidth}),
    },
    splitLine: {
      show: config.showSplitLine ?? false,
    },
  };

  if (config.type === 'category' && config.data !== undefined) {
    axis.data = config.data;
  }
  if (config.minInterval !== undefined) {
    axis.minInterval = config.minInterval;
  }
  if (config.min !== undefined) {
    axis.min = config.min;
  }
  if (config.max !== undefined) {
    axis.max = config.max;
  }
  if (config.scale === true) {
    axis.scale = true;
  }

  return axis;
}

/**
 * Build a grid option. Spacing is auto-computed by EChartView; only pass
 * explicit overrides for non-standard needs (e.g. heatmap color legend).
 */
export function buildGridOption(opts?: {
  top?: number;
  right?: number;
  bottom?: number;
  left?: number;
  containLabel?: boolean;
}): Record<string, unknown> {
  return {
    ...(opts?.top !== undefined && {top: opts.top}),
    ...(opts?.right !== undefined && {right: opts.right}),
    ...(opts?.bottom !== undefined && {bottom: opts.bottom}),
    ...(opts?.left !== undefined && {left: opts.left}),
    containLabel: opts?.containLabel ?? true,
  };
}

/**
 * Build a tooltip option.
 * Theme colors are applied by EChartView.
 * Extra options are merged in (e.g. trigger, formatter).
 */
export function buildTooltipOption(
  extra?: Record<string, unknown>,
): Record<string, unknown> {
  return {
    ...extra,
  };
}

/**
 * Build a brush configuration.
 * Theme colors are applied by EChartView.
 */
export function buildBrushOption(config: BrushConfig): Record<string, unknown> {
  return {
    ...(config.xAxisIndex !== undefined && {xAxisIndex: config.xAxisIndex}),
    ...(config.yAxisIndex !== undefined && {yAxisIndex: config.yAxisIndex}),
    brushType: config.brushType,
    brushMode: 'single' as const,
    brushStyle: {
      borderWidth: 1,
      color: 'rgba(0, 0, 0, 0.1)',
    },
    throttleType: 'debounce' as const,
    throttleDelay: 100,
  };
}

/**
 * Build a legend option.
 * Theme colors are applied by EChartView.
 */
export function buildLegendOption(
  position: 'top' | 'right' = 'top',
): Record<string, unknown> {
  if (position === 'right') {
    return {
      show: true,
      type: 'scroll',
      orient: 'vertical',
      right: 0,
      top: 20,
      bottom: 20,
      textStyle: {
        fontSize: 10,
        width: 120,
        overflow: 'truncate',
        ellipsis: '\u2026',
      },
      tooltip: {show: true},
      pageButtonPosition: 'end',
    };
  }
  return {
    show: true,
    top: 0,
    textStyle: {fontSize: 10},
  };
}

/**
 * Color used to highlight selected items (bars, buckets) in charts.
 */
export const SELECTION_COLOR = 'rgba(0, 120, 212, 0.45)';

/**
 * Build a markArea option to visually highlight a selection region.
 * Each entry in `areas` is a pair of coordinate objects that define
 * opposite corners of the highlighted rectangle.
 *
 * Coordinate objects use axis-specific keys (e.g. `{xAxis: 10}` or
 * `{yAxis: 'label'}`).
 */
export function buildSelectionMarkArea(
  areas: Array<[Record<string, unknown>, Record<string, unknown>]>,
): Record<string, unknown> {
  return {
    silent: true,
    itemStyle: {
      color: 'rgba(0, 120, 212, 0.08)',
      borderColor: 'rgba(0, 120, 212, 0.3)',
      borderWidth: 1,
    },
    data: areas,
  };
}

/**
 * Build a complete base chart option with grid, axes, and optional
 * tooltip/brush/legend. Charts add their own `series` on top.
 *
 * Theme colors (series colors, text colors, etc.) are applied by EChartView
 * which reads CSS variables from the DOM element.
 */
export function buildChartOption(config: {
  readonly grid?: Parameters<typeof buildGridOption>[0];
  readonly xAxis: AxisConfig;
  readonly yAxis: AxisConfig;
  readonly tooltip?: Record<string, unknown>;
  readonly brush?: BrushConfig;
  readonly legend?: Record<string, unknown>;
}): EChartsCoreOption {
  const {grid, xAxis, yAxis, tooltip, brush, legend} = config;

  const option: Record<string, unknown> = {
    animation: false,
    grid: buildGridOption(grid),
    xAxis: buildAxisOption(xAxis, true),
    yAxis: buildAxisOption(yAxis, false),
    tooltip: buildTooltipOption(tooltip),
  };

  if (brush !== undefined) {
    option.brush = buildBrushOption(brush);
    // Hide the default brush toolbox; we activate brush programmatically.
    option.toolbox = {show: false};
  }
  if (legend !== undefined) {
    option.legend = legend;
  }

  return option as EChartsCoreOption;
}

export type LabelFormatter = (value: number | string) => string;

/** Format a label value using an ECharts formatter (function or string). */
export function formatLabel(
  value: number | string,
  formatter: LabelFormatter | string | undefined,
): string {
  if (typeof formatter === 'function') return formatter(value);
  if (typeof formatter === 'string') {
    return formatter.replace('{value}', String(value));
  }
  return String(value);
}

/** Extract representative Y-axis label values for width measurement. */
export function collectYAxisLabels(
  opt: Record<string, unknown>,
): Array<number | string> {
  if (Array.isArray(opt.yAxis)) return [];
  const yAxis = opt.yAxis as Record<string, unknown> | undefined;
  if (yAxis === undefined) return [];
  const axisType = (yAxis.type as string) ?? 'value';

  if (axisType === 'category') {
    const data = yAxis.data;
    if (Array.isArray(data)) return data as string[];
    return [];
  }

  const series = opt.series;
  if (!Array.isArray(series)) return [];

  let min = Infinity;
  let max = -Infinity;
  for (const s of series as Array<Record<string, unknown>>) {
    const data = s.data;
    if (!Array.isArray(data)) continue;
    for (const d of data) {
      if (typeof d === 'number') {
        if (isFinite(d)) {
          if (d < min) min = d;
          if (d > max) max = d;
        }
      } else if (Array.isArray(d)) {
        // Handles [x, y] pairs and boxplot [min, Q1, median, Q3, max].
        for (const el of d) {
          if (typeof el === 'number' && isFinite(el)) {
            if (el < min) min = el;
            if (el > max) max = el;
          }
        }
      } else if (typeof d === 'object' && d !== null) {
        const v = (d as {value?: unknown}).value;
        if (typeof v === 'number' && isFinite(v)) {
          if (v < min) min = v;
          if (v > max) max = v;
        }
      }
    }
  }

  if (!isFinite(min) || !isFinite(max)) return [];

  return generateNiceTicks(min, max, axisType === 'log');
}

/** Generate ~5 nice tick values for a range (approximate, for width estimation). */
export function generateNiceTicks(
  min: number,
  max: number,
  isLog: boolean,
): number[] {
  if (isLog) {
    const minExp = Math.floor(Math.log10(Math.max(min, 1)));
    const maxExp = Math.ceil(Math.log10(Math.max(max, 1)));
    const ticks: number[] = [];
    for (let e = minExp; e <= maxExp; e++) {
      ticks.push(Math.pow(10, e));
    }
    return ticks;
  }

  const range = max - min;
  if (range <= 0) return [min];

  const roughInterval = range / 5;
  const magnitude = Math.pow(10, Math.floor(Math.log10(roughInterval)));
  const residual = roughInterval / magnitude;
  const niceMultiplier =
    residual <= 1.5 ? 1 : residual <= 3 ? 2 : residual <= 7 ? 5 : 10;
  const interval = niceMultiplier * magnitude;

  const ticks: number[] = [];
  const start = Math.floor(min / interval) * interval;
  for (let v = start; v <= max + interval * 0.5; v += interval) {
    ticks.push(v);
  }
  return ticks;
}
