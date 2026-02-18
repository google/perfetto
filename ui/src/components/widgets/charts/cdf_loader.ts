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
import {NUM, STR, QueryResult} from '../../../trace_processor/query_result';
import {LineChartData} from './line_chart';
import {createChartLoader, ChartLoader, rangeFilters} from './chart_sql_source';
import type {QueryResult as SlotResult} from '../../../base/query_slot';

/**
 * Configuration for CDF loaders.
 */
export interface CdfLoaderConfig {
  /**
   * Range filter to apply to the data (e.g., from brush selection).
   * Only values within [min, max] are included.
   */
  readonly filter?: {
    readonly min: number;
    readonly max: number;
  };

  /**
   * Maximum number of points to return. When the dataset is larger,
   * results are capped via SQL LIMIT. Defaults to 500.
   */
  readonly maxPoints?: number;
}

/**
 * Configuration for SQLCdfLoader.
 */
export interface SQLCdfLoaderOpts {
  /** The trace processor engine to run queries against. */
  readonly engine: Engine;

  /** SQL query that provides the data. */
  readonly query: string;

  /** Column name containing the values to compute CDF over. */
  readonly valueColumn: string;

  /** Optional series/breakdown column name. */
  readonly seriesColumn?: string;
}

/**
 * SQL-based CDF (Cumulative Distribution Function) loader.
 *
 * Fetches sorted values from SQL and computes cumulative percentages
 * in JS. The result is a LineChartData that can be rendered directly
 * with the LineChart widget.
 *
 * Each point has x = value, y = cumulative percentage (0-100).
 */
export class SQLCdfLoader {
  private readonly loader: ChartLoader<CdfLoaderConfig, LineChartData>;

  constructor(opts: SQLCdfLoaderOpts) {
    const valCol = opts.valueColumn;
    const seriesCol = opts.seriesColumn;

    const schema: Record<string, 'real' | 'text'> = {
      [valCol]: 'real',
    };
    if (seriesCol !== undefined) {
      schema[seriesCol] = 'text';
    }

    this.loader = createChartLoader({
      engine: opts.engine,
      query: opts.query,
      schema,
      buildQueryConfig: (config) => ({
        type: 'points',
        columns: [{column: valCol, alias: '_x', cast: 'real' as const}],
        breakdown: seriesCol,
        filters: rangeFilters(valCol, config.filter),
        orderBy: [{column: '_x', direction: 'asc'}],
        maxPointsPerSeries: config.maxPoints ?? 500,
      }),
      parseResult: (queryResult: QueryResult) => {
        return parseCdfResult(queryResult, seriesCol !== undefined);
      },
    });
  }

  use(config: CdfLoaderConfig): SlotResult<LineChartData> {
    return this.loader.use(config);
  }

  dispose(): void {
    this.loader.dispose();
  }
}

function parseCdfResult(
  queryResult: QueryResult,
  hasSeries: boolean,
): LineChartData {
  const seriesMap = new Map<string, number[]>();

  if (hasSeries) {
    const iter = queryResult.iter({_x: NUM, _series: STR});
    for (; iter.valid(); iter.next()) {
      const name = iter._series;
      let values = seriesMap.get(name);
      if (values === undefined) {
        values = [];
        seriesMap.set(name, values);
      }
      values.push(iter._x);
    }
  } else {
    const values: number[] = [];
    seriesMap.set('CDF', values);
    const iter = queryResult.iter({_x: NUM});
    for (; iter.valid(); iter.next()) {
      values.push(iter._x);
    }
  }

  const series = Array.from(seriesMap.entries()).map(([name, values]) => {
    // Values are already sorted (ORDER BY in SQL)
    const n = values.length;
    const points = values.map((x, i) => ({
      x,
      y: ((i + 1) / n) * 100,
    }));
    return {name, points};
  });

  return {series};
}
