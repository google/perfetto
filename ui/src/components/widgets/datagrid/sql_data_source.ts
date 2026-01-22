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

import {AsyncLimiter} from '../../../base/async_limiter';
import {maybeUndefined} from '../../../base/utils';
import {Engine} from '../../../trace_processor/engine';
import {NUM, Row, SqlValue} from '../../../trace_processor/query_result';
import {runQueryForQueryTable} from '../../query_table/queries';
import {
  DataSource,
  DataSourceModel,
  DataSourceRows,
  Pagination,
} from './data_source';
import {Column, Filter, Pivot, TreeGrouping} from './model';
import {
  isSQLExpressionDef,
  SQLSchemaRegistry,
  SQLSchemaResolver,
} from './sql_schema';
import {filterToSql, sqlValue, toAlias} from './sql_utils';

/**
 * Configuration for SQLDataSource.
 */
export interface SQLDataSourceConfig {
  /**
   * The trace processor engine to run queries against.
   */
  readonly engine: Engine;

  /**
   * SQL schema registry defining tables and their relationships.
   * Enables automatic JOIN generation for nested column paths.
   *
   * For simple queries without explicit column definitions, use
   * createSimpleSchema() to generate a passthrough schema.
   */
  readonly sqlSchema: SQLSchemaRegistry;

  /**
   * The root schema name to query from (e.g., 'slice', 'query').
   */
  readonly rootSchemaName: string;

  /**
   * Optional SQL prelude to execute before each query.
   * Useful for imports like "INCLUDE PERFETTO MODULE xyz;"
   */
  readonly preamble?: string;
}

// Cache entry for row count resolution
interface RowCountCache {
  query: string;
  count: number;
}

// Cache entry for rows resolution
interface RowsCache {
  query: string;
  pagination: Pagination | undefined;
  offset: number;
  rows: Row[];
}

// Cache entry for aggregate resolution
interface AggregatesCache {
  query: string;
  totals: Map<string, SqlValue>;
}

// Cache entry for distinct values resolution
interface DistinctValuesCache {
  query: string;
  values: ReadonlyArray<SqlValue>;
}

// Cache entry for tree metadata (has_children computation)
// This is cached separately because it only depends on base data + filters,
// NOT on expansion state. So we can reuse it across expand/collapse operations.
interface TreeMetadataCache {
  // Cache key based on: base table, filters, tree field, delimiter
  cacheKey: string;
  // Name of the temp table storing has_children paths
  tempTableName: string;
}

// Counter for generating unique temp table names
let treeMetadataTempTableCounter = 0;

// Cache entry for intrinsic pivot virtual table
// This caches the virtual table creation so we don't recreate it on every query.
// The table needs to be recreated when filters change (different base data).
interface IntrinsicPivotCache {
  // Cache key based on: base table, filters, groupBy cols, aggregates
  cacheKey: string;
  // Name of the virtual table
  tableName: string;
}

// Counter for generating unique intrinsic pivot table names
let intrinsicPivotTableCounter = 0;

/**
 * SQL data source for DataGrid.
 *
 * Generates optimized SQL queries with JOINs based on column paths like
 * 'parent.name' or 'thread.process.pid'. Supports parameterized columns
 * like 'args.foo'.
 *
 * For arbitrary queries without explicit schema, use createSimpleSchema():
 * ```typescript
 * import {createSimpleSchema} from './sql_schema';
 *
 * const dataSource = new SQLDataSource({
 *   engine,
 *   sqlSchema: createSimpleSchema('SELECT * FROM slice WHERE dur > 0'),
 *   rootSchemaName: 'query',
 * });
 * ```
 *
 * For tables with relationships:
 * ```typescript
 * const schema: SQLSchemaRegistry = {
 *   slice: {
 *     table: 'slice',
 *     columns: {
 *       id: {},
 *       name: {},
 *       parent: { ref: 'slice', foreignKey: 'parent_id' },
 *     },
 *   },
 * };
 *
 * const dataSource = new SQLDataSource({
 *   engine,
 *   sqlSchema: schema,
 *   rootSchemaName: 'slice',
 * });
 * ```
 */
export class SQLDataSource implements DataSource {
  private readonly engine: Engine;
  private readonly limiter = new AsyncLimiter();
  private readonly sqlSchema: SQLSchemaRegistry;
  private readonly rootSchemaName: string;
  private readonly prelude?: string;

  // Cache for each resolution type
  private rowCountCache?: RowCountCache;
  private rowsCache?: RowsCache;
  private aggregatesCache?: AggregatesCache;
  private distinctValuesCache = new Map<string, DistinctValuesCache>();
  private parameterKeysCache = new Map<string, ReadonlyArray<string>>();
  private treeMetadataCache?: TreeMetadataCache;
  // Current tree metadata temp table name (for sync access during query building)
  private currentTreeMetadataTable?: string;
  // Cache for intrinsic pivot virtual table
  private intrinsicPivotCache?: IntrinsicPivotCache;

  // Current results
  private cachedResult?: DataSourceRows;
  private cachedDistinctValues?: ReadonlyMap<string, ReadonlyArray<SqlValue>>;
  private cachedAggregateTotals?: ReadonlyMap<string, SqlValue>;
  private isLoadingFlag = false;

  constructor(config: SQLDataSourceConfig) {
    this.engine = config.engine;
    this.sqlSchema = config.sqlSchema;
    this.rootSchemaName = config.rootSchemaName;
    this.prelude = config.preamble;
  }

  /**
   * Getter for the current rows result
   */
  get rows(): DataSourceRows | undefined {
    return this.cachedResult;
  }

  get isLoading(): boolean {
    return this.isLoadingFlag;
  }

  get distinctValues(): ReadonlyMap<string, readonly SqlValue[]> | undefined {
    return this.cachedDistinctValues;
  }

  get parameterKeys(): ReadonlyMap<string, readonly string[]> | undefined {
    return this.parameterKeysCache.size > 0
      ? this.parameterKeysCache
      : undefined;
  }

