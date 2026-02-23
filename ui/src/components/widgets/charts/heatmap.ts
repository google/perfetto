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
 * Data provided to a HeatmapChart.
 * The grid is defined by xLabels, yLabels, and a values matrix.
 */
export interface HeatmapData {
  /** Labels for the X axis (columns). */
  readonly xLabels: readonly string[];
  /** Labels for the Y axis (rows). */
  readonly yLabels: readonly string[];
  /**
   * Values as [xIndex, yIndex, value] triples.
   * Missing entries are treated as 0.
   */
  readonly values: ReadonlyArray<readonly [number, number, number]>;
  /** Minimum value in the dataset (for color scale). */
  readonly min: number;
  /** Maximum value in the dataset (for color scale). */
  readonly max: number;
}

export interface HeatmapAttrs {
  /**
   * Heatmap data to display, or undefined if loading.
   * When undefined, a loading spinner is shown.
   */
  readonly data: HeatmapData | undefined;

  /**
   * Height of the chart in pixels. Defaults to 300.
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
   * Fill parent container. Defaults to false.
   */
  readonly fillParent?: boolean;

  /**
   * Custom class name for the container.
   */
  readonly className?: string;

  /**
   * Format function for cell values.
   */
  readonly formatValue?: (value: number) => string;
}

export class HeatmapChart implements m.ClassComponent<HeatmapAttrs> {
  view({attrs}: m.CVnode<HeatmapAttrs>) {
    const {data, height = 300, fillParent, className} = attrs;

    const isEmpty =
      data !== undefined &&
      (data.values.length === 0 ||
        data.xLabels.length === 0 ||
        data.yLabels.length === 0);
    const option =
      data !== undefined && !isEmpty
        ? buildHeatmapOption(attrs, data)
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

function buildHeatmapOption(
  attrs: HeatmapAttrs,
  data: HeatmapData,
): EChartsCoreOption {
  const {xAxisLabel, yAxisLabel, formatValue} = attrs;
  const fmtVal = formatValue ?? formatNumber;
  const theme = getChartThemeColors();

  const option: Record<string, unknown> = {
    animation: false,
    grid: buildGridOption({
      top: 10,
      right: 80,
      bottom: xAxisLabel ? 40 : 25,
    }),
    xAxis: {
      ...buildAxisOption(
        {
          type: 'category',
          data: [...data.xLabels],
          name: xAxisLabel,
          labelOverflow: 'truncate',
          labelWidth: 60,
        },
        true,
      ),
      splitArea: {show: true},
    },
    yAxis: {
      ...buildAxisOption(
        {
          type: 'category',
          data: [...data.yLabels],
          name: yAxisLabel,
          labelOverflow: 'truncate',
          labelWidth: 80,
        },
        false,
      ),
      splitArea: {show: true},
    },
    tooltip: buildTooltipOption({
      trigger: 'item',
      formatter: (params: {
        value?: [number, number, number];
        name?: string;
      }) => {
        const v = params.value;
        if (v === undefined) return '';
        const xLabel = data.xLabels[v[0]] ?? '';
        const yLabel = data.yLabels[v[1]] ?? '';
        return [`${xLabel} / ${yLabel}`, `Value: ${fmtVal(v[2])}`].join('<br>');
      },
    }),
    visualMap: {
      min: data.min,
      max: data.max,
      calculable: true,
      orient: 'vertical',
      right: 0,
      top: 'center',
      textStyle: {color: theme.textColor, fontSize: 10},
      inRange: {
        color: [theme.chartColors[0] + '22', theme.chartColors[0]],
      },
    },
    series: [
      {
        type: 'heatmap',
        data: [...data.values],
        label: {show: false},
        emphasis: {
          itemStyle: {
            shadowBlur: 10,
            shadowColor: 'rgba(0, 0, 0, 0.5)',
          },
        },
      },
    ],
  };

  return option as EChartsCoreOption;
}
