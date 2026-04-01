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
import {
  buildChartOption,
  buildLegendOption,
  buildSelectionMarkArea,
} from './chart_option_builder';

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
 * A vertical marker line drawn at a specific x value.
 */
export interface LineChartMarker {
  /** X-axis value where the marker should be drawn */
  readonly x: number;
  /** Label shown next to the marker line */
  readonly label: string;
  /** Color for the marker line and label. Defaults to red. */
  readonly color?: string;
}

/**
 * Data provided to a LineChart.
 */
export interface LineChartData {
  /** The series to display */
  readonly series: readonly LineChartSeries[];
  /** Optional vertical marker lines (e.g. for events) */
  readonly markers?: readonly LineChartMarker[];
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
   * Selection range to highlight on the chart. When provided, a shaded
   * region is drawn over the specified X range. The consumer controls this
   * state — typically by feeding the `onBrush` output back in.
   */
  readonly selection?: {readonly start: number; readonly end: number};

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

  /**
   * Show grid lines. 'horizontal' draws lines parallel to the X axis,
   * 'vertical' draws lines parallel to the Y axis, 'both' shows both.
   * Defaults to no grid lines.
   */
  readonly gridLines?: 'horizontal' | 'vertical' | 'both';

  /**
   * When true, series are stacked and shown as filled areas.
   * The total height is the sum of all series values. Defaults to false.
   * Note: When stacked, all series must be aligned to the same X values.
   */
  readonly stacked?: boolean;

  /**
   * Position of the legend. Defaults to 'bottom' when multiple series,
   * 'top' otherwise.
   */
  readonly legendPosition?: 'top' | 'right' | 'bottom';

  /**
   * Callback when a series is clicked. Called with the series name.
   */
  readonly onSeriesClick?: (seriesName: string) => void;
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
    gridLines,
    stacked = false,
    legendPosition,
  } = attrs;
  const fmtX = formatXValue ?? formatNumber;
  const fmtY = formatYValue ?? formatNumber;

  const displayLegend = showLegend ?? data.series.length > 1;
  const legendPos =
    legendPosition ?? (data.series.length > 1 ? 'bottom' : 'top');

  // When stacking, reverse the series order so that the first series is drawn
  // on top. This aligns the series order with the legend and tooltip.
  const series = data.series.map((s, i) => {
    const base: Record<string, unknown> = {
      type: 'line',
      name: s.name,
      data: s.points.map((p) => [p.x, p.y]),
      lineStyle:
        s.color !== undefined
          ? {width: lineWidth, color: s.color}
          : {width: lineWidth},
      itemStyle: s.color !== undefined ? {color: s.color} : undefined,
      showSymbol: showPoints,
      symbolSize: 6,
      triggerLineEvent: true,
      legendHoverLink: false,
      emphasis: stacked
        ? {focus: 'series', blurScope: 'global' as const}
        : {focus: 'series', itemStyle: {borderWidth: 2}},
      blur: stacked
        ? {lineStyle: {opacity: 0.15}, areaStyle: {opacity: 0.05}}
        : {lineStyle: {opacity: 0.15}},
      stack: stacked ? 'total' : undefined,
      areaStyle: stacked ? {opacity: 0.8} : undefined,
      // invisible wider hitbox
      silent: false,
    };

    // Render selection highlight on the first series only.
    if (i === 0 && attrs.selection !== undefined) {
      base.markArea = buildSelectionMarkArea([
        [{xAxis: attrs.selection.start}, {xAxis: attrs.selection.end}],
      ]);
    }

    // Render vertical marker lines on the first series only.
    if (i === 0 && data.markers !== undefined && data.markers.length > 0) {
      base.markLine = {
        silent: false,
        symbol: 'none',
        animation: false,
        data: data.markers.map((mk) => ({
          xAxis: mk.x,
          name: mk.label,
          lineStyle: {
            color: mk.color ?? '#e53935',
            type: 'dashed' as const,
            width: 2,
          },
          label: {
            formatter: mk.label,
            position: 'insideEndTop' as const,
            fontSize: 10,
            color: mk.color ?? '#e53935',
          },
        })),
      };
    }
    return base;
  });

  const option = buildChartOption({
    grid: {
      top: legendPos === 'top' && displayLegend ? 30 : 10,
      right: legendPos === 'right' && displayLegend ? 150 : undefined,
      bottom:
        legendPos === 'bottom' && displayLegend
          ? xAxisLabel
            ? 70
            : 55
          : xAxisLabel
            ? 40
            : 25,
    },
    xAxis: {
      // Nasty ECharts quirk: when stacking, the xAxis must be type 'category'
      // or 'time'. Since we want to support x-values at irregular intervals, we
      // use 'time' type which allows numeric timestamps, and override the label
      // formatter to show numbers.
      type: stacked ? 'time' : 'value',
      name: xAxisLabel,
      formatter: (v) => fmtX(v as number),
      minInterval: integerX ? 1 : undefined,
      min: attrs.xAxisMin,
      max: attrs.xAxisMax,
      scale: attrs.scaleAxes,
      showSplitLine: gridLines === 'vertical' || gridLines === 'both',
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
      showSplitLine: gridLines === 'horizontal' || gridLines === 'both',
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
        const header = xVal !== undefined ? fmtX(xVal) : '';
        const ordered = stacked ? [...params].reverse() : params;
        const lines = ordered.map(
          (p) =>
            `${p.marker ?? ''} ${p.seriesName ?? ''}: ${fmtY(p.data?.[1] ?? 0)}`,
        );
        return [header, ...lines].join('<br>');
      },
    },
    brush: attrs.onBrush ? {xAxisIndex: 0, brushType: 'lineX'} : undefined,
    legend: displayLegend
      ? {
          ...buildLegendOption(legendPos),
          ...(stacked
            ? {data: [...data.series].reverse().map((s) => s.name)}
            : {}),
          formatter: (name: string) => {
            const s = data.series.find((sr) => sr.name === name);
            if (s !== undefined && s.points.length > 0) {
              return `${name}  ${fmtY(s.points[s.points.length - 1].y)}`;
            }
            return name;
          },
        }
      : {show: false},
  });

  (option as Record<string, unknown>).series = series;
  return option;
}

function buildLineEventHandlers(
  attrs: LineChartAttrs,
  data: LineChartData | undefined,
): ReadonlyArray<EChartEventHandler> {
  const handlers: EChartEventHandler[] = [];

  if (
    attrs.onBrush &&
    data !== undefined &&
    data.series.length > 0 &&
    data.series.some((s) => s.points.length > 0)
  ) {
    const onBrush = attrs.onBrush;
    handlers.push({
      eventName: 'brushEnd',
      handler: (params) => {
        const range = extractBrushRange(params);
        if (range !== undefined) {
          const [start, end] = range;
          onBrush({start, end});
        }
      },
    });
  }

  if (attrs.onSeriesClick) {
    const onSeriesClick = attrs.onSeriesClick;
    handlers.push({
      eventName: 'click',
      handler: (params) => {
        const p = params as {seriesName?: string};
        if (p.seriesName !== undefined) {
          onSeriesClick(p.seriesName);
        }
      },
    });
  }

  return handlers;
}
