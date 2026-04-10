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
import {
  NUM,
  STR,
  QueryResult as TPQueryResult,
} from '../../../trace_processor/query_result';
import {AggregateFunction} from '../datagrid/model';
import {sqlAggregateExpr} from '../datagrid/sql_utils';
import {HeatmapData} from './heatmap';
import {validateColumnName} from './chart_utils';

/**
 * Configuration for heatmap loaders.
 */
export interface HeatmapLoaderConfig {
  /** Aggregation function for cell values. Defaults to 'SUM'. */
  readonly aggregation?: AggregateFunction;

  /** Maximum number of X-axis categories. */
  readonly xLimit?: number;

  /** Maximum number of Y-axis categories. */
  readonly yLimit?: number;
}

/**
 * Configuration for SQLHeatmapLoader.
 */
export interface SQLHeatmapLoaderOpts {
  /** The trace processor engine to run queries against. */
  readonly engine: Engine;

  /** SQL query that provides the data. */
  readonly query: string;

  /** Column name for the X axis (columns of the heatmap). */
  readonly xColumn: string;

  /** Column name for the Y axis (rows of the heatmap). */
  readonly yColumn: string;

  /** Column name containing the numeric values to aggregate. */
  readonly valueColumn: string;
}

/**
 * SQL-based heatmap loader.
 *
 * Groups data by two categorical dimensions (x, y) and aggregates a
 * numeric value, producing a grid suitable for the HeatmapChart widget.
 */
export class SQLHeatmapLoader {
  private readonly engine: Engine;
  private readonly query: string;
  private readonly xColumn: string;
  private readonly yColumn: string;
  private readonly valueColumn: string;
  private readonly taskQueue = new SerialTaskQueue();
  private readonly querySlot = new QuerySlot<HeatmapData>(this.taskQueue);

  constructor(opts: SQLHeatmapLoaderOpts) {
    validateColumnName(opts.xColumn);
    validateColumnName(opts.yColumn);
    validateColumnName(opts.valueColumn);
    this.engine = opts.engine;
    this.query = opts.query;
    this.xColumn = opts.xColumn;
    this.yColumn = opts.yColumn;
    this.valueColumn = opts.valueColumn;
  }

  use(config: HeatmapLoaderConfig): QueryResult<HeatmapData> {
    const agg = config.aggregation ?? 'SUM';
    const xLimit = config.xLimit ?? 20;
    const yLimit = config.yLimit ?? 20;
    const aggExpr = sqlAggregateExpr(agg, this.valueColumn);

    const sql = `
WITH _src AS (
  SELECT
    CAST(${this.xColumn} AS TEXT) AS _x,
    CAST(${this.yColumn} AS TEXT) AS _y,
    ${aggExpr} AS _val
  FROM (${this.query})
  WHERE ${this.valueColumn} IS NOT NULL
  GROUP BY _x, _y
),
_top_x AS (
  SELECT _x, SUM(_val) AS _total
  FROM _src
  GROUP BY _x
  ORDER BY _total DESC
  LIMIT ${xLimit}
),
_top_y AS (
  SELECT _y, SUM(_val) AS _total
  FROM _src
  GROUP BY _y
  ORDER BY _total DESC
  LIMIT ${yLimit}
)
SELECT _x, _y, _val
FROM _src
WHERE _x IN (SELECT _x FROM _top_x)
  AND _y IN (SELECT _y FROM _top_y)
ORDER BY _x, _y`.trim();

    return this.querySlot.use({
      key: {sql},
      retainOn: ['sql'],
      queryFn: async () => {
        const queryResult = await this.engine.query(sql);
        return parseHeatmapResult(queryResult);
      },
    });
  }

  dispose(): void {
    this.querySlot.dispose();
  }
}

function parseHeatmapResult(queryResult: TPQueryResult): HeatmapData {
  const xSet = new Map<string, number>();
  const ySet = new Map<string, number>();
  const rawValues: Array<{x: string; y: string; val: number}> = [];

  const iter = queryResult.iter({
    _x: STR,
    _y: STR,
    _val: NUM,
  });

  for (; iter.valid(); iter.next()) {
    rawValues.push({x: iter._x, y: iter._y, val: iter._val});
    if (!xSet.has(iter._x)) {
      xSet.set(iter._x, xSet.size);
    }
    if (!ySet.has(iter._y)) {
      ySet.set(iter._y, ySet.size);
    }
  }

  const xLabels = Array.from(xSet.keys());
  const yLabels = Array.from(ySet.keys());

  let min = Infinity;
  let max = -Infinity;
  const values: Array<readonly [number, number, number]> = rawValues.map(
    ({x, y, val}) => {
      min = Math.min(min, val);
      max = Math.max(max, val);
      const xIdx = xSet.get(x) ?? 0;
      const yIdx = ySet.get(y) ?? 0;
      return [xIdx, yIdx, val] as const;
    },
  );

  if (min === Infinity) {
    min = 0;
    max = 0;
  }

  return {xLabels, yLabels, values, min, max};
}
