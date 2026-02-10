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

import {
  QueryResult,
  QuerySlot,
  SerialTaskQueue,
} from '../../../../base/query_slot';
import {shortUuid} from '../../../../base/uuid';
import {Engine} from '../../../../trace_processor/engine';
import {NUM, Row} from '../../../../trace_processor/query_result';
import {
  createPerfettoTable,
  DisposableSqlEntity,
} from '../../../../trace_processor/sql_utils';
import {runQueryForQueryTable} from '../../../query_table/queries';
import {DataSourceRows, TreeModel} from '../data_source';
import {SQLSchemaRegistry, SQLSchemaResolver} from '../sql_schema';
import {filterToSql, sqlValue, toAlias} from '../sql_utils';

/**
 * SQL data source for tree mode using id/parent_id columns.
 *
 * Materializes a base table with __id, __parent_id, __has_children, and user
 * columns. Then uses a recursive CTE to traverse in hierarchical order with:
 * - Proper sorting within siblings
 * - Expand/collapse support via ID sets
 * - Additional metadata columns (__depth)
 */
export class SQLDataSourceTree {
  private readonly uuid = shortUuid();
  private readonly treeTableSlot: QuerySlot<DisposableSqlEntity>;
  private readonly rowCountSlot: QuerySlot<number>;
  private readonly rowsSlot: QuerySlot<{
    readonly rows: readonly Row[];
    readonly rowOffset: number;
  }>;

  constructor(
    queue: SerialTaskQueue,
    private readonly engine: Engine,
    private readonly sqlSchema: SQLSchemaRegistry,
    private readonly rootSchemaName: string,
  ) {
    this.treeTableSlot = new QuerySlot<DisposableSqlEntity>(queue);
    this.rowCountSlot = new QuerySlot<number>(queue);
    this.rowsSlot = new QuerySlot<{
      readonly rows: readonly Row[];
      readonly rowOffset: number;
    }>(queue);
  }

  getRows(model: TreeModel): DataSourceRows {
    const {columns, filters = [], pagination, sort, tree} = model;

    // First, ensure the base table is materialized
    const treeTableResult = this.useTreeTable(model);
    if (treeTableResult.isPending || !treeTableResult.data) {
      return {isPending: true};
    }

    const treeTableName = treeTableResult.data.name;

    // Serialize expansion state for cache key (Sets don't compare by value)
    const serializedExpanded = tree.expandedIds
      ? Array.from(tree.expandedIds).map((id) => id.toString())
      : undefined;
    const serializedCollapsed = tree.collapsedIds
      ? Array.from(tree.collapsedIds).map((id) => id.toString())
      : undefined;

    // Build column alias mappings
    const columnAliases: Record<string, string> = {};
    for (const col of columns) {
      columnAliases[col.field] = toAlias(col.alias);
    }

    // Determine sort column and direction
    let sortColumn = '__id';
    let sortDirection: 'ASC' | 'DESC' = 'ASC';
    if (sort) {
      const sortCol = columns.find((c) => c.alias === sort.alias);
      if (sortCol) {
        sortColumn = toAlias(sortCol.alias);
      }
      sortDirection = sort.direction;
    }

    const baseKey = {
      columns,
      filters: serializeFilters(filters),
      tree: {
        idColumn: tree.idField,
        parentIdColumn: tree.parentIdField,
      },
    };

    const rowCountResult = this.rowCountSlot.use({
      key: {
        ...baseKey,
        expandedIds: serializedExpanded,
        collapsedIds: serializedCollapsed,
        sortColumn,
        sortDirection,
      },
      retainOn: ['expandedIds', 'collapsedIds'],
      queryFn: async () => {
        const query = buildTreeQuery(treeTableName, {
          expandedIds: tree.expandedIds,
          collapsedIds: tree.collapsedIds,
          sortColumn,
          sortDirection,
          countOnly: true,
        });
        const result = await this.engine.query(query);
        return result.firstRow({count: NUM}).count;
      },
    });

    const rowsResult = this.rowsSlot.use({
      key: {
        ...baseKey,
        expandedIds: serializedExpanded,
        collapsedIds: serializedCollapsed,
        pagination,
        sortColumn,
        sortDirection,
      },
      retainOn: [
        'expandedIds',
        'collapsedIds',
        'pagination',
        'sortColumn',
        'sortDirection',
      ],
      queryFn: async () => {
        const query = buildTreeQuery(treeTableName, {
          expandedIds: tree.expandedIds,
          collapsedIds: tree.collapsedIds,
          sortColumn,
          sortDirection,
          offset: pagination?.offset,
          limit: pagination?.limit,
        });
        const result = await runQueryForQueryTable(query, this.engine);
        return {
          rows: result.rows,
          rowOffset: pagination?.offset ?? 0,
        };
      },
    });

    return {
      totalRows: rowCountResult.data,
      rowOffset: rowsResult.data?.rowOffset,
      rows: rowsResult.data?.rows,
      isPending: rowCountResult.isPending || rowsResult.isPending,
    };
  }

