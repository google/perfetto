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
 * Type definitions for bar charts following clean architecture principles.
 */

import {Filter} from '../../../../components/widgets/datagrid/model';

/**
 * A single bar in the chart.
 */
export interface BarDatum {
  /** Category label (X-axis value) */
  readonly category: string;
  /** Numeric value (Y-axis height) */
  readonly value: number;
  /** Optional group/series name for grouped/stacked bars */
  readonly group?: string;
}

/**
 * Data for simple bar charts (single series).
 */
export interface SimpleBarData {
  readonly bars: readonly BarDatum[];
}

/**
 * Data for grouped or stacked bar charts (multi-series).
 */
export interface GroupedBarData {
  readonly bars: readonly BarDatum[];
  readonly groups: readonly string[];
}

/**
 * Sort configuration for bar charts.
 */
export interface SortConfig {
  readonly by: 'category' | 'value';
  readonly direction: 'asc' | 'desc';
}

/**
 * Common attributes shared by all bar chart types.
 */
export interface BaseBarChartAttrs {
  /**
   * Complete filter array. Chart uses this to:
   * 1. Show filter overlays (e.g., highlight selected categories)
   * 2. Compute new filters when user interacts
   */
  readonly filters: readonly Filter[];

  /**
   * Column/field name for the category (X-axis).
   * Used to identify which filters belong to this chart.
   */
  readonly column: string;

  /**
   * Called when user adds or removes filters via interaction.
   * Chart returns COMPLETE new filter array, not just changes.
   * Parent should assign: this.filters = newFilters
   * No m.redraw() needed - Mithril auto-redraws after event handlers.
   */
  readonly onFiltersChanged?: (filters: readonly Filter[]) => void;

  /**
   * Display height in pixels. Defaults to 200.
   */
  readonly height?: number;

  /**
   * Label for the X axis.
   */
  readonly xAxisLabel?: string;

  /**
   * Label for the Y axis.
   */
  readonly yAxisLabel?: string;

  /**
   * Fill parent container. Defaults to false.
   */
  readonly fillParent?: boolean;

  /**
   * Custom class name for the container.
   */
  readonly className?: string;

  /**
   * Format function for X axis labels (category names).
   */
  readonly formatXValue?: (value: string) => string;

  /**
   * Format function for Y axis values (numeric).
   */
  readonly formatYValue?: (value: number) => string;

  /**
   * Sort configuration.
   */
  readonly sort?: SortConfig;
}

/**
 * Attributes for simple bar chart (single series).
 */
export interface SimpleBarChartAttrs extends BaseBarChartAttrs {
  /**
   * Bar chart data to display, or undefined if loading.
   */
  readonly data: SimpleBarData | undefined;

  /**
   * Custom color for bars. Defaults to theme color.
   */
  readonly color?: string;
}

/**
 * Attributes for grouped bar chart (multi-series, side-by-side).
 */
export interface GroupedBarChartAttrs extends BaseBarChartAttrs {
  /**
   * Grouped bar chart data to display, or undefined if loading.
   */
  readonly data: GroupedBarData | undefined;

  /**
   * Custom colors for each group.
   * If not provided, uses default color scheme.
   */
  readonly colors?: readonly string[];

  /**
   * Whether to show a legend. Defaults to true for grouped charts.
   */
  readonly showLegend?: boolean;
}

/**
 * Attributes for stacked bar chart (multi-series, stacked).
 */
export interface StackedBarChartAttrs extends BaseBarChartAttrs {
  /**
   * Stacked bar chart data to display, or undefined if loading.
   */
  readonly data: GroupedBarData | undefined;

  /**
   * Custom colors for each group.
   * If not provided, uses default color scheme.
   */
  readonly colors?: readonly string[];

  /**
   * Whether to show a legend. Defaults to true for stacked charts.
   */
  readonly showLegend?: boolean;
}
