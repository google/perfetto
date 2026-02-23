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
import {EChartView, EChartEventHandler, EChartClickParams} from './echart_view';
import {buildLegendOption, buildTooltipOption} from './chart_option_builder';
import {getChartThemeColors} from './chart_theme';

/**
 * A single slice in the pie chart.
 */
export interface PieChartSlice {
  /** Label for this slice */
  readonly label: string;
  /** Numeric value for this slice */
  readonly value: number;
  /** Optional custom color for this slice */
  readonly color?: string;
}

/**
 * Data provided to a PieChart.
 */
export interface PieChartData {
  /** The slices to display */
  readonly slices: readonly PieChartSlice[];
}

export interface PieChartAttrs {
  /**
   * Pie chart data to display, or undefined if loading.
   * When undefined, a loading spinner is shown.
   */
  readonly data: PieChartData | undefined;

  /**
   * Height of the chart in pixels. Defaults to 200.
   */
  readonly height?: number;

  /**
   * Fill parent container. Defaults to false.
   */
  readonly fillParent?: boolean;

  /**
   * Custom class name for the container.
   */
  readonly className?: string;

  /**
   * Format function for values in tooltips.
   */
  readonly formatValue?: (value: number) => string;

  /**
   * Show legend. Defaults to true.
   */
  readonly showLegend?: boolean;

  /**
   * Show percentage labels on slices. Defaults to false.
   */
  readonly showLabels?: boolean;

  /**
   * Inner radius ratio for donut chart (0-1). 0 = pie, >0 = donut.
   * Defaults to 0 (pie chart).
   */
  readonly innerRadiusRatio?: number;

  /**
   * Callback when a slice is clicked.
   */
  readonly onSliceClick?: (slice: PieChartSlice) => void;
}

export class PieChart implements m.ClassComponent<PieChartAttrs> {
  view({attrs}: m.Vnode<PieChartAttrs>) {
    const {data, height, fillParent, className} = attrs;

    const validSlices = data?.slices.filter((s) => s.value > 0) ?? [];
    const isEmpty = data !== undefined && validSlices.length === 0;
    const option =
      validSlices.length > 0 ? buildPieOption(attrs, validSlices) : undefined;

    return m(EChartView, {
      option,
      height,
      fillParent,
      className,
      empty: isEmpty,
      eventHandlers: buildPieEventHandlers(attrs, validSlices),
    });
  }
}

function buildPieOption(
  attrs: PieChartAttrs,
  slices: readonly PieChartSlice[],
): EChartsCoreOption {
  const {
    formatValue = (v: number) => formatNumber(v),
    showLegend = true,
    showLabels = false,
    innerRadiusRatio = 0,
  } = attrs;

  const theme = getChartThemeColors();

  const pieData = slices.map((s) => ({
    name: s.label,
    value: s.value,
    itemStyle: s.color !== undefined ? {color: s.color} : undefined,
  }));

  const outerPct = showLegend ? 65 : 75;
  const outerRadius = `${outerPct}%`;
  const innerRadius = `${Math.round(outerPct * innerRadiusRatio)}%`;

  return {
    animation: false,
    color: [...theme.chartColors],
    tooltip: buildTooltipOption({
      trigger: 'item' as const,
      formatter: (params: {
        name?: string;
        value?: number;
        percent?: number;
      }) => {
        const name = params.name ?? '';
        const value = params.value ?? 0;
        const pct = params.percent?.toFixed(1) ?? '0';
        return [name, `Value: ${formatValue(value)}`, `${pct}%`].join('<br>');
      },
    }),
    legend: showLegend ? buildLegendOption('right') : {show: false},
    series: [
      {
        type: 'pie',
        radius: [innerRadius, outerRadius],
        center: showLegend ? ['35%', '50%'] : ['50%', '50%'],
        data: pieData,
        label: {
          show: showLabels,
          formatter: '{d}%',
          fontSize: 10,
        },
        emphasis: {
          scaleSize: 5,
        },
        itemStyle: {
          borderColor: theme.backgroundColor,
          borderWidth: 2,
        },
      },
    ],
  };
}

function buildPieEventHandlers(
  attrs: PieChartAttrs,
  slices: readonly PieChartSlice[],
): ReadonlyArray<EChartEventHandler> {
  if (!attrs.onSliceClick || slices.length === 0) return [];
  const onSliceClick = attrs.onSliceClick;
  return [
    {
      eventName: 'click',
      handler: (params) => {
        const p = params as EChartClickParams;
        const idx = p.dataIndex;
        if (idx !== undefined && idx >= 0 && idx < slices.length) {
          onSliceClick(slices[idx]);
        }
      },
    },
  ];
}