  /**
   * Tree mode doesn't support aggregate summaries.
   */
  getSummaries(_model: TreeModel): QueryResult<Row> {
    return {data: undefined, isPending: false, isFresh: true};
  }

  /**
   * Export all data with current filters/sorting applied (no pagination).
   */
  async exportData(model: TreeModel): Promise<readonly Row[]> {
    const treeTableResult = this.useTreeTable(model);
    if (!treeTableResult.data) {
      return [];
    }

    const {columns, sort, tree} = model;
    let sortColumn = '__id';
    let sortDirection: 'ASC' | 'DESC' = 'ASC';
    if (sort) {
      const sortCol = columns.find((c) => c.alias === sort.alias);
      if (sortCol) {
        sortColumn = toAlias(sortCol.alias);
      }
      sortDirection = sort.direction;
    }

    const query = buildTreeQuery(treeTableResult.data.name, {
      expandedIds: tree.expandedIds,
      collapsedIds: tree.collapsedIds,
      sortColumn,
      sortDirection,
    });
    const result = await runQueryForQueryTable(query, this.engine);
    return result.rows;
  }

  /**
   * Materialize the base tree table with __id, __parent_id, __has_children,
   * and all user columns. This is cached and only rebuilt when columns or
   * filters change.
   */
  private useTreeTable(model: TreeModel): QueryResult<DisposableSqlEntity> {
    const {columns, filters = [], tree} = model;

    const resolver = new SQLSchemaResolver(this.sqlSchema, this.rootSchemaName);
    const baseTable = resolver.getBaseTable();
    const baseAlias = resolver.getBaseAlias();

    // Resolve the id and parent_id columns
    const idExpr = resolver.resolveColumnPath(tree.idField) ?? tree.idField;
    const parentIdExpr =
      resolver.resolveColumnPath(tree.parentIdField) ?? tree.parentIdField;

    // Build the SELECT clause for user columns
    const selectExprs: string[] = [];
    for (const col of columns) {
      const sqlExpr = resolver.resolveColumnPath(col.field);
      if (sqlExpr) {
        selectExprs.push(`${sqlExpr} AS ${toAlias(col.alias)}`);
      }
    }

    // Build join clauses
    const joinClauses = resolver.buildJoinClauses();

    // Build filter clause
    let whereClause = '';
    if (filters.length > 0) {
      const whereConditions = filters.map((filter) => {
        const sqlExpr = resolver.resolveColumnPath(filter.field);
        return filterToSql(filter, sqlExpr ?? filter.field);
      });
      whereClause = `WHERE ${whereConditions.join(' AND ')}`;
    }

    const userColumns =
      selectExprs.length > 0 ? selectExprs.join(', ') : `${baseAlias}.*`;

    // The source query that we'll materialize
    const sourceQuery = `
      SELECT
        ${idExpr} AS __id,
        ${parentIdExpr} AS __parent_id,
        ${userColumns}
      FROM ${baseTable} AS ${baseAlias}
      ${joinClauses}
      ${whereClause}
    `;

    return this.treeTableSlot.use({
      key: {
        columns,
        filters: serializeFilters(filters),
        idColumn: tree.idField,
        parentIdColumn: tree.parentIdField,
      },
      queryFn: async () => {
        return await createTreeTable(this.engine, {
          sourceQuery,
          tableName: `tree_${this.uuid}`,
        });
      },
    });
  }

  dispose(): void {
    this.treeTableSlot.dispose();
    this.rowCountSlot.dispose();
    this.rowsSlot.dispose();
  }
}

/**
 * Serialize filters for cache key comparison.
 */
function serializeFilters(filters: TreeModel['filters']) {
  return filters?.map((filter) => filterToSql(filter, filter.field));
}

/**
 * Configuration for creating a tree table.
 */
interface TreeTableConfig {
  sourceQuery: string;
  tableName: string;
}

/**
 * Creates a materialized tree table with __id, __parent_id, __has_children,
 * and all user columns from the source query.
 */
