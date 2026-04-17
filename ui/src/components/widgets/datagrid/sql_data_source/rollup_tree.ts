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
import {Engine} from '../../../../trace_processor/engine';
import {NUM, Row, SqlValue} from '../../../../trace_processor/query_result';
import {
  createPerfettoTable,
  DisposableSqlEntity,
} from '../../../../trace_processor/sql_utils';
import {runQueryForQueryTable} from '../../../query_table/queries';
import {DataSourceRows, PivotModel} from '../data_source';
import {GroupPath} from '../model';
import {serializeFilters} from './group_by';
import {SQLSchemaRegistry, SQLSchemaResolver} from '../sql_schema';
import {filterToSql, sqlAggregateExpr, toAlias} from '../sql_utils';
import {stringifyJsonWithBigints} from '../../../../base/json_utils';

/**
 * Serializes a GroupPath to a string for cache key comparison.
 * Uses JSON for proper handling of nulls, strings, numbers, etc.
 */
function serializePathForKey(path: GroupPath): string {
  return stringifyJsonWithBigints(path);
}

/**
 * Converts a SqlValue to a SQL literal for use in WHERE clauses.
 */
function sqlLiteral(value: SqlValue): string {
  if (value === null || value === undefined) {
    return 'NULL';
  }
  if (typeof value === 'string') {
    // Escape single quotes by doubling them
    return `'${value.replace(/'/g, "''")}'`;
  }
  if (typeof value === 'number' || typeof value === 'bigint') {
    return String(value);
  }
  if (value instanceof Uint8Array) {
    // Convert blob to hex literal
    const hex = Array.from(value)
      .map((b) => b.toString(16).padStart(2, '0'))
      .join('');
    return `X'${hex}'`;
  }
  return String(value);
}

/**
 * Builds a SQL condition that matches a specific GroupPath.
 * Uses proper column comparisons instead of string concatenation.
 */
function buildPathCondition(
  path: GroupPath,
  columnPrefix: string = '',
): string {
  if (path.length === 0) {
    // Empty path matches root (depth 0)
    return `${columnPrefix}__depth = 0`;
  }

  const conditions: string[] = [];
  for (let i = 0; i < path.length; i++) {
    const col = `${columnPrefix}__group_${i}`;
    const value = path[i];
    if (value === null || value === undefined) {
      conditions.push(`${col} IS NULL`);
    } else {
      conditions.push(`${col} = ${sqlLiteral(value)}`);
    }
  }
  // Also match the depth to ensure we're at the right level
  conditions.push(`${columnPrefix}__depth = ${path.length}`);
  return `(${conditions.join(' AND ')})`;
}

// Rollup tree datasource - uses pure SQL with UNION ALL and window functions.
export class SQLDataSourceRollupTree {
  private readonly rowCountSlot: QuerySlot<number>;
  private readonly rowsSlot: QuerySlot<{
    readonly rows: readonly Row[];
    readonly rowOffset: number;
  }>;
  private readonly rollupTableSlot: QuerySlot<DisposableSqlEntity>;
  private readonly summariesSlot: QuerySlot<Row>;

  constructor(
    private readonly uuid: string,
    queue: SerialTaskQueue,
    private readonly engine: Engine,
    private readonly sqlSchema: SQLSchemaRegistry,
    private readonly rootSchemaName: string,
  ) {
    this.rowCountSlot = new QuerySlot<number>(queue);
    this.rowsSlot = new QuerySlot<{
      readonly rows: readonly Row[];
      readonly rowOffset: number;
    }>(queue);
    this.rollupTableSlot = new QuerySlot<DisposableSqlEntity>(queue);
    this.summariesSlot = new QuerySlot<Row>(queue);
  }

