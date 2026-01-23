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
import {AggregateFunction} from './model';

const DEFAULT_PIVOT_TABLE_NAME = '__intrinsic_pivot_default__';

/**
 * Configuration for creating a pivot virtual table.
 */
export interface PivotTableConfig {
  /** The source table or subquery to pivot. */
  sourceTable: string;
  /** Columns to group by (hierarchy columns). */
  groupByColumns: readonly string[];
  /** Aggregate expressions (e.g., 'COUNT(*)', 'SUM(dur)'). */
  aggregateExprs: readonly string[];
  /** Optional custom table name. */
  tableName?: string;
}

/**
 * Query options for fetching rows from the pivot table.
 */
export interface PivotQueryOptions {
  /** IDs of expanded nodes (allowlist mode). */
  expandedIds?: ReadonlySet<bigint>;
  /** IDs of collapsed nodes (denylist mode, takes precedence). */
  collapsedIds?: ReadonlySet<bigint>;
  /** Sort specification (e.g., 'agg_0 DESC', 'name ASC'). */
  sort?: string;
  /** Pagination offset. */
  offset?: number;
  /** Pagination limit. */
  limit?: number;
  /**
   * Column aliases to apply to result rows.
   * Maps original column names to new names.
   * e.g., { 'category': 'col_cat', 'agg_0': 'col_count' }
   * Metadata columns (__id__, __depth__, etc.) are always preserved.
   */
  columnAliases?: Record<string, string>;
}

/**
 * Result from querying the pivot table.
 */
export interface PivotQueryResult {
  rows: Row[];
  totalRows: number;
}

/**
 * Creates a pivot virtual table using __intrinsic_pivot.
 *
 * The virtual table supports:
 * - Hierarchical grouping with expand/collapse
 * - Multiple aggregate functions
 * - Efficient pagination via constraints
 * - Sorting by aggregates or group columns
 *
 * @example
 * ```typescript
 * const table = await createPivotTable(engine, {
 *   sourceTable: 'slice',
 *   groupByColumns: ['category', 'name'],
 *   aggregateExprs: ['COUNT(*)', 'SUM(dur)'],
 * });
 *
 * const result = await queryPivotTable(engine, table.name, {
 *   expandedIds: new Set([1, 2]),
 *   sort: 'agg_0 DESC',
 *   offset: 0,
 *   limit: 100,
 * });
 *
 * // Clean up when done
 * await table[Symbol.asyncDispose]();
 * ```
 */
export async function createPivotTable(
  engine: Engine,
  config: PivotTableConfig,
): Promise<DisposableSqlEntity> {
  const {
    sourceTable,
    groupByColumns,
    aggregateExprs,
    tableName = DEFAULT_PIVOT_TABLE_NAME,
  } = config;

  const hierarchyCols = groupByColumns.join(', ');
  const aggExprs = aggregateExprs.join(', ');

  const usingClause = `__intrinsic_pivot(
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
 * Queries rows from a pivot virtual table.
 *
 * The virtual table uses WHERE constraints to control:
 * - Expansion state (__expanded_ids__ or __collapsed_ids__)
 * - Sorting (__sort__)
 * - Pagination (__offset__, __limit__)
 */
export async function queryPivotTable(
  engine: Engine,
  tableName: string,
  options: PivotQueryOptions = {},
): Promise<PivotQueryResult> {
  const {
    expandedIds,
    collapsedIds,
    sort = 'agg_0 DESC',
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
  const whereConditions = [expansionConstraint, `__sort__ = '${sort}'`];

  if (offset !== undefined) {
    whereConditions.push(`__offset__ = ${offset}`);
  }
  if (limit !== undefined) {
    whereConditions.push(`__limit__ = ${limit}`);
  }

  // Build SELECT clause with aliases
  const selectClause = columnAliases
    ? buildSelectWithAliases(columnAliases)
    : '*';

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

// Metadata columns that should always be included
const METADATA_COLUMNS = [
  '__id__',
  '__parent_id__',
  '__depth__',
  '__has_children__',
  '__child_count__',
];

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
export function buildAggregateExpr(func: AggregateFunction, field: string): string {
  if (func === 'ANY') {
    return `MIN(${field})`; // ANY maps to MIN
  }
  return `${func}(${field})`;
}
