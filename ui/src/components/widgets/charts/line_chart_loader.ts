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
  createChartLoader,
  ChartLoader,
  PointColumnSpec,
  rangeFilters,
} from './chart_sql_source';
import {LineChartData, LineChartPoint, LineChartSeries} from './line_chart';
import type {QueryResult as SlotResult} from '../../../base/query_slot';

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
  /** Filter to only include points within this X range (e.g., from brush). */
  readonly xRange?: {readonly min: number; readonly max: number};

  /**
   * Maximum number of points per series. When the query returns more
   * points, evenly-spaced samples are kept (first and last are always
   * retained). Defaults to no limit.
   */
  readonly maxPoints?: number;
}

/** Result returned by the line chart loader. */
export type LineChartLoaderResult = SlotResult<LineChartData>;

/**
 * SQL-based line chart loader with async loading and caching.
 *
 * Fetches ordered (x, y) points directly from SQL, optionally grouped
 * into multiple series by a grouping column.
 */
export class SQLLineChartLoader {
  private readonly loader: ChartLoader<LineChartLoaderConfig, LineChartData>;

  constructor(opts: SQLLineChartLoaderOpts) {
    const xCol = opts.xColumn;
    const yCol = opts.yColumn;
    const seriesCol = opts.seriesColumn;

    const schema: Record<string, 'text' | 'real'> = {
      [xCol]: 'real',
      [yCol]: 'real',
    };
    if (seriesCol !== undefined) {
      schema[seriesCol] = 'text';
    }

    this.loader = createChartLoader({
      engine: opts.engine,
      query: opts.query,
      schema,
      buildQueryConfig: (config) => {
        const columns: PointColumnSpec[] = [
          {column: xCol, alias: '_x', cast: 'real'},
          {column: yCol, alias: '_y', cast: 'real'},
        ];
        // When no breakdown column is configured, buildPoints() won't add
        // _series to the SELECT. Add NULL AS _series explicitly so that
        // parseResult's iter spec always finds the column.
        if (seriesCol === undefined) {
          columns.push({alias: '_series', cast: 'text'});
        }
        return {
          type: 'points',
          columns,
          breakdown: seriesCol,
          filters: rangeFilters(xCol, config.xRange),
          orderBy:
            seriesCol !== undefined
              ? [{column: '_series'}, {column: '_x'}]
              : [{column: '_x'}],
        };
      },
      parseResult: (queryResult: QueryResult, config) => {
        const seriesMap = new Map<string, LineChartPoint[]>();
        const defaultName = seriesCol !== undefined ? '' : yCol;
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
      extraCacheKey: (config) => ({maxPoints: config.maxPoints}),
    });
  }

  use(config: LineChartLoaderConfig): LineChartLoaderResult {
    return this.loader.use(config);
  }

  dispose(): void {
    this.loader.dispose();
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
