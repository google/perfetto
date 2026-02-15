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
import {SelectionStrategy, SelectionContext} from './selection_strategy';

/**
 * Handles brush selections visually without filtering data.
 * Brush highlights selected items but other charts still receive filter updates.
 */
export class OpacitySelectionStrategy implements SelectionStrategy {
  usesClipPaths(): boolean {
    return true;
  }

  onSelection(
    selectedData: Row[],
    filters: Filter[],
    context: SelectionContext,
  ): void {
    this.applyVisualSelection(
      context.g,
      selectedData,
      filters,
      context.getData,
    );

    // Send filters to other charts while keeping this chart's data unchanged
    if (context.onFilterRequest) {
      context.onFilterRequest(filters);
    }
  }

  onClear(context: SelectionContext): void {
    this.clearVisualSelection(context.g);
    if (context.onFilterRequest) {
      context.onFilterRequest([]);
    }
  }

  private applyVisualSelection(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
    selectedData: Row[],
    _filters: Filter[],
    getData: (d: unknown) => Row = (d) => d as Row,
  ): void {
    const isEmpty = selectedData.length === 0;
    const selectedSet = new Set(selectedData);

    g.selectAll('.selectable').style('opacity', (d: unknown) => {
      if (isEmpty) return 1.0;

      const row = getData(d);
      const isSelected = selectedSet.has(row);
      return isSelected ? 1.0 : 0.2;
    });
  }

  private clearVisualSelection(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
  ): void {
    g.selectAll('.selectable').style('opacity', 1.0);
  }
}