  get aggregateTotals(): ReadonlyMap<string, SqlValue> | undefined {
    return this.cachedAggregateTotals;
  }

  /**
   * Get the current working query for the datasource.
   * Useful for debugging or creating debug tracks.
   */
  getCurrentQuery(): string {
    return this.rowsCache?.query ?? '';
  }

  /**
   * Notify of parameter changes and trigger data update
   */
  notify(model: DataSourceModel): void {
    this.limiter.schedule(async () => {
      // Defer setting loading flag to avoid setting it synchronously during the
      // view() call that triggered notify(). This avoids the bug that the
      // current frame always has isLoading = true.
      await Promise.resolve();
      this.isLoadingFlag = true;

      try {
        // Ensure tree metadata table is ready if in tree mode
        await this.maybeRefreshTreeMetadata(model);

        // Ensure intrinsic pivot table is ready if using ID-based expansion
        await this.maybeRefreshIntrinsicPivot(model);

        // Resolve row count
        const rowCount = await this.resolveRowCount(model);

        // Resolve aggregates
        const aggregateTotals = await this.resolveAggregates(model);

        // Resolve rows
        const {offset, rows} = await this.resolveRows(model);

        // Resolve distinct values
        const distinctValues = await this.resolveDistinctValues(model);

        // Resolve parameter keys
        await this.resolveParameterKeys(model);

        // Build final result
        this.cachedResult = {
          rowOffset: offset,
          totalRows: rowCount,
          rows,
        };
        this.cachedDistinctValues = distinctValues;
        this.cachedAggregateTotals = aggregateTotals;
      } finally {
        this.isLoadingFlag = false;
      }
    });
  }

  /**
   * Resolves the row count. Compares query against cache and reuses if unchanged.
   */
  private async resolveRowCount(model: DataSourceModel): Promise<number> {
    // Build query without ORDER BY - ordering is irrelevant for counting
    const countQuery = this.buildQuery(model, {includeOrderBy: false});

    // Check cache
    if (this.rowCountCache?.query === countQuery) {
      return this.rowCountCache.count;
    }

    // Fetch new count
    const result = await this.engine.query(
      this.wrapQueryWithPrelude(`
      WITH data AS (${countQuery})
      SELECT COUNT(*) AS total_count
      FROM data
    `),
    );
    const count = result.firstRow({total_count: NUM}).total_count;

    // Update cache
    this.rowCountCache = {query: countQuery, count};

    return count;
  }

  /**
   * Resolves the rows for the current page. Compares query and pagination against cache.
   */
  private async resolveRows(
    model: DataSourceModel,
  ): Promise<{offset: number; rows: Row[]}> {
    const {pagination} = model;

    // Build query with ORDER BY for proper pagination ordering
    const rowsQuery = this.buildQuery(model, {includeOrderBy: true});

    // Check cache - both query and pagination must match
    if (
      this.rowsCache?.query === rowsQuery &&
      comparePagination(this.rowsCache.pagination, pagination)
    ) {
      return {offset: this.rowsCache.offset, rows: this.rowsCache.rows};
    }

    // Fetch new rows
    let query = `
      WITH data AS (${rowsQuery})
      SELECT *
      FROM data
    `;

    if (pagination) {
      query += `LIMIT ${pagination.limit} OFFSET ${pagination.offset}`;
    }

    const result = await runQueryForQueryTable(
      this.wrapQueryWithPrelude(query),
      this.engine,
    );

    const offset = pagination?.offset ?? 0;
    const rows = result.rows;

    // Update cache
    this.rowsCache = {query: rowsQuery, pagination, offset, rows};

    return {offset, rows};
  }

  /**
   * Resolves aggregate totals. Handles both pivot aggregates and column aggregates.
   */
  private async resolveAggregates(
    model: DataSourceModel,
  ): Promise<Map<string, SqlValue> | undefined> {
    const {columns, filters = [], pivot} = model;

    // Build a unique query string for the aggregates
    const aggregateQuery = this.buildAggregateQuery(model);

    // If no aggregates needed, return undefined
    if (!aggregateQuery) {
      this.aggregatesCache = undefined;
      return undefined;
    }

    // Check cache
    if (this.aggregatesCache?.query === aggregateQuery) {
      return this.aggregatesCache.totals;
    }

    // Compute aggregates
    const totals = new Map<string, SqlValue>();

    // Pivot aggregates (but not drill-down mode)
    const pivotAggregates = pivot?.aggregates ?? [];
    if (pivot && !pivot.drillDown && pivotAggregates.length > 0) {
      const aggregates = await this.fetchPivotAggregates(filters, pivot);
      for (const [key, value] of Object.entries(aggregates)) {
        totals.set(key, value);
      }
    }

    // Column-level aggregations (non-pivot mode)
    const columnsWithAggregation = columns?.filter((c) => c.aggregate);
    if (columnsWithAggregation && columnsWithAggregation.length > 0 && !pivot) {
      const columnAggregates = await this.fetchColumnAggregates(
        filters,
        columnsWithAggregation,
      );
      for (const [key, value] of Object.entries(columnAggregates)) {
        totals.set(key, value as SqlValue);
      }
    }

    // Update cache
    this.aggregatesCache = {query: aggregateQuery, totals};

    return totals;
  }

