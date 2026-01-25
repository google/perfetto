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
import {NUM, Row} from '../../../trace_processor/query_result';
import {
  createVirtualTable,
  DisposableSqlEntity,
} from '../../../trace_processor/sql_utils';
import {runQueryForQueryTable} from '../../query_table/queries';

const DEFAULT_TREE_TABLE_NAME = '__intrinsic_tree_default__';

/**
 * Configuration for creating a tree virtual table.
 */
export interface TreeTableConfig {
  /** The source table or subquery to display as a tree. */
  sourceTable: string;
  /** Column containing the row's unique ID. */
  idColumn: string;
  /** Column containing the parent's ID (NULL for root nodes). */
  parentIdColumn: string;
  /** Optional custom table name. */
  tableName?: string;
}

/**
 * Query options for fetching rows from the tree table.
 */
export interface TreeQueryOptions {
  /** IDs of expanded nodes (allowlist mode). */
  expandedIds?: ReadonlySet<bigint>;
  /** IDs of collapsed nodes (denylist mode, takes precedence). */
  collapsedIds?: ReadonlySet<bigint>;
  /** Sort specification (e.g., 'name DESC', 'id ASC'). */
  sort?: string;
  /** Pagination offset. */
  offset?: number;
  /** Pagination limit. */
  limit?: number;
  /**
   * Column aliases to apply to result rows.
   * Maps original column names to new names.
   * Metadata columns (__depth__, etc.) are always preserved.
   */
  columnAliases?: Record<string, string>;
}

/**
 * Result from querying the tree table.
 */
export interface TreeQueryResult {
  rows: Row[];
  totalRows: number;
}

/**
 * Creates a tree virtual table using __intrinsic_tree.
 *
 * The virtual table supports:
 * - Hierarchical display with expand/collapse
 * - Original row data (no aggregation)
 * - Efficient pagination via constraints
 * - Sorting by any column
 *
 * @example
 * ```typescript
 * const table = await createTreeTable(engine, {
 *   sourceTable: 'thread',
 *   idColumn: 'id',
 *   parentIdColumn: 'parent_id',
 * });
 *
 * const result = await queryTreeTable(engine, table.name, {
 *   expandedIds: new Set([1n, 2n]),
 *   sort: 'name ASC',
 *   offset: 0,
 *   limit: 100,
 * });
 *
 * // Clean up when done
 * await table[Symbol.asyncDispose]();
 * ```
 */
export async function createTreeTable(
  engine: Engine,
  config: TreeTableConfig,
): Promise<DisposableSqlEntity> {
  const {
    sourceTable,
    idColumn,
    parentIdColumn,
    tableName = DEFAULT_TREE_TABLE_NAME,
  } = config;

  const usingClause = `__intrinsic_tree(
  "${sourceTable.replace(/"/g, '""')}",
  '${idColumn}',
  '${parentIdColumn}'
)`;

  return await createVirtualTable({
    engine,
    using: usingClause,
    name: tableName,
  });
}

/**
 * Queries rows from a tree virtual table.
 *
 * The virtual table uses WHERE constraints to control:
 * - Expansion state (__expanded_ids__ or __collapsed_ids__)
 * - Sorting (__sort__)
 * - Pagination (__offset__, __limit__)
 */
export async function queryTreeTable(
  engine: Engine,
  tableName: string,
  options: TreeQueryOptions = {},
): Promise<TreeQueryResult> {
  const {
    expandedIds,
    collapsedIds,
    sort = '',
    offset,
    limit,
    columnAliases,
  } = options;

  // Build expansion constraint
  let expansionConstraint: string;
  if (collapsedIds !== undefined) {
    const ids = Array.from(collapsedIds).join(',');
    expansionConstraint = `__collapsed_ids__ = '${ids}'`;
  } else {
    const ids = expandedIds ? Array.from(expandedIds).join(',') : '';
    expansionConstraint = `__expanded_ids__ = '${ids}'`;
  }

  // Build WHERE clause
  const whereConditions = [expansionConstraint];

  if (sort) {
    whereConditions.push(`__sort__ = '${sort}'`);
  }
  if (offset !== undefined) {
    whereConditions.push(`__offset__ = ${offset}`);
  }
  if (limit !== undefined) {
    whereConditions.push(`__limit__ = ${limit}`);
  }

  // Build SELECT clause with aliases
  // Always use buildSelectWithAliases to ensure metadata columns are properly aliased
  const selectClause = buildSelectWithAliases(columnAliases);

  const query = `
SELECT ${selectClause}
FROM ${tableName}
WHERE ${whereConditions.join('\n  AND ')}
ORDER BY rowid`;

  const result = await runQueryForQueryTable(query, engine);

  // Get total count (without pagination)
  const countQuery = `
SELECT COUNT(*) as total_count
FROM ${tableName}
WHERE ${expansionConstraint}`;

  const countResult = await engine.query(countQuery);
  const totalRows = countResult.firstRow({total_count: NUM}).total_count;

  return {rows: result.rows as Row[], totalRows};
}

// Metadata columns from the tree operator.
const METADATA_SELECT = ['__depth__', '__has_children__', '__child_count__'];

function buildSelectWithAliases(
  aliases: Record<string, string> | undefined,
): string {
  const clauses: string[] = [];

  if (aliases && Object.keys(aliases).length > 0) {
    // Add aliased columns
    for (const [original, alias] of Object.entries(aliases)) {
      clauses.push(`${original} AS ${alias}`);
    }
  } else {
    // No aliases - select all source columns
    clauses.push('*');
  }

  // Always include properly aliased metadata columns
  for (const col of METADATA_SELECT) {
    clauses.push(col);
  }

  return clauses.join(', ');
}
