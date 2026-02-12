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
import {AggregationType, sqlAggExpression, sqlInClause} from './chart_utils';
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

  /**
   * Column name for the dimension (slice labels).
   */
  readonly dimensionColumn: string;

  /**
   * Column name for the measure (numeric values to aggregate).
   */
  readonly measureColumn: string;
}

/**
 * Per-use configuration for the pie chart loader.
 */
export interface PieChartLoaderConfig {
  /** Aggregation function to apply to the measure column per group. */
  readonly aggregation: AggregationType;

  /**
   * Maximum number of slices to return. Groups are sorted by aggregated
   * value descending; the top N are kept and all remaining groups are
   * collapsed into an "(Other)" slice. Defaults to no limit.
   */
  readonly limit?: number;

  /**
   * Filter to only include specific dimension values.
   */
  readonly filter?: ReadonlyArray<string | number>;
}

/**
 * Result returned by the pie chart loader.
 */
export interface PieChartLoaderResult {
  /** The computed pie chart data, or undefined if loading. */
  readonly data: PieChartData | undefined;

  /** Whether a query is currently pending. */
  readonly isPending: boolean;
}

/**
 * SQL-based pie chart loader with async loading and caching.
 *
 * Performs grouping and aggregation directly in SQL. When a limit is
 * set, groups beyond the top N are collapsed into an "(Other)" slice.
 * Uses QuerySlot for caching and request deduplication.
 */
export class SQLPieChartLoader {
  private readonly engine: Engine;
  private readonly baseQuery: string;
  private readonly dimensionColumn: string;
  private readonly measureColumn: string;
  private readonly taskQueue = new SerialTaskQueue();
  private readonly querySlot = new QuerySlot<PieChartData>(this.taskQueue);

  constructor(opts: SQLPieChartLoaderOpts) {
    this.engine = opts.engine;
    this.baseQuery = opts.query;
    this.dimensionColumn = opts.dimensionColumn;
    this.measureColumn = opts.measureColumn;
  }

  use(config: PieChartLoaderConfig): PieChartLoaderResult {
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

        let sql: string;
        if (config.limit !== undefined) {
          // Top-N with "(Other)" bucket
          sql = `
            WITH _agg AS (
              SELECT
                CAST(${dim} AS TEXT) AS _dim,
                ${aggExpr} AS _value
              FROM (${this.baseQuery})
              ${filterClause}
              GROUP BY ${dim}
              ORDER BY _value DESC
            ),
            _top AS (
              SELECT _dim, _value FROM _agg LIMIT ${config.limit}
            ),
            _other AS (
              SELECT '(Other)' AS _dim, SUM(_value) AS _value
              FROM _agg
              WHERE _dim NOT IN (SELECT _dim FROM _top)
            )
            SELECT _dim, _value FROM _top
            UNION ALL
            SELECT _dim, _value FROM _other WHERE _value > 0
          `;
        } else {
          sql = `
            SELECT
              CAST(${dim} AS TEXT) AS _dim,
              ${aggExpr} AS _value
            FROM (${this.baseQuery})
            ${filterClause}
            GROUP BY ${dim}
            ORDER BY _value DESC
          `;
        }

        const queryResult = await this.engine.query(sql);

        const slices: PieChartSlice[] = [];
        const iter = queryResult.iter({
          _dim: STR_NULL,
          _value: NUM,
        });
        for (; iter.valid(); iter.next()) {
          const label = iter._dim ?? '(null)';
          slices.push({label, value: iter._value});
        }

        return {slices};
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