  /**
   * Builds a unique string representing the aggregate query for cache comparison.
   */
  private buildAggregateQuery(model: DataSourceModel): string | undefined {
    const {columns, filters = [], pivot} = model;

    const parts: string[] = [];

    // Include pivot aggregates
    if (pivot && !pivot.drillDown && Boolean(pivot.aggregates?.length)) {
      parts.push(`pivot:${JSON.stringify(pivot.aggregates)}`);
    }

    // Include column aggregates (use column ID for uniqueness)
    const columnsWithAggregation = columns?.filter((c) => c.aggregate);
    if (columnsWithAggregation && columnsWithAggregation.length > 0 && !pivot) {
      const colAggs = columnsWithAggregation.map(
        (c) => `${c.id}:${c.field}:${c.aggregate}`,
      );
      parts.push(`columns:${colAggs.join(',')}`);
    }

    if (parts.length === 0) {
      return undefined;
    }

    // Include filters in the cache key
    const filterKey = filters.map((f) => {
      const value = 'value' in f ? f.value : '';
      return `${f.field}:${f.op}:${value}`;
    });
    parts.push(`filters:${filterKey.join(',')}`);

    return parts.join('|');
  }

  /**
   * Resolves distinct values for requested columns.
   */
  private async resolveDistinctValues(
    model: DataSourceModel,
  ): Promise<Map<string, ReadonlyArray<SqlValue>>> {
    const {distinctValuesColumns} = model;

    const result = new Map<string, ReadonlyArray<SqlValue>>();

    if (!distinctValuesColumns) {
      return result;
    }

    for (const columnPath of distinctValuesColumns) {
      const query = this.buildDistinctValuesQuery(columnPath);
      if (!query) continue;

      // Check cache
      const cached = this.distinctValuesCache.get(columnPath);
      if (cached?.query === query) {
        result.set(columnPath, cached.values);
        continue;
      }

      // Fetch new values
      const queryResult = await runQueryForQueryTable(
        this.wrapQueryWithPrelude(query),
        this.engine,
      );
      const values = queryResult.rows.map((r) => r['value']);

      // Update cache
      this.distinctValuesCache.set(columnPath, {query, values});
      result.set(columnPath, values);
    }

    return result;
  }

  /**
   * Resolves parameter keys for parameterized columns.
   */
  private async resolveParameterKeys(model: DataSourceModel): Promise<void> {
    const {parameterKeyColumns} = model;

    if (!parameterKeyColumns) {
      return;
    }

    for (const prefix of parameterKeyColumns) {
      // Already cached
      if (this.parameterKeysCache.has(prefix)) {
        continue;
      }

      const schema = this.sqlSchema[this.rootSchemaName];
      const colDef = maybeUndefined(schema?.columns[prefix]);

      if (colDef && isSQLExpressionDef(colDef) && colDef.parameterized) {
        if (colDef.parameterKeysQuery) {
          const baseTable = schema.table;
          // Use 'subquery_0' for subqueries (tables starting with '(')
          const aliasBase = baseTable.startsWith('(') ? 'subquery' : baseTable;
          const baseAlias = `${aliasBase}_0`;
          const query = colDef.parameterKeysQuery(baseTable, baseAlias);

          try {
            const result = await runQueryForQueryTable(
              this.wrapQueryWithPrelude(query),
              this.engine,
            );
            const keys = result.rows.map((r) => String(r['key']));
            this.parameterKeysCache.set(prefix, keys);
          } catch {
            this.parameterKeysCache.set(prefix, []);
          }
        }
      }
    }
  }

  private wrapQueryWithPrelude(query: string): string {
    if (this.prelude) {
      return `${this.prelude};\n${query}`;
    }
    return query;
  }

  /**
   * Export all data with current filters/sorting applied.
   */
  async exportData(): Promise<Row[]> {
    const workingQuery = this.rowsCache?.query;
    if (!workingQuery) {
      return [];
    }

    const query = `SELECT * FROM (${workingQuery})`;
    const result = await runQueryForQueryTable(
      this.wrapQueryWithPrelude(query),
      this.engine,
    );
    return result.rows;
  }

  /**
   * Builds a complete SQL query from the model.
   */
  private buildQuery(
    model: DataSourceModel,
    options: {includeOrderBy: boolean},
  ): string {
    const {columns, filters = [], pivot, tree} = model;

    const resolver = new SQLSchemaResolver(this.sqlSchema, this.rootSchemaName);

    const baseTable = resolver.getBaseTable();
    const baseAlias = resolver.getBaseAlias();

    // For pivot mode without drill-down, we build aggregates differently
    if (pivot && !pivot.drillDown) {
      return this.buildPivotQuery(resolver, filters, pivot, options, columns);
    }

    // Tree grouping mode: hierarchical display without aggregation
    if (tree) {
      return this.buildTreeQuery(resolver, filters, tree, options, columns);
    }

    // Normal mode or drill-down: select individual columns
    const selectExprs: string[] = [];

    for (const col of columns ?? []) {
      const sqlExpr = resolver.resolveColumnPath(col.field);
      if (sqlExpr) {
        // Use column ID as alias to support duplicate columns with same field
        const alias = toAlias(col.id);
        selectExprs.push(`${sqlExpr} AS ${alias}`);
      }
    }

    // Resolve filter column paths first to ensure JOINs are added
    for (const filter of filters) {
      resolver.resolveColumnPath(filter.field);
    }

    // Resolve drill-down groupBy fields to ensure their JOINs are added
    if (pivot?.drillDown) {
      for (const col of pivot.groupBy) {
        resolver.resolveColumnPath(col.field);
      }
    }

    // If no columns specified, select all from base table
    if (selectExprs.length === 0) {
      selectExprs.push(`${baseAlias}.*`);
    }

    const joinClauses = resolver.buildJoinClauses();

    let query = `
SELECT ${selectExprs.join(',\n       ')}
FROM ${baseTable} AS ${baseAlias}
${joinClauses}`;

    // Add WHERE clause for filters
    if (filters.length > 0) {
      const whereConditions = filters.map((filter) => {
        const sqlExpr = resolver.resolveColumnPath(filter.field);
        if (!sqlExpr) {
          return filterToSql(filter, filter.field);
        }
        return filterToSql(filter, sqlExpr);
      });
      query += `\nWHERE ${whereConditions.join(' AND ')}`;
    }

    // Add drill-down conditions
    if (pivot?.drillDown) {
      // Build conditions for each groupBy column, using IS NULL for null values
      const drillDownConditions = pivot.drillDown
        .map(({field, value}) => {
          const sqlExpr = resolver.resolveColumnPath(field) ?? field;
          // Use IS NULL for null values, otherwise use equality
          if (value === null) {
            return `${sqlExpr} IS NULL`;
          }
          return `${sqlExpr} = ${sqlValue(value)}`;
        })
        .join(' AND ');

      if (drillDownConditions) {
        if (filters.length > 0) {
          query += ` AND ${drillDownConditions}`;
        } else {
          query += `\nWHERE ${drillDownConditions}`;
        }
      }
    }

    // Add ORDER BY clause if requested
    if (options.includeOrderBy) {
      const sortedColumn = this.findSortedColumn(columns, pivot);
      if (sortedColumn) {
        const {field, direction} = sortedColumn;
        const sqlExpr = resolver.resolveColumnPath(field);
        if (sqlExpr) {
          query += `\nORDER BY ${sqlExpr} ${direction.toUpperCase()}`;
        }
      }
    }

    // Include column aggregates in the query string so changes trigger a reload
    const aggregateSuffix = columns
      ?.filter((c) => c.aggregate)
      .map((c) => `${c.id}:${c.aggregate}`)
      .join(',');
    if (aggregateSuffix) {
      query += ` /* aggregates: ${aggregateSuffix} */`;
    }

    return query;
  }

