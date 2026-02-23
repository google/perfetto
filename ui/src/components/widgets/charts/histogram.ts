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
import {buildChartOption} from './chart_option_builder';
import {getChartThemeColors} from './chart_theme';

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

    const isEmpty = data !== undefined && data.buckets.length === 0;
    const option =
      data !== undefined && !isEmpty ? buildOption(attrs, data) : undefined;

    return m(EChartView, {
      option,
      height,
      fillParent,
      className,
      empty: isEmpty,
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

  const theme = getChartThemeColors();
  const categories = data.buckets.map((b) => formatXValue(b.start));

  const option = buildChartOption({
    grid: {bottom: xAxisLabel ? 40 : 25},
    xAxis: {
      type: 'category',
      data: categories,
      name: xAxisLabel,
    },
    yAxis: {
      type: logScale ? 'log' : 'value',
      name: yAxisLabel,
      formatter:
        formatYValue !== undefined
          ? (v) => formatYValue(v as number)
          : undefined,
      minInterval: 1,
    },
    tooltip: {
      trigger: 'axis' as const,
      axisPointer: {type: 'shadow' as const},
      formatter: (params: Array<{dataIndex?: number}>) => {
        const p = Array.isArray(params) ? params[0] : params;
        const idx = p?.dataIndex;
        if (idx === undefined || idx < 0 || idx >= data.buckets.length) {
          return '';
        }
        const bucket = data.buckets[idx];
        const pct =
          data.totalCount > 0
            ? ((bucket.count / data.totalCount) * 100).toFixed(1)
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

  // Add series on top of the base option
  (option as Record<string, unknown>).series = [
    {
      type: 'bar',
      data: data.buckets.map((b) => b.count),
      barWidth: '100%',
      barCategoryGap: '0%',
      itemStyle: barColor !== undefined ? {color: barColor} : undefined,
      emphasis: {
        itemStyle: {color: barHoverColor ?? theme.accentColor},
      },
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
