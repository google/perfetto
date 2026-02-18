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
import type {EChartsCoreOption} from 'echarts/core';
import {extractBrushRange, formatNumber} from './chart_utils';
import {EChartView, EChartEventHandler} from './echart_view';
import {buildChartOption, buildLegendOption} from './chart_option_builder';
import {getChartThemeColors} from './chart_theme';

/**
 * A single data point in a scatter chart.
 */
export interface ScatterChartPoint {
  /** X-axis value */
  readonly x: number;
  /** Y-axis value */
  readonly y: number;
  /** Optional bubble size (for bubble charts) */
  readonly size?: number;
  /** Optional per-point color */
  readonly color?: string;
  /** Optional tooltip label */
  readonly label?: string;
}

/**
 * A series (group) of points in the scatter chart.
 */
export interface ScatterChartSeries {
  /** Display name for this series (shown in legend) */
  readonly name: string;
  /** Data points for this series */
  readonly points: readonly ScatterChartPoint[];
  /** Optional custom color for this series (applies to all points without individual color) */
  readonly color?: string;
}

/**
 * Data provided to a ScatterChart.
 */
export interface ScatterChartData {
  /** The series to display */
  readonly series: readonly ScatterChartSeries[];
}

export interface ScatterChartAttrs {
  /**
   * Scatter chart data to display, or undefined if loading.
   * When undefined, a loading spinner is shown.
   */
  readonly data: ScatterChartData | undefined;

  /**
   * Height of the chart in pixels. Defaults to 200.
   */
  readonly height?: number;

  /**
   * Label for the X axis.
   */
  readonly xAxisLabel?: string;

  /**
   * Label for the Y axis.
   */
  readonly yAxisLabel?: string;

  /**
   * Callback when brush selection completes (on mouseup).
   * Called with the selected X range.
   */
  readonly onBrush?: (range: {start: number; end: number}) => void;

  /**
   * Fill parent container. Defaults to false.
   */
  readonly fillParent?: boolean;

  /**
   * Custom class name for the container.
   */
  readonly className?: string;

  /**
   * Format function for X axis tick values.
   */
  readonly formatXValue?: (value: number) => string;

  /**
   * Format function for Y axis tick values.
   */
  readonly formatYValue?: (value: number) => string;

  /**
   * Use logarithmic scale for X axis. Defaults to false.
   */
  readonly logScaleX?: boolean;

  /**
   * Use logarithmic scale for Y axis. Defaults to false.
   */
  readonly logScaleY?: boolean;

  /**
   * Show legend. Defaults to true when multiple series.
   */
  readonly showLegend?: boolean;

  /**
   * Default symbol size for points without explicit size.
   * Defaults to 8.
   */
  readonly symbolSize?: number;

  /**
   * Min/max symbol size for bubble charts (when points have size values).
   * Defaults to [5, 30].
   */
  readonly symbolSizeRange?: [number, number];

  /**
   * When true, axis ranges are computed from data min/max instead of
   * always including zero. Defaults to false.
   */
  readonly scaleAxes?: boolean;
}

export class Scatterplot implements m.ClassComponent<ScatterChartAttrs> {
  view({attrs}: m.Vnode<ScatterChartAttrs>) {
    const {data, height, fillParent, className, onBrush} = attrs;

    const isEmpty =
      data !== undefined &&
      (data.series.length === 0 ||
        data.series.every((s) => s.points.length === 0));
    const option =
      data !== undefined && !isEmpty
        ? buildScatterOption(attrs, data)
        : undefined;

    return m(EChartView, {
      option,
      height,
      fillParent,
      className,
      empty: isEmpty,
      eventHandlers: buildScatterEventHandlers(attrs),
      activeBrushType: onBrush !== undefined ? 'lineX' : undefined,
    });
  }
}

