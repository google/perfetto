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
import {
  AggregationType,
  sqlAggExpression,
  sqlInClause,
  validateColumnName,
} from './chart_utils';
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
  readonly aggregation?: AggregationType;

  /**
   * Maximum number of leaf nodes to return per group.
   * Nodes are sorted by size descending. Defaults to no limit.
   */
  readonly limit?: number;

  /**
   * Filter to only include specific labels.
   */
  readonly labelFilter?: ReadonlyArray<string | number>;

  /**
   * Filter to only include specific groups.
   */
  readonly groupFilter?: ReadonlyArray<string | number>;
}

/**
 * Result returned by the treemap loader.
 */
export interface TreemapLoaderResult {
  /** The computed treemap data, or undefined if loading. */
  readonly data: TreemapData | undefined;

  /** Whether a query is currently pending. */
  readonly isPending: boolean;
}

/**
 * SQL-based treemap loader with async loading and caching.
 *
 * Creates 1 or 2 level hierarchy from SQL data. When groupColumn is
 * provided, creates parent nodes for each group with children for labels.
 * Uses QuerySlot for caching and request deduplication.
 */
export class SQLTreemapLoader {
  private readonly engine: Engine;
  private readonly baseQuery: string;
  private readonly labelColumn: string;
  private readonly sizeColumn: string;
  private readonly groupColumn: string | undefined;
  private readonly taskQueue = new SerialTaskQueue();
  private readonly querySlot = new QuerySlot<TreemapData>(this.taskQueue);

  constructor(opts: SQLTreemapLoaderOpts) {
    validateColumnName(opts.labelColumn);
    validateColumnName(opts.sizeColumn);
    if (opts.groupColumn !== undefined) validateColumnName(opts.groupColumn);
    this.engine = opts.engine;
    this.baseQuery = opts.query;
    this.labelColumn = opts.labelColumn;
    this.sizeColumn = opts.sizeColumn;
    this.groupColumn = opts.groupColumn;
  }

  use(config: TreemapLoaderConfig): TreemapLoaderResult {
    const aggregation = config.aggregation ?? 'SUM';

    const result = this.querySlot.use({
      key: {
        baseQuery: this.baseQuery,
        labelColumn: this.labelColumn,
        sizeColumn: this.sizeColumn,
        groupColumn: this.groupColumn,
        aggregation,
        limit: config.limit,
        labelFilter: config.labelFilter,
        groupFilter: config.groupFilter,
      },
      queryFn: async () => {
        const label = this.labelColumn;
        const size = this.sizeColumn;
        const aggExpr = sqlAggExpression(size, aggregation);

        const filterClauses: string[] = [];
        if (config.labelFilter !== undefined) {
          const clause = sqlInClause(label, config.labelFilter);
          if (clause !== '') filterClauses.push(clause);
        }
        if (
          config.groupFilter !== undefined &&
          this.groupColumn !== undefined
        ) {
          const clause = sqlInClause(this.groupColumn, config.groupFilter);
          if (clause !== '') filterClauses.push(clause);
        }
        const whereClause =
          filterClauses.length > 0
            ? `WHERE ${filterClauses.join(' AND ')}`
            : '';

        let sql: string;

        if (this.groupColumn !== undefined) {
          // Two-level hierarchy: group -> label
          const group = this.groupColumn;

          // Use window function to rank within each group
          sql = `
            WITH _agg AS (
              SELECT
                CAST(${group} AS TEXT) AS _group,
                CAST(${label} AS TEXT) AS _label,
                ${aggExpr} AS _value
              FROM (${this.baseQuery})
              ${whereClause}
              GROUP BY ${group}, ${label}
            ),
            _ranked AS (
              SELECT
                _group,
                _label,
                _value,
                ROW_NUMBER() OVER (PARTITION BY _group ORDER BY _value DESC) AS _rank
              FROM _agg
            )
            SELECT _group, _label, _value
            FROM _ranked
            ${config.limit !== undefined ? `WHERE _rank <= ${config.limit}` : ''}
            ORDER BY _group, _value DESC
          `;
        } else {
          // Single-level: just labels
          const limitClause =
            config.limit !== undefined ? `LIMIT ${config.limit}` : '';

          sql = `
            SELECT
              NULL AS _group,
              CAST(${label} AS TEXT) AS _label,
              ${aggExpr} AS _value
            FROM (${this.baseQuery})
            ${whereClause}
            GROUP BY ${label}
            ORDER BY _value DESC
            ${limitClause}
          `;
        }

        const queryResult = await this.engine.query(sql);

        // Build hierarchical structure
        const groupMap = new Map<string | null, TreemapNode[]>();

        const iter = queryResult.iter({
          _group: STR_NULL,
          _label: STR_NULL,
          _value: NUM,
        });

        for (; iter.valid(); iter.next()) {
          const groupName = iter._group;
          const labelName = iter._label ?? '(null)';
          const value = iter._value;

          let children = groupMap.get(groupName);
          if (children === undefined) {
            children = [];
            groupMap.set(groupName, children);
          }

          children.push({
            name: labelName,
            value,
            category: groupName ?? undefined,
          });
        }

        // Convert to nodes
        const nodes: TreemapNode[] = [];

        if (this.groupColumn !== undefined) {
          // Two-level: create parent nodes for each group
          for (const [groupName, children] of groupMap) {
            const name = groupName ?? '(uncategorized)';
            nodes.push({
              name,
              value: children.reduce((sum, c) => sum + c.value, 0),
              category: name,
              children,
            });
          }
        } else {
          // Single-level: flat list of nodes
          const flatNodes = groupMap.get(null);
          if (flatNodes !== undefined) {
            nodes.push(...flatNodes);
          }
        }

        return {nodes};
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
