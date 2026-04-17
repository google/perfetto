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
import {
  ChartSource,
  SQLChartLoader,
  QueryConfig,
  ChartLoaderResult,
  inFilter,
} from './chart_sql_source';
import {ChartAggregation} from './chart_utils';
import {PieChartData, PieChartSlice} from './pie_chart';

/**
 * Configuration for SQLPieChartLoader.
 */
export interface SQLPieChartLoaderOpts {
  /** The trace processor engine to run queries against. */
  readonly engine: Engine;

  /**
   * SQL query that provides the raw data.
   * Must include both the dimension and measure columns in its output.
   */
  readonly query: string;

  /** Column name for the dimension (slice labels). */
  readonly dimensionColumn: string;

  /** Column name for the measure (numeric values to aggregate). */
  readonly measureColumn: string;
}

/**
 * Per-use configuration for the pie chart loader.
 */
export interface PieChartLoaderConfig {
  /** Aggregation function to apply to the measure column per group. */
  readonly aggregation: ChartAggregation;

  /**
   * Maximum number of slices to return. Groups are sorted by aggregated
   * value descending; the top N are kept and all remaining groups are
   * collapsed into an "(Other)" slice. Defaults to no limit.
   */
  readonly limit?: number;

  /** Filter to only include specific dimension values. */
  readonly filter?: ReadonlyArray<string | number>;
}

/** Result returned by the pie chart loader. */
export type PieChartLoaderResult = ChartLoaderResult<PieChartData>;

/**
 * SQL-based pie chart loader with async loading and caching.
 *
 * Performs grouping and aggregation directly in SQL. When a limit is
 * set, groups beyond the top N are collapsed into an "(Other)" slice.
 * Uses QuerySlot for caching and request deduplication.
 */
export class SQLPieChartLoader extends SQLChartLoader<
  PieChartLoaderConfig,
  PieChartData
> {
  private readonly dimensionColumn: string;
  private readonly measureColumn: string;

  constructor(opts: SQLPieChartLoaderOpts) {
    super(
      opts.engine,
      new ChartSource({
        query: opts.query,
        schema: {
          [opts.dimensionColumn]: 'text',
          [opts.measureColumn]: 'real',
        },
      }),
    );
    this.dimensionColumn = opts.dimensionColumn;
    this.measureColumn = opts.measureColumn;
  }

  protected buildQueryConfig(config: PieChartLoaderConfig): QueryConfig {
    return {
      type: 'aggregated',
      dimensions: [{column: this.dimensionColumn}],
      measures: [{column: this.measureColumn, aggregation: config.aggregation}],
      filters: inFilter(this.dimensionColumn, config.filter),
      limit: config.limit,
      includeOther: config.limit !== undefined,
    };
  }

  protected parseResult(queryResult: QueryResult): PieChartData {
    const slices: PieChartSlice[] = [];
    const iter = queryResult.iter({_dim: STR_NULL, _value: NUM});
    for (; iter.valid(); iter.next()) {
      slices.push({label: iter._dim ?? '(null)', value: iter._value});
    }
    return {slices};
  }
}