async function createTreeTable(
  engine: Engine,
  config: TreeTableConfig,
): Promise<DisposableSqlEntity> {
  const {sourceQuery, tableName} = config;

  // Build the complete table with child counts
  const createQuery = `
    WITH base_data AS (
      ${sourceQuery}
    ),
    with_children AS (
      SELECT
        b.*,
        COALESCE(cc.cnt, 0) AS __has_children
      FROM base_data b
      LEFT JOIN (
        SELECT __parent_id, COUNT(*) as cnt
        FROM base_data
        WHERE __parent_id IS NOT NULL
        GROUP BY __parent_id
      ) cc ON b.__id = cc.__parent_id
    )
    SELECT * FROM with_children
  `;

  return await createPerfettoTable({
    engine,
    name: tableName,
    as: createQuery,
  });
}

/**
 * Options for querying the tree table.
 */
interface TreeQueryOptions {
  expandedIds?: ReadonlySet<bigint>;
  collapsedIds?: ReadonlySet<bigint>;
  sortColumn?: string;
  sortDirection?: 'ASC' | 'DESC';
  offset?: number;
  limit?: number;
  countOnly?: boolean;
}

/**
 * Builds a query to traverse the tree table in hierarchical order.
 *
 * Uses a recursive CTE to:
 * 1. Start from root nodes (parent_id IS NULL)
 * 2. Recursively traverse to children of expanded nodes
 * 3. Build a sort path that maintains hierarchical ordering
 */
function buildTreeQuery(tableName: string, options: TreeQueryOptions): string {
  const {
    expandedIds,
    collapsedIds,
    sortColumn = '__id',
    sortDirection = 'ASC',
    offset,
    limit,
    countOnly,
  } = options;

  const expansionExpr = buildExpansionExpr(expandedIds, collapsedIds);
  const expansionExprChild = expansionExpr.replace(/\b__id\b/g, 'c.__id');

  const query = `
    WITH RECURSIVE
    -- Add local rank for sorting within siblings
    ranked AS (
      SELECT
        t.*,
        ROW_NUMBER() OVER (
          PARTITION BY __parent_id
          ORDER BY ${sortColumn} ${sortDirection}
        ) AS __local_rank
      FROM ${tableName} t
    ),
    -- Recursive traversal
    tree_traversal AS (
      -- Base case: root nodes (parent_id IS NULL)
      SELECT
        r.*,
        0 AS __depth,
        PRINTF('%010d', r.__local_rank) AS __sort_path,
        ${expansionExpr} AS __is_expanded
      FROM ranked r
      WHERE r.__parent_id IS NULL

      UNION ALL

      -- Recursive case: children of expanded nodes
      SELECT
        c.*,
        t.__depth + 1 AS __depth,
        t.__sort_path || '/' || PRINTF('%010d', c.__local_rank) AS __sort_path,
        ${expansionExprChild} AS __is_expanded
      FROM ranked c
      JOIN tree_traversal t ON c.__parent_id = t.__id
      WHERE t.__is_expanded = 1
    )
    ${
      countOnly
        ? 'SELECT COUNT(*) as count FROM tree_traversal'
        : `
    SELECT *
    FROM tree_traversal
    ORDER BY __sort_path
    ${limit !== undefined ? `LIMIT ${limit}` : ''}
    ${offset !== undefined ? `OFFSET ${offset}` : ''}
    `
    }
  `;

  return query;
}

/**
 * Builds an expression that evaluates to 1 if the node should be expanded, 0 otherwise.
 *
 * Expansion modes:
 * - Denylist (collapsedIds): All nodes expanded except those in the set
 * - Allowlist (expandedIds): Only nodes in the set are expanded
 * - Neither: Default to collapsed (only show root level)
 */
function buildExpansionExpr(
  expandedIds: ReadonlySet<bigint> | undefined,
  collapsedIds: ReadonlySet<bigint> | undefined,
): string {
  // Denylist mode takes precedence
  if (collapsedIds !== undefined) {
    if (collapsedIds.size === 0) {
      return '1'; // All expanded
    }
    const ids = Array.from(collapsedIds)
      .map((id) => sqlValue(id))
      .join(', ');
    return `CASE WHEN __id IN (${ids}) THEN 0 ELSE 1 END`;
  }

  // Allowlist mode
  if (expandedIds !== undefined) {
    if (expandedIds.size === 0) {
      return '0'; // None expanded
    }
    const ids = Array.from(expandedIds)
      .map((id) => sqlValue(id))
      .join(', ');
    return `CASE WHEN __id IN (${ids}) THEN 1 ELSE 0 END`;
  }

  // Default: collapsed
  return '0';
}