  /**
   * Builds a pivot query using the __intrinsic_pivot virtual table.
   * All pivot modes now use ID-based expansion via the virtual table.
   */
  private buildPivotQuery(
    resolver: SQLSchemaResolver,
    filters: ReadonlyArray<Filter>,
    pivot: Pivot,
    options: {includeOrderBy: boolean},
    dependencyColumns?: ReadonlyArray<Column>,
  ): string {
    return this.buildIntrinsicPivotQuery(
      resolver,
      filters,
      pivot,
      options,
      dependencyColumns,
    );
  }

  /**
   * Checks if tree mode is active and refreshes the tree metadata table if needed.
   * Called from notify() before building queries.
   */
  private async maybeRefreshTreeMetadata(
    model: DataSourceModel,
  ): Promise<void> {
    const {filters = [], pivot, tree} = model;

    // Check for tree mode in flat mode
    if (tree) {
      const resolver = new SQLSchemaResolver(
        this.sqlSchema,
        this.rootSchemaName,
      );
      this.currentTreeMetadataTable = await this.ensureTreeMetadataTable(
        resolver,
        filters,
        tree.field,
        tree.delimiter ?? '/',
      );
      return;
    }

    // Check for tree mode in pivot mode
    if (pivot && !pivot.drillDown && pivot.groupBy.length === 1) {
      const groupByCol = pivot.groupBy[0];
      if (groupByCol.tree) {
        const resolver = new SQLSchemaResolver(
          this.sqlSchema,
          this.rootSchemaName,
        );
        this.currentTreeMetadataTable = await this.ensureTreeMetadataTable(
          resolver,
          filters,
          groupByCol.field,
          groupByCol.tree.delimiter ?? '/',
        );
        return;
      }
    }

    // Not in tree mode - clear the cached table name
    this.currentTreeMetadataTable = undefined;
  }

  /**
   * Ensures the tree metadata temp table exists and is up-to-date.
   * Returns the temp table name to use in queries.
   *
   * The tree metadata table contains paths that have children, computed once
   * and cached until filters or tree config changes. This avoids recomputing
   * the expensive LEAD window function on every expand/collapse.
   */
  private async ensureTreeMetadataTable(
    resolver: SQLSchemaResolver,
    filters: ReadonlyArray<Filter>,
    treeField: string,
    delimiter: string,
  ): Promise<string> {
    const baseTable = resolver.getBaseTable();
    const baseAlias = resolver.getBaseAlias();

    // Build cache key from factors that affect which paths have children
    const filterKey = filters
      .map((f) => `${f.field}:${f.op}:${'value' in f ? f.value : ''}`)
      .join('|');
    const cacheKey = `${baseTable}:${treeField}:${delimiter}:${filterKey}`;

    // Check if cache is valid
    if (this.treeMetadataCache?.cacheKey === cacheKey) {
      return this.treeMetadataCache.tempTableName;
    }

    // Generate unique temp table name
    const tempTableName = `__tree_meta_${treeMetadataTempTableCounter++}__`;

    // Resolve the path column expression
    const pathExpr = resolver.resolveColumnPath(treeField);
    if (!pathExpr) {
      throw new Error(`Could not resolve tree field: ${treeField}`);
    }

    // Build filter clause
    let filterClause = '';
    if (filters.length > 0) {
      const whereConditions = filters.map((filter) => {
        const sqlExpr = resolver.resolveColumnPath(filter.field);
        return filterToSql(filter, sqlExpr ?? filter.field);
      });
      filterClause = `WHERE ${whereConditions.join(' AND ')}`;
    }

    const joinClauses = resolver.buildJoinClauses();

    // Create temp table with has_children paths
    // Uses LEAD window function: a path has children if the next path (sorted)
    // starts with it + delimiter and has level = current level + 1
    const createQuery = `
CREATE TEMP TABLE ${tempTableName} AS
WITH __source__ AS (
  SELECT ${pathExpr} AS __tree_path__
  FROM ${baseTable} AS ${baseAlias}
  ${joinClauses}
  ${filterClause}
),
__tree__ AS (
  SELECT
    __tree_path__,
    length(__tree_path__) - length(replace(__tree_path__, '${delimiter}', '')) AS "__level__"
  FROM __source__
)
SELECT DISTINCT __tree_path__
FROM (
  SELECT
    __tree_path__,
    "__level__",
    LEAD(__tree_path__) OVER (ORDER BY __tree_path__) AS __next_path__,
    LEAD("__level__") OVER (ORDER BY __tree_path__) AS __next_level__
  FROM __tree__
)
WHERE __next_path__ LIKE __tree_path__ || '${delimiter}%'
  AND __next_level__ = "__level__" + 1`;

    // Drop old temp table if it exists (from previous cache)
    if (this.treeMetadataCache?.tempTableName) {
      try {
        await this.engine.query(
          `DROP TABLE IF EXISTS ${this.treeMetadataCache.tempTableName}`,
        );
      } catch {
        // Ignore errors dropping old table
      }
    }

    // Create the new temp table
    await this.engine.query(this.wrapQueryWithPrelude(createQuery));

    // Update cache
    this.treeMetadataCache = {cacheKey, tempTableName};

    return tempTableName;
  }

