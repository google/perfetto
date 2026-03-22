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
 * Brush behavior for categorical data (e.g., bar charts).
 * Selects discrete categories based on their position.
 */
export class CategoricalBrush extends BrushBehavior {
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
    if (!selection || !scales.x || spec.type !== 'bar') {
      return {filters: [], selectedData: []};
    }

    const [x0, x1] = selection.extent as [number, number];
    const xScale = scales.x as d3.ScaleBand<string>;

    // Find all categories whose bars intersect with the brush selection
    const selectedData = data.filter((d) => {
      const barX = xScale(String(d[spec.x]));
      if (barX === undefined) return false;
      const barWidth = xScale.bandwidth();
      return barX + barWidth > x0 && barX < x1;
    });

    const selectedValues = selectedData
      .map((d) => d[spec.x])
      .filter((v): v is string | number => v !== null && v !== undefined);

    // Determine if values are strings or numbers and cast appropriately
    const isNumeric =
      selectedValues.length > 0 && typeof selectedValues[0] === 'number';
    const val = isNumeric
      ? (selectedValues as number[])
      : selectedValues.map((v) => String(v));

    return {
      filters: [{col: spec.x, op: FilterOp.In, val}],
      selectedData,
    };
  }
}
