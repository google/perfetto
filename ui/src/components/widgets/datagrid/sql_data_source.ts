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
import {Column, Filter, Pivot} from './model';
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

    // Include column aggregates
    const columnsWithAggregation = columns?.filter((c) => c.aggregate);
    if (columnsWithAggregation && columnsWithAggregation.length > 0 && !pivot) {
      const colAggs = columnsWithAggregation.map(
        (c) => `${c.field}:${c.aggregate}`,
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
    const colPaths = columns?.map((c) => c.field) ?? [];
    const selectExprs: string[] = [];

    for (const path of colPaths) {
      const sqlExpr = resolver.resolveColumnPath(path);
      if (sqlExpr) {
        const alias = this.pathToAlias(path);
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
          return this.filterToSql(filter, filter.field);
        }
        return this.filterToSql(filter, sqlExpr);
      });
      query += `\nWHERE ${whereConditions.join(' AND ')}`;
    }

    // Add drill-down conditions
    if (pivot?.drillDown) {
      const drillDownConditions = pivot.groupBy
        .map((col) => {
          const field = col.field;
          const value = pivot.drillDown![field];
          const sqlExpr = resolver.resolveColumnPath(field) ?? field;
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
      .map((c) => `${c.field}:${c.aggregate}`)
      .join(',');
    if (aggregateSuffix) {
      query += ` /* aggregates: ${aggregateSuffix} */`;
    }

    return query;
  }

  /**
   * Builds a pivot query with GROUP BY and aggregations.
   */
  private buildPivotQuery(
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
      const field = col.field;
      groupByFields.push(field);
      const sqlExpr = resolver.resolveColumnPath(field);
      if (sqlExpr) {
        const alias = this.pathToAlias(field);
        groupByExprs.push(`${sqlExpr} AS ${alias}`);
        groupByAliases.push(alias);
      }
    }

    // Build aggregate expressions from pivot.aggregates
    const aggregates = pivot.aggregates ?? [];
    const aggregateExprs = aggregates.map((agg) => {
      if (agg.function === 'COUNT') {
        return `COUNT(*) AS __count__`;
      }
      const field = 'field' in agg ? agg.field : null;
      if (!field) {
        return `NULL AS __unknown__`;
      }
      const alias = this.pathToAlias(field);
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
            const alias = this.pathToAlias(col.field);
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
          return this.filterToSql(filter, filter.field);
        }
        return this.filterToSql(filter, sqlExpr);
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
        const {field, direction} = sortedColumn;
        const aggregateFields = aggregates.map((a) =>
          'field' in a ? a.field : '__count__',
        );
        const pivotColumns = [...groupByFields, ...aggregateFields];
        if (pivotColumns.includes(field)) {
          const alias = groupByFields.includes(field)
            ? this.pathToAlias(field)
            : field;
          query += `\nORDER BY ${alias} ${direction.toUpperCase()}`;
        }
      }
    }

    return query;
  }

  /**
   * Find the column that has sorting applied.
   */
  private findSortedColumn(
    columns: ReadonlyArray<Column> | undefined,
    pivot?: Pivot,
  ): {field: string; direction: 'ASC' | 'DESC'} | undefined {
    // In drill-down mode, we display flat columns, so only check those for sort
    if (pivot?.drillDown) {
      if (columns) {
        for (const col of columns) {
          if (col.sort) {
            return {field: col.field, direction: col.sort};
          }
        }
      }
      return undefined;
    }

    // Check pivot groupBy columns for sort
    if (pivot) {
      for (const col of pivot.groupBy) {
        if (typeof col !== 'string' && col.sort) {
          return {field: col.field, direction: col.sort};
        }
      }
      // Check pivot aggregates for sort
      for (const agg of pivot.aggregates ?? []) {
        if (agg.sort) {
          const field = 'field' in agg ? agg.field : '__count__';
          return {field, direction: agg.sort};
        }
      }
    }

    // Check regular columns for sort
    if (columns) {
      for (const col of columns) {
        if (col.sort) {
          return {field: col.field, direction: col.sort};
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

  /**
   * Converts a column path to a valid SQL alias.
   */
  private pathToAlias(path: string): string {
    if (/^[a-zA-Z_][a-zA-Z0-9_]*$/.test(path)) {
      return path;
    }
    return `"${path}"`;
  }

  /**
   * Converts a filter to SQL using the resolved column expression.
   */
  private filterToSql(filter: Filter, sqlExpr: string): string {
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
        if (agg.function === 'COUNT') {
          return `COUNT(*) AS __count__`;
        }
        const field = 'field' in agg ? agg.field : null;
        if (!field) return null;
        const alias = this.pathToAlias(field);
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
        return this.filterToSql(filter, sqlExpr ?? filter.field);
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
        const alias = this.pathToAlias(col.field);

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
        return this.filterToSql(filter, sqlExpr ?? filter.field);
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