  /**
   * Builds a tree query for flat mode with hierarchical display.
   *
   * This displays raw
   * column values with tree filtering based on expansion state.
   * No GROUP BY, no aggregates - just filtering rows by what's expanded.
   */
  private buildTreeQuery(
    resolver: SQLSchemaResolver,
    filters: ReadonlyArray<Filter>,
    tree: TreeGrouping,
    options: {includeOrderBy: boolean},
    columns?: ReadonlyArray<Column>,
  ): string {
    const baseTable = resolver.getBaseTable();
    const baseAlias = resolver.getBaseAlias();

    const delimiter = tree.delimiter ?? '/';
    const treeField = tree.field;

    // Resolve the tree path column expression
    const pathExpr = resolver.resolveColumnPath(treeField);
    if (!pathExpr) {
      // Fallback to normal query if tree field not found
      return this.buildNormalQuery(resolver, filters, options, columns);
    }

    // Build column expressions
    const selectExprs: string[] = [];
    const addedFields = new Set<string>();

    // Add tree path column
    selectExprs.push(`${pathExpr} AS __tree_path__`);
    addedFields.add(treeField);

    // Add other columns
    for (const col of columns ?? []) {
      if (!addedFields.has(col.field)) {
        const sqlExpr = resolver.resolveColumnPath(col.field);
        if (sqlExpr) {
          const alias = toAlias(col.id);
          selectExprs.push(`${sqlExpr} AS ${alias}`);
          addedFields.add(col.field);
        }
      }
    }

    // Resolve filter column paths
    for (const filter of filters) {
      resolver.resolveColumnPath(filter.field);
    }

    const joinClauses = resolver.buildJoinClauses();

    // Build WHERE clause for filters
    let filterClause = '';
    if (filters.length > 0) {
      const whereConditions = filters.map((filter) => {
        const sqlExpr = resolver.resolveColumnPath(filter.field);
        return filterToSql(filter, sqlExpr ?? filter.field);
      });
      filterClause = `\nWHERE ${whereConditions.join(' AND ')}`;
    }

    // Build the expansion filter based on whitelist (expandedPaths) or blacklist (collapsedPaths) mode
    let expansionFilter: string;

    if ('collapsedPaths' in tree) {
      // Blacklist mode: all expanded except collapsed paths
      // A row is visible if its parent is not collapsed
      // This is done by excluding rows whose path starts with a collapsed path + delimiter
      const collapsedPathsByLevel = new Map<number, string[]>();
      for (const path of tree.collapsedPaths) {
        if (path.length === 1 && typeof path[0] === 'string') {
          const pathStr = path[0];
          const level =
            pathStr.length - pathStr.replaceAll(delimiter, '').length;
          if (!collapsedPathsByLevel.has(level)) {
            collapsedPathsByLevel.set(level, []);
          }
          collapsedPathsByLevel.get(level)!.push(pathStr);
        }
      }

      if (collapsedPathsByLevel.size === 0) {
        // No collapsed paths - show all rows
        expansionFilter = '1=1';
      } else {
        // Exclude children of collapsed paths
        const exclusionConditions: string[] = [];
        for (const [_, paths] of collapsedPathsByLevel) {
          for (const p of paths) {
            exclusionConditions.push(
              `t.__tree_path__ NOT GLOB ${sqlValue(p + delimiter + '*')}`,
            );
          }
        }
        expansionFilter = exclusionConditions.join('\n   AND ');
      }
    } else {
      // Whitelist mode: only expanded paths are visible
      // A path is "fully expanded" only if ALL its ancestors are also expanded
      const allExpandedPaths = new Set<string>();
      if ('expandedPaths' in tree && tree.expandedPaths) {
        for (const path of tree.expandedPaths) {
          if (path.length === 1 && typeof path[0] === 'string') {
            allExpandedPaths.add(path[0]);
          }
        }
      }

      // Compute fully expanded paths (where all ancestors are also expanded)
      const fullyExpandedPaths: string[] = [];
      for (const pathStr of allExpandedPaths) {
        const level = pathStr.length - pathStr.replaceAll(delimiter, '').length;

        if (level === 0) {
          // Level 0 paths have no ancestors - always fully expanded
          fullyExpandedPaths.push(pathStr);
        } else {
          // Check if all ancestors are expanded
          const parts = pathStr.split(delimiter);
          let ancestorPath = '';
          let allAncestorsExpanded = true;

          for (let i = 0; i < parts.length - 1; i++) {
            ancestorPath =
              i === 0 ? parts[0] : ancestorPath + delimiter + parts[i];
            if (!allExpandedPaths.has(ancestorPath)) {
              allAncestorsExpanded = false;
              break;
            }
          }

          if (allAncestorsExpanded) {
            fullyExpandedPaths.push(pathStr);
          }
        }
      }

      // Build the expansion filter using GLOB prefix matching
      // Note: Use t. prefix since this is used after LEFT JOIN with __has_children__
      const expansionConditions: string[] = ['t."__level__" = 0'];
      for (const pathStr of fullyExpandedPaths) {
        const pathLevel =
          pathStr.length - pathStr.replaceAll(delimiter, '').length;
        const childLevel = pathLevel + 1;
        expansionConditions.push(
          `(t."__level__" = ${childLevel} AND t.__tree_path__ GLOB ${sqlValue(pathStr + delimiter + '*')})`,
        );
      }
      expansionFilter = expansionConditions.join('\n   OR ');
    }

    // Find the tree field column for output alias
    const treeColumn = columns?.find((c) => c.field === treeField);
    const pathAlias = treeColumn ? toAlias(treeColumn.id) : toAlias(treeField);

    // Build column expressions for final SELECT (raw values, no aggregation)
    const otherColumnExprs = columns
      ?.filter((c) => c.field !== treeField)
      .map((c) => `t.${toAlias(c.id)}`)
      .join(',\n  ');

    // Build the complete query
    // The __has_children__ data comes from a cached temp table (created in maybeRefreshTreeMetadata)
    // to avoid recomputing the expensive LEAD window function on every expand/collapse.
    const hasChildrenTable =
      this.currentTreeMetadataTable ?? '__has_children__';
    const query = `WITH __source__ AS (
  SELECT ${selectExprs.join(', ')}
  FROM ${baseTable} AS ${baseAlias}
  ${joinClauses}${filterClause}
),
__tree__ AS (
  SELECT *,
    length(__tree_path__) - length(replace(__tree_path__, '${delimiter}', '')) AS "__level__"
  FROM __source__
)
SELECT
  t.__tree_path__ AS ${pathAlias},
  t."__level__",
  ${otherColumnExprs || 'NULL AS __placeholder__'},
  (h.__tree_path__ IS NOT NULL) AS __tree_has_children__
FROM __tree__ t
LEFT JOIN ${hasChildrenTable} h ON h.__tree_path__ = t.__tree_path__
WHERE ${expansionFilter}${options.includeOrderBy ? '\nORDER BY t.__tree_path__' : ''}`;

    return query;
  }

