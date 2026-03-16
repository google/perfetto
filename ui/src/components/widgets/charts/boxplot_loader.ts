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

import {
  QuerySlot,
  SerialTaskQueue,
  type QueryResult,
} from '../../../base/query_slot';
import {Engine} from '../../../trace_processor/engine';
import {NUM, STR} from '../../../trace_processor/query_result';
import {BoxplotData} from './boxplot';
import {validateColumnName} from './chart_utils';

/**
 * Configuration for boxplot loaders.
 */
export interface BoxplotLoaderConfig {
  /**
   * Maximum number of categories to show.
   * Categories are ordered by median value descending.
   */
  readonly limit?: number;
}

/**
 * Configuration for SQLBoxplotLoader.
 */
export interface SQLBoxplotLoaderOpts {
  /** The trace processor engine to run queries against. */
  readonly engine: Engine;

  /** SQL query that provides the data. */
  readonly query: string;

  /** Column name containing the category/grouping values. */
  readonly categoryColumn: string;

  /** Column name containing the numeric values to compute stats over. */
  readonly valueColumn: string;
}

/**
 * SQL-based boxplot loader.
 *
 * Computes quartile statistics (min, Q1, median, Q3, max) per category
 * using ROW_NUMBER and conditional aggregation to approximate percentiles
 * in SQLite (which lacks native PERCENTILE_CONT).
 */
export class SQLBoxplotLoader {
  private readonly engine: Engine;
  private readonly query: string;
  private readonly categoryColumn: string;
  private readonly valueColumn: string;
  private readonly taskQueue = new SerialTaskQueue();
  private readonly querySlot = new QuerySlot<BoxplotData>(this.taskQueue);

  constructor(opts: SQLBoxplotLoaderOpts) {
    validateColumnName(opts.categoryColumn);
    validateColumnName(opts.valueColumn);
    this.engine = opts.engine;
    this.query = opts.query;
    this.categoryColumn = opts.categoryColumn;
    this.valueColumn = opts.valueColumn;
  }

  use(config: BoxplotLoaderConfig): QueryResult<BoxplotData> {
    const limit = config.limit ?? 20;
    const cat = this.categoryColumn;
    const val = this.valueColumn;

    const sql = `
WITH _src AS (
  SELECT
    CAST(${cat} AS TEXT) AS _cat,
    CAST(${val} AS REAL) AS _val
  FROM (${this.query})
  WHERE ${val} IS NOT NULL
),
_stats AS (
  SELECT
    _cat,
    MIN(_val) AS _min,
    MAX(_val) AS _max,
    COUNT(*) AS _cnt
  FROM _src
  GROUP BY _cat
),
_ranked AS (
  SELECT
    s._cat,
    s._val,
    ROW_NUMBER() OVER (PARTITION BY s._cat ORDER BY s._val) AS _rn,
    st._cnt
  FROM _src s
  JOIN _stats st ON s._cat = st._cat
),
_quartiles AS (
  SELECT
    _cat,
    MAX(CASE WHEN _rn = MAX(1, CAST((_cnt + 1) * 0.25 AS INT)) THEN _val END) AS _q1,
    MAX(CASE WHEN _rn = MAX(1, CAST((_cnt + 1) * 0.50 AS INT)) THEN _val END) AS _median,
    MAX(CASE WHEN _rn = MAX(1, CAST((_cnt + 1) * 0.75 AS INT)) THEN _val END) AS _q3
  FROM _ranked
  GROUP BY _cat
)
SELECT
  s._cat,
  s._min,
  q._q1,
  q._median,
  q._q3,
  s._max
FROM _stats s
JOIN _quartiles q ON s._cat = q._cat
ORDER BY q._median DESC
LIMIT ${limit}`.trim();

    return this.querySlot.use({
      key: {sql},
      retainOn: ['sql'],
      queryFn: async () => {
        const queryResult = await this.engine.query(sql);
        const items = [];

        const iter = queryResult.iter({
          _cat: STR,
          _min: NUM,
          _q1: NUM,
          _median: NUM,
          _q3: NUM,
          _max: NUM,
        });

        for (; iter.valid(); iter.next()) {
          items.push({
            label: iter._cat,
            min: iter._min,
            q1: iter._q1,
            median: iter._median,
            q3: iter._q3,
            max: iter._max,
          });
        }

        return {items};
      },
    });
  }

  dispose(): void {
    this.querySlot.dispose();
  }
}
