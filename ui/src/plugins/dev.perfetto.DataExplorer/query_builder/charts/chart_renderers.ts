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
import {assertUnreachable} from '../../../../base/assert';
import {classNames} from '../../../../base/classnames';
import {
  ChartConfig,
  ChartType,
  getDefaultChartLabel,
} from '../nodes/visualisation_node';
import {ColumnInfo} from '../column_info';
import {BarChart} from '../../../../components/widgets/charts/bar_chart';
import {Histogram} from '../../../../components/widgets/charts/histogram';
import {LineChart} from '../../../../components/widgets/charts/line_chart';
import {Scatterplot} from '../../../../components/widgets/charts/scatterplot';
import {PieChart} from '../../../../components/widgets/charts/pie_chart';
import {
  Treemap,
  TreemapNode,
  countTreemapLeaves,
} from '../../../../components/widgets/charts/treemap';
import {BoxplotChart} from '../../../../components/widgets/charts/boxplot';
import {HeatmapChart} from '../../../../components/widgets/charts/heatmap';
import {SQLBarChartLoader} from '../../../../components/widgets/charts/bar_chart_loader';
import {SQLHistogramLoader} from '../../../../components/widgets/charts/histogram_loader';
import {SQLLineChartLoader} from '../../../../components/widgets/charts/line_chart_loader';
import {SQLScatterChartLoader} from '../../../../components/widgets/charts/scatterplot_loader';
import {SQLPieChartLoader} from '../../../../components/widgets/charts/pie_chart_loader';
import {SQLTreemapLoader} from '../../../../components/widgets/charts/treemap_loader';
import {SQLBoxplotLoader} from '../../../../components/widgets/charts/boxplot_loader';
import {SQLHeatmapLoader} from '../../../../components/widgets/charts/heatmap_loader';
import {SQLCdfLoader} from '../../../../components/widgets/charts/cdf_loader';
import {SQLSingleValueLoader} from '../../../../components/widgets/charts/single_value_loader';
import {Scorecard} from '../../../../components/widgets/charts/scorecard';
import {Icon} from '../../../../widgets/icon';
import {Tooltip} from '../../../../widgets/tooltip';
import {PopupPosition} from '../../../../widgets/popup';
import {EmptyState} from '../../../../widgets/empty_state';
import {SqlValue} from '../../../../trace_processor/query_result';
import {Engine} from '../../../../trace_processor/engine';
import {isIntegerColumn, getNumericFormatter} from './chart_column_formatters';
import {ChartAggregation} from '../../../../components/widgets/charts/chart_utils';
import {countDistinctPieSlices} from '../../../../components/widgets/charts/chart_sql_source';

// shown + total must be set together to prevent half-populated truncation state.
interface TruncationInfo {
  readonly shown: number;
  readonly total: number;
}

interface ChartRenderResult {
  readonly content: m.Child;
  readonly truncation?: TruncationInfo;
}

/**
 * Formats a human-readable measure label for chart axes.
 */
function formatMeasureLabel(
  agg: ChartAggregation,
  config: ChartConfig,
): string {
  switch (agg) {
    case 'COUNT':
      return 'Count';
    case 'COUNT_DISTINCT':
      // COUNT_DISTINCT uses config.column (not measureColumn) because
      // it operates on the dimension column itself.
      return `Count Distinct(${config.column})`;
    default:
      return `${agg}(${config.measureColumn ?? config.column})`;
  }
}

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
  lineLoader?: SQLLineChartLoader;
  scatterLoader?: SQLScatterChartLoader;
  pieLoader?: SQLPieChartLoader;
  treemapLoader?: SQLTreemapLoader;
  boxplotLoader?: SQLBoxplotLoader;
  heatmapLoader?: SQLHeatmapLoader;
  cdfLoader?: SQLCdfLoader;
  singleValueLoader?: SQLSingleValueLoader;
}

/**
 * Interface for chart-aware nodes/adapters. Implemented by VisualisationNode
 * and the dashboard chart adapter so that chart renderers and config popup
 * are decoupled from the specific node implementation.
 */
export interface ChartColumnProvider {
  readonly sourceCols: ReadonlyArray<ColumnInfo>;
  getChartableColumns(chartType: ChartType): ReadonlyArray<ColumnInfo>;
  clearChartFiltersForColumn(column: string): void;
  setBrushSelection(column: string, values: SqlValue[]): void;
  addRangeFilter(column: string, min: number, max: number): void;
  updateChart(chartId: string, updates: Partial<Omit<ChartConfig, 'id'>>): void;
  removeChart(chartId: string): void;
  readonly state: {readonly chartConfigs: ReadonlyArray<ChartConfig>};
}

