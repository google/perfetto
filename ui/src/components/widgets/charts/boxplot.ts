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
import {formatNumber} from './chart_utils';
import {EChartView} from './echart_view';
import {
  buildAxisOption,
  buildGridOption,
  buildTooltipOption,
} from './chart_option_builder';
import {getChartThemeColors} from './chart_theme';

/**
 * A single box in a boxplot chart.
 */
export interface BoxplotItem {
  /** Label for this box (category name) */
  readonly label: string;
  /** Minimum value (lower whisker) */
  readonly min: number;
  /** First quartile (Q1 / 25th percentile) */
  readonly q1: number;
  /** Median (Q2 / 50th percentile) */
  readonly median: number;
  /** Third quartile (Q3 / 75th percentile) */
  readonly q3: number;
  /** Maximum value (upper whisker) */
  readonly max: number;
}

/**
 * Data provided to a BoxplotChart.
 */
export interface BoxplotData {
  readonly items: readonly BoxplotItem[];
}

export interface BoxplotAttrs {
  /**
   * Boxplot data to display, or undefined if loading.
   * When undefined, a loading spinner is shown.
   */
  readonly data: BoxplotData | undefined;

  /**
   * Height of the chart in pixels. Defaults to 200.
   */
  readonly height?: number;

  /**
   * Label for the category axis.
   */
  readonly categoryLabel?: string;

  /**
   * Label for the value axis.
   */
  readonly valueLabel?: string;

  /**
   * Orientation: 'vertical' (categories on X) or 'horizontal' (categories on Y).
   * Defaults to 'vertical'.
   */
  readonly orientation?: 'vertical' | 'horizontal';

  /**
   * Fill parent container. Defaults to false.
   */
  readonly fillParent?: boolean;

  /**
   * Custom class name for the container.
   */
  readonly className?: string;

  /**
   * Format function for value axis tick values.
   */
  readonly formatValue?: (value: number) => string;
}

export class BoxplotChart implements m.ClassComponent<BoxplotAttrs> {
  view({attrs}: m.CVnode<BoxplotAttrs>) {
    const {data, height, fillParent, className} = attrs;

    const isEmpty = data !== undefined && data.items.length === 0;
    const option =
      data !== undefined && !isEmpty
        ? buildBoxplotOption(attrs, data)
        : undefined;

    return m(EChartView, {
      option,
      height,
      fillParent,
      className,
      empty: isEmpty,
    });
  }
}

function buildBoxplotOption(
  attrs: BoxplotAttrs,
  data: BoxplotData,
): EChartsCoreOption {
  const {
    categoryLabel,
    valueLabel,
    orientation = 'vertical',
    formatValue,
  } = attrs;
  const fmtVal = formatValue ?? formatNumber;
  const horizontal = orientation === 'horizontal';
  const theme = getChartThemeColors();

  const categories = data.items.map((item) => item.label);
  // ECharts boxplot series data format: [min, Q1, median, Q3, max].
  // In tooltip params.value, ECharts prepends the x-axis index, so the
  // actual values are at indices 1-5.
  const boxData = data.items.map((item) => [
    item.min,
    item.q1,
    item.median,
    item.q3,
    item.max,
  ]);

  const categoryAxis = buildAxisOption(
    {
      type: 'category',
      data: categories,
      name: categoryLabel,
      labelOverflow: 'truncate',
      labelWidth: 80,
    },
    !horizontal,
  );

  const valueAxis = buildAxisOption(
    {
      type: 'value',
      name: valueLabel,
      formatter:
        formatValue !== undefined
          ? (v: number | string) => fmtVal(v as number)
          : undefined,
      scale: true,
    },
    horizontal,
  );

  const option: Record<string, unknown> = {
    animation: false,
    color: [...theme.chartColors],
    grid: buildGridOption({
      bottom: horizontal ? 25 : categoryLabel ? 40 : 25,
    }),
    xAxis: horizontal ? valueAxis : categoryAxis,
    yAxis: horizontal ? categoryAxis : valueAxis,
    tooltip: buildTooltipOption({
      trigger: 'item' as const,
      formatter: (params: {name?: string; value?: number[]}) => {
        const v = params.value;
        if (v === undefined) return '';
        return [
          `<b>${params.name ?? ''}</b>`,
          `Max: ${fmtVal(v[5] ?? v[4])}`,
          `Q3: ${fmtVal(v[4] ?? v[3])}`,
          `Median: ${fmtVal(v[3] ?? v[2])}`,
          `Q1: ${fmtVal(v[2] ?? v[1])}`,
          `Min: ${fmtVal(v[1] ?? v[0])}`,
        ].join('<br>');
      },
    }),
    series: [
      {
        type: 'boxplot',
        data: boxData,
      },
    ],
  };

  return option as EChartsCoreOption;
}
