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
import {createChartLoader, ChartLoader, inFilter} from './chart_sql_source';
import {AggregateFunction} from '../datagrid/model';
import {TreemapData, TreemapNode} from './treemap';
import type {QueryResult as SlotResult} from '../../../base/query_slot';

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
  readonly aggregation?: AggregateFunction;

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
export type TreemapLoaderResult = SlotResult<TreemapData>;

/**
 * SQL-based treemap loader with async loading and caching.
 *
 * Creates 1 or 2 level hierarchy from SQL data. When groupColumn is
 * provided, creates parent nodes for each group with children for labels.
 */
export class SQLTreemapLoader {
  private readonly loader: ChartLoader<TreemapLoaderConfig, TreemapData>;

  constructor(opts: SQLTreemapLoaderOpts) {
    const labelCol = opts.labelColumn;
    const sizeCol = opts.sizeColumn;
    const groupCol = opts.groupColumn;

    const schema: Record<string, 'text' | 'real'> = {
      [labelCol]: 'text',
      [sizeCol]: 'real',
    };
    if (groupCol !== undefined) {
      schema[groupCol] = 'text';
    }

    this.loader = createChartLoader({
      engine: opts.engine,
      query: opts.query,
      schema,
      buildQueryConfig: (config) => {
        const aggregation = config.aggregation ?? 'SUM';
        const dimensions =
          groupCol !== undefined
            ? [
                {column: groupCol, alias: '_group'},
                {column: labelCol, alias: '_label'},
              ]
            : [{column: labelCol, alias: '_label'}];

        const filters = [
          ...inFilter(labelCol, config.labelFilter),
          ...(groupCol !== undefined
            ? inFilter(groupCol, config.groupFilter)
            : []),
        ];

        return {
          type: 'aggregated',
          dimensions,
          measures: [{column: sizeCol, aggregation}],
          filters,
          limitPerGroup: groupCol !== undefined ? config.limit : undefined,
          limit: groupCol === undefined ? config.limit : undefined,
          orderDirection: 'desc',
        };
      },
      parseResult: (queryResult: QueryResult) => {
        if (groupCol !== undefined) {
          return parseGrouped(queryResult);
        }
        return parseFlat(queryResult);
      },
    });
  }

  use(config: TreemapLoaderConfig): TreemapLoaderResult {
    return this.loader.use(config);
  }

  dispose(): void {
    this.loader.dispose();
  }
}

function parseGrouped(queryResult: QueryResult): TreemapData {
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

function parseFlat(queryResult: QueryResult): TreemapData {
  const nodes: TreemapNode[] = [];
  const iter = queryResult.iter({_label: STR_NULL, _value: NUM});

  for (; iter.valid(); iter.next()) {
    nodes.push({name: iter._label ?? '(null)', value: iter._value});
  }
  return {nodes};
}
