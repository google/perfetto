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

import {QuerySlot, SerialTaskQueue} from '../../../base/query_slot';
import {Engine} from '../../../trace_processor/engine';
import {NUM, STR_NULL} from '../../../trace_processor/query_result';
import {BarChartData, BarChartItem} from './bar_chart';
import {AggregationType, sqlAggExpression, sqlInClause} from './chart_utils';

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
  readonly aggregation: AggregationType;

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

/**
 * Result returned by the bar chart loader.
 */
export interface BarChartLoaderResult {
  /** The computed bar chart data, or undefined if loading. */
  readonly data: BarChartData | undefined;

  /** Whether a query is currently pending. */
  readonly isPending: boolean;
}

/**
 * SQL-based bar chart loader with async loading and caching.
 *
 * Performs grouping and aggregation directly in SQL for efficiency with
 * large datasets. Uses QuerySlot for caching and request deduplication.
 *
 * Usage:
 * ```typescript
 * class MyPanel {
 *   private loader: SQLBarChartLoader;
 *
 *   constructor(engine: Engine) {
 *     this.loader = new SQLBarChartLoader({
 *       engine,
 *       query: 'SELECT process_name, dur FROM slice WHERE dur > 0',
 *       dimensionColumn: 'process_name',
 *       measureColumn: 'dur',
 *     });
 *   }
 *
 *   view() {
 *     const {data} = this.loader.use({aggregation: 'SUM'});
 *     return m(BarChart, {data, dimensionLabel: 'Process', measureLabel: 'Total Duration'});
 *   }
 *
 *   onremove() {
 *     this.loader.dispose();
 *   }
 * }
 * ```
 */
export class SQLBarChartLoader {
  private readonly engine: Engine;
  private readonly baseQuery: string;
  private readonly dimensionColumn: string;
  private readonly measureColumn: string;
  private readonly taskQueue = new SerialTaskQueue();
  private readonly querySlot = new QuerySlot<BarChartData>(this.taskQueue);

  constructor(opts: SQLBarChartLoaderOpts) {
    this.engine = opts.engine;
    this.baseQuery = opts.query;
    this.dimensionColumn = opts.dimensionColumn;
    this.measureColumn = opts.measureColumn;
  }

  use(config: BarChartLoaderConfig): BarChartLoaderResult {
    const result = this.querySlot.use({
      key: {
        baseQuery: this.baseQuery,
        dimensionColumn: this.dimensionColumn,
        measureColumn: this.measureColumn,
        aggregation: config.aggregation,
        limit: config.limit,
        filter: config.filter,
      },
      queryFn: async () => {
        const dim = this.dimensionColumn;
        const meas = this.measureColumn;
        const aggExpr = sqlAggExpression(meas, config.aggregation);

        const inExpr =
          config.filter !== undefined ? sqlInClause(dim, config.filter) : '';
        const filterClause = inExpr !== '' ? `WHERE ${inExpr}` : '';

        const limitClause =
          config.limit !== undefined ? `LIMIT ${config.limit}` : '';

        const sql = `
          SELECT
            CAST(${dim} AS TEXT) AS _dim,
            ${aggExpr} AS _value
          FROM (${this.baseQuery})
          ${filterClause}
          GROUP BY ${dim}
          ORDER BY _value DESC
          ${limitClause}
        `;

        const queryResult = await this.engine.query(sql);

        const items: BarChartItem[] = [];
        const iter = queryResult.iter({
          _dim: STR_NULL,
          _value: NUM,
        });
        for (; iter.valid(); iter.next()) {
          const label = iter._dim ?? '(null)';
          items.push({label, value: iter._value});
        }

        return {items};
      },
    });

    return {
      data: result.data,
      isPending: result.isPending,
    };
  }

  dispose(): void {
    this.querySlot.dispose();
  }
}
