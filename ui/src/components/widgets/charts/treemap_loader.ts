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
  ChartSource,
  SQLChartLoader,
  QueryConfig,
  ChartLoaderResult,
  inFilter,
} from './chart_sql_source';
import {ChartAggregation} from './chart_utils';
import {TreemapData, TreemapNode} from './treemap';

/**
 * Configuration for SQLTreemapLoader.
 */
export interface SQLTreemapLoaderOpts {
  /** The trace processor engine to run queries against. */
  readonly engine: Engine;

  /**
   * SQL query that provides the raw data.
   * Must include label and size columns (and optionally group column).
   */
  readonly query: string;

  /** Column name for leaf node labels. */
  readonly labelColumn: string;

  /** Column name for rectangle size (measure). */
  readonly sizeColumn: string;

  /**
   * Optional column for parent grouping (creates 2-level hierarchy).
   * When omitted, all nodes are at the top level.
   */
  readonly groupColumn?: string;
}

/**
 * Per-use configuration for the treemap loader.
 */
export interface TreemapLoaderConfig {
  /** Aggregation function to apply to the size column. Defaults to 'SUM'. */
  readonly aggregation?: ChartAggregation;

  /**
   * Maximum number of leaf nodes to return per group.
   * Nodes are sorted by size descending. Defaults to no limit.
   */
  readonly limit?: number;

  /** Filter to only include specific labels. */
  readonly labelFilter?: ReadonlyArray<string | number>;

  /** Filter to only include specific groups. */
  readonly groupFilter?: ReadonlyArray<string | number>;
}

/** Result returned by the treemap loader. */
export type TreemapLoaderResult = ChartLoaderResult<TreemapData>;

/**
 * SQL-based treemap loader with async loading and caching.
 *
 * Creates 1 or 2 level hierarchy from SQL data. When groupColumn is
 * provided, creates parent nodes for each group with children for labels.
 * Uses QuerySlot for caching and request deduplication.
 */
export class SQLTreemapLoader extends SQLChartLoader<
  TreemapLoaderConfig,
  TreemapData
> {
  private readonly labelColumn: string;
  private readonly sizeColumn: string;
  private readonly groupColumn: string | undefined;

  constructor(opts: SQLTreemapLoaderOpts) {
    const schema: Record<string, 'text' | 'real'> = {
      [opts.labelColumn]: 'text',
      [opts.sizeColumn]: 'real',
    };
    if (opts.groupColumn !== undefined) {
      schema[opts.groupColumn] = 'text';
    }
    super(opts.engine, new ChartSource({query: opts.query, schema}));
    this.labelColumn = opts.labelColumn;
    this.sizeColumn = opts.sizeColumn;
    this.groupColumn = opts.groupColumn;
  }

  protected buildQueryConfig(config: TreemapLoaderConfig): QueryConfig {
    const aggregation = config.aggregation ?? 'SUM';
    const dimensions =
      this.groupColumn !== undefined
        ? [
            {column: this.groupColumn, alias: '_group'},
            {column: this.labelColumn, alias: '_label'},
          ]
        : [{column: this.labelColumn, alias: '_label'}];

    const filters = [
      ...inFilter(this.labelColumn, config.labelFilter),
      ...(this.groupColumn !== undefined
        ? inFilter(this.groupColumn, config.groupFilter)
        : []),
    ];

    return {
      type: 'aggregated',
      dimensions,
      measures: [{column: this.sizeColumn, aggregation}],
      filters,
      limitPerGroup: this.groupColumn !== undefined ? config.limit : undefined,
      limit: this.groupColumn === undefined ? config.limit : undefined,
      orderDirection: 'desc',
    };
  }

  protected parseResult(queryResult: QueryResult): TreemapData {
    if (this.groupColumn !== undefined) {
      return this.parseGrouped(queryResult);
    }
    return this.parseFlat(queryResult);
  }

  private parseGrouped(queryResult: QueryResult): TreemapData {
    const groupMap = new Map<string, TreemapNode[]>();
    const iter = queryResult.iter({
      _group: STR_NULL,
      _label: STR_NULL,
      _value: NUM,
    });

    for (; iter.valid(); iter.next()) {
      const groupName = iter._group ?? '(uncategorized)';
      const labelName = iter._label ?? '(null)';

      let children = groupMap.get(groupName);
      if (children === undefined) {
        children = [];
        groupMap.set(groupName, children);
      }
      children.push({name: labelName, value: iter._value, category: groupName});
    }

    const nodes: TreemapNode[] = [];
    for (const [groupName, children] of groupMap) {
      nodes.push({
        name: groupName,
        value: children.reduce((sum, c) => sum + c.value, 0),
        category: groupName,
        children,
      });
    }
    return {nodes};
  }

  private parseFlat(queryResult: QueryResult): TreemapData {
    const nodes: TreemapNode[] = [];
    const iter = queryResult.iter({_label: STR_NULL, _value: NUM});

    for (; iter.valid(); iter.next()) {
      nodes.push({name: iter._label ?? '(null)', value: iter._value});
    }
    return {nodes};
  }
}
