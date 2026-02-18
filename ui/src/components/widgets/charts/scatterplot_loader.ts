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
  NUM_NULL,
  STR_NULL,
  QueryResult,
} from '../../../trace_processor/query_result';
import {
  createChartLoader,
  ChartLoader,
  PointColumnSpec,
  rangeFilters,
} from './chart_sql_source';
import {
  ScatterChartData,
  ScatterChartPoint,
  ScatterChartSeries,
} from './scatterplot';
import type {QueryResult as SlotResult} from '../../../base/query_slot';

/**
 * Configuration for SQLScatterChartLoader.
 */
export interface SQLScatterChartLoaderOpts {
  /** The trace processor engine to run queries against. */
  readonly engine: Engine;

  /**
   * SQL query that provides the raw data.
   * Must include x and y columns (and optionally size, label, series columns).
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
  /** Filter to only include points within this X range. */
  readonly xRange?: {readonly min: number; readonly max: number};

  /** Filter to only include points within this Y range. */
  readonly yRange?: {readonly min: number; readonly max: number};

  /**
   * Maximum number of points per series. When exceeded, stride-based
   * sampling is applied. Defaults to no limit.
   */
  readonly maxPoints?: number;
}

/** Result returned by the scatter chart loader. */
export type ScatterChartLoaderResult = SlotResult<ScatterChartData>;

/**
 * SQL-based scatter chart loader with async loading and caching.
 *
 * Fetches (x, y) points with optional size and series grouping from SQL.
 */
export class SQLScatterChartLoader {
  private readonly loader: ChartLoader<
    ScatterChartLoaderConfig,
    ScatterChartData
  >;

  constructor(opts: SQLScatterChartLoaderOpts) {
    const xCol = opts.xColumn;
    const yCol = opts.yColumn;
    const sizeCol = opts.sizeColumn;
    const labelCol = opts.labelColumn;
    const seriesCol = opts.seriesColumn;

    const schema: Record<string, 'text' | 'real'> = {
      [xCol]: 'real',
      [yCol]: 'real',
    };
    if (sizeCol !== undefined) schema[sizeCol] = 'real';
    if (labelCol !== undefined) schema[labelCol] = 'text';
    if (seriesCol !== undefined) schema[seriesCol] = 'text';

    this.loader = createChartLoader({
      engine: opts.engine,
      query: opts.query,
      schema,
      buildQueryConfig: (config) => {
        // Always include _size and _label columns (NULL when not configured)
        // so that parseResult can use a single iter spec.
        const columns: PointColumnSpec[] = [
          {column: xCol, alias: '_x', cast: 'real'},
          {column: yCol, alias: '_y', cast: 'real'},
          sizeCol !== undefined
            ? {column: sizeCol, alias: '_size', cast: 'real'}
            : {alias: '_size', cast: 'real'},
          labelCol !== undefined
            ? {column: labelCol, alias: '_label', cast: 'text'}
            : {alias: '_label', cast: 'text'},
        ];
        // When no breakdown column, add NULL AS _series so parseResult's
        // iter spec always finds the column.
        if (seriesCol === undefined) {
          columns.push({alias: '_series', cast: 'text'});
        }
        return {
          type: 'points',
          columns,
          breakdown: seriesCol,
          filters: rangeFilters(xCol, config.xRange).concat(
            rangeFilters(yCol, config.yRange),
          ),
          orderBy: seriesCol !== undefined ? [{column: '_series'}] : undefined,
          maxPointsPerSeries: config.maxPoints,
        };
      },
      parseResult: (queryResult: QueryResult) => {
        const seriesMap = new Map<string, ScatterChartPoint[]>();
        const defaultName = seriesCol !== undefined ? '' : 'Points';

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
  }

  use(config: ScatterChartLoaderConfig): ScatterChartLoaderResult {
    return this.loader.use(config);
  }

  dispose(): void {
    this.loader.dispose();
  }
}
