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

import {QuerySlot, SerialTaskQueue} from '../../../../base/query_slot';
import {Engine} from '../../../../trace_processor/engine';
import {NUM, STR} from '../../../../trace_processor/query_result';
import {Filter} from '../../../../components/widgets/datagrid/model';
import {CDFData, CDFLine, CDFPoint} from './cdf';
import {isValidNumber, toNumber} from '../chart_utils';
import {InMemoryFilterEngine, RawRow} from '../filter_utils';

/**
 * Configuration for CDF loaders.
 */
export interface CDFLoaderConfig {
  /**
   * DataGrid-style filters. The loader will extract relevant filters
   * for its value column (using >= and <= operators).
   */
  readonly filters?: readonly Filter[];

  /**
   * Number of points to compute for the CDF.
   * Defaults to 100.
   */
  readonly points?: number;
}

/**
 * Result returned by CDF loaders.
 */
export interface CDFLoaderResult {
  /**
   * The computed CDF data, or undefined if loading.
   */
  readonly data: CDFData | undefined;

  /**
   * Whether a query is currently pending.
   */
  readonly isPending: boolean;
}

/**
 * Loader interface for CDF data.
 */
export interface CDFLoader {
  use(config: CDFLoaderConfig): CDFLoaderResult;
  dispose(): void;
}

/**
 * Compute CDF from sorted values using CUME_DIST logic.
 */
function computeCDFPoints(values: number[]): CDFPoint[] {
  if (values.length === 0) return [];

  const sorted = [...values].sort((a, b) => a - b);
  const n = sorted.length;

  if (sorted[0] === sorted[n - 1]) {
    return [{value: sorted[0], probability: 1.0}];
  }

  // Generate 100 percentile targets (0.01 to 1.00)
  const percentiles: number[] = [];
  for (let p = 0.01; p <= 1.0; p += 0.01) {
    percentiles.push(Math.round(p * 100) / 100);
  }

  // For each percentile, find minimum value where CUME_DIST >= percentile
  const points: CDFPoint[] = [];
  for (const p of percentiles) {
    const targetIdx = Math.ceil(p * n) - 1;
    points.push({
      value: sorted[targetIdx],
      probability: p,
    });
  }

  return points;
}

/**
 * In-memory CDF loader for static datasets.
 *
 * Takes raw values and computes CDF synchronously.
 * Supports grouping to create multiple CDF lines.
 *
 * Usage:
 * ```typescript
 * class MyComponent {
 *   private loader = new InMemoryCDFLoader({
 *     values: myValues,
 *     groupBy: myGroups, // Optional
 *   });
 *
 *   view() {
 *     const {data} = this.loader.use({points: 100});
 *     return m(CDFChart, {data});
 *   }
 * }
 * ```
 */
export class InMemoryCDFLoader implements CDFLoader {
  private readonly rawData: readonly RawRow[];
  private readonly valueCol: string;
  private readonly groupCol?: string;

  constructor(opts: {
    readonly data: readonly RawRow[];
    readonly valueCol: string;
    readonly groupCol?: string;
  }) {
    this.rawData = opts.data;
    this.valueCol = opts.valueCol;
    this.groupCol = opts.groupCol;
  }

  use(config: CDFLoaderConfig): CDFLoaderResult {
    // Apply ALL filters to enable cross-filtering
    const filteredData = InMemoryFilterEngine.apply(
      this.rawData,
      config.filters ?? [],
    );

    // Extract values and groups from filtered data
    const values: number[] = [];
    const groupBy: string[] | undefined = this.groupCol ? [] : undefined;

    for (const row of filteredData) {
      const val = toNumber(row[this.valueCol]);
      if (val === undefined) continue;

      values.push(val);
      if (groupBy && this.groupCol) {
        groupBy.push(String(row[this.groupCol] ?? 'default'));
      }
    }

    // Group values
    const grouped = new Map<string, number[]>();
    if (groupBy) {
      values.forEach((v, i) => {
        const group = groupBy[i];
        if (!grouped.has(group)) {
          grouped.set(group, []);
        }
        grouped.get(group)!.push(v);
      });
    } else {
      grouped.set('default', values);
    }

    // Compute CDF for each group
    const lines: CDFLine[] = [];
    let globalMin = Infinity;
    let globalMax = -Infinity;

    for (const [groupName, groupValues] of grouped) {
      const points = computeCDFPoints(groupValues);
      if (points.length > 0) {
        lines.push({
          name: groupName === 'default' && !groupBy ? 'CDF' : groupName,
          points,
        });
        globalMin = Math.min(globalMin, points[0].value);
        globalMax = Math.max(globalMax, points[points.length - 1].value);
      }
    }

    const data: CDFData = {
      lines,
      min: globalMin === Infinity ? 0 : globalMin,
      max: globalMax === -Infinity ? 1 : globalMax,
    };

    return {data, isPending: false};
  }

  dispose(): void {
    // No-op
  }
}

