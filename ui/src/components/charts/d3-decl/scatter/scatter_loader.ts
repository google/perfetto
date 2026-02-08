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
import {ScatterData, ScatterPoint, CorrelationStats} from './scatter';
import {isValidNumber, toNumber, calculateCorrelation} from '../chart_utils';
import {InMemoryFilterEngine, RawRow} from '../filter_utils';

/**
 * Configuration for scatter plot loaders.
 */
export interface ScatterLoaderConfig {
  /**
   * DataGrid-style filters. The loader will extract relevant filters
   * for x and y columns (using >= and <= operators).
   */
  readonly filters?: Filter[];

  /**
   * Whether to compute correlation statistics.
   */
  readonly computeCorrelation?: boolean;
}

/**
 * Result returned by scatter loaders.
 */
export interface ScatterLoaderResult {
  readonly data: ScatterData | undefined;
  readonly isPending: boolean;
}

/**
 * Loader interface for scatter plot data.
 */
export interface ScatterLoader {
  use(config: ScatterLoaderConfig): ScatterLoaderResult;
  dispose(): void;
}

/**
 * In-memory scatter plot loader.
 */
export class InMemoryScatterLoader implements ScatterLoader {
  private readonly rawData: readonly RawRow[];
  private readonly xCol: string;
  private readonly yCol: string;
  private readonly labelCol?: string;
  private readonly categoryCol?: string;

  constructor(opts: {
    readonly data: readonly RawRow[];
    readonly xCol: string;
    readonly yCol: string;
    readonly labelCol?: string;
    readonly categoryCol?: string;
  }) {
    this.rawData = opts.data;
    this.xCol = opts.xCol;
    this.yCol = opts.yCol;
    this.labelCol = opts.labelCol;
    this.categoryCol = opts.categoryCol;
  }

  use(config: ScatterLoaderConfig): ScatterLoaderResult {
    // Apply ALL filters to enable cross-filtering
    const filteredData = InMemoryFilterEngine.apply(
      this.rawData,
      config.filters ?? [],
    );
    // Extract points from filtered data
    const points: ScatterPoint[] = [];
    let xMin = Infinity;
    let xMax = -Infinity;
    let yMin = Infinity;
    let yMax = -Infinity;
    const categorySet = new Set<string>();

    for (const row of filteredData) {
      const x = toNumber(row[this.xCol]);
      const y = toNumber(row[this.yCol]);
      if (x === undefined || y === undefined) continue;

      const label = this.labelCol ? String(row[this.labelCol]) : undefined;
      const category = this.categoryCol
        ? String(row[this.categoryCol])
        : undefined;

      points.push({
        x,
        y,
        label,
        category,
      });

      xMin = Math.min(xMin, x);
      xMax = Math.max(xMax, x);
      yMin = Math.min(yMin, y);
      yMax = Math.max(yMax, y);

      if (category) {
        categorySet.add(category);
      }
    }

    // Compute correlation if requested
    let correlation: CorrelationStats | undefined;
    if (config.computeCorrelation && points.length > 1) {
      const xValues = points.map((p) => p.x);
      const yValues = points.map((p) => p.y);
      correlation = calculateCorrelation(xValues, yValues);
    }

    const data: ScatterData = {
      points,
      xMin: xMin === Infinity ? 0 : xMin,
      xMax: xMax === -Infinity ? 1 : xMax,
      yMin: yMin === Infinity ? 0 : yMin,
      yMax: yMax === -Infinity ? 1 : yMax,
      correlation,
      categories: categorySet.size > 0 ? Array.from(categorySet) : undefined,
    };

    return {data, isPending: false};
  }

  dispose(): void {
    // No-op
  }
}

/**
 * Configuration for SQLScatterLoader.
 */
export interface SQLScatterLoaderOpts {
  readonly engine: Engine;
  readonly query: string;
  readonly xCol: string;
  readonly yCol: string;
  readonly labelCol?: string;
  readonly categoryCol?: string;
}

/**
 * SQL-based scatter plot loader.
 */
export class SQLScatterLoader implements ScatterLoader {
  private readonly engine: Engine;
  private readonly baseQuery: string;
  private readonly xCol: string;
  private readonly yCol: string;
  private readonly labelCol?: string;
  private readonly categoryCol?: string;
  private readonly taskQueue = new SerialTaskQueue();
  private readonly querySlot = new QuerySlot<ScatterData>(this.taskQueue);

