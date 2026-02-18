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
import {AggregateFunction} from '../datagrid/model';
import {extractBrushRange, formatNumber} from './chart_utils';
import {EChartView, EChartEventHandler} from './echart_view';
import {
  buildAxisOption,
  buildGridOption,
  buildBrushOption,
  buildTooltipOption,
} from './chart_option_builder';
import {getChartThemeColors} from './chart_theme';

/**
 * A single bar in the bar chart.
 */
export interface BarChartItem {
  /** Label for this bar (displayed on the dimension axis). */
  readonly label: string | number;
  /** Numeric value for this bar. */
  readonly value: number;
}

/**
 * Data provided to a BarChart.
 */
export interface BarChartData {
  /** The bars to display. */
  readonly items: readonly BarChartItem[];
}

export interface BarChartAttrs {
  /**
   * Bar chart data to display, or undefined if loading.
   * When undefined, a loading spinner is shown.
   */
  readonly data: BarChartData | undefined;

  /**
   * Height of the chart in pixels. Defaults to 200.
   */
  readonly height?: number;

  /**
   * Label for the dimension axis (the categorical/label axis).
   * Placed on the X axis in vertical mode, Y axis in horizontal mode.
   */
  readonly dimensionLabel?: string;

  /**
   * Label for the measure axis (the numeric value axis).
   * Placed on the Y axis in vertical mode, X axis in horizontal mode.
   */
  readonly measureLabel?: string;

  /**
   * Fill parent container. Defaults to false.
   */
  readonly fillParent?: boolean;

  /**
   * Custom class name for the container.
   */
  readonly className?: string;

  /**
   * Format function for measure axis tick values.
   */
  readonly formatMeasure?: (value: number) => string;

  /**
   * Bar color. Defaults to theme primary color.
   */
  readonly barColor?: string;

  /**
   * Bar hover color. Defaults to theme accent color.
   */
  readonly barHoverColor?: string;

  /**
   * Use logarithmic scale for the measure axis. Defaults to false.
   */
  readonly logScale?: boolean;

  /**
   * When true, measure axis ticks will be snapped to integer values.
   */
  readonly integerMeasure?: boolean;

  /**
   * Chart orientation. Defaults to 'vertical'.
   * - 'vertical': bars grow upward, dimension on X axis, measure on Y axis.
   * - 'horizontal': bars grow rightward, dimension on Y axis, measure on X.
   */
  readonly orientation?: 'vertical' | 'horizontal';

  /**
   * Callback when brush selection completes (on mouseup).
   * Called with the labels of all bars in the brushed range.
   */
  readonly onBrush?: (labels: Array<string | number>) => void;
}

export class BarChart implements m.ClassComponent<BarChartAttrs> {
  view({attrs}: m.Vnode<BarChartAttrs>) {
    const {data, height, fillParent, className, onBrush, orientation} = attrs;
    const horizontal = orientation === 'horizontal';

    const isEmpty = data !== undefined && data.items.length === 0;
    const option =
      data !== undefined && !isEmpty ? buildBarOption(attrs, data) : undefined;

    return m(EChartView, {
      option,
      height,
      fillParent,
      className,
      empty: isEmpty,
      eventHandlers: buildBarEventHandlers(attrs, data),
      activeBrushType:
        onBrush !== undefined ? (horizontal ? 'lineY' : 'lineX') : undefined,
    });
  }
}

