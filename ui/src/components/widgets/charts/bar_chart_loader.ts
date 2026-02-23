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
import {BarChartData, BarChartItem} from './bar_chart';
import {createChartLoader, ChartLoader, inFilter} from './chart_sql_source';
import {AggregateFunction} from '../datagrid/model';
import type {QueryResult as SlotResult} from '../../../base/query_slot';

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
}

/**
 * Per-use configuration for the bar chart loader.
 */
export interface BarChartLoaderConfig {
  /** Aggregation function to apply to the measure column per group. */
  readonly aggregation: AggregateFunction;

  /**
   * Maximum number of bars to return. Groups are sorted by aggregated
   * value descending, and only the top N are kept. Defaults to no limit.
   */
  readonly limit?: number;

  /**
   * Filter to only include specific dimension values (e.g., from brush).
   */
  readonly filter?: ReadonlyArray<string | number>;
}

/** Result returned by the bar chart loader. */
export type BarChartLoaderResult = SlotResult<BarChartData>;

/**
 * SQL-based bar chart loader with async loading and caching.
 *
 * Performs grouping and aggregation directly in SQL for efficiency with
 * large datasets.
 */
export class SQLBarChartLoader {
  private readonly loader: ChartLoader<BarChartLoaderConfig, BarChartData>;

  constructor(opts: SQLBarChartLoaderOpts) {
    const dimCol = opts.dimensionColumn;
    const measCol = opts.measureColumn;

    this.loader = createChartLoader({
      engine: opts.engine,
      query: opts.query,
      schema: {[dimCol]: 'text', [measCol]: 'real'},
      buildQueryConfig: (config) => ({
        type: 'aggregated',
        dimensions: [{column: dimCol}],
        measures: [{column: measCol, aggregation: config.aggregation}],
        filters: inFilter(dimCol, config.filter),
        limit: config.limit,
      }),
      parseResult: (queryResult: QueryResult) => {
        const items: BarChartItem[] = [];
        const iter = queryResult.iter({_dim: STR_NULL, _value: NUM});
        for (; iter.valid(); iter.next()) {
          items.push({label: iter._dim ?? '(null)', value: iter._value});
        }
        return {items};
      },
    });
  }

  use(config: BarChartLoaderConfig): BarChartLoaderResult {
    return this.loader.use(config);
  }

  dispose(): void {
    this.loader.dispose();
  }
}