/**
 * Minimal context passed to chart render functions.
 * Avoids passing the full ChartViewAttrs (which includes Trace and
 * QueryExecutionService that renderers don't need).
 */
export interface ChartRenderContext {
  readonly node: ChartColumnProvider;
  readonly onFilterChange?: () => void;
  /** When set, charts that support grid lines will render them. */
  readonly gridLines?: 'horizontal' | 'vertical' | 'both';
}

/** Dispose all loaders on a ChartLoaderEntry. */
export function disposeChartLoaders(entry: ChartLoaderEntry): void {
  entry.barLoader?.dispose();
  entry.histogramLoader?.dispose();
  entry.lineLoader?.dispose();
  entry.scatterLoader?.dispose();
  entry.pieLoader?.dispose();
  entry.treemapLoader?.dispose();
  entry.boxplotLoader?.dispose();
  entry.heatmapLoader?.dispose();
  entry.cdfLoader?.dispose();
  entry.singleValueLoader?.dispose();
}

/** Build a stable cache key for a chart loader entry. */
export function buildLoaderCacheKey(
  tableName: string,
  config: ChartConfig,
  ...extras: string[]
): string {
  return [
    tableName,
    config.chartType,
    config.column,
    config.measureColumn ?? '',
    config.yColumn ?? '',
    config.groupColumn ?? '',
    config.sizeColumn ?? '',
    ...extras,
  ].join('|');
}

/** Create the appropriate SQL loader(s) for a chart config. */
export function createChartLoaders(
  engine: Engine,
  query: string,
  config: ChartConfig,
  entry: ChartLoaderEntry,
): void {
  switch (config.chartType) {
    case 'bar':
      entry.barLoader = new SQLBarChartLoader({
        engine,
        query,
        dimensionColumn: config.column,
        measureColumn: config.measureColumn ?? config.column,
        seriesColumn: config.groupColumn,
      });
      break;
    case 'histogram':
      entry.histogramLoader = new SQLHistogramLoader({
        engine,
        query: `SELECT ${config.column} FROM (${query})`,
        valueColumn: config.column,
      });
      break;
    case 'line':
      if (config.yColumn) {
        entry.lineLoader = new SQLLineChartLoader({
          engine,
          query,
          xColumn: config.column,
          yColumn: config.yColumn,
          seriesColumn: config.groupColumn,
        });
      }
      break;
    case 'scatter':
      if (config.yColumn) {
        entry.scatterLoader = new SQLScatterChartLoader({
          engine,
          query,
          xColumn: config.column,
          yColumn: config.yColumn,
          sizeColumn: config.sizeColumn,
          seriesColumn: config.groupColumn,
        });
      }
      break;
    case 'pie':
      entry.pieLoader = new SQLPieChartLoader({
        engine,
        query,
        dimensionColumn: config.column,
        measureColumn: config.measureColumn ?? config.column,
      });
      break;
    case 'treemap':
      entry.treemapLoader = new SQLTreemapLoader({
        engine,
        query,
        labelColumn: config.column,
        sizeColumn: config.measureColumn ?? config.column,
        groupColumn: config.groupColumn,
      });
      break;
    case 'boxplot':
      if (config.yColumn) {
        entry.boxplotLoader = new SQLBoxplotLoader({
          engine,
          query,
          categoryColumn: config.column,
          valueColumn: config.yColumn,
        });
      }
      break;
    case 'heatmap':
      if (config.yColumn) {
        entry.heatmapLoader = new SQLHeatmapLoader({
          engine,
          query,
          xColumn: config.column,
          yColumn: config.yColumn,
          valueColumn: config.measureColumn ?? config.column,
        });
      }
      break;
    case 'cdf':
      entry.cdfLoader = new SQLCdfLoader({
        engine,
        query,
        valueColumn: config.column,
        seriesColumn: config.groupColumn,
      });
      break;
    case 'scorecard':
      entry.singleValueLoader = new SQLSingleValueLoader({
        engine,
        query,
        measureColumn: config.measureColumn ?? config.column,
      });
      break;
  }
}

