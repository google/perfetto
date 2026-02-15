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
import {Row, Filter} from '../../data/types';

export interface SelectionContext {
  g: d3.Selection<SVGGElement, unknown, null, undefined>;
  allData: Row[];
  onFilterRequest?: (filters: Filter[]) => void;
  updateSourceFilter?: boolean;
  getData?: (d: unknown) => Row;
}

/**
 * Defines how brush selections affect chart visuals and cross-chart filtering.
 */
export interface SelectionStrategy {
  onSelection(
    selectedData: Row[],
    filters: Filter[],
    context: SelectionContext,
  ): void;

  onClear(context: SelectionContext): void;

  // Whether to use SVG clip paths for visual selection
  usesClipPaths(): boolean;
}