/**
 * Configuration for SQLCDFLoader.
 */
export interface SQLCDFLoaderOpts {
  /**
   * The trace processor engine to run queries against.
   */
  readonly engine: Engine;

  /**
   * SQL query that returns numeric values for the CDF.
   */
  readonly query: string;

  /**
   * Column name for values.
   */
  readonly valueCol: string;

  /**
   * Optional column for grouping (creates multiple CDF lines).
   */
  readonly groupCol?: string;
}

/**
 * SQL-based CDF loader with async loading and caching.
 *
 * Computes CDF directly in SQL for efficiency.
 *
 * Usage:
 * ```typescript
 * class SQLCDFPanel {
 *   private loader: SQLCDFLoader;
 *   private rangeFilter?: {min: number; max: number};
 *
 *   constructor(engine: Engine) {
 *     this.loader = new SQLCDFLoader({
 *       engine,
 *       query: 'SELECT dur FROM slice WHERE dur > 0',
 *       valueCol: 'dur',
 *     });
 *   }
 *
 *   view() {
 *     const {data, isPending} = this.loader.use({
 *       filter: this.rangeFilter,
 *       points: 100,
 *     });
 *     return m(CDFChart, {
 *       data,
 *       onBrush: (range) => {
 *         this.rangeFilter = {min: range.start, max: range.end};
 *       },
 *     });
 *   }
 *
 *   onremove() {
 *     this.loader.dispose();
 *   }
 * }
 * ```
 */
export class SQLCDFLoader implements CDFLoader {
  private readonly engine: Engine;
  private readonly baseQuery: string;
  private readonly valueCol: string;
  private readonly groupCol?: string;
  private readonly taskQueue = new SerialTaskQueue();
  private readonly querySlot = new QuerySlot<CDFData>(this.taskQueue);

  constructor(opts: SQLCDFLoaderOpts) {
    this.engine = opts.engine;
    this.baseQuery = opts.query;
    this.valueCol = opts.valueCol;
    this.groupCol = opts.groupCol;
  }

  use(config: CDFLoaderConfig): CDFLoaderResult {
    const numPoints = config.points ?? 100;

    const result = this.querySlot.use({
      key: {
        baseQuery: this.baseQuery,
        valueCol: this.valueCol,
        groupCol: this.groupCol,
        filters: JSON.stringify(config.filters),
        points: numPoints,
      },
      queryFn: async () => {
        // Extract >= and <= filters for value column
        let filterClause = '';
        if ((config.filters?.length ?? 0) > 0) {
          let min = -Infinity;
          let max = Infinity;

          for (const filter of config.filters!) {
            if (filter.field === this.valueCol) {
              if (filter.op === '>=' && typeof filter.value === 'number') {
                min = Math.max(min, filter.value);
              } else if (
                filter.op === '<=' &&
                typeof filter.value === 'number'
              ) {
                max = Math.min(max, filter.value);
              }
            }
          }

          if (min !== -Infinity && max !== Infinity) {
            filterClause = `WHERE ${this.valueCol} >= ${min} AND ${this.valueCol} <= ${max}`;
          } else if (min !== -Infinity) {
            filterClause = `WHERE ${this.valueCol} >= ${min}`;
          } else if (max !== Infinity) {
            filterClause = `WHERE ${this.valueCol} <= ${max}`;
          }
        }

        // Query to get sorted values
        const sql = `
          SELECT ${this.valueCol} AS value ${this.groupCol ? `, ${this.groupCol} AS grp` : ''}
          FROM (${this.baseQuery})
          ${filterClause}
          ORDER BY ${this.groupCol ? `${this.groupCol}, ` : ''}${this.valueCol}
        `;

        const queryResult = await this.engine.query(sql);

        // Group values from SQL result
        const grouped = new Map<string, number[]>();
        const iter = this.groupCol
          ? queryResult.iter({value: NUM, grp: STR})
          : queryResult.iter({value: NUM});

        for (; iter.valid(); iter.next()) {
          const value = iter.value;
          if (!isValidNumber(value)) continue;

          const groupName =
            this.groupCol && 'grp' in iter
              ? (iter as {grp: string}).grp
              : 'default';

          if (!grouped.has(groupName)) {
            grouped.set(groupName, []);
          }
          grouped.get(groupName)!.push(value);
        }

        // Compute CDF for each group using same logic as in-memory
        const lines: CDFLine[] = [];
        let globalMin = Infinity;
        let globalMax = -Infinity;

        for (const [groupName, values] of grouped) {
          const points = computeCDFPoints(values);
          if (points.length > 0) {
            lines.push({
              name:
                groupName === 'default' && !this.groupCol ? 'CDF' : groupName,
              points,
            });
            globalMin = Math.min(globalMin, points[0].value);
            globalMax = Math.max(globalMax, points[points.length - 1].value);
          }
        }

        return {
          lines,
          min: globalMin === Infinity ? 0 : globalMin,
          max: globalMax === -Infinity ? 1 : globalMax,
        };
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
