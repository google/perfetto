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

/**
 * Bar chart components - unified export.
 *
 * This module provides three bar chart variants, all following clean
 * architecture principles with zero anti-patterns:
 *
 * - SimpleBarChart: Single series, categorical data
 * - GroupedBarChart: Multi-series, side-by-side bars
 * - StackedBarChart: Multi-series, vertically stacked bars
 *
 * All components are:
 * - Always controlled (no internal state)
 * - Use complete filter arrays (no delta tracking)
 * - Use shared BrushHandlerCategorical (no justBrushed)
 * - Use shared axis renderers (no duplication)
 * - Immutable data flow
 * - TypeScript strict mode
 *
 * @example Simple Bar Chart
 * ```typescript
 * import {SimpleBarChart} from './bar';
 *
 * m(SimpleBarChart, {
 *   data: {bars: [{category: 'A', value: 10}, ...]},
 *   filters: this.filters,
 *   column: 'status',
 *   onFiltersChanged: (filters) => { this.filters = filters; },
 * })
 * ```
 *
 * @example Grouped Bar Chart
 * ```typescript
 * import {GroupedBarChart} from './bar';
 *
 * m(GroupedBarChart, {
 *   data: {
 *     bars: [
 *       {category: 'Q1', value: 100, group: 'Product A'},
 *       {category: 'Q1', value: 80, group: 'Product B'},
 *     ],
 *     groups: ['Product A', 'Product B'],
 *   },
 *   filters: this.filters,
 *   column: 'quarter',
 *   onFiltersChanged: (filters) => { this.filters = filters; },
 * })
 * ```
 *
 * @example Stacked Bar Chart
 * ```typescript
 * import {StackedBarChart} from './bar';
 *
 * m(StackedBarChart, {
 *   data: {
 *     bars: [
 *       {category: 'Q1', value: 100, group: 'Revenue'},
 *       {category: 'Q1', value: 30, group: 'Profit'},
 *     ],
 *     groups: ['Revenue', 'Profit'],
 *   },
 *   filters: this.filters,
 *   column: 'quarter',
 *   onFiltersChanged: (filters) => { this.filters = filters; },
 * })
 * ```
 */

// Component exports
// Import first, then export to preserve full type information through re-export chain
import {SimpleBarChart} from './simple_bar_chart';
import {GroupedBarChart} from './grouped_bar_chart';
import {StackedBarChart} from './stacked_bar_chart';

export {SimpleBarChart, GroupedBarChart, StackedBarChart};

// Type exports
export type {
  BarDatum,
  SimpleBarData,
  GroupedBarData,
  SortConfig,
  BaseBarChartAttrs,
  SimpleBarChartAttrs,
  GroupedBarChartAttrs,
  StackedBarChartAttrs,
} from './bar_types';

// Utility exports (for advanced usage)
export {
  extractCategories,
  extractGroups,
  sortBars,
  getSelectedCategories,
  createFiltersWithCategories,
  toggleCategoryFilter,
  clearColumnFilters,
  computeStackedLayout,
  getMaxStackedValue,
} from './bar_utils';
