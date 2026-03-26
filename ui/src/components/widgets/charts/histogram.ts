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
import {
  HistogramBucket,
  HistogramData,
  HistogramConfig,
  computeHistogram,
} from './histogram_loader';
import {EChartView, EChartEventHandler} from './echart_view';
import {buildChartOption, SELECTION_COLOR} from './chart_option_builder';

// Re-export data types for convenience
export {HistogramBucket, HistogramData, HistogramConfig, computeHistogram};

// Color for the selection highlight background.
const SELECTION_BG_COLOR = 'rgba(0, 120, 212, 0.08)';

export interface HistogramAttrs {
  /**
   * Histogram data to display, or undefined if loading.
   * When undefined, a loading spinner is shown.
   * Use the computeHistogram() utility function to compute this from raw values.
   */
  readonly data: HistogramData | undefined;

  /**
   * Height of the histogram in pixels. Defaults to 200.
   */
  readonly height?: number;

  /**
   * Label for the X axis.
   */
  readonly xAxisLabel?: string;

  /**
   * Label for the Y axis. Defaults to 'Count'.
   */
  readonly yAxisLabel?: string;

  /**
   * Callback when brush selection completes (on mouseup).
   * Called with the selected range based on mousedown and mouseup positions.
   */
  readonly onBrush?: (range: {start: number; end: number}) => void;

  /**
   * Selection range to highlight on the chart. Buckets overlapping this
   * range are drawn with a highlight color. The consumer controls this
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
   * Bar color. Defaults to theme primary color.
   */
  readonly barColor?: string;

  /**
   * Bar hover color. Defaults to theme accent color.
   */
  readonly barHoverColor?: string;

  /**
   * Use logarithmic scale for Y axis. Useful when count values
   * span multiple orders of magnitude. Defaults to false.
   */
  readonly logScale?: boolean;

  /**
   * When true, X axis (dimension) ticks will be snapped to integer values.
   * Use when the histogram data represents integer-valued quantities.
   * The Y axis (measure) always uses integer ticks since it shows counts.
   */
  readonly integerDimension?: boolean;
}

export class Histogram implements m.ClassComponent<HistogramAttrs> {
  view({attrs}: m.Vnode<HistogramAttrs>) {
    const {data, height, fillParent, className, onBrush} = attrs;

    const nullCount = data?.nullCount ?? 0;
    const hasData =
      data !== undefined && (data.buckets.length > 0 || nullCount > 0);
    const option = hasData ? buildOption(attrs, data) : undefined;

    return m(EChartView, {
      option,
      height,
      fillParent,
      className,
      empty: data !== undefined && !hasData,
      eventHandlers: buildEventHandlers(attrs, data),
      activeBrushType: onBrush !== undefined ? 'lineX' : undefined,
    });
  }
}

