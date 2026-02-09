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

/**
 * A single data point in a line chart series.
 */
export interface LineChartPoint {
  /** X-axis value (typically time or sequential index) */
  readonly x: number;
  /** Y-axis value */
  readonly y: number;
}

/**
 * A single series (line) in the chart.
 */
export interface LineChartSeries {
  /** Display name for this series (shown in legend) */
  readonly name: string;
  /** Data points for this series, sorted by x value */
  readonly points: readonly LineChartPoint[];
  /** Optional custom color for this series */
  readonly color?: string;
}

/**
 * Data provided to a LineChart.
 */
export interface LineChartData {
  /** The series to display */
  readonly series: readonly LineChartSeries[];
}

export interface LineChartAttrs {
  /**
   * Line chart data to display, or undefined if loading.
   * When undefined, a loading spinner is shown.
   */
  readonly data: LineChartData | undefined;

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
   * Use logarithmic scale for Y axis. Defaults to false.
   */
  readonly logScale?: boolean;

  /**
   * When true, X axis ticks will be snapped to integer values.
   */
  readonly integerX?: boolean;

  /**
   * When true, Y axis ticks will be snapped to integer values.
   */
  readonly integerY?: boolean;

  /**
   * Show legend. Defaults to true when multiple series.
   */
  readonly showLegend?: boolean;

  /**
   * Show data points as circles. Defaults to true.
   */
  readonly showPoints?: boolean;

  /**
   * Line width in pixels. Defaults to 2.
   */
  readonly lineWidth?: number;

  /**
   * Explicit minimum value for X axis. When set, the axis starts at this value.
   */
  readonly xAxisMin?: number;

  /**
   * Explicit maximum value for X axis. When set, the axis ends at this value.
   */
  readonly xAxisMax?: number;

  /**
   * When true, axis ranges are computed from data min/max instead of
   * always including zero. Defaults to false.
   */
  readonly scaleAxes?: boolean;
}

export class LineChart implements m.ClassComponent<LineChartAttrs> {
  view({attrs}: m.Vnode<LineChartAttrs>) {
    const {data, height, fillParent, className, onBrush} = attrs;

    const isEmpty =
      data !== undefined &&
      (data.series.length === 0 ||
        data.series.every((s) => s.points.length === 0));
    const option =
      data !== undefined && !isEmpty ? buildLineOption(attrs, data) : undefined;

    return m(EChartView, {
      option,
      height,
      fillParent,
      className,
      empty: isEmpty,
      eventHandlers: buildLineEventHandlers(attrs, data),
      activeBrushType: onBrush !== undefined ? 'lineX' : undefined,
    });
  }
}

function buildLineOption(
  attrs: LineChartAttrs,
  data: LineChartData,
): EChartsCoreOption {
  const {
    xAxisLabel,
    yAxisLabel,
    formatXValue,
    formatYValue,
    logScale = false,
    integerX = false,
    integerY = false,
    showLegend,
    showPoints = true,
    lineWidth = 2,
  } = attrs;
  const fmtX = formatXValue ?? formatNumber;
  const fmtY = formatYValue ?? formatNumber;

  const displayLegend = showLegend ?? data.series.length > 1;

  const series = data.series.map((s) => {
    return {
      type: 'line' as const,
      name: s.name,
      data: s.points.map((p) => [p.x, p.y]),
      lineStyle:
        s.color !== undefined
          ? {width: lineWidth, color: s.color}
          : {width: lineWidth},
      itemStyle: s.color !== undefined ? {color: s.color} : undefined,
      showSymbol: showPoints,
      symbolSize: 6,
      emphasis: {itemStyle: {borderWidth: 2}},
    };
  });

  const option = buildChartOption({
    grid: {
      top: displayLegend ? 30 : 10,
      bottom: xAxisLabel ? 40 : 25,
    },
    xAxis: {
      type: 'value',
      name: xAxisLabel,
      formatter:
        formatXValue !== undefined
          ? (v) => formatXValue(v as number)
          : undefined,
      minInterval: integerX ? 1 : undefined,
      min: attrs.xAxisMin,
      max: attrs.xAxisMax,
      scale: attrs.scaleAxes,
    },
    yAxis: {
      type: logScale ? 'log' : 'value',
      name: yAxisLabel,
      formatter:
        formatYValue !== undefined
          ? (v) => formatYValue(v as number)
          : undefined,
      minInterval: integerY ? 1 : undefined,
      scale: attrs.scaleAxes,
    },
    tooltip: {
      trigger: 'axis' as const,
      formatter: (
        params: Array<{
          seriesName?: string;
          data?: [number, number];
          marker?: string;
        }>,
      ) => {
        if (!Array.isArray(params) || params.length === 0) return '';
        const xVal = params[0].data?.[0];
        const header = xVal !== undefined ? `X: ${fmtX(xVal)}` : '';
        const lines = params.map(
          (p) =>
            `${p.marker ?? ''} ${p.seriesName ?? ''}: ${fmtY(p.data?.[1] ?? 0)}`,
        );
        return [header, ...lines].join('<br>');
      },
    },
    brush: attrs.onBrush ? {xAxisIndex: 0, brushType: 'lineX'} : undefined,
    legend: displayLegend ? buildLegendOption() : {show: false},
  });

  (option as Record<string, unknown>).series = series;
  return option;
}

function buildLineEventHandlers(
  attrs: LineChartAttrs,
  data: LineChartData | undefined,
): ReadonlyArray<EChartEventHandler> {
  if (
    !attrs.onBrush ||
    data === undefined ||
    data.series.length === 0 ||
    data.series.every((s) => s.points.length === 0)
  ) {
    return [];
  }
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
