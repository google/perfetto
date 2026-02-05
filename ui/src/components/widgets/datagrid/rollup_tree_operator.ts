// Copyright (C) 2025 The Android Open Source Project
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
  createVirtualTable,
  DisposableSqlEntity,
} from '../../../trace_processor/sql_utils';
import {AggregateFunction} from './model';

const DEFAULT_ROLLUP_TABLE_NAME = '__intrinsic_rollup_tree_default__';

/**
 * Configuration for creating a rollup tree virtual table.
 */
export interface RollupTreeConfig {
  /** The source table or subquery. */
  sourceTable: string;
  /** Columns to group by (hierarchy columns). */
  groupByColumns: readonly string[];
  /** Aggregate expressions (e.g., 'COUNT(*)', 'SUM(dur)'). */
  aggregateExprs: readonly string[];
  /** Optional custom table name. */
  tableName?: string;
}

/**
 * Query options for fetching rows from the rollup tree table.
 */
export interface RollupTreeQueryOptions {
  /** IDs of expanded nodes (allowlist mode). */
  expandedIds?: ReadonlySet<bigint>;
  /** IDs of collapsed nodes (denylist mode, takes precedence). */
  collapsedIds?: ReadonlySet<bigint>;
  /** Sort specification (e.g., '__agg_0 DESC', 'name ASC'). */
  sort?: string;
  /** Pagination offset. */
  offset?: number;
  /** Pagination limit. */
  limit?: number;
  /**
   * Minimum depth to include (filters out shallower rows).
   * e.g., 1 to exclude the root node (depth 0).
   */
  minDepth?: number;
  /**
   * Maximum depth to include (efficient - stops tree traversal).
   * e.g., 0 to get only the root node.
   */
  maxDepth?: number;
  /**
   * Column aliases to apply to result rows.
   * Maps original column names to new names.
   * e.g., { 'category': 'col_cat', '__agg_0': 'col_count' }
   * Metadata columns (__id, __depth, etc.) are always preserved.
   */
  columnAliases?: Record<string, string>;
  /**
   * If true, returns COUNT(*) instead of full rows.
   * Useful for getting total row count without fetching all data.
   */
  countOnly?: boolean;
}

/**
 * Creates a rollup tree virtual table using __intrinsic_rollup_tree.
 *
 * The virtual table supports:
 * - Hierarchical grouping with expand/collapse
 * - Multiple aggregate functions
 * - Efficient pagination via constraints
 * - Sorting by aggregates or group columns
 *
 * @example
 * ```typescript
 * const table = await createRollupTree(engine, {
 *   sourceTable: 'slice',
 *   groupByColumns: ['category', 'name'],
 *   aggregateExprs: ['COUNT(*)', 'SUM(dur)'],
 * });
 *
 * const result = await queryRollupTree(engine, table.name, {
 *   expandedIds: new Set([1, 2]),
 *   sort: '__agg_0 DESC',
 *   offset: 0,
 *   limit: 100,
 * });
 *
 * // Clean up when done
 * await table[Symbol.asyncDispose]();
 * ```
 */
export async function createRollupTree(
  engine: Engine,
  config: RollupTreeConfig,
): Promise<DisposableSqlEntity> {
  const {
    sourceTable,
    groupByColumns,
    aggregateExprs,
    tableName = DEFAULT_ROLLUP_TABLE_NAME,
  } = config;

  const hierarchyCols = groupByColumns.join(', ');
  const aggExprs = aggregateExprs.join(', ');

  const usingClause = `__intrinsic_rollup_tree(
  "${sourceTable.replace(/"/g, '""')}",
  '${hierarchyCols}',
  '${aggExprs}'
)`;

  return await createVirtualTable({
    engine,
    using: usingClause,
    name: tableName,
  });
}

/**
 * Builds a query for fetching rows from a rollup tree virtual table.
 *
 * The virtual table uses WHERE constraints to control:
 * - Expansion state (__expanded_ids or __collapsed_ids)
 * - Sorting (__sort)
 * - Pagination (__offset, __limit)
 */
export function buildRollupTreeQuery(
  tableName: string,
  options: RollupTreeQueryOptions = {},
): string {
  const {
    expandedIds,
    collapsedIds,
    sort,
    offset,
    limit,
    minDepth,
    maxDepth,
    columnAliases,
    countOnly,
  } = options;

  // Build expansion constraint
  let expansionConstraint: string;
  if (collapsedIds !== undefined) {
    const ids = Array.from(collapsedIds).join(',');
    expansionConstraint = `__collapsed_ids = '${ids}'`;
  } else {
    const ids = expandedIds ? Array.from(expandedIds).join(',') : '';
    expansionConstraint = `__expanded_ids = '${ids}'`;
  }

  // Build WHERE clause
  const whereConditions = [expansionConstraint];

  if (sort !== undefined) {
    whereConditions.push(`__sort = '${sort}'`);
  }
  if (offset !== undefined) {
    whereConditions.push(`__offset = ${offset}`);
  }
  if (limit !== undefined) {
    whereConditions.push(`__limit = ${limit}`);
  }
  if (minDepth !== undefined) {
    whereConditions.push(`__min_depth = ${minDepth}`);
  }
  if (maxDepth !== undefined) {
    whereConditions.push(`__max_depth = ${maxDepth}`);
  }

  // Build SELECT clause with aliases
  const selectClause = columnAliases
    ? buildSelectWithAliases(columnAliases)
    : '*';

  const baseQuery = `SELECT ${selectClause}
FROM ${tableName}
WHERE ${whereConditions.join('\n  AND ')}
ORDER BY rowid`;

  if (countOnly) {
    return `SELECT COUNT(*) as count FROM (${baseQuery})`;
  }

  return baseQuery;
}

// Metadata columns that should always be included
const METADATA_COLUMNS = ['__id', '__parent_id', '__depth', '__child_count'];

function buildSelectWithAliases(aliases: Record<string, string>): string {
  const clauses: string[] = [];

  // Add aliased columns
  for (const [original, alias] of Object.entries(aliases)) {
    clauses.push(`${original} AS ${alias}`);
  }

  // Always include metadata columns
  for (const col of METADATA_COLUMNS) {
    clauses.push(col);
  }

  return clauses.join(', ');
}

/**
 * Builds an aggregate expression string from function and field.
 * Note: COUNT is not part of AggregateFunction - use 'COUNT(*)' directly.
 */
export function buildAggregateExpr(
  func: AggregateFunction,
  field: string,
): string {
  if (func === 'ANY') {
    return `MIN(${field})`; // ANY maps to MIN
  }
  return `${func}(${field})`;
}

// Legacy aliases for backward compatibility
export {
  createRollupTree as createPivotTable,
  buildRollupTreeQuery as buildPivotQuery,
};
export type {RollupTreeConfig as PivotTableConfig};
export type {RollupTreeQueryOptions as PivotQueryOptions};