/**
 * Render the appropriate chart widget for a config + loader entry.
 *
 * `globalShowTruncationWarning` is a dashboard-level override: when false it
 * suppresses the warning for all charts regardless of their per-chart config.
 */
export function renderChartByType(
  ctx: ChartRenderContext,
  config: ChartConfig,
  entry: ChartLoaderEntry,
  globalShowTruncationWarning?: boolean,
): m.Child {
  let result: ChartRenderResult;
  switch (config.chartType) {
    case 'bar':
      result = renderBarChart(ctx, config, entry);
      break;
    case 'histogram':
      result = renderHistogram(ctx, config, entry);
      break;
    case 'line':
      result = renderLineChart(ctx, config, entry);
      break;
    case 'scatter':
      result = renderScatterChart(ctx, config, entry);
      break;
    case 'pie':
      result = renderPieChart(ctx, config, entry);
      break;
    case 'treemap':
      result = renderTreemap(ctx, config, entry);
      break;
    case 'boxplot':
      result = renderBoxplot(ctx, config, entry);
      break;
    case 'heatmap':
      result = renderHeatmap(ctx, config, entry);
      break;
    case 'cdf':
      result = renderCdf(ctx, config, entry);
      break;
    case 'scorecard':
      result = renderScorecard(ctx, config, entry);
      break;
    default:
      assertUnreachable(config.chartType);
  }
  const showWarning =
    (globalShowTruncationWarning ?? true) &&
    config.showTruncationWarning !== false;
  return wrapWithTruncationWarning(showWarning, result);
}

// Overlays a warning icon+tooltip if the chart data is truncated and warnings
// are enabled. `showWarning` is the resolved effective flag (per-chart &&
// global dashboard setting).
function wrapWithTruncationWarning(
  showWarning: boolean,
  result: ChartRenderResult,
): m.Child {
  const trunc = result.truncation;
  if (!showWarning || trunc === undefined || trunc.total <= trunc.shown) {
    return result.content;
  }
  const tooltipText = `Showing ${trunc.shown} of ${trunc.total} items`;
  return m('.pf-chart-truncation-wrapper', [
    result.content,
    m(
      Tooltip,
      {
        trigger: m(Icon, {
          className: classNames('pf-chart-truncation-warning'),
          icon: 'warning',
          filled: true,
        }),
        position: PopupPosition.BottomEnd,
        showArrow: false,
      },
      tooltipText,
    ),
  ]);
}

// Returns undefined when totalCount is absent (query had no LIMIT), so the
// warning overlay is skipped entirely for unlimited queries.
function truncationInfo(
  shown: number,
  total: number | undefined,
): TruncationInfo | undefined {
  return total === undefined ? undefined : {shown, total};
}

function renderBarChart(
  ctx: ChartRenderContext,
  config: ChartConfig,
  entry: ChartLoaderEntry,
): ChartRenderResult {
  if (!entry.barLoader) {
    return {
      content: m(EmptyState, {icon: 'bar_chart', title: 'No data to display'}),
    };
  }

  const agg = config.aggregation ?? 'COUNT';
  const {data, totalCount} = entry.barLoader.use({
    aggregation: agg,
    limit: 100,
  });

  const measureLabel = formatMeasureLabel(agg, config);

  const dimFormatter = getNumericFormatter(ctx.node, config.column);
  const formatDimension =
    dimFormatter !== undefined
      ? (v: string | number) => (typeof v === 'number' ? dimFormatter(v) : v)
      : undefined;

  const needsMeasureFormatter = agg !== 'COUNT' && agg !== 'COUNT_DISTINCT';
  const formatMeasure = needsMeasureFormatter
    ? getNumericFormatter(ctx.node, config.measureColumn ?? config.column)
    : undefined;

  return {
    content: m(BarChart, {
      data,
      height: 250,
      dimensionLabel: config.column,
      measureLabel,
      orientation: config.orientation ?? 'vertical',
      fillParent: true,
      formatDimension,
      formatMeasure,
      gridLines: ctx.gridLines,
      onBrush: (labels) => handleBarBrush(ctx, config, labels),
    }),
    truncation: truncationInfo(data?.items.length ?? 0, totalCount),
  };
}

