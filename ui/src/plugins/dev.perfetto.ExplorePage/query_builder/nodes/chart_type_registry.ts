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

import {ChartType} from './visualisation_node';

/**
 * Definition of a chart type, including metadata and capabilities.
 *
 * This registry pattern allows for extensible chart types. To add a new chart
 * type:
 * 1. Add the new type to the ChartType union in visualisation_node.ts
 * 2. Add a new entry to CHART_TYPES below
 * 3. Implement the rendering logic in chart_view.ts
 * 4. Implement the data loading logic in chart_data_loader.ts
 */
export interface ChartTypeDefinition {
  /** The chart type identifier (must match ChartType union) */
  readonly type: ChartType;

  /** Human-readable label for the chart type */
  readonly label: string;

  /** Material icon name for the chart type */
  readonly icon: string;

  /**
   * Whether this chart type supports aggregation functions (COUNT, SUM, etc.)
   * When true, the chart can show aggregated values per category.
   * Example: Bar chart showing SUM(duration) by thread_name
   */
  readonly supportsAggregation: boolean;

  /**
   * Whether this chart type supports binning of continuous values.
   * When true, the chart groups values into bins/buckets.
   * Example: Histogram showing distribution of durations in 10 bins
   */
  readonly supportsBinning: boolean;

  /**
   * Whether the primary column must be numeric.
   * When true, the column picker is filtered to quantitative types.
   * Example: Histogram, line chart, scatter plot all require numeric X.
   */
  readonly requiresNumericDimension: boolean;

  /** Label shown for the primary column picker in the config popup. */
  readonly primaryColumnLabel: string;

  /**
   * Whether the chart requires a second numeric column (Y axis).
   * When true, a Y column picker is shown in the config popup.
   * Example: Line chart (Y values), scatter plot (Y values).
   */
  readonly supportsYColumn: boolean;

  /**
   * Whether the chart supports an optional grouping/series column (any type).
   * When true, a group column picker is shown in the config popup.
   * Example: Line chart series grouping, treemap parent grouping.
   */
  readonly supportsGroupColumn: boolean;

  /**
   * Whether the chart supports an optional numeric size column.
   * When true, a size column picker is shown in the config popup.
   * Example: Scatter plot bubble size.
   */
  readonly supportsSizeColumn: boolean;
}

/**
 * Registry of all supported chart types.
 *
 * This array defines all available chart types and their capabilities.
 * The order here determines the order in UI dropdowns.
 */
export const CHART_TYPES: readonly ChartTypeDefinition[] = [
  {
    type: 'bar',
    label: 'Bar Chart',
    icon: 'bar_chart',
    supportsAggregation: true,
    supportsBinning: false,
    requiresNumericDimension: false,
    primaryColumnLabel: 'Dimension',
    supportsYColumn: false,
    supportsGroupColumn: false,
    supportsSizeColumn: false,
  },
  {
    type: 'histogram',
    label: 'Histogram',
    icon: 'ssid_chart',
    supportsAggregation: false,
    supportsBinning: true,
    requiresNumericDimension: true,
    primaryColumnLabel: 'Column',
    supportsYColumn: false,
    supportsGroupColumn: false,
    supportsSizeColumn: false,
  },
] as const;

/**
 * Get the definition for a specific chart type.
 *
 * @param type The chart type to look up
 * @returns The chart type definition, or undefined if not found
 */
export function getChartTypeDefinition(
  type: ChartType,
): ChartTypeDefinition | undefined {
  return CHART_TYPES.find((d) => d.type === type);
}

/**
 * Check if a given string is a valid chart type.
 *
 * @param type String to validate
 * @returns True if the string is a valid ChartType
 */
export function isValidChartType(type: string): type is ChartType {
  return CHART_TYPES.some((d) => d.type === type);
}
