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
 * Declarative charting library for Perfetto UI.
 *
 * This module provides stateless, Mithril-native chart components that follow
 * the principle of UI = f(State). Charts receive data via attrs and emit
 * events via callbacks, with no internal state management.
 *
 * Key features:
 * - Pure Mithril components (no d3.select())
 * - D3 used only for math (scales, path generators)
 * - Strict TypeScript (zero `any` types)
 * - Composable and easy to integrate
 * - Responsive via viewBox
 *
 * @module d3-decl
 */

import {Filter} from '../../../components/widgets/datagrid/model';

// Chart utilities
export * from './chart_utils';

/**
 * Helper to create cross-filtering handlers for charts.
 * Simplifies the common pattern of managing filters across multiple charts.
 *
 * @param getFilters Function to get the current filter array
 * @param setFilters Function to update the filter array
 * @returns Object with onFiltersAdd and onFiltersRemove handlers
 *
 * @example
 * ```typescript
 * class CrossFilteringDemo {
 *   private filters: Filter[] = [];
 *
 *   view() {
 *     const handlers = createCrossFilterHandlers(
 *       () => this.filters,
 *       (filters) => { this.filters = [...filters]; m.redraw(); }
 *     );
 *
 *     return m('.container', [
 *       m(Histogram, {
 *         data: histogramLoader.use({filters: this.filters}),
 *         valueColumn: 'duration',
 *         filters: this.filters,
 *         onFiltersAdd: handlers.onFiltersAdd('duration'),
 *         onFiltersRemove: handlers.onFiltersRemove,
 *       }),
 *     ]);
 *   }
 * }
 * ```
 */
export function createCrossFilterHandlers(
  getFilters: () => readonly Filter[],
  setFilters: (filters: readonly Filter[]) => void,
) {
  return {
    /**
     * Creates a handler for adding filters for a specific column.
     * Replaces any existing filters for that column with the new ones.
     *
     * @param column The column name to filter on
     * @returns Handler function for onFiltersAdd
     */
    onFiltersAdd: (column: string) => (newFilters: readonly Filter[]) => {
      setFilters([
        ...getFilters().filter((f) => f.field !== column),
        ...newFilters,
      ]);
    },

    /**
     * Handler for removing specific filters.
     * Removes the provided filters from the filter array.
     *
     * @param filtersToRemove The filters to remove
     */
    onFiltersRemove: (filtersToRemove: readonly Filter[]) => {
      const filtersSet = new Set(filtersToRemove);
      setFilters(getFilters().filter((f) => !filtersSet.has(f)));
    },
  };
}

// Histogram
export {
  Histogram,
  HistogramAttrs,
  HistogramBucket,
  HistogramData,
  HistogramConfig,
  computeHistogram,
} from './histogram/histogram';

export {
  InMemoryHistogramLoader,
  SQLHistogramLoader,
  HistogramLoader,
  HistogramLoaderConfig,
  HistogramLoaderResult,
  SQLHistogramLoaderOpts,
} from './histogram/histogram_loader';

// Bar Chart
export {SimpleBarChart} from './bar/simple_bar_chart';
export {GroupedBarChart} from './bar/grouped_bar_chart';
export {StackedBarChart} from './bar/stacked_bar_chart';

export type {
  BarDatum,
  SimpleBarData,
  GroupedBarData,
  SortConfig,
  BaseBarChartAttrs,
  SimpleBarChartAttrs,
  GroupedBarChartAttrs,
  StackedBarChartAttrs,
} from './bar/bar_types';

// Legacy export alias for backwards compatibility
export {SimpleBarChart as BarChart} from './bar/simple_bar_chart';
export type {SimpleBarData as BarData} from './bar/bar_types';
export type {SimpleBarChartAttrs as BarChartAttrs} from './bar/bar_types';

export {
  InMemoryBarLoader,
  SQLBarLoader,
  BarLoader,
  BarLoaderConfig,
  BarLoaderResult,
  SQLBarLoaderOpts,
  RawRow,
} from './bar/bar_loader';

// CDF Chart
export {CDFChart, CDFChartAttrs, CDFData, CDFLine, CDFPoint} from './cdf/cdf';

export {
  InMemoryCDFLoader,
  SQLCDFLoader,
  CDFLoader,
  CDFLoaderConfig,
  CDFLoaderResult,
  SQLCDFLoaderOpts,
} from './cdf/cdf_loader';

// Scatter Plot
export {
  ScatterPlot,
  ScatterPlotAttrs,
  ScatterData,
  ScatterPoint,
  CorrelationStats,
} from './scatter/scatter';

export {
  InMemoryScatterLoader,
  SQLScatterLoader,
  ScatterLoader,
  ScatterLoaderConfig,
  ScatterLoaderResult,
  SQLScatterLoaderOpts,
} from './scatter/scatter_loader';
