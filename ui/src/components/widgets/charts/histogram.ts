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

  const categories = data.buckets.map((b) => formatXValue(b.start));
  const seriesData: number[] = data.buckets.map((b) => b.count);
  const bucketCount = data.buckets.length;

  // Append NULL bar when there are null values
  if (nullCount > 0) {
    categories.push('NULL');
    seriesData.push(nullCount);
  }

  const option = buildChartOption({
    grid: {bottom: xAxisLabel ? 40 : 25},
    xAxis: {
      type: 'category',
      data: categories,
      name: xAxisLabel,
    },
    yAxis: {
      type: logScale ? 'log' : 'value',
      min: 'dataMin',
      max: 'dataMax',
      name: yAxisLabel,
      formatter:
        formatYValue !== undefined
          ? (v) => formatYValue(v as number)
          : undefined,
      // minInterval: 1 ensures integer ticks for count values, but on a log
      // axis ECharts interprets it as the min interval in log-space, capping
      // the axis at the nearest power of 10 instead of the actual max.
      ...(logScale ? {} : {minInterval: 1}),
    },
    tooltip: {
      trigger: 'axis' as const,
      axisPointer: {type: 'shadow' as const},
      formatter: (params: Array<{dataIndex?: number}>) => {
        const p = Array.isArray(params) ? params[0] : params;
        const idx = p?.dataIndex;
        if (idx === undefined || idx < 0 || idx >= categories.length) {
          return '';
        }

        // NULL bar
        if (idx >= bucketCount) {
          const pct =
            totalWithNull > 0
              ? ((nullCount / totalWithNull) * 100).toFixed(1)
              : '0';
          return [`NULL`, `Count: ${fmtY(nullCount)}`, `${pct}%`].join('<br>');
        }

        // Regular bucket
        const bucket = data.buckets[idx];
        const pct =
          totalWithNull > 0
            ? ((bucket.count / totalWithNull) * 100).toFixed(1)
            : '0';
        return [
          `Range: ${formatXValue(bucket.start)} - ${formatXValue(bucket.end)}`,
          `Count: ${fmtY(bucket.count)}`,
          `${pct}%`,
        ].join('<br>');
      },
    },
    brush: attrs.onBrush ? {xAxisIndex: 0, brushType: 'lineX'} : undefined,
  });

  // Build per-bar styles, highlighting buckets that overlap the selection.
  // We use a separate background bar series to shade selected buckets —
  // markArea doesn't align reliably on category axes.
  const sel = attrs.selection;
  const bgData: Array<Record<string, unknown>> = [];
  let hasSelection = false;

  const styledData = seriesData.map((count, idx) => {
    const item: Record<string, unknown> = {value: count};
    const bucket = data.buckets[idx];

    if (bucket === undefined) {
      bgData.push({value: 0});
      return item;
    }

    // Use a small epsilon to avoid floating point inaccuracies causing
    // adjacent buckets to be included in the selection highlight.
    const eps = (bucket.end - bucket.start) * 0.01;
    const inSelection =
      sel !== undefined &&
      bucket.end > sel.start + eps &&
      bucket.start < sel.end - eps;

    if (inSelection) {
      item.itemStyle = {color: SELECTION_COLOR};
      hasSelection = true;
    }
    bgData.push({value: inSelection ? 1 : 0});
    return item;
  });

  const series: Array<Record<string, unknown>> = [];

  // Background highlight series — uses yAxisIndex 1 (fixed 0-1 range) so
  // bars with value=1 fill the full chart height behind selected buckets.
  if (hasSelection) {
    series.push({
      type: 'bar',
      data: bgData,
      barWidth: '100%',
      barCategoryGap: '0%',
      yAxisIndex: 1,
      silent: true,
      itemStyle: {
        color: 'rgba(0, 120, 212, 0.08)',
        borderColor: 'rgba(0, 120, 212, 0.3)',
        borderWidth: 1,
      },
      animation: false,
      z: 0,
    });
  }

  // Main data series. barGap: '-100%' ensures it overlaps the background
  // series rather than sitting side-by-side.
  series.push({
    type: 'bar',
    data: styledData,
    barWidth: '100%',
    barCategoryGap: '0%',
    barGap: '-100%',
    itemStyle: barColor !== undefined ? {color: barColor} : undefined,
    emphasis:
      barHoverColor !== undefined
        ? {itemStyle: {color: barHoverColor}}
        : undefined,
    z: 1,
  });

  // Add a hidden secondary y-axis for the background series (fixed 0-1 range).
  const optionAny = option as Record<string, unknown>;
  if (hasSelection) {
    const primaryYAxis = optionAny.yAxis;
    optionAny.yAxis = [
      primaryYAxis,
      {type: 'value', min: 0, max: 1, show: false},
    ];
  }
  optionAny.series = series;

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
  const buckets = data.buckets;

  return [
    {
      eventName: 'brushEnd',
      handler: (params) => {
        // For category axes, coordRange returns category indices
        const range = extractBrushRange(params);
        if (range !== undefined) {
          const [startIdx, endIdx] = range;
          const minIdx = Math.max(0, startIdx);
          const maxIdx = Math.min(buckets.length - 1, endIdx);
          if (minIdx <= maxIdx && minIdx < buckets.length) {
            onBrush({
              start: buckets[minIdx].start,
              end: buckets[maxIdx].end,
            });
          }
        }
      },
    },
  ];
}
