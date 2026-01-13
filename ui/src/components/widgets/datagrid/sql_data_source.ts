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
import {assertUnreachable} from '../../../base/logging';
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
import {Column, Filter, PathSet, Pivot, AggregateColumn} from './model';
import {
  isSQLExpressionDef,
  SQLSchemaRegistry,
  SQLSchemaResolver,
} from './sql_schema';

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
          const baseAlias = `${baseTable}_0`;
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
    const {columns, filters = [], pivot} = model;

    const resolver = new SQLSchemaResolver(this.sqlSchema, this.rootSchemaName);

    const baseTable = resolver.getBaseTable();
    const baseAlias = resolver.getBaseAlias();

    // For pivot mode without drill-down, we build aggregates differently
    if (pivot && !pivot.drillDown) {
      return this.buildPivotQuery(resolver, filters, pivot, options, columns);
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
      // Only add conditions for groupBy columns that have actual values.
      // Rollup rows have NULL for rolled-up columns, which we should skip
      // (not filter by IS NULL, as that would return no results).
      const drillDownConditions = pivot.groupBy
        .filter((col) => {
          const value = pivot.drillDown![col.field];
          // Skip columns that are NULL (rolled-up columns)
          return value !== null && value !== undefined;
        })
        .map((col) => {
          const field = col.field;
          const value = pivot.drillDown![field];
          const sqlExpr = resolver.resolveColumnPath(field) ?? field;
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
   * Builds a pivot query with GROUP BY and aggregations.
   * For multiple groupBy columns, generates rollup queries using UNION ALL.
   */
  private buildPivotQuery(
    resolver: SQLSchemaResolver,
    filters: ReadonlyArray<Filter>,
    pivot: Pivot,
    options: {includeOrderBy: boolean},
    dependencyColumns?: ReadonlyArray<Column>,
  ): string {
    // For multiple groupBy columns, use rollup query generation
    if (pivot.groupBy.length > 1) {
      return this.buildRollupPivotQuery(
        resolver,
        filters,
        pivot,
        options,
        dependencyColumns,
      );
    }

    // Single groupBy column - use simple pivot query
    return this.buildSimplePivotQuery(
      resolver,
      filters,
      pivot,
      options,
      dependencyColumns,
    );
  }

  /**
   * Builds a simple pivot query for single groupBy column.
   */
  private buildSimplePivotQuery(
    resolver: SQLSchemaResolver,
    filters: ReadonlyArray<Filter>,
    pivot: Pivot,
    options: {includeOrderBy: boolean},
    dependencyColumns?: ReadonlyArray<Column>,
  ): string {
    const baseTable = resolver.getBaseTable();
    const baseAlias = resolver.getBaseAlias();

    // Resolve groupBy columns
    const groupByExprs: string[] = [];
    const groupByAliases: string[] = [];
    const groupByFields: string[] = [];

    for (const col of pivot.groupBy) {
      const {id, field} = col;
      groupByFields.push(field);
      const sqlExpr = resolver.resolveColumnPath(field);
      if (sqlExpr) {
        const alias = toAlias(id);
        groupByExprs.push(`${sqlExpr} AS ${alias}`);
        groupByAliases.push(alias);
      }
    }

    // Build aggregate expressions from pivot.aggregates
    const aggregates = pivot.aggregates ?? [];
    const aggregateExprs = aggregates.map((agg) => {
      const alias = toAlias(agg.id);
      if (agg.function === 'COUNT') {
        return `COUNT(*) AS ${alias}`;
      }
      const field = 'field' in agg ? agg.field : null;
      if (!field) {
        return `NULL AS ${alias}`;
      }
      const colExpr = resolver.resolveColumnPath(field);
      if (!colExpr) {
        return `NULL AS ${alias}`;
      }
      if (agg.function === 'ANY') {
        return `MIN(${colExpr}) AS ${alias}`;
      }
      return `${agg.function}(${colExpr}) AS ${alias}`;
    });

    // Build dependency column expressions (columns needed for rendering but not
    // part of groupBy or aggregates). Use ANY (MIN) to get a representative value.
    const dependencyExprs: string[] = [];
    if (dependencyColumns) {
      // Get all fields already in groupBy or aggregates
      const existingFields = new Set([
        ...groupByFields,
        ...aggregates
          .filter((a) => 'field' in a)
          .map((a) => (a as {field: string}).field),
      ]);

      for (const col of dependencyColumns) {
        if (!existingFields.has(col.field)) {
          const sqlExpr = resolver.resolveColumnPath(col.field);
          if (sqlExpr) {
            // Use column ID as alias to support duplicate columns
            const alias = toAlias(col.id);
            // Use MIN as a proxy for ANY to get a value from the group
            dependencyExprs.push(`MIN(${sqlExpr}) AS ${alias}`);
          }
        }
      }
    }

    const selectClauses = [
      ...groupByExprs,
      ...aggregateExprs,
      ...dependencyExprs,
    ];
    const joinClauses = resolver.buildJoinClauses();

    let query = `
SELECT ${selectClauses.join(',\n       ')}
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

    // Add GROUP BY
    if (groupByAliases.length > 0) {
      const groupByOrigExprs = groupByFields.map(
        (field) => resolver.resolveColumnPath(field) ?? field,
      );
      query += `\nGROUP BY ${groupByOrigExprs.join(', ')}`;
    }

    // Add ORDER BY if requested
    if (options.includeOrderBy) {
      const sortedColumn = this.findSortedColumn(undefined, pivot);
      if (sortedColumn) {
        const {id, direction} = sortedColumn;
        // Use the column ID as the alias for ORDER BY
        const alias = toAlias(id);
        query += `\nORDER BY ${alias} ${direction.toUpperCase()}`;
      }
    }

    return query;
  }

  /**
   * Builds a rollup pivot query for multiple groupBy columns using UNION ALL.
   *
   * For N groupBy columns, generates N SELECT statements:
   * - Level 0: GROUP BY first column only (columns 1..N-1 are rollups)
   * - Level 1: GROUP BY first 2 columns (columns 2..N-1 are rollups)
   * - ...
   * - Level N-1: GROUP BY all columns
   *
   * Each level includes __<field>_is_rollup columns (0 or 1) to indicate
   * whether a column value is a rollup aggregate or an actual value.
   *
   * Levels beyond 0 are filtered by expandedGroups to only show children
   * of expanded parent groups.
   */
  private buildRollupPivotQuery(
    resolver: SQLSchemaResolver,
    filters: ReadonlyArray<Filter>,
    pivot: Pivot,
    options: {includeOrderBy: boolean},
    dependencyColumns?: ReadonlyArray<Column>,
  ): string {
    const baseTable = resolver.getBaseTable();
    const baseAlias = resolver.getBaseAlias();

    // Collect groupBy info
    // We track both field (for CTE source column) and id (for output alias)
    const groupByInfo: Array<{field: string; id: string; sqlExpr: string}> = [];

    for (const col of pivot.groupBy) {
      const sqlExpr = resolver.resolveColumnPath(col.field);
      if (sqlExpr) {
        groupByInfo.push({field: col.field, id: col.id, sqlExpr});
      }
    }

    const groupByFields = groupByInfo.map((g) => g.field);
    const groupByAliases = groupByInfo.map((g) => toAlias(g.id)); // Use id as alias everywhere
    const groupByExprs = groupByInfo.map((g) => g.sqlExpr);

    // Collect aggregate source fields
    const aggregates = pivot.aggregates ?? [];
    const aggregateSourceFields: Array<{field: string; alias: string}> = [];
    for (const agg of aggregates) {
      if ('field' in agg && agg.field) {
        const alias = toAlias(agg.field);
        const sqlExpr = resolver.resolveColumnPath(agg.field);
        if (sqlExpr) {
          aggregateSourceFields.push({field: agg.field, alias});
        }
      }
    }

    // Build the CTE that contains all source data with JOINs applied
    const cteSelectClauses: string[] = [];
    const addedFields = new Set<string>();

    // Add groupBy columns to CTE
    for (let i = 0; i < groupByFields.length; i++) {
      cteSelectClauses.push(`${groupByExprs[i]} AS ${groupByAliases[i]}`);
      addedFields.add(groupByFields[i]);
    }

    // Add aggregate source columns to CTE (deduplicated)
    for (const {field, alias} of aggregateSourceFields) {
      if (!addedFields.has(field)) {
        const sqlExpr = resolver.resolveColumnPath(field);
        if (sqlExpr) {
          cteSelectClauses.push(`${sqlExpr} AS ${alias}`);
          addedFields.add(field);
        }
      }
    }

    // Add dependency columns to CTE (columns needed for rendering but not in
    // groupBy or aggregates)
    if (dependencyColumns) {
      for (const col of dependencyColumns) {
        if (!addedFields.has(col.field)) {
          const sqlExpr = resolver.resolveColumnPath(col.field);
          if (sqlExpr) {
            const alias = toAlias(col.field);
            cteSelectClauses.push(`${sqlExpr} AS ${alias}`);
            addedFields.add(col.field);
          }
        }
      }
    }

    // If no explicit columns, select all
    if (cteSelectClauses.length === 0) {
      cteSelectClauses.push(`${baseAlias}.*`);
    }

    const joinClauses = resolver.buildJoinClauses();

    let cteQuery = `SELECT ${cteSelectClauses.join(', ')}
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
      cteQuery += `\nWHERE ${whereConditions.join(' AND ')}`;
    }

    // Build UNION ALL for each rollup level
    const unionQueries: string[] = [];
    const numLevels = groupByFields.length;

    for (let level = 0; level < numLevels; level++) {
      const levelQuery = this.buildRollupLevelQuery(
        level,
        groupByFields,
        groupByAliases,
        aggregates,
        pivot,
        dependencyColumns,
      );
      unionQueries.push(levelQuery);
    }

    // Check if we're sorting by an aggregate
    const sortedColumn = this.findSortedColumn(undefined, pivot);
    const sortedAggregate =
      sortedColumn && aggregates.find((a) => a.id === sortedColumn.id);

    // Build the full query with CTE
    let query = `WITH __data__ AS (
${cteQuery}
), __union__ AS (
${unionQueries.join('\nUNION ALL\n')}
)`;

    // When sorting by aggregate, add window functions to propagate parent
    // aggregate values down to children. This allows sorting parent groups
    // by their aggregate while keeping children under their parent.
    if (sortedAggregate && options.includeOrderBy) {
      const aggAlias = toAlias(sortedAggregate.id);
      const sortKeyExprs: string[] = [];

      // For each level except the last, create a sort key that captures
      // the aggregate value at that level
      for (let level = 0; level < groupByAliases.length - 1; level++) {
        // Partition by columns 0..level, get the aggregate where __level__ = level
        const partitionCols = groupByAliases.slice(0, level + 1).join(', ');
        sortKeyExprs.push(
          `FIRST_VALUE(${aggAlias}) OVER (PARTITION BY ${partitionCols} ORDER BY "__level__") AS "__sort_${level}__"`,
        );
      }

      query += `
SELECT *, ${sortKeyExprs.join(', ')}
FROM __union__`;
    } else {
      query += `
SELECT * FROM __union__`;
    }

    // Add ORDER BY if requested
    if (options.includeOrderBy) {
      const orderByClauses = this.buildRollupOrderBy(
        pivot,
        groupByFields,
        groupByAliases,
        sortedAggregate,
      );
      if (orderByClauses.length > 0) {
        query += `\nORDER BY ${orderByClauses.join(', ')}`;
      }
    }

    return query;
  }

  /**
   * Builds a single rollup level SELECT query.
   *
   * The level indicates how many groupBy columns have real values:
   * - Level 0: Only first groupBy column has value, rest are NULL (most aggregated)
   * - Level N-1: All groupBy columns have values (leaf level)
   *
   * Each row includes a __level__ column to indicate its rollup depth.
   */
  private buildRollupLevelQuery(
    level: number,
    groupByFields: string[],
    groupByAliases: string[],
    aggregates: readonly AggregateColumn[],
    pivot: Pivot,
    dependencyColumns?: ReadonlyArray<Column>,
  ): string {
    const selectClauses: string[] = [];
    const numGroupBy = groupByFields.length;

    // Add __level__ column to indicate rollup depth
    selectClauses.push(`${level} AS "__level__"`);

    // Add groupBy columns - real value if i <= level, NULL otherwise
    for (let i = 0; i < numGroupBy; i++) {
      const alias = groupByAliases[i];
      const isRollup = i > level;

      if (isRollup) {
        selectClauses.push(`NULL AS ${alias}`);
      } else {
        selectClauses.push(alias);
      }
    }

    // Add aggregate expressions
    // Note: In the CTE, source fields are aliased by field name (e.g., "dur")
    // But the output alias should be the aggregate's id (e.g., "dur_sum")
    for (const agg of aggregates) {
      const outputAlias = toAlias(agg.id);
      if (agg.function === 'COUNT') {
        selectClauses.push(`COUNT(*) AS ${outputAlias}`);
      } else if ('field' in agg && agg.field) {
        const sourceAlias = toAlias(agg.field);
        if (agg.function === 'ANY') {
          selectClauses.push(`MIN(${sourceAlias}) AS ${outputAlias}`);
        } else {
          selectClauses.push(
            `${agg.function}(${sourceAlias}) AS ${outputAlias}`,
          );
        }
      }
    }

    // Add dependency columns (using MIN as ANY proxy)
    if (dependencyColumns) {
      const existingFields = new Set([
        ...groupByFields,
        ...aggregates
          .filter((a) => 'field' in a)
          .map((a) => (a as {field: string}).field),
      ]);

      for (const col of dependencyColumns) {
        if (!existingFields.has(col.field)) {
          const alias = toAlias(col.field);
          selectClauses.push(`MIN(${alias}) AS ${alias}`);
        }
      }
    }

    let query = `SELECT ${selectClauses.join(', ')}\nFROM __data__`;

    // Add WHERE clause for group expansion filter (levels > 0)
    // For a row at level N, ALL ancestor paths must be expanded (not collapsed).
    if (level > 0) {
      if ('collapsedGroups' in pivot && pivot.collapsedGroups) {
        // Blacklist mode: Show all rows EXCEPT those whose parent is collapsed
        // A row at level N is hidden if any of its ancestor paths are in collapsedGroups
        const collapsedGroups = pivot.collapsedGroups;
        if (collapsedGroups.size > 0) {
          // Collect collapsed paths that would hide rows at this level.
          // A collapsed path of length L hides all rows at levels > L.
          // For level N, we need to exclude rows whose ancestor at any level < N
          // is collapsed. A path of length L is an ancestor of level N if L < N.
          const collapsedParentPaths: SqlValue[][] = [];
          for (const path of collapsedGroups) {
            if (path.length <= level) {
              // This collapsed path affects rows at this level
              // (path.length == level means direct parent is collapsed)
              collapsedParentPaths.push([...path]);
            }
          }

          if (collapsedParentPaths.length > 0) {
            // Build NOT IN clause for each prefix length
            // Group collapsed paths by their length
            const pathsByLength = new Map<number, SqlValue[][]>();
            for (const path of collapsedParentPaths) {
              const len = path.length;
              if (!pathsByLength.has(len)) {
                pathsByLength.set(len, []);
              }
              pathsByLength.get(len)!.push(path);
            }

            // Build WHERE clause: exclude rows whose ancestor is collapsed
            const notInClauses: string[] = [];
            for (const [len, paths] of pathsByLength) {
              const cols = groupByAliases.slice(0, len);
              if (cols.length === 1) {
                // Single column: use simple NOT IN syntax
                const values = paths
                  .map((path) => sqlValue(path[0]))
                  .join(', ');
                notInClauses.push(`${cols[0]} NOT IN (${values})`);
              } else {
                // Multiple columns: use tuple NOT IN syntax
                const colTuple = cols.join(', ');
                const valueTuples = paths
                  .map((path) => `(${path.map(sqlValue).join(', ')})`)
                  .join(', ');
                notInClauses.push(`(${colTuple}) NOT IN (${valueTuples})`);
              }
            }
            query += `\nWHERE ${notInClauses.join(' AND ')}`;
          }
          // If no collapsed paths affect this level, no WHERE clause needed (show all)
        }
        // If collapsedGroups is empty, no WHERE clause needed (show all)
      } else {
        // Whitelist mode (or no expansion state): Only show rows whose parent
        // path is in expandedGroups. If expandedGroups is undefined/empty,
        // default to all collapsed.
        const expandedGroups =
          'expandedGroups' in pivot && pivot.expandedGroups
            ? pivot.expandedGroups
            : new PathSet();

        // Collect paths of length `level` whose all ancestor prefixes are also expanded
        const validParentPaths: SqlValue[][] = [];
        for (const path of expandedGroups) {
          if (path.length === level) {
            // Check that all ancestor prefixes are also expanded
            let allAncestorsExpanded = true;
            for (let prefixLen = 1; prefixLen < level; prefixLen++) {
              const prefix = path.slice(0, prefixLen);
              if (!expandedGroups.has(prefix)) {
                allAncestorsExpanded = false;
                break;
              }
            }
            if (allAncestorsExpanded) {
              validParentPaths.push([...path]);
            }
          }
        }

        if (validParentPaths.length > 0) {
          // Build tuple comparison: (col0, col1, ...) IN ((v0, v1, ...), ...)
          const colTuple = groupByAliases.slice(0, level).join(', ');
          const valueTuples = validParentPaths
            .map((path) => `(${path.map(sqlValue).join(', ')})`)
            .join(', ');
          query += `\nWHERE (${colTuple}) IN (${valueTuples})`;
        } else {
          // No valid expanded paths at this level, so nothing to show
          query += `\nWHERE FALSE`;
        }
      }
    }

    // Add GROUP BY for columns at this level
    if (level >= 0) {
      const groupByClause = groupByAliases.slice(0, level + 1).join(', ');
      if (groupByClause) {
        query += `\nGROUP BY ${groupByClause}`;
      }
    }

    return query;
  }

  /**
   * Builds ORDER BY clauses for rollup query.
   *
   * When sorting by an aggregate, we use __sort_N__ columns (computed via
   * window functions) to sort parent groups by their aggregate value while
   * keeping children under their parent.
   *
   * Example with groupBy [process, thread] sorted by SUM(dur) DESC:
   * ORDER BY __sort_0__ DESC, process NULLS FIRST, sum_dur DESC, thread NULLS FIRST
   *
   * This sorts processes by their total duration, keeps rollup rows before
   * children, and sorts threads within each process by their duration.
   */
  private buildRollupOrderBy(
    pivot: Pivot,
    groupByFields: string[],
    groupByAliases: string[],
    sortedAggregate?: AggregateColumn,
  ): string[] {
    const orderByClauses: string[] = [];
    const sortedColumn = this.findSortedColumn(undefined, pivot);
    const direction = sortedColumn?.direction ?? 'ASC';

    if (sortedAggregate) {
      // When sorting by aggregate, use the __sort_N__ keys to sort parent groups
      // Pattern: __sort_0__, groupBy[0] NULLS FIRST, __sort_1__, groupBy[1] NULLS FIRST, ...
      // The __sort_N__ key contains the aggregate value at level N, propagated to children
      // NULLS FIRST ensures rollup rows come before their children
      const aggAlias = toAlias(sortedAggregate.id);
      for (let i = 0; i < groupByAliases.length; i++) {
        // Add sort key for this level to sort groups by aggregate
        if (i < groupByAliases.length - 1) {
          orderByClauses.push(`"__sort_${i}__" ${direction}`);
        } else {
          // For the last level, use the aggregate directly
          orderByClauses.push(`${aggAlias} ${direction}`);
        }
        // Add groupBy column with NULLS FIRST to keep rollup before children
        orderByClauses.push(`${groupByAliases[i]} NULLS FIRST`);
      }
    } else {
      // When not sorting by aggregate, just use hierarchical ordering
      for (let i = 0; i < groupByFields.length; i++) {
        const col = pivot.groupBy[i];
        const alias = groupByAliases[i];
        const colDirection = col.sort ?? 'ASC';
        orderByClauses.push(`${alias} ${colDirection} NULLS FIRST`);
      }
    }

    return orderByClauses;
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

function sqlValue(value: SqlValue): string {
  if (typeof value === 'string') {
    return `'${value.replace(/'/g, "''")}'`;
  } else if (typeof value === 'number' || typeof value === 'bigint') {
    return value.toString();
  } else if (typeof value === 'boolean') {
    return value ? '1' : '0';
  } else {
    return `'${String(value)}'`;
  }
}

function comparePagination(a?: Pagination, b?: Pagination): boolean {
  if (!a && !b) return true;
  if (!a || !b) return false;
  return a.limit === b.limit && a.offset === b.offset;
}

/**
 * Converts a string to a valid SQL alias by wrapping in double quotes.
 */
function toAlias(id: string): string {
  return `"${id}"`;
}

/**
 * Converts a filter to SQL using the resolved column expression.
 */
function filterToSql(filter: Filter, sqlExpr: string): string {
  switch (filter.op) {
    case '=':
    case '!=':
    case '<':
    case '<=':
    case '>':
    case '>=':
      return `${sqlExpr} ${filter.op} ${sqlValue(filter.value)}`;
    case 'glob':
      return `${sqlExpr} GLOB ${sqlValue(filter.value)}`;
    case 'not glob':
      return `${sqlExpr} NOT GLOB ${sqlValue(filter.value)}`;
    case 'is null':
      return `${sqlExpr} IS NULL`;
    case 'is not null':
      return `${sqlExpr} IS NOT NULL`;
    case 'in':
      return `${sqlExpr} IN (${filter.value.map(sqlValue).join(', ')})`;
    case 'not in':
      return `${sqlExpr} NOT IN (${filter.value.map(sqlValue).join(', ')})`;
    default:
      assertUnreachable(filter);
  }
}
