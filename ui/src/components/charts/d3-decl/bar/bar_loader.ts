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
import {BarDatum, SimpleBarData, GroupedBarData, SortConfig} from './bar_types';
import {isValidNumber, toNumber} from '../chart_utils';
import {InMemoryFilterEngine} from '../filter_utils';

/**
 * Union type for bar chart data (simple or grouped).
 */
export type BarData = SimpleBarData | GroupedBarData;

/**
 * Row data type for bar chart input.
 */
export type RawRow = Record<
  string,
  string | number | boolean | null | undefined
>;

/**
 * Configuration for bar chart loaders.
 */
export interface BarLoaderConfig {
  /**
   * Whether to render as stacked bars (only for grouped data).
   */
  readonly stacked?: boolean;

  /**
   * Sort configuration.
   */
  readonly sort?: SortConfig;

  /**
   * DataGrid-style filters. The loader will extract relevant filters
   * for its category column (using 'in' operator).
   */
  readonly filters?: readonly Filter[];
}

/**
 * Result returned by bar chart loaders.
 */
export interface BarLoaderResult {
  /**
   * The computed bar chart data, or undefined if loading.
   */
  readonly data: BarData | undefined;

  /**
   * Whether a query is currently pending.
   */
  readonly isPending: boolean;
}

/**
 * Loader interface for bar chart data.
 */
export interface BarLoader {
  use(config: BarLoaderConfig): BarLoaderResult;
  dispose(): void;
}

/**
 * In-memory bar chart loader for static datasets.
 *
 * Takes raw row data and column specifications, then computes bar data synchronously.
 * Caches the result when config hasn't changed.
 *
 * Usage:
 * ```typescript
 * class MyComponent {
 *   private loader = new InMemoryBarLoader({
 *     data: myRawData,
 *     categoryCol: 'name',
 *     valueCol: 'count',
 *   });
 *
 *   view() {
 *     const {data} = this.loader.use({stacked: false});
 *     return m(BarChart, {data});
 *   }
 * }
 * ```
 */
export class InMemoryBarLoader implements BarLoader {
  private readonly rawData: readonly RawRow[];
  private readonly categoryCol: string;
  private readonly valueCol: string;
  private readonly groupCol?: string;

  constructor(opts: {
    readonly data: readonly RawRow[];
    readonly categoryCol: string;
    readonly valueCol: string;
    readonly groupCol?: string;
  }) {
    this.rawData = opts.data;
    this.categoryCol = opts.categoryCol;
    this.valueCol = opts.valueCol;
    this.groupCol = opts.groupCol;
  }

  use(config: BarLoaderConfig): BarLoaderResult {
    // Apply ALL filters using InMemoryFilterEngine
    const filteredData = InMemoryFilterEngine.apply(
      this.rawData,
      config.filters ?? [],
    );

    // Aggregate by category (and optionally group)
    const aggregated = new Map<string, number>();

    for (const row of filteredData) {
      const category = String(row[this.categoryCol]);
      const value = toNumber(row[this.valueCol]);
      if (value === undefined) continue;

      const key = this.groupCol
        ? `${category}:${String(row[this.groupCol])}`
        : category;

      aggregated.set(key, (aggregated.get(key) ?? 0) + value);
    }

    // Convert to BarDatum
    const bars: BarDatum[] = [];
    const groupSet = new Set<string>();

    for (const [key, value] of aggregated.entries()) {
      if (this.groupCol) {
        const [category, group] = key.split(':');
        bars.push({category, value, group});
        groupSet.add(group);
      } else {
        bars.push({category: key, value, group: undefined});
      }
    }

    // Return appropriate type based on whether we have groups
    const data: BarData = this.groupCol
      ? {bars, groups: Array.from(groupSet)}
      : {bars};

    return {data, isPending: false};
  }

  dispose(): void {
    // No-op
  }
}

/**
 * Configuration for SQLBarLoader.
 */
export interface SQLBarLoaderOpts {
  /**
   * The trace processor engine to run queries against.
   */
  readonly engine: Engine;

  /**
   * SQL query that returns rows for the bar chart.
   * Should include categoryCol, valueCol, and optionally groupCol in output.
   */
  readonly query: string;

  /**
   * Column name for categories (X-axis).
   */
  readonly categoryCol: string;

  /**
   * Column name for values (Y-axis).
   */
  readonly valueCol: string;

  /**
   * Optional column name for grouping (for grouped/stacked bars).
   */
  readonly groupCol?: string;