function renderHistogram(
  ctx: ChartRenderContext,
  config: ChartConfig,
  entry: ChartLoaderEntry,
): ChartRenderResult {
  if (!entry.histogramLoader) {
    return {
      content: m(EmptyState, {
        icon: 'ssid_chart',
        title: 'No data to display',
      }),
    };
  }

  const isInteger = isIntegerColumn(ctx.node, config.column);
  const {data} = entry.histogramLoader.use({
    bucketCount: config.binCount,
    integer: isInteger,
  });

  const formatXValue = getNumericFormatter(ctx.node, config.column);

  return {
    content: m(Histogram, {
      data,
      height: 250,
      xAxisLabel: config.column,
      integerDimension: isInteger,
      fillParent: true,
      formatXValue,
      onBrush: (range) =>
        handleHistogramBrush(ctx, config, range.start, range.end),
    }),
  };
}

function renderLineChart(
  ctx: ChartRenderContext,
  config: ChartConfig,
  entry: ChartLoaderEntry,
): ChartRenderResult {
  if (!entry.lineLoader) {
    return {
      content: m(EmptyState, {icon: 'show_chart', title: 'No data to display'}),
    };
  }

  const {data, totalCount} = entry.lineLoader.use({});

  const formatXValue = getNumericFormatter(ctx.node, config.column);
  const formatYValue = config.yColumn
    ? getNumericFormatter(ctx.node, config.yColumn)
    : undefined;

  return {
    content: m(LineChart, {
      data,
      height: 250,
      xAxisLabel: config.column,
      yAxisLabel: config.yColumn,
      fillParent: true,
      scaleAxes: true,
      formatXValue,
      formatYValue,
      gridLines: ctx.gridLines,
      onBrush: (range) =>
        handleHistogramBrush(ctx, config, range.start, range.end),
    }),
    // `shown` counts sampled points across all series; `totalCount` is source
    // rows before the per-series stride-sampling. The warning correctly fires
    // whenever sampled points < total rows.
    truncation: truncationInfo(
      data?.series.reduce((sum, s) => sum + s.points.length, 0) ?? 0,
      totalCount,
    ),
  };
}

function renderScatterChart(
  ctx: ChartRenderContext,
  config: ChartConfig,
  entry: ChartLoaderEntry,
): ChartRenderResult {
  if (!entry.scatterLoader) {
    return {
      content: m(EmptyState, {
        icon: 'scatter_plot',
        title: 'No data to display',
      }),
    };
  }

  const {data, totalCount} = entry.scatterLoader.use({maxPoints: 2000});

  const formatXValue = getNumericFormatter(ctx.node, config.column);
  const formatYValue = config.yColumn
    ? getNumericFormatter(ctx.node, config.yColumn)
    : undefined;

  return {
    content: m(Scatterplot, {
      data,
      height: 250,
      xAxisLabel: config.column,
      yAxisLabel: config.yColumn,
      fillParent: true,
      scaleAxes: true,
      formatXValue,
      formatYValue,
      gridLines: ctx.gridLines,
    }),
    // Same sampling semantics as renderLineChart above.
    truncation: truncationInfo(
      data?.series.reduce((sum, s) => sum + s.points.length, 0) ?? 0,
      totalCount,
    ),
  };
}

function renderPieChart(
  ctx: ChartRenderContext,
  config: ChartConfig,
  entry: ChartLoaderEntry,
): ChartRenderResult {
  if (!entry.pieLoader) {
    return {
      content: m(EmptyState, {icon: 'pie_chart', title: 'No data to display'}),
    };
  }

  const agg = config.aggregation ?? 'COUNT';
  const {data, totalCount} = entry.pieLoader.use({aggregation: agg, limit: 20});

  const formatValue =
    agg !== 'COUNT' && agg !== 'COUNT_DISTINCT'
      ? getNumericFormatter(ctx.node, config.measureColumn ?? config.column)
      : undefined;

  return {
    content: m(PieChart, {
      data,
      height: 250,
      fillParent: true,
      formatValue,
      onSliceClick: (slice) => {
        ctx.node.clearChartFiltersForColumn(config.column);
        ctx.node.setBrushSelection(config.column, [slice.label]);
        ctx.onFilterChange?.();
      },
    }),
    truncation: truncationInfo(
      countDistinctPieSlices(data?.slices),
      totalCount,
    ),
  };
}