  /**
   * Builds a normal (non-pivot, non-tree) query. Extracted for reuse.
   */
  private buildNormalQuery(
    resolver: SQLSchemaResolver,
    filters: ReadonlyArray<Filter>,
    options: {includeOrderBy: boolean},
    columns?: ReadonlyArray<Column>,
  ): string {
    const baseTable = resolver.getBaseTable();
    const baseAlias = resolver.getBaseAlias();

    const selectExprs: string[] = [];
    for (const col of columns ?? []) {
      const sqlExpr = resolver.resolveColumnPath(col.field);
      if (sqlExpr) {
        const alias = toAlias(col.id);
        selectExprs.push(`${sqlExpr} AS ${alias}`);
      }
    }

    for (const filter of filters) {
      resolver.resolveColumnPath(filter.field);
    }

    if (selectExprs.length === 0) {
      selectExprs.push(`${baseAlias}.*`);
    }

    const joinClauses = resolver.buildJoinClauses();

    let query = `
SELECT ${selectExprs.join(',\n       ')}
FROM ${baseTable} AS ${baseAlias}
${joinClauses}`;

    if (filters.length > 0) {
      const whereConditions = filters.map((filter) => {
        const sqlExpr = resolver.resolveColumnPath(filter.field);
        if (!sqlExpr) {
          return filterToSql(filter, filter.field);
        }
        return filterToSql(filter, sqlExpr);
      });
      query += `\nWHERE ${whereConditions.join(' AND ')}`;
    }

    if (options.includeOrderBy) {
      const sortedColumn = columns?.find((c) => c.sort);
      if (sortedColumn) {
        const sqlExpr = resolver.resolveColumnPath(sortedColumn.field);
        if (sqlExpr) {
          query += `\nORDER BY ${sqlExpr} ${sortedColumn.sort!.toUpperCase()}`;
        }
      }
    }

    return query;
  }

  /**
   * Builds a query using the __intrinsic_pivot virtual table.
   *
   * This approach is more efficient than buildRollupPivotQuery because:
   * 1. The virtual table maintains the tree structure internally
   * 2. Expansion state is passed as a simple comma-separated ID list
   * 3. No complex UNION ALL or window functions needed
   *
   * The virtual table must be created/recreated when filters change.
   * This is handled by ensureIntrinsicPivotTable() called from notify().
   */
  private buildIntrinsicPivotQuery(
    _resolver: SQLSchemaResolver,
    _filters: ReadonlyArray<Filter>,
    pivot: Pivot,
    options: {includeOrderBy: boolean},
    dependencyColumns?: ReadonlyArray<Column>,
  ): string {
    // Get the virtual table name (should be set by ensureIntrinsicPivotTable)
    const tableName =
      this.intrinsicPivotCache?.tableName ?? '__intrinsic_pivot_default__';

    // Build expansion constraint - collapsedIds takes precedence if both set
    // collapsedIds = blacklist mode (all expanded except listed)
    // expandedIds = whitelist mode (only listed are expanded)
    let expansionConstraint: string;
    if (pivot.collapsedIds !== undefined) {
      const collapsedIdsStr = Array.from(pivot.collapsedIds).join(',');
      expansionConstraint = `__collapsed_ids__ = '${collapsedIdsStr}'`;
    } else {
      const expandedIdsStr = pivot.expandedIds
        ? Array.from(pivot.expandedIds).join(',')
        : '';
      expansionConstraint = `__expanded_ids__ = '${expandedIdsStr}'`;
    }

    // Build sort spec string
    const sortedColumn = this.findSortedColumn(undefined, pivot);
    let sortSpec = 'agg_0 DESC'; // Default
    if (sortedColumn) {
      const aggregates = pivot.aggregates ?? [];
      const aggIndex = aggregates.findIndex((a) => a.id === sortedColumn.id);
      if (aggIndex >= 0) {
        sortSpec = `agg_${aggIndex} ${sortedColumn.direction}`;
      } else {
        // Sorting by groupBy column - use 'name'
        sortSpec = `name ${sortedColumn.direction}`;
      }
    }

    // Build the SELECT clause with column aliases
    // Map virtual table columns to the expected output aliases
    const selectClauses: string[] = [];

    // Add groupBy columns with their proper aliases
    for (let i = 0; i < pivot.groupBy.length; i++) {
      const col = pivot.groupBy[i];
      const alias = toAlias(col.id);
      // Virtual table uses the original column names for hierarchy cols
      selectClauses.push(`${col.field} AS ${alias}`);
    }

    // Add metadata columns
    selectClauses.push('__id__');
    selectClauses.push('__parent_id__');
    selectClauses.push('__depth__ AS "__level__"'); // Alias for consistency with rollup
    selectClauses.push('__has_children__ AS __tree_has_children__');
    selectClauses.push('__child_count__');

    // Add aggregate columns with proper aliases
    const aggregates = pivot.aggregates ?? [];
    for (let i = 0; i < aggregates.length; i++) {
      const agg = aggregates[i];
      const alias = toAlias(agg.id);
      selectClauses.push(`agg_${i} AS ${alias}`);
    }

    // Add dependency columns (they won't be in the pivot output, so use NULL)
    if (dependencyColumns) {
      const existingFields = new Set([
        ...pivot.groupBy.map((g) => g.field),
        ...aggregates
          .filter((a) => 'field' in a)
          .map((a) => (a as {field: string}).field),
      ]);

      for (const col of dependencyColumns) {
        if (!existingFields.has(col.field)) {
          const alias = toAlias(col.id);
          selectClauses.push(`NULL AS ${alias}`);
        }
      }
    }

    // Build the query
    let query = `SELECT ${selectClauses.join(',\n       ')}
FROM ${tableName}
WHERE ${expansionConstraint}
  AND __sort__ = '${sortSpec}'`;

    // Add ORDER BY if requested (virtual table handles sorting, but we need
    // to maintain proper tree ordering which the table provides)
    if (options.includeOrderBy) {
      // The virtual table returns rows in tree order, but we can add
      // explicit ordering for the UI
      query += '\nORDER BY rowid'; // Preserve tree order from virtual table
    }

    return query;
  }