  getRows(model: PivotModel): DataSourceRows {
    const {
      groupBy,
      aggregates,
      filters = [],
      pagination,
      expandedGroups,
      collapsedGroups,
      sort,
    } = model;

    const rollupTableResult = this.useRollupTable(model);
    if (rollupTableResult.isPending || !rollupTableResult.data) {
      return {isPending: true};
    }

    const rollupTableName = rollupTableResult.data.name;

    // Build column alias mappings
    const columnAliases: Record<string, string> = {};
    const aliasToColumn: Record<string, string> = {};
    for (let i = 0; i < groupBy.length; i++) {
      columnAliases[`__group_${i}`] = toAlias(groupBy[i].alias);
      aliasToColumn[groupBy[i].alias] = `__group_${i}`;
    }
    for (let i = 0; i < aggregates.length; i++) {
      columnAliases[`__agg_${i}`] = toAlias(aggregates[i].alias);
      aliasToColumn[aggregates[i].alias] = `__agg_${i}`;
    }

    // Determine sort column and direction
    // Default to __id ASC to preserve natural order when no sort is specified
    let sortColumn = '__id';
    let sortDirection: 'ASC' | 'DESC' = 'ASC';
    if (sort) {
      const column = aliasToColumn[sort.alias];
      if (column) {
        sortColumn = column;
      }
      sortDirection = sort.direction;
    }

    const pivotKey = {
      groupBy,
      aggregates,
      filters: serializeFilters(filters),
    };

    // Serialize expansion state for key comparison (arrays don't compare by value)
    const serializedExpanded = expandedGroups?.map((p) =>
      serializePathForKey(p),
    );
    const serializedCollapsed = collapsedGroups?.map((p) =>
      serializePathForKey(p),
    );

    const rowCountResult = this.rowCountSlot.use({
      key: {
        ...pivotKey,
        expandedGroups: serializedExpanded,
        collapsedGroups: serializedCollapsed,
        sortDirection,
      },
      retainOn: ['expandedGroups', 'collapsedGroups'],
      queryFn: async () => {
        const query = buildTreeQuery(rollupTableName, {
          expandedGroups,
          collapsedGroups,
          sortColumn,
          sortDirection,
          minDepth: 1,
          countOnly: true,
        });
        const result = await this.engine.query(query);
        return result.firstRow({count: NUM}).count;
      },
    });

    const rowsResult = this.rowsSlot.use({
      key: {
        ...pivotKey,
        expandedGroups: serializedExpanded,
        collapsedGroups: serializedCollapsed,
        pagination,
        sortColumn,
        sortDirection,
      },
      retainOn: [
        'expandedGroups',
        'collapsedGroups',
        'pagination',
        'sortColumn',
        'sortDirection',
      ],
      queryFn: async () => {
        const query = buildTreeQuery(rollupTableName, {
          expandedGroups,
          collapsedGroups,
          sortColumn,
          sortDirection,
          offset: pagination?.offset,
          limit: pagination?.limit,
          minDepth: 1,
          columnAliases,
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

  getSummaries(model: PivotModel): QueryResult<Row> {
    const {groupBy, aggregates, filters = []} = model;

    const rollupTableResult = this.useRollupTable(model);
    if (rollupTableResult.isPending || !rollupTableResult.data) {
      return {isPending: true, data: undefined, isFresh: false};
    }

    const rollupTableName = rollupTableResult.data.name;

    const columnAliases: Record<string, string> = {};
    for (let i = 0; i < groupBy.length; i++) {
      columnAliases[`__group_${i}`] = toAlias(groupBy[i].alias);
    }
    for (let i = 0; i < aggregates.length; i++) {
      columnAliases[`__agg_${i}`] = toAlias(aggregates[i].alias);
    }

    return this.summariesSlot.use({
      key: {
        groupBy,
        aggregates,
        filters: serializeFilters(filters),
      },
      queryFn: async () => {
        const query = buildTreeQuery(rollupTableName, {
          maxDepth: 0,
          columnAliases,
        });
        const result = await runQueryForQueryTable(query, this.engine);
        return (result.rows[0] as Row) ?? ({} as Row);
      },
    });
  }

  private useRollupTable(model: PivotModel): QueryResult<DisposableSqlEntity> {
    const {groupBy, aggregates, filters = []} = model;

    const sourceQuery = this.buildSourceQuery(filters);
    const groupByColumns = groupBy.map((col) => col.field);
    const aggregateExprs = aggregates.map((agg) => {
      if (agg.function === 'COUNT') {
        return 'COUNT(*)';
      } else {
        return sqlAggregateExpr(agg.function, agg.field);
      }
    });

    return this.rollupTableSlot.use({
      key: {
        sourceQuery,
        groupByColumns,
        aggregateExprs,
      },
      queryFn: async () => {
        return await createRollupTable(this.engine, {
          sourceTable: sourceQuery,
          groupByColumns,
          aggregateExprs,
          tableName: `rollup_${this.uuid}`,
        });
      },
    });
  }

  private buildSourceQuery(filters: PivotModel['filters']): string {
    const resolver = new SQLSchemaResolver(this.sqlSchema, this.rootSchemaName);
    const baseTable = resolver.getBaseTable();
    const baseAlias = resolver.getBaseAlias();

    let sql = `SELECT ${baseAlias}.* FROM ${baseTable} AS ${baseAlias}`;

    if (filters && filters.length > 0) {
      for (const filter of filters) {
        resolver.resolveColumnPath(filter.field);
      }

      const joinClauses = resolver.buildJoinClauses();
      if (joinClauses) {
        sql += `\n${joinClauses}`;
      }

      const whereConditions = filters.map((filter) => {
        const sqlExpr = resolver.resolveColumnPath(filter.field);
        return filterToSql(filter, sqlExpr ?? filter.field);
      });
      sql += `\nWHERE ${whereConditions.join(' AND ')}`;
    }

    return `(${sql})`;
  }

  dispose(): void {
    this.rowCountSlot.dispose();
    this.rowsSlot.dispose();
    this.rollupTableSlot.dispose();
    this.summariesSlot.dispose();
  }
}

/**
 * Configuration for creating a rollup table.
 */
interface RollupTableConfig {
  sourceTable: string;
  groupByColumns: readonly string[];
  aggregateExprs: readonly string[];
  tableName?: string;
}

/**
 * Creates a rollup table using UNION ALL queries.
 *
 * The table contains all rollup levels with:
 * - __id: unique row identifier
 * - __parent_id: parent row id (NULL for root)
 * - __depth: hierarchy depth (0 for root, 1+ for groups)
 * - __child_count: number of direct children
 * - __group_0, __group_1, ...: hierarchy column values
 * - __agg_0, __agg_1, ...: aggregate values
 */
async function createRollupTable(
  engine: Engine,
  config: RollupTableConfig,
): Promise<DisposableSqlEntity> {
  const {
    sourceTable,
    groupByColumns,
    aggregateExprs,
    tableName = '__rollup_tree_default__',
  } = config;

  const numHier = groupByColumns.length;
  const numAggs = aggregateExprs.length;

  // Helper to build path key expression from group columns
  const buildPathKey = (upToLevel: number): string => {
    if (upToLevel < 0) return "''";
    const parts: string[] = [];
    for (let i = 0; i <= upToLevel; i++) {
      parts.push(`COALESCE(CAST(${groupByColumns[i]} AS TEXT), '__NULL__')`);
    }
    return parts.join(` || '|' || `);
  };

  // Build UNION ALL query for all rollup levels
  // Each level includes both __path_key and __parent_path_key
  let unionQuery = '';

  // Grand total query (depth 0): all group columns are NULL
  unionQuery += `SELECT 0 AS __depth`;
  unionQuery += `, '' AS __path_key`;
  unionQuery += `, NULL AS __parent_path_key`; // Root has no parent
  for (let i = 0; i < numHier; i++) {
    unionQuery += `, NULL AS __group_${i}`;
  }
  for (let i = 0; i < numAggs; i++) {
    unionQuery += `, ${aggregateExprs[i]} AS __agg_${i}`;
  }
  unionQuery += ` FROM ${sourceTable}`;

  // One query per hierarchy level
  for (let level = 0; level < numHier; level++) {
    unionQuery += ` UNION ALL SELECT ${level + 1} AS __depth`;

    // Path key: concatenation of group values up to this level
    unionQuery += `, ${buildPathKey(level)} AS __path_key`;

    // Parent path key: concatenation of group values up to level-1
    // For level 0 (depth 1), parent is root with empty path
    unionQuery += `, ${level === 0 ? "''" : buildPathKey(level - 1)} AS __parent_path_key`;

    // Group columns: real values up to this level, NULL for rest
    for (let i = 0; i < numHier; i++) {
      if (i <= level) {
        unionQuery += `, ${groupByColumns[i]} AS __group_${i}`;
      } else {
        unionQuery += `, NULL AS __group_${i}`;
      }
    }

    // Aggregates
    for (let i = 0; i < numAggs; i++) {
      unionQuery += `, ${aggregateExprs[i]} AS __agg_${i}`;
    }

    unionQuery += ` FROM ${sourceTable} GROUP BY `;
    for (let i = 0; i <= level; i++) {
      if (i > 0) {
        unionQuery += ', ';
      }
      unionQuery += groupByColumns[i];
    }
  }

  // Build the complete table creation query with IDs and parent relationships
  const groupCols = Array.from({length: numHier}, (_, i) => `__group_${i}`);
  const aggCols = Array.from({length: numAggs}, (_, i) => `__agg_${i}`);

  // Helper to prefix columns with table alias
  const prefixCols = (cols: string[], prefix: string) =>
    cols.map((col) => `${prefix}.${col}`).join(', ');

  // Build column selections - need at least one column
  const groupColsSelect =
    groupCols.length > 0 ? prefixCols(groupCols, 'c') + ',' : '';
  const aggColsSelect = prefixCols(aggCols, 'c'); // Always have at least one agg

  const groupColsSelectW =
    groupCols.length > 0 ? prefixCols(groupCols, 'w') + ',' : '';
  const aggColsSelectW = prefixCols(aggCols, 'w');

  // Drop existing table if it exists (in case of stale data)
  await engine.query(`DROP TABLE IF EXISTS ${tableName}`);

  const createQuery = `
    WITH rollup_raw AS (
      ${unionQuery}
    ),
    -- Assign unique IDs
    with_ids AS (
      SELECT
        ROW_NUMBER() OVER (ORDER BY __depth, __path_key) AS __id,
        *
      FROM rollup_raw
    ),
    -- Join to find parent IDs
    with_parent_id AS (
      SELECT
        c.__id,
        CASE
          WHEN c.__depth = 0 THEN NULL
          ELSE p.__id
        END AS __parent_id,
        c.__depth,
        c.__path_key,
        ${groupColsSelect}
        ${aggColsSelect}
      FROM with_ids c
      LEFT JOIN with_ids p ON c.__parent_path_key = p.__path_key AND p.__depth = c.__depth - 1
    ),
    -- Compute child count
    with_child_count AS (
      SELECT
        w.__id,
        w.__parent_id,
        w.__depth,
        w.__path_key,
        ${groupColsSelectW}
        ${aggColsSelectW},
        COALESCE(cc.cnt, 0) AS __child_count
      FROM with_parent_id w
      LEFT JOIN (
        SELECT __parent_id, COUNT(*) as cnt
        FROM with_parent_id
        WHERE __parent_id IS NOT NULL
        GROUP BY __parent_id
      ) cc ON w.__id = cc.__parent_id
    )
    SELECT * FROM with_child_count
  `;

  await engine.query(createQuery);

  return await createPerfettoTable({
    engine,
    name: tableName,
    as: createQuery,
  });
}

/**
 * Options for querying the rollup tree.
 */
interface TreeQueryOptions {
  expandedGroups?: readonly GroupPath[];
  collapsedGroups?: readonly GroupPath[];
  sortColumn?: string;
  sortDirection?: 'ASC' | 'DESC';
  offset?: number;
  limit?: number;
  minDepth?: number;
  maxDepth?: number;
  columnAliases?: Record<string, string>;
  countOnly?: boolean;
}

// Metadata columns always included in output
const METADATA_COLUMNS = [
  '__id',
  '__parent_id',
  '__depth',
  '__child_count',
  '__path_key',
];

/**
 * Builds a query to traverse the rollup tree in sorted hierarchical order.
 *
 * Uses a recursive CTE to:
 * 1. Start from visible root nodes
 * 2. Recursively traverse to children of expanded nodes
 * 3. Build a sort path that maintains hierarchical ordering
 */
function buildTreeQuery(
  tableName: string,
  options: TreeQueryOptions = {},
): string {
  const {
    expandedGroups,
    collapsedGroups,
    sortColumn = '__agg_0',
    sortDirection = 'DESC',
    offset,
    limit,
    minDepth = 0,
    maxDepth,
    columnAliases,
    countOnly,
  } = options;

  // Determine expansion mode
  const useDenylist = collapsedGroups !== undefined;
  const expansionPaths = useDenylist ? collapsedGroups : expandedGroups ?? [];

  // Build expansion check expressions using actual column comparisons
  // This properly handles nulls, empty strings, and blobs
  // We need two versions: one for base case (no prefix) and one for recursive (c. prefix)
  const buildExpansionExpr = (prefix: string): string => {
    if (useDenylist) {
      // Denylist: expanded if NOT in the collapsed set
      if (expansionPaths.length === 0) {
        return '1'; // All expanded
      } else {
        const conditions = expansionPaths
          .map((p) => buildPathCondition(p, prefix))
          .join(' OR ');
        return `NOT (${conditions})`;
      }
    } else {
      // Allowlist: expanded only if in the expanded set
      if (expansionPaths.length === 0) {
        return '0'; // None expanded
      } else {
        const conditions = expansionPaths
          .map((p) => buildPathCondition(p, prefix))
          .join(' OR ');
        return `(${conditions})`;
      }
    }
  };

  const isExpandedExpr = buildExpansionExpr('');
  const isExpandedExprChild = buildExpansionExpr('c.');

  // The recursive CTE for hierarchical traversal with sorting
  const query = `
    WITH RECURSIVE
    -- First, compute local sort ranks within siblings
    ranked AS (
      SELECT
        *,
        ROW_NUMBER() OVER (
          PARTITION BY __parent_id
          ORDER BY ${sortColumn} ${sortDirection}
        ) AS __local_rank
      FROM ${tableName}
    ),
    -- Recursive traversal building sort path
    tree_traversal AS (
      -- Base case: start from root (depth 0) or its children (depth 1)
      SELECT
        r.*,
        PRINTF('%010d', r.__local_rank) AS __sort_path,
        ${isExpandedExpr} AS __is_expanded
      FROM ranked r
      WHERE r.__depth = ${minDepth}
      ${maxDepth !== undefined ? `AND r.__depth <= ${maxDepth}` : ''}

      UNION ALL

      -- Recursive case: children of expanded nodes
      SELECT
        c.*,
        t.__sort_path || '/' || PRINTF('%010d', c.__local_rank) AS __sort_path,
        ${isExpandedExprChild} AS __is_expanded
      FROM ranked c
      JOIN tree_traversal t ON c.__parent_id = t.__id
      WHERE t.__is_expanded = 1
        AND c.__depth >= ${minDepth}
        ${maxDepth !== undefined ? `AND c.__depth <= ${maxDepth}` : ''}
    )
    SELECT ${buildSelectClause(columnAliases)}
    FROM tree_traversal
    ORDER BY __sort_path
    ${limit !== undefined ? `LIMIT ${limit}` : ''}
    ${offset !== undefined ? `OFFSET ${offset}` : ''}
  `;

  if (countOnly) {
    // Wrap to get count
    return `
      WITH RECURSIVE
      ranked AS (
        SELECT
          *,
          ROW_NUMBER() OVER (
            PARTITION BY __parent_id
            ORDER BY ${sortColumn} ${sortDirection}
          ) AS __local_rank
        FROM ${tableName}
      ),
      tree_traversal AS (
        SELECT
          r.*,
          PRINTF('%010d', r.__local_rank) AS __sort_path,
          ${isExpandedExpr} AS __is_expanded
        FROM ranked r
        WHERE r.__depth = ${minDepth}
        ${maxDepth !== undefined ? `AND r.__depth <= ${maxDepth}` : ''}

        UNION ALL

        SELECT
          c.*,
          t.__sort_path || '/' || PRINTF('%010d', c.__local_rank) AS __sort_path,
          ${isExpandedExprChild} AS __is_expanded
        FROM ranked c
        JOIN tree_traversal t ON c.__parent_id = t.__id
        WHERE t.__is_expanded = 1
          AND c.__depth >= ${minDepth}
          ${maxDepth !== undefined ? `AND c.__depth <= ${maxDepth}` : ''}
      )
      SELECT COUNT(*) as count FROM tree_traversal
    `;
  }

  return query;
}

/**
 * Builds the SELECT clause with optional column aliasing.
 * @param aliases Map of internal column names to display aliases
 */
function buildSelectClause(aliases?: Record<string, string>): string {
  if (!aliases) {
    return '*';
  }

  const clauses: string[] = [];

  // Add aliased columns
  for (const [original, alias] of Object.entries(aliases)) {
    clauses.push(`${original} AS ${alias}`);
  }

  // Always include metadata columns
  for (const col of METADATA_COLUMNS) {
    clauses.push(col);
  }

  // Also include raw group columns for expansion tracking
  // These are needed in addition to aliases so datagrid can read __group_N directly
  // Count how many __group_N columns are in the aliases
  const groupColPattern = /^__group_(\d+)$/;
  const groupColIndices = Object.keys(aliases)
    .map((key) => {
      const match = key.match(groupColPattern);
      return match ? parseInt(match[1], 10) : -1;
    })
    .filter((idx) => idx >= 0);

  for (const idx of groupColIndices) {
    clauses.push(`__group_${idx}`);
  }

  return clauses.join(', ');
}