function renderTreemap(
  ctx: ChartRenderContext,
  config: ChartConfig,
  entry: ChartLoaderEntry,
): ChartRenderResult {
  if (!entry.treemapLoader) {
    return {
      content: m(EmptyState, {icon: 'grid_view', title: 'No data to display'}),
    };
  }

  const agg = config.aggregation ?? 'SUM';
  const {data, totalCount} = entry.treemapLoader.use({
    aggregation: agg,
    limit: 50,
  });

  const formatValue = getNumericFormatter(
    ctx.node,
    config.measureColumn ?? config.column,
  );

  return {
    content: m(Treemap, {
      data,
      height: 250,
      fillParent: true,
      formatValue,
      onNodeClick: (node: TreemapNode) => {
        ctx.node.clearChartFiltersForColumn(config.column);
        ctx.node.setBrushSelection(config.column, [node.name]);
        ctx.onFilterChange?.();
      },
    }),
    truncation: truncationInfo(countTreemapLeaves(data?.nodes), totalCount),
  };
}

function renderBoxplot(
  ctx: ChartRenderContext,
  config: ChartConfig,
  entry: ChartLoaderEntry,
): ChartRenderResult {
  if (!entry.boxplotLoader) {
    return {
      content: m(EmptyState, {
        icon: 'candlestick_chart',
        title: 'No data to display',
      }),
    };
  }

  const {data, totalCount} = entry.boxplotLoader.use({limit: 30});

  const formatValue = config.yColumn
    ? getNumericFormatter(ctx.node, config.yColumn)
    : undefined;

  return {
    content: m(BoxplotChart, {
      data,
      height: 250,
      categoryLabel: config.column,
      valueLabel: config.yColumn,
      fillParent: true,
      formatValue,
      gridLines: ctx.gridLines,
    }),
    truncation: truncationInfo(data?.items.length ?? 0, totalCount),
  };
}

function renderHeatmap(
  _ctx: ChartRenderContext,
  config: ChartConfig,
  entry: ChartLoaderEntry,
): ChartRenderResult {
  if (!entry.heatmapLoader) {
    return {
      content: m(EmptyState, {icon: 'grid_on', title: 'No data to display'}),
    };
  }

  // Heatmap uses AggregateFunction (SUM/AVG/MIN/MAX) not ChartAggregation.
  // If aggregation is COUNT or unset, default to SUM.
  const rawAgg = config.aggregation;
  const agg =
    rawAgg === 'SUM' || rawAgg === 'AVG' || rawAgg === 'MIN' || rawAgg === 'MAX'
      ? rawAgg
      : 'SUM';
  const {data} = entry.heatmapLoader.use({aggregation: agg});

  return {
    content: m(HeatmapChart, {
      data,
      height: 250,
      xAxisLabel: config.column,
      yAxisLabel: config.yColumn,
      fillParent: true,
    }),
  };
}

function renderCdf(
  ctx: ChartRenderContext,
  config: ChartConfig,
  entry: ChartLoaderEntry,
): ChartRenderResult {
  if (!entry.cdfLoader) {
    return {
      content: m(EmptyState, {
        icon: 'trending_up',
        title: 'No data to display',
      }),
    };
  }

  const {data, totalCount} = entry.cdfLoader.use({maxPoints: 500});

  const formatXValue = getNumericFormatter(ctx.node, config.column);

  return {
    content: m(LineChart, {
      data,
      height: 250,
      xAxisLabel: config.column,
      yAxisLabel: 'Cumulative %',
      fillParent: true,
      scaleAxes: true,
      formatXValue,
      onBrush: (range) =>
        handleHistogramBrush(ctx, config, range.start, range.end),
    }),
    // Same sampling semantics as renderLineChart above.
    truncation: truncationInfo(
      data?.series.reduce((sum, s) => sum + s.points.length, 0) ?? 0,
      totalCount,
    ),
  };
}

function renderScorecard(
  ctx: ChartRenderContext,
  config: ChartConfig,
  entry: ChartLoaderEntry,
): ChartRenderResult {
  if (!entry.singleValueLoader) {
    return {
      content: m(EmptyState, {icon: 'numbers', title: 'No data to display'}),
    };
  }

  const agg = config.aggregation ?? 'COUNT_DISTINCT';
  const {data, isPending} = entry.singleValueLoader.use({aggregation: agg});

  const formatValue = getNumericFormatter(
    ctx.node,
    config.measureColumn ?? config.column,
  );

  return {
    content: m(Scorecard, {
      label: getDefaultChartLabel(config),
      value: data?.value,
      isPending,
      fillParent: true,
      formatValue,
    }),
  };
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