function buildBarOption(
  attrs: BarChartAttrs,
  data: BarChartData,
): EChartsCoreOption {
  const {
    dimensionLabel,
    measureLabel = 'Value',
    formatMeasure,
    barColor,
    barHoverColor,
    logScale = false,
    integerMeasure = false,
    orientation = 'vertical',
  } = attrs;
  const fmtMeasure = formatMeasure ?? formatNumber;

  const theme = getChartThemeColors();
  const horizontal = orientation === 'horizontal';
  const labels = data.items.map((item) => String(item.label));

  const categoryAxis = buildAxisOption(
    {
      type: 'category',
      data: labels,
      name: dimensionLabel,
      nameGap: horizontal ? 55 : 35,
      labelOverflow: 'truncate',
      labelWidth: horizontal ? 65 : undefined,
    },
    !horizontal,
  );

  const valueAxis = buildAxisOption(
    {
      type: logScale ? 'log' : 'value',
      name: measureLabel,
      nameGap: horizontal ? 25 : undefined,
      formatter:
        formatMeasure !== undefined
          ? (v) => formatMeasure(v as number)
          : undefined,
      minInterval: integerMeasure ? 1 : undefined,
    },
    horizontal,
  );

  const option: Record<string, unknown> = {
    animation: false,
    color: [...theme.chartColors],
    grid: buildGridOption({
      bottom: dimensionLabel && !horizontal ? 45 : 25,
    }),
    tooltip: buildTooltipOption({
      trigger: 'axis' as const,
      axisPointer: {type: 'shadow' as const},
      formatter: (params: Array<{name?: string; value?: number}>) => {
        const p = Array.isArray(params) ? params[0] : params;
        return `${p.name ?? ''}<br>${measureLabel}: ${fmtMeasure(p.value ?? 0)}`;
      },
    }),
    xAxis: horizontal ? valueAxis : categoryAxis,
    yAxis: horizontal ? categoryAxis : valueAxis,
    series: [
      {
        type: 'bar',
        data: data.items.map((item) => item.value),
        itemStyle: barColor !== undefined ? {color: barColor} : undefined,
        emphasis: {
          itemStyle: {
            color: barHoverColor ?? theme.accentColor,
          },
        },
      },
    ],
  };

  if (attrs.onBrush) {
    option.brush = buildBrushOption({
      xAxisIndex: horizontal ? undefined : 0,
      yAxisIndex: horizontal ? 0 : undefined,
      brushType: horizontal ? 'lineY' : 'lineX',
    });
    // Hide the default brush toolbox; we activate brush programmatically.
    option.toolbox = {show: false};
  }

  return option as EChartsCoreOption;
}

function buildBarEventHandlers(
  attrs: BarChartAttrs,
  data: BarChartData | undefined,
): ReadonlyArray<EChartEventHandler> {
  if (!attrs.onBrush || data === undefined || data.items.length === 0) {
    return [];
  }
  const onBrush = attrs.onBrush;
  const items = data.items;

  return [
    {
      eventName: 'brushEnd',
      handler: (params) => {
        // For category axes, coordRange returns category indices
        const range = extractBrushRange(params);
        if (range !== undefined) {
          const [startIdx, endIdx] = range;
          const minIdx = Math.max(0, startIdx);
          const maxIdx = Math.min(items.length - 1, endIdx);
          if (minIdx <= maxIdx) {
            const labels: Array<string | number> = [];
            for (let i = minIdx; i <= maxIdx; i++) {
              labels.push(items[i].label);
            }
            if (labels.length > 0) {
              onBrush(labels);
            }
          }
        }
      },
    },
  ];
}

/**
 * Aggregate raw data into BarChartData by grouping on a dimension and
 * applying an aggregation function to the measure values.
 *
 * Results are sorted by aggregated value (descending).
 *
 * @param items The raw data items to aggregate.
 * @param dimension Extracts the grouping key (bar label) from each item.
 * @param measure Extracts the numeric value from each item.
 * @param aggregation The aggregation function to apply per group.
 */
export function aggregateBarChartData<T>(
  items: readonly T[],
  dimension: (item: T) => string | number,
  measure: (item: T) => number,
  aggregation: AggregateFunction,
): BarChartData {
  const groups = new Map<string | number, number[]>();
  for (const item of items) {
    const key = dimension(item);
    let values = groups.get(key);
    if (values === undefined) {
      values = [];
      groups.set(key, values);
    }
    values.push(measure(item));
  }

  const result: BarChartItem[] = [];
  for (const [label, values] of groups) {
    result.push({label, value: aggregate(values, aggregation)});
  }

  result.sort((a, b) => b.value - a.value);
  return {items: result};
}

function aggregate(values: number[], agg: AggregateFunction): number {
  switch (agg) {
    case 'ANY':
    case 'MIN':
      return values.reduce((a, b) => Math.min(a, b), Infinity);
    case 'SUM':
      return values.reduce((a, b) => a + b, 0);
    case 'AVG':
      return values.reduce((a, b) => a + b, 0) / values.length;
    case 'MAX':
      return values.reduce((a, b) => Math.max(a, b), -Infinity);
    case 'COUNT_DISTINCT':
      return new Set(values).size;
  }
}
