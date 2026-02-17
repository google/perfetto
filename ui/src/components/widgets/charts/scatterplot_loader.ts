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
import {NUM, NUM_NULL, STR_NULL} from '../../../trace_processor/query_result';
import {sqlRangeClause, validateColumnName} from './chart_utils';
import {
  ScatterChartData,
  ScatterChartPoint,
  ScatterChartSeries,
} from './scatterplot';

/**
 * Configuration for SQLScatterChartLoader.
 */
export interface SQLScatterChartLoaderOpts {
  /** The trace processor engine to run queries against. */
  readonly engine: Engine;

  /**
   * SQL query that provides the raw data.
   * Must include x and y columns (and optionally size, color, series columns).
   */
  readonly query: string;

  /** Column name for the X axis (numeric). */
  readonly xColumn: string;

  /** Column name for the Y axis (numeric). */
  readonly yColumn: string;

  /** Optional column for bubble size (numeric). */
  readonly sizeColumn?: string;

  /** Optional column for per-point label. */
  readonly labelColumn?: string;

  /**
   * Optional column for grouping points into separate series.
   * When omitted, all points belong to a single series.
   */
  readonly seriesColumn?: string;
}

/**
 * Per-use configuration for the scatter chart loader.
 */
export interface ScatterChartLoaderConfig {
  /**
   * Filter to only include points within this X range.
   */
  readonly xRange?: {readonly min: number; readonly max: number};

  /**
   * Filter to only include points within this Y range.
   */
  readonly yRange?: {readonly min: number; readonly max: number};

  /**
   * Maximum number of points per series. When exceeded, random sampling
   * is applied. Defaults to no limit.
   */
  readonly maxPoints?: number;
}

/**
 * Result returned by the scatter chart loader.
 */
export interface ScatterChartLoaderResult {
  /** The computed scatter chart data, or undefined if loading. */
  readonly data: ScatterChartData | undefined;

  /** Whether a query is currently pending. */
  readonly isPending: boolean;
}

/**
 * SQL-based scatter chart loader with async loading and caching.
 *
 * Fetches (x, y) points with optional size and series grouping from SQL.
 * Uses QuerySlot for caching and request deduplication.
 */
export class SQLScatterChartLoader {
  private readonly engine: Engine;
  private readonly baseQuery: string;
  private readonly xColumn: string;
  private readonly yColumn: string;
  private readonly sizeColumn: string | undefined;
  private readonly labelColumn: string | undefined;
  private readonly seriesColumn: string | undefined;
  private readonly taskQueue = new SerialTaskQueue();
  private readonly querySlot = new QuerySlot<ScatterChartData>(this.taskQueue);

  constructor(opts: SQLScatterChartLoaderOpts) {
    validateColumnName(opts.xColumn);
    validateColumnName(opts.yColumn);
    if (opts.sizeColumn !== undefined) validateColumnName(opts.sizeColumn);
    if (opts.labelColumn !== undefined) validateColumnName(opts.labelColumn);
    if (opts.seriesColumn !== undefined) validateColumnName(opts.seriesColumn);
    this.engine = opts.engine;
    this.baseQuery = opts.query;
    this.xColumn = opts.xColumn;
    this.yColumn = opts.yColumn;
    this.sizeColumn = opts.sizeColumn;
    this.labelColumn = opts.labelColumn;
    this.seriesColumn = opts.seriesColumn;
  }

  use(config: ScatterChartLoaderConfig): ScatterChartLoaderResult {
    const result = this.querySlot.use({
      key: {
        baseQuery: this.baseQuery,
        xColumn: this.xColumn,
        yColumn: this.yColumn,
        sizeColumn: this.sizeColumn,
        labelColumn: this.labelColumn,
        seriesColumn: this.seriesColumn,
        xRange: config.xRange,
        yRange: config.yRange,
        maxPoints: config.maxPoints,
      },
      queryFn: async () => {
        const xCol = this.xColumn;
        const yCol = this.yColumn;

        const filterClauses: string[] = [];
        if (config.xRange !== undefined) {
          filterClauses.push(sqlRangeClause(xCol, config.xRange));
        }
        if (config.yRange !== undefined) {
          filterClauses.push(sqlRangeClause(yCol, config.yRange));
        }
        const whereClause =
          filterClauses.length > 0
            ? `WHERE ${filterClauses.join(' AND ')}`
            : '';

        const sizeExpr =
          this.sizeColumn !== undefined
            ? `CAST(${this.sizeColumn} AS REAL)`
            : 'NULL';
        const labelExpr =
          this.labelColumn !== undefined
            ? `CAST(${this.labelColumn} AS TEXT)`
            : 'NULL';
        const seriesExpr =
          this.seriesColumn !== undefined
            ? `CAST(${this.seriesColumn} AS TEXT)`
            : 'NULL';

        // Build the inner query with optional per-series stride sampling.
        // When maxPoints is set, window functions stride-sample each series
        // down to at most maxPoints rows in SQL rather than loading all rows
        // into JS and discarding most of them.
        const maxPoints = config.maxPoints;
        const orderBy =
          this.seriesColumn !== undefined ? 'ORDER BY _series' : '';

        let sql: string;
        if (maxPoints !== undefined) {
          sql = `
            SELECT _x, _y, _size, _label, _series
            FROM (
              SELECT
                CAST(${xCol} AS REAL) AS _x,
                CAST(${yCol} AS REAL) AS _y,
                ${sizeExpr} AS _size,
                ${labelExpr} AS _label,
                ${seriesExpr} AS _series,
                ROW_NUMBER() OVER (PARTITION BY ${seriesExpr}) AS _rn,
                COUNT(*) OVER (PARTITION BY ${seriesExpr}) AS _cnt
              FROM (${this.baseQuery})
              ${whereClause}
            )
            WHERE _rn % MAX(1, (_cnt + ${maxPoints} - 1) / ${maxPoints}) = 0
            ${orderBy}
          `;
        } else {
          sql = `
            SELECT
              CAST(${xCol} AS REAL) AS _x,
              CAST(${yCol} AS REAL) AS _y,
              ${sizeExpr} AS _size,
              ${labelExpr} AS _label,
              ${seriesExpr} AS _series
            FROM (${this.baseQuery})
            ${whereClause}
            ${orderBy}
          `;
        }

        const queryResult = await this.engine.query(sql);

        // Group points by series
        const seriesMap = new Map<string, ScatterChartPoint[]>();
        const defaultName = this.seriesColumn !== undefined ? '' : 'Points';

        const iter = queryResult.iter({
          _x: NUM,
          _y: NUM,
          _size: NUM_NULL,
          _label: STR_NULL,
          _series: STR_NULL,
        });

        for (; iter.valid(); iter.next()) {
          const name = iter._series ?? defaultName;
          let points = seriesMap.get(name);
          if (points === undefined) {
            points = [];
            seriesMap.set(name, points);
          }
          const point: ScatterChartPoint = {
            x: iter._x,
            y: iter._y,
            ...(iter._size !== null && {size: iter._size}),
            ...(iter._label !== null && {label: iter._label}),
          };
          points.push(point);
        }

        const series: ScatterChartSeries[] = [];
        for (const [name, points] of seriesMap) {
          series.push({name, points});
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