function buildOption(
  attrs: HistogramAttrs,
  data: HistogramData,
): EChartsCoreOption {
  const {
    xAxisLabel,
    yAxisLabel = 'Count',
    formatXValue = (v: number) => formatNumber(v),
    formatYValue,
    barColor,
    barHoverColor,
    logScale = false,
  } = attrs;
  const fmtY = formatYValue ?? formatNumber;
  const nullCount = data.nullCount ?? 0;
  const totalWithNull = data.totalCount + nullCount;
  const buckets = data.buckets;
  const sel = attrs.selection;

  // Build dataset: each row is [start, end, count, isSelected].
  const source = buckets.map((b) => {
    const eps = (b.end - b.start) * 0.01;
    const inSel =
      sel !== undefined &&
      b.end > sel.start + eps &&
      b.start < sel.end - eps;
    return [b.start, b.end, b.count, inSel ? 1 : 0];
  });

  // Compute x-axis range from bucket extents.
  const xMin = buckets.length > 0 ? buckets[0].start : 0;
  const xMax = buckets.length > 0 ? buckets[buckets.length - 1].end : 1;

  const option = buildChartOption({
    grid: {bottom: xAxisLabel ? 40 : 25},
    xAxis: {
      type: 'value',
      name: xAxisLabel,
      min: xMin,
      max: xMax,
      formatter: (v) => formatXValue(v as number),
    },
    yAxis: {
      type: logScale ? 'log' : 'value',
      name: yAxisLabel,
      formatter:
        formatYValue !== undefined
          ? (v) => formatYValue(v as number)
          : undefined,
      ...(logScale ? {} : {minInterval: 1}),
    },
    tooltip: {
      trigger: 'item' as const,
      formatter: (params: {dataIndex?: number}) => {
        const idx = params?.dataIndex;
        if (idx === undefined || idx < 0 || idx >= buckets.length) {
          return '';
        }
        const bucket = buckets[idx];
        const pct =
          totalWithNull > 0
            ? ((bucket.count / totalWithNull) * 100).toFixed(1)
            : '0';
        return [
          `Range: ${formatXValue(bucket.start)} \u2013 ${formatXValue(bucket.end)}`,
          `Count: ${fmtY(bucket.count)}`,
          `${pct}%`,
        ].join('<br>');
      },
    },
    brush: attrs.onBrush ? {xAxisIndex: 0, brushType: 'lineX'} : undefined,
  });

  const optionAny = option as Record<string, unknown>;

  // Custom series: renderItem draws each bar as a rect from bucket.start
  // to bucket.end on a value axis, giving precise pixel alignment.
  // We pass bucket data directly rather than using dataset/encode to avoid
  // ECharts' dimension mapping complexities.
  const renderItem = (
    params: {dataIndex: number; coordSys: {x: number; y: number; width: number; height: number}},
    api: {
      coord: (point: [number, number]) => [number, number];
      style: (extra?: Record<string, unknown>) => Record<string, unknown>;
    },
  ) => {
    const idx = params.dataIndex;
    const bucket = source[idx];
    const start = bucket[0];
    const end = bucket[1];
    const count = bucket[2];
    const selected = bucket[3];

    const topLeft = api.coord([start, count]);
    const bottomRight = api.coord([end, logScale ? 1 : 0]);
    const x = topLeft[0];
    const y = topLeft[1];
    const width = bottomRight[0] - topLeft[0];
    const height = bottomRight[1] - topLeft[1];

    const children: Array<Record<string, unknown>> = [];

    // Selection background — full height of the chart area.
    if (selected) {
      const coordSys = params.coordSys;
      children.push({
        type: 'rect',
        shape: {x, y: coordSys.y, width, height: coordSys.height},
        style: {fill: SELECTION_BG_COLOR},
        silent: true,
        z2: 0,
      });
    }

    // The bar itself. api.style() returns the theme-applied default style
    // (including fill color from the series color palette).
    const defaultStyle = api.style();
    const fill = selected
      ? SELECTION_COLOR
      : barColor ?? defaultStyle.fill;
    children.push({
      type: 'rect',
      shape: {x, y, width, height},
      style: {...defaultStyle, fill},
      emphasis: barHoverColor !== undefined
        ? {style: {fill: barHoverColor}}
        : undefined,
      z2: 1,
    });

    return {type: 'group', children};
  };

  optionAny.series = [
    {
      type: 'custom',
      renderItem,
      encode: {x: [0, 1], y: 2},
      data: source,
      animation: false,
    },
  ];

  return option;
}

function buildEventHandlers(
  attrs: HistogramAttrs,
  data: HistogramData | undefined,
): ReadonlyArray<EChartEventHandler> {
  if (!attrs.onBrush || data === undefined || data.buckets.length === 0) {
    return [];
  }
  const onBrush = attrs.onBrush;

  return [
    {
      eventName: 'brushEnd',
      handler: (params) => {
        // On a value axis, coordRange returns actual values.
        const range = extractBrushRange(params);
        if (range !== undefined) {
          onBrush({start: range[0], end: range[1]});
        }
      },
    },
  ];
}