function buildScatterOption(
  attrs: ScatterChartAttrs,
  data: ScatterChartData,
): EChartsCoreOption {
  const {
    xAxisLabel,
    yAxisLabel,
    formatXValue,
    formatYValue,
    logScaleX = false,
    logScaleY = false,
    showLegend,
    symbolSize = 8,
    symbolSizeRange = [5, 30],
  } = attrs;
  const fmtX = formatXValue ?? formatNumber;
  const fmtY = formatYValue ?? formatNumber;

  const theme = getChartThemeColors();
  const displayLegend = showLegend ?? data.series.length > 1;

  // Compute size range for normalization if any points have sizes
  let minSize = Infinity;
  let maxSize = -Infinity;
  for (const s of data.series) {
    for (const p of s.points) {
      if (p.size !== undefined) {
        minSize = Math.min(minSize, p.size);
        maxSize = Math.max(maxSize, p.size);
      }
    }
  }
  const hasSizes = minSize !== Infinity;
  const sizeRange = maxSize - minSize || 1;

  const series = data.series.map((s) => {
    return {
      type: 'scatter' as const,
      name: s.name,
      // ECharts scatter series requires data as arrays with positional indices:
      // [0]: x value (number)
      // [1]: y value (number)
      // [2]: size value (number | null) - used for bubble sizing
      // [3]: label (string | undefined) - used for tooltip display
      // This positional format is mandated by ECharts API for scatter/bubble.
      data: s.points.map((p) => {
        const pointData: [number, number, ...unknown[]] = [p.x, p.y];
        if (p.size !== undefined) {
          pointData.push(p.size);
        } else if (p.label !== undefined) {
          // Placeholder null so label is always at index 3
          pointData.push(null);
        }
        if (p.label !== undefined) pointData.push(p.label);
        return {
          value: pointData,
          itemStyle: p.color !== undefined ? {color: p.color} : undefined,
        };
      }),
      symbolSize: hasSizes
        ? (value: Array<number | null>) => {
            const size = value.length > 2 ? value[2] : undefined;
            if (size === undefined || size === null) return symbolSize;
            const normalized = (size - minSize) / sizeRange;
            return (
              symbolSizeRange[0] +
              normalized * (symbolSizeRange[1] - symbolSizeRange[0])
            );
          }
        : symbolSize,
      itemStyle: s.color !== undefined ? {color: s.color} : undefined,
      emphasis: {
        itemStyle: {borderWidth: 2, borderColor: theme.backgroundColor},
      },
    };
  });

  const option = buildChartOption({
    grid: {
      top: displayLegend ? 30 : 10,
      bottom: xAxisLabel !== undefined ? 40 : 25,
    },
    xAxis: {
      type: logScaleX ? 'log' : 'value',
      name: xAxisLabel,
      formatter:
        formatXValue !== undefined
          ? (v) => formatXValue(v as number)
          : undefined,
      scale: attrs.scaleAxes,
    },
    yAxis: {
      type: logScaleY ? 'log' : 'value',
      name: yAxisLabel,
      formatter:
        formatYValue !== undefined
          ? (v) => formatYValue(v as number)
          : undefined,
      scale: attrs.scaleAxes,
    },
    tooltip: {
      trigger: 'item' as const,
      formatter: (params: {
        seriesName?: string;
        value?: [number, number, (number | null)?, string?];
        color?: string;
        marker?: string;
      }) => {
        const value = params.value;
        if (value === undefined) return '';
        const [x, y, size, label] = value;
        const lines = [
          `${params.marker ?? ''} ${params.seriesName ?? ''}`,
          `X: ${fmtX(x)}`,
          `Y: ${fmtY(y)}`,
        ];
        if (size !== undefined && size !== null) {
          lines.push(`Size: ${formatNumber(size)}`);
        }
        if (label !== undefined) lines.push(label);
        return lines.join('<br>');
      },
    },
    brush: attrs.onBrush
      ? {xAxisIndex: 0, brushType: 'lineX' as const}
      : undefined,
    legend: displayLegend ? buildLegendOption() : {show: false},
  });

  (option as Record<string, unknown>).series = series;
  return option;
}

function buildScatterEventHandlers(
  attrs: ScatterChartAttrs,
): ReadonlyArray<EChartEventHandler> {
  if (!attrs.onBrush) return [];
  const onBrush = attrs.onBrush;

  return [
    {
      eventName: 'brushEnd',
      handler: (params) => {
        const range = extractBrushRange(params);
        if (range !== undefined) {
          const [start, end] = range;
          onBrush({start, end});
        }
      },
    },
  ];
}