  /**
   * Ensures the intrinsic pivot virtual table exists and is up-to-date.
   * Called from maybeRefreshIntrinsicPivot() before building queries.
   *
   * The virtual table needs to be recreated when:
   * - Filters change (different base data)
   * - GroupBy columns change
   * - Aggregate expressions change
   */
  private async ensureIntrinsicPivotTable(
    resolver: SQLSchemaResolver,
    filters: ReadonlyArray<Filter>,
    pivot: Pivot,
  ): Promise<string> {
    const baseTable = resolver.getBaseTable();
    const baseAlias = resolver.getBaseAlias();

    // Build cache key from factors that affect the tree structure
    const filterKey = filters
      .map((f) => `${f.field}:${f.op}:${'value' in f ? f.value : ''}`)
      .join('|');
    const groupByKey = pivot.groupBy.map((g) => g.field).join(',');
    const aggKey = (pivot.aggregates ?? [])
      .map((a) => {
        if (a.function === 'COUNT') return 'COUNT(*)';
        return `${a.function}(${'field' in a ? a.field : ''})`;
      })
      .join(',');
    const cacheKey = `${baseTable}:${filterKey}:${groupByKey}:${aggKey}`;

    // Check if cache is valid
    if (this.intrinsicPivotCache?.cacheKey === cacheKey) {
      return this.intrinsicPivotCache.tableName;
    }

    // Generate unique table name
    const tableName = `__pivot_${intrinsicPivotTableCounter++}__`;

    // Build the base table expression (with filters as subquery if needed)
    let sourceTable = baseTable;
    if (filters.length > 0) {
      // Resolve filter column paths
      for (const filter of filters) {
        resolver.resolveColumnPath(filter.field);
      }
      const joinClauses = resolver.buildJoinClauses();

      const whereConditions = filters.map((filter) => {
        const sqlExpr = resolver.resolveColumnPath(filter.field);
        return filterToSql(filter, sqlExpr ?? filter.field);
      });

      sourceTable = `(SELECT ${baseAlias}.* FROM ${baseTable} AS ${baseAlias} ${joinClauses} WHERE ${whereConditions.join(' AND ')})`;
    }

    // Build hierarchy columns string
    const hierarchyCols = pivot.groupBy.map((g) => g.field).join(', ');

    // Build aggregation expressions string
    const aggExprs = (pivot.aggregates ?? [])
      .map((a) => {
        if (a.function === 'COUNT') return 'COUNT(*)';
        const field = 'field' in a ? a.field : '';
        if (a.function === 'ANY') return `MIN(${field})`; // ANY -> MIN
        return `${a.function}(${field})`;
      })
      .join(', ');

    // Drop old table if it exists
    if (this.intrinsicPivotCache?.tableName) {
      try {
        await this.engine.query(
          `DROP TABLE IF EXISTS ${this.intrinsicPivotCache.tableName}`,
        );
      } catch {
        // Ignore errors dropping old table
      }
    }

    // Create the virtual table
    // Use double quotes for sourceTable to avoid issues with single-quoted filter values
    const createQuery = `CREATE VIRTUAL TABLE ${tableName} USING __intrinsic_pivot(
  "${sourceTable.replace(/"/g, '""')}",
  '${hierarchyCols}',
  '${aggExprs}'
)`;

    await this.engine.query(this.wrapQueryWithPrelude(createQuery));

    // Update cache
    this.intrinsicPivotCache = {cacheKey, tableName};

    return tableName;
  }

  /**
   * Checks if pivot mode is active and refreshes the virtual table if needed.
   * Called from notify() before building queries.
   */
  private async maybeRefreshIntrinsicPivot(
    model: DataSourceModel,
  ): Promise<void> {
    const {filters = [], pivot} = model;

    if (pivot && !pivot.drillDown) {
      const resolver = new SQLSchemaResolver(
        this.sqlSchema,
        this.rootSchemaName,
      );
      await this.ensureIntrinsicPivotTable(resolver, filters, pivot);
    }
  }