  /**
   * Optional aggregation function to apply (default: SUM).
   * Only used if the query returns multiple rows per category.
   */
  readonly aggregation?: 'SUM' | 'AVG' | 'COUNT' | 'MIN' | 'MAX';
}

/**
 * SQL-based bar chart loader with async loading and caching.
 *
 * Executes SQL queries and transforms results into bar chart data.
 * Uses QuerySlot internally for caching and request deduplication.
 *
 * Usage:
 * ```typescript
 * class SQLBarChartPanel {
 *   private loader: SQLBarLoader;
 *   private selectedCategories?: string[];
 *
 *   constructor(engine: Engine) {
 *     this.loader = new SQLBarLoader({
 *       engine,
 *       query: 'SELECT name, count FROM slice_stats',
 *       categoryCol: 'name',
 *       valueCol: 'count',
 *     });
 *   }
 *
 *   view() {
 *     const {data, isPending} = this.loader.use({
 *       filter: this.selectedCategories
 *         ? {categories: this.selectedCategories}
 *         : undefined,
 *     });
 *     return m(BarChart, {
 *       data,
 *       onSelect: (sel) => {
 *         this.selectedCategories = [sel.category];
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
export class SQLBarLoader implements BarLoader {
  private readonly engine: Engine;
  private readonly baseQuery: string;
  private readonly categoryCol: string;
  private readonly valueCol: string;
  private readonly groupCol?: string;
  private readonly aggregation: string;
  private readonly taskQueue = new SerialTaskQueue();
  private readonly querySlot = new QuerySlot<BarData>(this.taskQueue);

  constructor(opts: SQLBarLoaderOpts) {
    this.engine = opts.engine;
    this.baseQuery = opts.query;
    this.categoryCol = opts.categoryCol;
    this.valueCol = opts.valueCol;
    this.groupCol = opts.groupCol;
    this.aggregation = opts.aggregation || 'SUM';
  }

  use(config: BarLoaderConfig): BarLoaderResult {
    const result = this.querySlot.use({
      key: {
        baseQuery: this.baseQuery,
        categoryCol: this.categoryCol,
        valueCol: this.valueCol,
        groupCol: this.groupCol,
        filters: JSON.stringify(config.filters),
        aggregation: this.aggregation,
      },
      queryFn: async () => {
        // Extract 'in' filters for category column
        let filterClause = '';
        if (config.filters && config.filters.length > 0) {
          const inFilters = config.filters.filter(
            (f) => f.field === this.categoryCol && f.op === 'in',
          );

          if (inFilters.length > 0) {
            const categories = new Set<string>();
            for (const filter of inFilters) {
              if (filter.op === 'in' && Array.isArray(filter.value)) {
                filter.value.forEach((v: unknown) => categories.add(String(v)));
              }
            }

            if (categories.size > 0) {
              const categoryList = Array.from(categories)
                .map((c) => `'${c.replace(/'/g, "''")}'`)
                .join(', ');
              filterClause = `WHERE ${this.categoryCol} IN (${categoryList})`;
            }
          }
        }

        // Build aggregation query
        const groupByClause = this.groupCol
          ? `${this.categoryCol}, ${this.groupCol}`
          : this.categoryCol;

        const sql = `
          SELECT
            ${this.categoryCol} AS category,
            ${this.groupCol ? `${this.groupCol} AS grp,` : ''}
            ${this.aggregation}(${this.valueCol}) AS value
          FROM (${this.baseQuery})
          ${filterClause}
          GROUP BY ${groupByClause}
          ORDER BY ${this.categoryCol}
        `;

        const queryResult = await this.engine.query(sql);

        const bars: BarDatum[] = [];
        const groupSet = new Set<string>();

        const iter = this.groupCol
          ? queryResult.iter({
              category: STR,
              grp: STR,
              value: NUM,
            })
          : queryResult.iter({
              category: STR,
              value: NUM,
            });

        for (; iter.valid(); iter.next()) {
          const category = iter.category;
          const value = iter.value;

          if (!isValidNumber(value)) continue;

          let bar: BarDatum;

          if (this.groupCol && 'grp' in iter) {
            const group = (iter as {grp: string}).grp;
            bar = {
              category,
              value,
              group,
            };
            groupSet.add(group);
          } else {
            bar = {
              category,
              value,
            };
          }

          bars.push(bar);
        }

        // Return appropriate type based on whether we have groups
        return this.groupCol ? {bars, groups: Array.from(groupSet)} : {bars};
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
