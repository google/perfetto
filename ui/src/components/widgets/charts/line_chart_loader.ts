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
import {sqlRangeClause} from './chart_utils';
import {LineChartData, LineChartPoint, LineChartSeries} from './line_chart';

/**
 * Configuration for SQLLineChartLoader.
 */
export interface SQLLineChartLoaderOpts {
  /** The trace processor engine to run queries against. */
  readonly engine: Engine;

  /**
   * SQL query that provides the raw data.
   * Must include the X and Y columns (and optionally the series column).
   */
  readonly query: string;

  /** Column name for the X axis (numeric). */
  readonly xColumn: string;

  /** Column name for the Y axis (numeric). */
  readonly yColumn: string;

  /**
   * Optional column name for grouping points into separate series.
   * When omitted, all points belong to a single series named after yColumn.
   */
  readonly seriesColumn?: string;
}

/**
 * Per-use configuration for the line chart loader.
 */
export interface LineChartLoaderConfig {
  /**
   * Filter to only include points within this X range (e.g., from brush).
   */
  readonly xRange?: {readonly min: number; readonly max: number};

  /**
   * Maximum number of points per series. When the query returns more
   * points, evenly-spaced samples are kept (first and last are always
   * retained). Defaults to no limit.
   */
  readonly maxPoints?: number;
}

/**
 * Result returned by the line chart loader.
 */
export interface LineChartLoaderResult {
  /** The computed line chart data, or undefined if loading. */
  readonly data: LineChartData | undefined;

  /** Whether a query is currently pending. */
  readonly isPending: boolean;
}

/**
 * SQL-based line chart loader with async loading and caching.
 *
 * Fetches ordered (x, y) points directly from SQL, optionally grouped
 * into multiple series by a grouping column. Uses QuerySlot for caching
 * and request deduplication.
 */
export class SQLLineChartLoader {
  private readonly engine: Engine;
  private readonly baseQuery: string;
  private readonly xColumn: string;
  private readonly yColumn: string;
  private readonly seriesColumn: string | undefined;
  private readonly taskQueue = new SerialTaskQueue();
  private readonly querySlot = new QuerySlot<LineChartData>(this.taskQueue);

  constructor(opts: SQLLineChartLoaderOpts) {
    this.engine = opts.engine;
    this.baseQuery = opts.query;
    this.xColumn = opts.xColumn;
    this.yColumn = opts.yColumn;
    this.seriesColumn = opts.seriesColumn;
  }

  use(config: LineChartLoaderConfig): LineChartLoaderResult {
    const result = this.querySlot.use({
      key: {
        baseQuery: this.baseQuery,
        xColumn: this.xColumn,
        yColumn: this.yColumn,
        seriesColumn: this.seriesColumn,
        xRange: config.xRange,
        maxPoints: config.maxPoints,
      },
      queryFn: async () => {
        const xCol = this.xColumn;
        const yCol = this.yColumn;

        const filterClause =
          config.xRange !== undefined
            ? `WHERE ${sqlRangeClause(xCol, config.xRange)}`
            : '';

        const seriesExpr =
          this.seriesColumn !== undefined
            ? `CAST(${this.seriesColumn} AS TEXT)`
            : 'NULL';

        const orderBy =
          this.seriesColumn !== undefined
            ? 'ORDER BY _series, _x'
            : 'ORDER BY _x';

        const sql = `
          SELECT
            CAST(${xCol} AS REAL) AS _x,
            CAST(${yCol} AS REAL) AS _y,
            ${seriesExpr} AS _series
          FROM (${this.baseQuery})
          ${filterClause}
          ${orderBy}
        `;

        const queryResult = await this.engine.query(sql);

        // Group points by series
        const seriesMap = new Map<string, LineChartPoint[]>();
        const defaultName = this.seriesColumn !== undefined ? '' : yCol;

        const iter = queryResult.iter({_x: NUM, _y: NUM, _series: STR_NULL});

        for (; iter.valid(); iter.next()) {
          const name = iter._series ?? defaultName;
          let points = seriesMap.get(name);
          if (points === undefined) {
            points = [];
            seriesMap.set(name, points);
          }
          points.push({x: iter._x, y: iter._y});
        }

        // Build series array, applying downsampling if needed
        const series: LineChartSeries[] = [];
        for (const [name, points] of seriesMap) {
          series.push({
            name,
            points:
              config.maxPoints !== undefined
                ? downsample(points, config.maxPoints)
                : points,
          });
        }

        return {series};
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

/**
 * Downsample an array of points to at most `maxPoints` entries.
 * Always keeps the first and last points; intermediate points are
 * evenly sampled.
 */
function downsample(
  points: readonly LineChartPoint[],
  maxPoints: number,
): LineChartPoint[] {
  if (points.length <= maxPoints) return [...points];
  if (maxPoints <= 2) {
    return [points[0], points[points.length - 1]];
  }
  const result: LineChartPoint[] = [points[0]];
  const step = (points.length - 1) / (maxPoints - 1);
  for (let i = 1; i < maxPoints - 1; i++) {
    result.push(points[Math.round(i * step)]);
  }
  result.push(points[points.length - 1]);
  return result;
}
