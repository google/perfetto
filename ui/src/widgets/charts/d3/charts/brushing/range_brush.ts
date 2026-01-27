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

import * as d3 from 'd3';
import {Row, ChartSpec, ChartType, FilterOp} from '../../data/types';
import {
  BrushBehavior,
  BrushSelection,
  BrushResult,
  BrushScales,
} from './brush_behavior';

/**
 * Brush behavior for continuous numeric data (e.g., histograms, line charts).
 * Selects a range of values on the x-axis.
 */
export class RangeBrush extends BrushBehavior {
  createBrush(width: number, height: number): d3.BrushBehavior<unknown> {
    return d3.brushX().extent([
      [0, 0],
      [width, height],
    ]);
  }

  onBrushEnd(
    selection: BrushSelection | null,
    data: Row[],
    spec: ChartSpec,
    scales: BrushScales,
  ): BrushResult {
    if (!selection || !scales.x) {
      return {filters: [], selectedData: []};
    }

    const [x0, x1] = selection.extent as [number, number];
    const xScale = scales.x as d3.ScaleLinear<number, number>;

    // Convert pixel coordinates to data values
    const minValue = xScale.invert(x0);
    const maxValue = xScale.invert(x1);

    // Get the x field name based on chart type
    const xField =
      spec.type === ChartType.Histogram || spec.type === ChartType.Cdf
        ? spec.x
        : spec.type === ChartType.Scatter || spec.type === ChartType.Line
          ? spec.x
          : spec.type === ChartType.Bar || spec.type === ChartType.Boxplot
            ? spec.x
            : spec.type === ChartType.Heatmap
              ? spec.x
              : '';

    if (!xField) {
      return {filters: [], selectedData: []};
    }

    // Filter data within the selected range
    const selectedData = data.filter((d) => {
      const value = Number(d[xField]);
      return !isNaN(value) && value >= minValue && value <= maxValue;
    });

    return {
      filters: [
        {col: xField, op: FilterOp.Gte, val: minValue},
        {col: xField, op: FilterOp.Lte, val: maxValue},
      ],
      selectedData,
    };
  }
}