  /**
   * Find the column that has sorting applied.
   * Returns the column ID (used as SQL alias) and sort direction.
   */
  private findSortedColumn(
    columns: ReadonlyArray<Column> | undefined,
    pivot?: Pivot,
  ): {id: string; field: string; direction: 'ASC' | 'DESC'} | undefined {
    // In drill-down mode, we display flat columns, so only check those for sort
    if (pivot?.drillDown) {
      if (columns) {
        for (const col of columns) {
          if (col.sort) {
            return {id: col.id, field: col.field, direction: col.sort};
          }
        }
      }
      return undefined;
    }

    // Check pivot groupBy columns for sort
    if (pivot) {
      for (const col of pivot.groupBy) {
        if (col.sort) {
          return {id: col.id, field: col.field, direction: col.sort};
        }
      }
      // Check pivot aggregates for sort
      for (const agg of pivot.aggregates ?? []) {
        if (agg.sort) {
          const field = 'field' in agg ? agg.field : '';
          return {id: agg.id, field, direction: agg.sort};
        }
      }
    }

    // Check regular columns for sort
    if (columns) {
      for (const col of columns) {
        if (col.sort) {
          return {id: col.id, field: col.field, direction: col.sort};
        }
      }
    }

    return undefined;
  }

  /**
   * Builds a distinct values query.
   */
  private buildDistinctValuesQuery(columnPath: string): string | undefined {
    const resolver = new SQLSchemaResolver(this.sqlSchema, this.rootSchemaName);
    const sqlExpr = resolver.resolveColumnPath(columnPath);
    if (!sqlExpr) return undefined;

    const baseTable = resolver.getBaseTable();
    const baseAlias = resolver.getBaseAlias();
    const joinClauses = resolver.buildJoinClauses();

    return `
      SELECT DISTINCT ${sqlExpr} AS value
      FROM ${baseTable} AS ${baseAlias}
      ${joinClauses}
      ORDER BY ${sqlExpr} IS NULL, ${sqlExpr}
    `;
  }

  private async fetchPivotAggregates(
    filters: ReadonlyArray<Filter>,
    pivot: Pivot,
  ): Promise<Row> {
    const resolver = new SQLSchemaResolver(this.sqlSchema, this.rootSchemaName);

    const baseTable = resolver.getBaseTable();
    const baseAlias = resolver.getBaseAlias();

    // Resolve filter columns first to ensure JOINs are added
    for (const filter of filters) {
      resolver.resolveColumnPath(filter.field);
    }

    const aggregates = pivot.aggregates ?? [];
    const selectClauses = aggregates
      .map((agg) => {
        const alias = toAlias(agg.id);
        if (agg.function === 'COUNT') {
          return `COUNT(*) AS ${alias}`;
        }
        const field = 'field' in agg ? agg.field : null;
        if (!field) return null;
        if (agg.function === 'ANY') {
          return `NULL AS ${alias}`;
        }
        const colExpr = resolver.resolveColumnPath(field);
        if (!colExpr) {
          return `NULL AS ${alias}`;
        }
        return `${agg.function}(${colExpr}) AS ${alias}`;
      })
      .filter(Boolean)
      .join(', ');

    if (!selectClauses) {
      return {};
    }

    const joinClauses = resolver.buildJoinClauses();

    let query = `
SELECT ${selectClauses}
FROM ${baseTable} AS ${baseAlias}
${joinClauses}`;

    if (filters.length > 0) {
      const filterResolver = new SQLSchemaResolver(
        this.sqlSchema,
        this.rootSchemaName,
      );
      const whereConditions = filters.map((filter) => {
        const sqlExpr = filterResolver.resolveColumnPath(filter.field);
        return filterToSql(filter, sqlExpr ?? filter.field);
      });
      query += `\nWHERE ${whereConditions.join(' AND ')}`;
    }

    const result = await runQueryForQueryTable(
      this.wrapQueryWithPrelude(query),
      this.engine,
    );
    return result.rows[0] ?? {};
  }

  private async fetchColumnAggregates(
    filters: ReadonlyArray<Filter>,
    columns: ReadonlyArray<Column>,
  ): Promise<Row> {
    const resolver = new SQLSchemaResolver(this.sqlSchema, this.rootSchemaName);

    const baseTable = resolver.getBaseTable();
    const baseAlias = resolver.getBaseAlias();

    const selectClauses = columns
      .filter((col) => col.aggregate)
      .map((col) => {
        const func = col.aggregate!;
        const colExpr = resolver.resolveColumnPath(col.field);
        // Use column ID as alias to support duplicate columns
        const alias = toAlias(col.id);

        if (!colExpr) {
          return `NULL AS ${alias}`;
        }
        if (func === 'ANY') {
          return `MIN(${colExpr}) AS ${alias}`;
        }
        return `${func}(${colExpr}) AS ${alias}`;
      })
      .join(', ');

    if (!selectClauses) {
      return {};
    }

    // Resolve filter column paths first to ensure JOINs are added
    for (const filter of filters) {
      resolver.resolveColumnPath(filter.field);
    }

    const joinClauses = resolver.buildJoinClauses();

    let query = `
SELECT ${selectClauses}
FROM ${baseTable} AS ${baseAlias}
${joinClauses}`;

    if (filters.length > 0) {
      const whereConditions = filters.map((filter) => {
        const sqlExpr = resolver.resolveColumnPath(filter.field);
        return filterToSql(filter, sqlExpr ?? filter.field);
      });
      query += `\nWHERE ${whereConditions.join(' AND ')}`;
    }

    const result = await runQueryForQueryTable(
      this.wrapQueryWithPrelude(query),
      this.engine,
    );
    return result.rows[0] ?? {};
  }
}

function comparePagination(a?: Pagination, b?: Pagination): boolean {
  if (!a && !b) return true;
  if (!a || !b) return false;
  return a.limit === b.limit && a.offset === b.offset;
}