  constructor(opts: SQLScatterLoaderOpts) {
    this.engine = opts.engine;
    this.baseQuery = opts.query;
    this.xCol = opts.xCol;
    this.yCol = opts.yCol;
    this.labelCol = opts.labelCol;
    this.categoryCol = opts.categoryCol;
  }

  use(config: ScatterLoaderConfig): ScatterLoaderResult {
    const result = this.querySlot.use({
      key: {
        baseQuery: this.baseQuery,
        xCol: this.xCol,
        yCol: this.yCol,
        labelCol: this.labelCol,
        categoryCol: this.categoryCol,
        filters: JSON.stringify(config.filters),
        computeCorrelation: config.computeCorrelation,
      },
      queryFn: async () => {
        // Extract range filters for x and y columns
        let filterClause = '';
        if (config.filters && config.filters.length > 0) {
          let xMin = -Infinity;
          let xMax = Infinity;
          let yMin = -Infinity;
          let yMax = Infinity;

          for (const filter of config.filters) {
            if (filter.field === this.xCol) {
              if (filter.op === '>=' && typeof filter.value === 'number') {
                xMin = Math.max(xMin, filter.value);
              } else if (
                filter.op === '<=' &&
                typeof filter.value === 'number'
              ) {
                xMax = Math.min(xMax, filter.value);
              }
            } else if (filter.field === this.yCol) {
              if (filter.op === '>=' && typeof filter.value === 'number') {
                yMin = Math.max(yMin, filter.value);
              } else if (
                filter.op === '<=' &&
                typeof filter.value === 'number'
              ) {
                yMax = Math.min(yMax, filter.value);
              }
            }
          }

          const conditions: string[] = [];
          if (xMin !== -Infinity) conditions.push(`${this.xCol} >= ${xMin}`);
          if (xMax !== Infinity) conditions.push(`${this.xCol} <= ${xMax}`);
          if (yMin !== -Infinity) conditions.push(`${this.yCol} >= ${yMin}`);
          if (yMax !== Infinity) conditions.push(`${this.yCol} <= ${yMax}`);

          if (conditions.length > 0) {
            filterClause = `WHERE ${conditions.join(' AND ')}`;
          }
        }

        const selectCols = [
          `${this.xCol} AS x`,
          `${this.yCol} AS y`,
          this.labelCol ? `${this.labelCol} AS label` : null,
          this.categoryCol ? `${this.categoryCol} AS category` : null,
        ]
          .filter((c) => c !== null)
          .join(', ');

        const sql = `
          SELECT ${selectCols}
          FROM (${this.baseQuery})
          ${filterClause}
        `;

        const queryResult = await this.engine.query(sql);

        const points: ScatterPoint[] = [];
        let xMin = Infinity;
        let xMax = -Infinity;
        let yMin = Infinity;
        let yMax = -Infinity;
        const categorySet = new Set<string>();

        const iterConfig: {
          x: typeof NUM;
          y: typeof NUM;
          label?: typeof STR;
          category?: typeof STR;
        } = {
          x: NUM,
          y: NUM,
        };
        if (this.labelCol) iterConfig.label = STR;
        if (this.categoryCol) iterConfig.category = STR;

        const iter = queryResult.iter(iterConfig);

        for (; iter.valid(); iter.next()) {
          const x = iter.x;
          const y = iter.y;

          if (!isValidNumber(x) || !isValidNumber(y)) continue;

          let label: string | undefined;
          let category: string | undefined;

          if (this.labelCol && 'label' in iter) {
            label = (iter as {label: string}).label;
          }
          if (this.categoryCol && 'category' in iter) {
            category = (iter as {category: string}).category;
            categorySet.add(category);
          }

          const point: ScatterPoint = {x, y, label, category};
          points.push(point);

          xMin = Math.min(xMin, x);
          xMax = Math.max(xMax, x);
          yMin = Math.min(yMin, y);
          yMax = Math.max(yMax, y);
        }

        // Compute correlation if requested
        let correlation: CorrelationStats | undefined;
        if (config.computeCorrelation && points.length > 1) {
          const xValues = points.map((p) => p.x);
          const yValues = points.map((p) => p.y);
          correlation = calculateCorrelation(xValues, yValues);
        }

        return {
          points,
          xMin: xMin === Infinity ? 0 : xMin,
          xMax: xMax === -Infinity ? 1 : xMax,
          yMin: yMin === Infinity ? 0 : yMin,
          yMax: yMax === -Infinity ? 1 : yMax,
          correlation,
          categories:
            categorySet.size > 0 ? Array.from(categorySet) : undefined,
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
