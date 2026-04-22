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
import {extractBrushRect, formatNumber} from './chart_utils';
import {EChartView, EChartEventHandler} from './echart_view';
import {
  buildAxisOption,
  buildBrushOption,
  buildGridOption,
  buildTooltipOption,
  SELECTION_COLOR,
} from './chart_option_builder';

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

  /**
   * Callback when brush selection completes (on mouseup).
   * Called with the x and y labels of all cells in the brushed rectangle.
   */
  readonly onBrush?: (selection: {
    xLabels: string[];
    yLabels: string[];
  }) => void;

  /**
   * Selection to highlight on the chart. Cells at the intersection of
   * the selected xLabels and yLabels are drawn with a highlight color.
   * The consumer controls this state — typically by feeding the
   * `onBrush` output back in.
   */
  readonly selection?: {
    readonly xLabels: ReadonlyArray<string>;
    readonly yLabels: ReadonlyArray<string>;
  };
}

export class HeatmapChart implements m.ClassComponent<HeatmapAttrs> {
  view({attrs}: m.CVnode<HeatmapAttrs>) {
    const {data, height = 300, fillParent, className, onBrush} = attrs;

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
      eventHandlers: buildHeatmapEventHandlers(attrs, data),
      activeBrushType: onBrush !== undefined ? 'rect' : undefined,
    });
  }
}

function buildHeatmapOption(
  attrs: HeatmapAttrs,
  data: HeatmapData,
): EChartsCoreOption {
  const {xAxisLabel, yAxisLabel, formatValue} = attrs;
  const fmtVal = formatValue ?? formatNumber;

  const option: Record<string, unknown> = {
    animation: false,
    grid: buildGridOption({right: 80}),
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
      textStyle: {fontSize: 10},
    },
    series: [
      {
        type: 'heatmap',
        data: buildHeatmapSeriesData(data, attrs.selection),
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

  if (attrs.onBrush) {
    option.brush = buildBrushOption({
      xAxisIndex: 0,
      yAxisIndex: 0,
      brushType: 'rect',
    });
    option.toolbox = {show: false};
  }

  return option as EChartsCoreOption;
}

function buildHeatmapSeriesData(
  data: HeatmapData,
  selection: HeatmapAttrs['selection'],
): Array<Record<string, unknown>> {
  const xSet = selection !== undefined ? new Set(selection.xLabels) : undefined;
  const ySet = selection !== undefined ? new Set(selection.yLabels) : undefined;

  return data.values.map((triple) => {
    const [xIdx, yIdx, value] = triple;
    const item: Record<string, unknown> = {value: [xIdx, yIdx, value]};
    if (xSet !== undefined && ySet !== undefined) {
      const xLabel = data.xLabels[xIdx];
      const yLabel = data.yLabels[yIdx];
      if (
        xLabel !== undefined &&
        yLabel !== undefined &&
        xSet.has(xLabel) &&
        ySet.has(yLabel)
      ) {
        item.itemStyle = {
          color: SELECTION_COLOR,
          borderColor: 'rgba(0, 120, 212, 0.6)',
          borderWidth: 2,
        };
      }
    }
    return item;
  });
}

function buildHeatmapEventHandlers(
  attrs: HeatmapAttrs,
  data: HeatmapData | undefined,
): ReadonlyArray<EChartEventHandler> {
  if (
    !attrs.onBrush ||
    data === undefined ||
    data.xLabels.length === 0 ||
    data.yLabels.length === 0
  ) {
    return [];
  }
  const onBrush = attrs.onBrush;
  const xLabels = data.xLabels;
  const yLabels = data.yLabels;

  return [
    {
      eventName: 'brushEnd',
      handler: (params) => {
        const range = extractBrushRect(params);
        if (range !== undefined) {
          const xMin = Math.max(0, Math.round(range.xMin));
          const xMax = Math.min(xLabels.length - 1, Math.round(range.xMax));
          const yMin = Math.max(0, Math.round(range.yMin));
          const yMax = Math.min(yLabels.length - 1, Math.round(range.yMax));
          if (xMin <= xMax && yMin <= yMax) {
            const selectedX: string[] = [];
            for (let i = xMin; i <= xMax; i++) {
              selectedX.push(xLabels[i]);
            }
            const selectedY: string[] = [];
            for (let i = yMin; i <= yMax; i++) {
              selectedY.push(yLabels[i]);
            }
            if (selectedX.length > 0 && selectedY.length > 0) {
              onBrush({xLabels: selectedX, yLabels: selectedY});
            }
          }
        }
      },
    },
  ];
}
