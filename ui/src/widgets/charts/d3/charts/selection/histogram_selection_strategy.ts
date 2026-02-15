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

import {Row, Filter} from '../../data/types';
import {OpacitySelectionStrategy} from './opacity_selection_strategy';
import {SelectionContext} from './selection_strategy';

/**
 * Histogram-specific selection strategy for pre-aggregated bins.
 * Bins have {x0, x1, count} format from DataSource aggregation.
 */
export class HistogramSelectionStrategy extends OpacitySelectionStrategy {
  onSelection(
    selectedData: Row[],
    filters: Filter[],
    context: SelectionContext,
  ): void {
    this.applyHistogramVisualSelection(context.g, selectedData, filters);

    // Send filters to other charts
    if (context.onFilterRequest) {
      context.onFilterRequest(filters);
    }
  }

  private applyHistogramVisualSelection(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    _selectedData: Row[],
    filters: Filter[],
  ): void {
    // Extract the selected bin range from filters
    let minValue: number | null = null;
    let maxValue: number | null = null;

    for (const filter of filters) {
      if (
        (filter.op === '>=' || filter.op === '>') &&
        typeof filter.val === 'number'
      ) {
        minValue = filter.val;
      } else if (
        (filter.op === '<' || filter.op === '<=') &&
        typeof filter.val === 'number'
      ) {
        maxValue = filter.val;
      }
    }

    g.selectAll('.selectable').style('opacity', (d: unknown) => {
      // If no valid range, return full opacity
      if (minValue === null || maxValue === null) return 1.0;

      const bin = d as {x0: number; x1: number; count: number};

      // Pre-aggregated bins have {x0, x1, count} structure
      if (typeof bin.x0 !== 'number' || typeof bin.x1 !== 'number') {
        return 1.0;
      }

      // Check if this bin overlaps with the selected range
      // Bin overlaps if: bin starts before selection ends AND bin ends after selection starts
      const isSelected = bin.x0 < maxValue && bin.x1 > minValue;
      return isSelected ? 1.0 : 0.2;
    });
  }
}
