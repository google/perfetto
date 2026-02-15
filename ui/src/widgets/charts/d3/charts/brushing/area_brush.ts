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
import {Row, ChartSpec, FilterOp} from '../../data/types';
import {
  BrushBehavior,
  BrushSelection,
  BrushResult,
  BrushScales,
} from './brush_behavior';

/**
 * Brush behavior for 2D data (e.g., scatter plots).
 * Selects a rectangular region on both x and y axes.
 */
export class AreaBrush extends BrushBehavior {
  createBrush(width: number, height: number): d3.BrushBehavior<unknown> {
    return d3.brush().extent([
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
    if (!selection || !scales.x || !scales.y || spec.type !== 'scatter') {
      return {filters: [], selectedData: []};
    }

    const [[x0, y0], [x1, y1]] = selection.extent as [
      [number, number],
      [number, number],
    ];
    const xScale = scales.x as d3.ScaleLinear<number, number>;
    const yScale = scales.y as d3.ScaleLinear<number, number>;

    // Convert pixel coordinates to data values
    // Note: y-axis is inverted in SVG coordinates
    const xMin = xScale.invert(x0);
    const xMax = xScale.invert(x1);
    const yMin = yScale.invert(y1); // inverted
    const yMax = yScale.invert(y0); // inverted

    // Filter data within the selected rectangle
    const selectedData = data.filter((d) => {
      const xValue = Number(d[spec.x]);
      const yValue = Number(d[spec.y]);
      return (
        !isNaN(xValue) &&
        !isNaN(yValue) &&
        xValue >= xMin &&
        xValue <= xMax &&
        yValue >= yMin &&
        yValue <= yMax
      );
    });

    return {
      filters: [
        {col: spec.x, op: FilterOp.Gte, val: xMin},
        {col: spec.x, op: FilterOp.Lte, val: xMax},
        {col: spec.y, op: FilterOp.Gte, val: yMin},
        {col: spec.y, op: FilterOp.Lte, val: yMax},
      ],
      selectedData,
    };
  }
}
