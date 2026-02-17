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

import type {EChartsCoreOption} from 'echarts/core';
import {getChartThemeColors} from './chart_theme';

/**
 * Configuration for an axis in a chart.
 */
export interface AxisConfig {
  readonly type: 'value' | 'category' | 'log';
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
 * Explicitly includes theme colors because ECharts doesn't deep merge
 * option objects with theme objects - setting axisLabel overrides the theme's
 * axisLabel entirely, so we must include colors here.
 */
export function buildAxisOption(
  config: AxisConfig,
  isXAxis: boolean,
): Record<string, unknown> {
  const theme = getChartThemeColors();
  const axis: Record<string, unknown> = {
    type: config.type,
    name: config.name,
    nameLocation: isXAxis ? ('middle' as const) : ('end' as const),
    nameGap: config.nameGap ?? (isXAxis ? 25 : 10),
    nameTextStyle: {fontSize: 11, color: theme.textColor},
    axisLabel: {
      fontSize: 10,
      color: theme.textColor,
      ...(config.formatter !== undefined && {formatter: config.formatter}),
      ...(config.labelOverflow !== undefined && {
        overflow: config.labelOverflow,
      }),
      ...(config.labelWidth !== undefined && {width: config.labelWidth}),
    },
    axisTick: {lineStyle: {color: theme.borderColor}},
    axisLine: {lineStyle: {color: theme.borderColor}},
    splitLine: {
      show: config.showSplitLine ?? false,
      lineStyle: {color: theme.borderColor},
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
 * Build a themed grid option.
 */
export function buildGridOption(opts?: {
  top?: number;
  right?: number;
  bottom?: number;
  left?: number;
  containLabel?: boolean;
}): Record<string, unknown> {
  return {
    top: opts?.top ?? 20,
    right: opts?.right ?? 10,
    bottom: opts?.bottom ?? 25,
    left: opts?.left ?? 10,
    containLabel: opts?.containLabel ?? true,
  };
}

/**
 * Build a themed tooltip option.
 * Explicitly includes theme colors so tooltips adapt when the theme changes.
 * Extra options are merged in (e.g. trigger, formatter).
 */
export function buildTooltipOption(
  extra?: Record<string, unknown>,
): Record<string, unknown> {
  const theme = getChartThemeColors();
  return {
    backgroundColor: theme.backgroundColor,
    borderColor: theme.borderColor,
    textStyle: {color: theme.textColor},
    ...extra,
  };
}

/**
 * Build a brush configuration.
 * Uses accent color from theme (ECharts doesn't theme brush colors).
 */
export function buildBrushOption(config: BrushConfig): Record<string, unknown> {
  const theme = getChartThemeColors();
  return {
    ...(config.xAxisIndex !== undefined && {xAxisIndex: config.xAxisIndex}),
    ...(config.yAxisIndex !== undefined && {yAxisIndex: config.yAxisIndex}),
    brushType: config.brushType,
    brushMode: 'single' as const,
    brushStyle: {
      borderWidth: 1,
      color: 'rgba(0, 0, 0, 0.1)',
      borderColor: theme.accentColor,
    },
    throttleType: 'debounce' as const,
    throttleDelay: 100,
  };
}

/**
 * Build a legend option.
 * Explicitly includes theme colors because ECharts doesn't deep merge
 * option objects with theme objects.
 */
export function buildLegendOption(
  position: 'top' | 'right' = 'top',
): Record<string, unknown> {
  const theme = getChartThemeColors();
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
        color: theme.textColor,
      },
      tooltip: {show: true},
      pageButtonPosition: 'end',
    };
  }
  return {
    show: true,
    top: 0,
    textStyle: {fontSize: 10, color: theme.textColor},
  };
}

/**
 * Build a complete base chart option with grid, axes, and optional
 * tooltip/brush/legend. Charts add their own `series` on top.
 *
 * The `color` array (series colors) and tooltip theme colors are included
 * so that charts update automatically when the Mithril component re-renders
 * after a theme change — without needing to reinitialize the ECharts instance.
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
  const theme = getChartThemeColors();

  const option: Record<string, unknown> = {
    animation: false,
    color: [...theme.chartColors],
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
