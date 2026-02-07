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
 * Standard brush behavior that filters data across all charts.
 * Creates filters that cause all charts (including source) to reload.
 */
export class FilterSelectionStrategy implements SelectionStrategy {
  usesClipPaths(): boolean {
    return false;
  }

  onSelection(
    selectedData: Row[],
    filters: Filter[],
    context: SelectionContext,
  ): void {
    this.applyVisualSelection(
      context.g,
      context.allData,
      selectedData,
      context.getData,
    );

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
    _allData: Row[],
    selectedData: Row[],
    getData: (d: unknown) => Row = (d) => d as Row,
  ): void {
    const selectedSet = new Set(selectedData);
    const isEmpty = selectedData.length === 0;

    g.selectAll('.selectable').style('opacity', (d: unknown) => {
      const row = getData(d);
      const isSelected = isEmpty || selectedSet.has(row);
      return isSelected ? 1.0 : 0.2;
    });
  }

  private clearVisualSelection(
    g: d3.Selection<SVGGElement, unknown, null, undefined>,
  ): void {
    g.selectAll('.selectable').style('opacity', 1.0);
  }
}
