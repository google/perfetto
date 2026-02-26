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

import m from 'mithril';
import {VisualisationNode, ChartConfig} from '../nodes/visualisation_node';
import {BarChart} from '../../../../components/widgets/charts/bar_chart';
import {Histogram} from '../../../../components/widgets/charts/histogram';
import {SQLBarChartLoader} from '../../../../components/widgets/charts/bar_chart_loader';
import {SQLHistogramLoader} from '../../../../components/widgets/charts/histogram_loader';
import {EmptyState} from '../../../../widgets/empty_state';
import {SqlValue} from '../../../../trace_processor/query_result';
import {isIntegerColumn, getNumericFormatter} from './chart_column_formatters';

/**
 * Per-chart loader state.
 *
 * The `key` encodes the loader's constructor params so we know when to dispose
 * and recreate.
 */
export interface ChartLoaderEntry {
  key: string;
  barLoader?: SQLBarChartLoader;
  histogramLoader?: SQLHistogramLoader;
}

/**
 * Minimal context passed to chart render functions.
 * Avoids passing the full ChartViewAttrs (which includes Trace and
 * QueryExecutionService that renderers don't need).
 */
export interface ChartRenderContext {
  readonly node: VisualisationNode;
  readonly onFilterChange?: () => void;
}

export function renderBarChart(
  ctx: ChartRenderContext,
  config: ChartConfig,
  entry: ChartLoaderEntry,
): m.Child {
  if (!entry.barLoader) {
    return m(EmptyState, {icon: 'bar_chart', title: 'No data to display'});
  }

  const agg = config.aggregation ?? 'COUNT';
  const {data} = entry.barLoader.use({aggregation: agg, limit: 100});

  const measureLabel =
    agg === 'COUNT'
      ? 'Count'
      : `${agg}(${config.measureColumn ?? config.column})`;

  const dimFormatter = getNumericFormatter(ctx.node, config.column);
  const formatDimension =
    dimFormatter !== undefined
      ? (v: string | number) => (typeof v === 'number' ? dimFormatter(v) : v)
      : undefined;

  const formatMeasure =
    agg !== 'COUNT'
      ? getNumericFormatter(ctx.node, config.measureColumn ?? config.column)
      : undefined;

  return m(BarChart, {
    data,
    height: 250,
    dimensionLabel: config.column,
    measureLabel,
    orientation: config.orientation ?? 'vertical',
    fillParent: true,
    formatDimension,
    formatMeasure,
    onBrush: (labels) => handleBarBrush(ctx, config, labels),
  });
}

export function renderHistogram(
  ctx: ChartRenderContext,
  config: ChartConfig,
  entry: ChartLoaderEntry,
): m.Child {
  if (!entry.histogramLoader) {
    return m(EmptyState, {icon: 'ssid_chart', title: 'No data to display'});
  }

  const isInteger = isIntegerColumn(ctx.node, config.column);
  const {data} = entry.histogramLoader.use({
    bucketCount: config.binCount,
    integer: isInteger,
  });

  const formatXValue = getNumericFormatter(ctx.node, config.column);

  return m(Histogram, {
    data,
    height: 250,
    xAxisLabel: config.column,
    integerDimension: isInteger,
    fillParent: true,
    formatXValue,
    onBrush: (range) =>
      handleHistogramBrush(ctx, config, range.start, range.end),
  });
}

function handleBarBrush(
  ctx: ChartRenderContext,
  config: ChartConfig,
  labels: Array<string | number>,
): void {
  if (labels.length === 0) return;

  // Convert ECharts label strings back to SqlValues for filter construction.
  // ECharts delivers labels as strings (after CAST(column AS TEXT) in SQL), so
  // numeric-looking strings are converted back to numbers for accurate matching.
  //
  // TODO: If a formatDimension function is active (e.g. duration formatting),
  // ECharts still provides the *original* (unformatted) string here, so the
  // round-trip is correct. However, if the column stores numeric values that
  // were stored as text (e.g. "1,234"), Number("1,234") = NaN and the filter
  // will correctly fall back to the string. No known broken case at this time,
  // but verify if adding new formatDimension types that change the numeric value.
  const values: SqlValue[] = labels.map((label) => {
    const str = String(label);
    if (str === '(null)') return null;
    // Try to convert numeric-looking strings back to numbers
    const num = Number(str);
    if (!isNaN(num) && str !== '') return num;
    return str;
  });

  ctx.node.clearChartFiltersForColumn(config.column);
  ctx.node.setBrushSelection(config.column, values);
  ctx.onFilterChange?.();
}

function handleHistogramBrush(
  ctx: ChartRenderContext,
  config: ChartConfig,
  start: number,
  end: number,
): void {
  const isInteger = isIntegerColumn(ctx.node, config.column);

  // For integer columns, use floor/ceil to capture all integer values
  const filterStart = isInteger ? Math.floor(start) : start;
  const filterEnd = isInteger ? Math.ceil(end) : end;

  ctx.node.clearChartFiltersForColumn(config.column);
  ctx.node.addRangeFilter(config.column, filterStart, filterEnd);
  ctx.onFilterChange?.();
}
