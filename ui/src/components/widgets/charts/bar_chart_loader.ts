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

import {Engine} from '../../../trace_processor/engine';
import {
  NUM,
  STR_NULL,
  QueryResult,
} from '../../../trace_processor/query_result';
import {BarChartData, BarChartItem, BarChartSeries} from './bar_chart';
import {
  ChartSource,
  ColumnType,
  SQLChartLoader,
  QueryConfig,
  ChartLoaderResult,
  inFilter,
} from './chart_sql_source';
import {ChartAggregation} from './chart_utils';

/**
 * Configuration for SQLBarChartLoader.
 */
export interface SQLBarChartLoaderOpts {
  /** The trace processor engine to run queries against. */
  readonly engine: Engine;

  /**
   * SQL query that provides the raw data.
   * Must include both the dimension and measure columns in its output.
   */
  readonly query: string;

  /**
   * Column name for the dimension (grouping key / bar labels).
   * Values can be strings or numbers.
   */
  readonly dimensionColumn: string;

  /**
   * Column name for the measure (numeric values to aggregate).
   */
  readonly measureColumn: string;

  /**
   * Optional column for stacked bar series. Each unique value in this column
   * becomes a separate stacked series.
   */
  readonly seriesColumn?: string;
}

/**
 * Per-use configuration for the bar chart loader.
 */
export interface BarChartLoaderConfig {
  /** Aggregation function to apply to the measure column per group. */
  readonly aggregation: ChartAggregation;

  /**
   * Maximum number of bars to return. Groups are sorted by aggregated
   * value descending, and only the top N are kept. Defaults to no limit.
   */
  readonly limit?: number;

  /**
   * Filter to only include specific dimension values (e.g., from brush).
   * NULL values in the array produce `column IS NULL` SQL conditions.
   */
  readonly filter?: ReadonlyArray<string | number | null>;
}

/** Result returned by the bar chart loader. */
export type BarChartLoaderResult = ChartLoaderResult<BarChartData>;

/**
 * SQL-based bar chart loader with async loading and caching.
 *
 * Performs grouping and aggregation directly in SQL for efficiency with
 * large datasets. Uses QuerySlot for caching and request deduplication.
 */
export class SQLBarChartLoader extends SQLChartLoader<
  BarChartLoaderConfig,
  BarChartData
> {
  private readonly dimensionColumn: string;
  private readonly measureColumn: string;
  private readonly seriesColumn?: string;

  constructor(opts: SQLBarChartLoaderOpts) {
    const schema: Record<string, ColumnType> = {
      [opts.dimensionColumn]: 'text',
      [opts.measureColumn]: 'real',
    };
    if (opts.seriesColumn !== undefined) {
      schema[opts.seriesColumn] = 'text';
    }
    super(
      opts.engine,
      new ChartSource({
        query: opts.query,
        schema,
      }),
    );
    this.dimensionColumn = opts.dimensionColumn;
    this.measureColumn = opts.measureColumn;
    this.seriesColumn = opts.seriesColumn;
  }

  protected buildQueryConfig(config: BarChartLoaderConfig): QueryConfig {
    const dimensions = [{column: this.dimensionColumn}];
    if (this.seriesColumn !== undefined) {
      dimensions.push({column: this.seriesColumn});
    }
    return {
      type: 'aggregated',
      dimensions,
      measures: [{column: this.measureColumn, aggregation: config.aggregation}],
      filters: inFilter(this.dimensionColumn, config.filter),
      limit: config.limit,
    };
  }

  protected parseResult(queryResult: QueryResult): BarChartData {
    if (this.seriesColumn !== undefined) {
      return this.parseStacked(queryResult);
    }
    const items: BarChartItem[] = [];
    const iter = queryResult.iter({_dim: STR_NULL, _value: NUM});
    for (; iter.valid(); iter.next()) {
      items.push({label: iter._dim ?? '(null)', value: iter._value});
    }
    return {items};
  }

  private parseStacked(queryResult: QueryResult): BarChartData {
    // ChartSource names dimension columns as _dim, _dim_1, _dim_2, …
    // (see buildAggregatedQuery in chart_sql_source.ts). Here _dim is
    // the primary category and _dim_1 is the series/group column.
    const seriesMap = new Map<string, BarChartItem[]>();
    const iter = queryResult.iter({
      _dim: STR_NULL,
      _dim_1: STR_NULL,
      _value: NUM,
    });
    for (; iter.valid(); iter.next()) {
      const seriesName = iter._dim_1 ?? '(null)';
      const label = iter._dim ?? '(null)';
      let items = seriesMap.get(seriesName);
      if (items === undefined) {
        items = [];
        seriesMap.set(seriesName, items);
      }
      items.push({label, value: iter._value});
    }
    const series: BarChartSeries[] = [];
    for (const [name, items] of seriesMap) {
      series.push({name, items});
    }
    return {items: [], series};
  }
}
