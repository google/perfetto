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
  DataSourceResult,
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
 * Either provide a base query string, or a schema with root schema name.
 */
export interface SQLDataSourceConfig {
  /**
   * The trace processor engine to run queries against.
   */
  engine: Engine;

  /**
   * Base SQL query. Required when not using a schema.
   * When using a schema, this is ignored (the base table comes from the schema).
   */
  baseQuery?: string;

  /**
   * SQL schema registry defining tables and their relationships.
   * When provided, enables automatic JOIN generation for nested column paths.
   */
  sqlSchema?: SQLSchemaRegistry;

  /**
   * The root schema name to query from (e.g., 'slice').
   * Required when sqlSchema is provided.
   */
  rootSchemaName?: string;

  /**
   * Optional SQL prelude to execute before each query.
   * Useful for imports like "INCLUDE PERFETTO MODULE xyz;"
   */
  preamble?: string;
}

/**
 * SQL data source for DataGrid that can operate in two modes:
 *
 * 1. **Simple mode** (baseQuery only): Columns are queried directly from the
 *    base query. Column names must match exactly what's in the query.
 *
 * 2. **Schema mode** (sqlSchema + rootSchemaName): Generates optimized SQL
 *    queries with JOINs based on column paths like 'parent.name' or
 *    'thread.process.pid'. Supports parameterized columns like 'args.foo'.
 *
 * Example usage (simple mode):
 * ```typescript
 * const dataSource = new SQLDataSource({
 *   engine,
 *   baseQuery: 'SELECT * FROM slice WHERE dur > 0',
 * });
 * ```
 *
 * Example usage (schema mode):
 * ```typescript
 * const schema: SQLSchemaRegistry = {
 *   slice: {
 *     table: 'slice',
 *     columns: {
 *       id: {},
 *       name: {},
 *       parent: { ref: 'slice', foreignKey: 'parent_id' },
 *       args: {
 *         expression: (alias, key) => `extract_arg(${alias}.arg_set_id, '${key}')`,
 *         parameterized: true,
 *       },
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
  private readonly baseQuery?: string;
  private readonly sqlSchema?: SQLSchemaRegistry;
  private readonly rootSchemaName?: string;
  private readonly prelude?: string;

  private workingQuery = '';
  private pagination?: Pagination;
  private pivot?: Pivot;
  private cachedResult?: DataSourceResult;
  private isLoadingFlag = false;
  private parameterKeysCache = new Map<string, ReadonlyArray<string>>();

  constructor(config: SQLDataSourceConfig) {
    this.engine = config.engine;
    this.baseQuery = config.baseQuery;
    this.sqlSchema = config.sqlSchema;
    this.rootSchemaName = config.rootSchemaName;
    this.prelude = config.preamble;

    // Validate configuration
    if (!this.baseQuery && !this.sqlSchema) {
      throw new Error('SQLDataSource requires either baseQuery or sqlSchema');
    }
    if (this.sqlSchema && !this.rootSchemaName) {
      throw new Error(
        'SQLDataSource requires rootSchemaName when sqlSchema is provided',
      );
    }
  }

  /**
   * Returns true if this data source is using schema mode.
   */
  private get useSchema(): boolean {
    return this.sqlSchema !== undefined && this.rootSchemaName !== undefined;
  }

  /**
   * Getter for the current rows result
   */
  get result(): DataSourceResult | undefined {
    return this.cachedResult;
  }

  get isLoading(): boolean {
    return this.isLoadingFlag;
  }

  /**
   * Get the current working query for the datasource.
   * Useful for debugging or creating debug tracks.
   */
  getCurrentQuery(): string {
    return this.workingQuery;
  }

  /**
   * Notify of parameter changes and trigger data update
   */
  notify({
    columns,
    filters = [],
    pagination,
    pivot,
    distinctValuesColumns,
    parameterKeyColumns,
  }: DataSourceModel): void {
    this.limiter.schedule(async () => {
      this.isLoadingFlag = true;

      try {
        const workingQuery = this.buildWorkingQuery(columns, filters, pivot);

        if (
          workingQuery !== this.workingQuery ||
          !arePivotsEqual(this.pivot, pivot)
        ) {
          this.workingQuery = workingQuery;
          this.pivot = pivot;

          // Clear the cache
          this.cachedResult = undefined;
          this.pagination = undefined;

          // Update the cache with the total row count
          const rowCount = await this.getRowCount(workingQuery);

          // Compute aggregate totals for pivot mode (but not drill-down mode)
          let aggregateTotals: Map<string, SqlValue> | undefined;
          const pivotAggregates = pivot?.aggregates ?? [];
          if (pivot && !pivot.drillDown && pivotAggregates.length > 0) {
            const aggregates = await this.getPivotAggregates(
              columns,
              filters,
              pivot,
            );
            aggregateTotals = new Map<string, SqlValue>();
            for (const [key, value] of Object.entries(aggregates)) {
              aggregateTotals.set(key, value);
            }
          }

          // Compute column-level aggregations (non-pivot mode)
          const columnsWithAggregation = columns?.filter((c) => c.aggregate);
          if (
            columnsWithAggregation &&
            columnsWithAggregation.length > 0 &&
            !pivot
          ) {
            const columnAggregates = await this.getColumnAggregates(
              filters,
              columnsWithAggregation,
            );
            aggregateTotals = aggregateTotals ?? new Map<string, SqlValue>();
            for (const [key, value] of Object.entries(columnAggregates)) {
              aggregateTotals.set(key, value as SqlValue);
            }
          }

          this.cachedResult = {
            rowOffset: 0,
            totalRows: rowCount,
            rows: [],
            distinctValues: new Map<string, ReadonlyArray<SqlValue>>(),
            parameterKeys: this.parameterKeysCache,
            aggregateTotals,
          };
        }

        // Fetch data if pagination has changed
        if (!comparePagination(this.pagination, pagination)) {
          this.pagination = pagination;
          const {offset, rows} = await this.getRows(workingQuery, pagination);
          this.cachedResult = {
            ...this.cachedResult!,
            rowOffset: offset,
            rows,
          };
        }

        // Handle distinct values requests
        if (distinctValuesColumns) {
          for (const columnPath of distinctValuesColumns) {
            if (!this.cachedResult?.distinctValues?.has(columnPath)) {
              const query = this.buildDistinctValuesQuery(columnPath);
              if (query) {
                const result = await runQueryForQueryTable(
                  this.wrapQueryWithPrelude(query),
                  this.engine,
                );
                const values = result.rows.map((r) => r['value']);
                this.cachedResult = {
                  ...this.cachedResult!,
                  distinctValues: new Map<string, ReadonlyArray<SqlValue>>([
                    ...this.cachedResult!.distinctValues!,
                    [columnPath, values],
                  ]),
                };
              }
            }
          }
        }

        // Handle parameter keys requests (schema mode only)
        if (parameterKeyColumns && this.useSchema) {
          for (const prefix of parameterKeyColumns) {
            if (!this.parameterKeysCache.has(prefix)) {
              const schema = this.sqlSchema![this.rootSchemaName!];
              const colDef = maybeUndefined(schema?.columns[prefix]);

              if (
                colDef &&
                isSQLExpressionDef(colDef) &&
                colDef.parameterized
              ) {
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
                    this.cachedResult = {
                      ...this.cachedResult!,
                      parameterKeys: this.parameterKeysCache,
                    };
                  } catch {
                    this.parameterKeysCache.set(prefix, []);
                  }
                }
              }
            }
          }
        }
      } finally {
        this.isLoadingFlag = false;
      }
    });
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
    if (!this.workingQuery) {
      return [];
    }

    const query = `SELECT * FROM (${this.workingQuery})`;
    const result = await runQueryForQueryTable(
      this.wrapQueryWithPrelude(query),
      this.engine,
    );
    return result.rows;
  }

  /**
   * Builds a complete SQL query based on the current mode.
   */
  private buildWorkingQuery(
    columns: ReadonlyArray<Column> | undefined,
    filters: ReadonlyArray<Filter>,
    pivot?: Pivot,
  ): string {
    if (this.useSchema) {
      return this.buildSchemaWorkingQuery(columns, filters, pivot);
    } else {
      return this.buildSimpleWorkingQuery(columns, filters, pivot);
    }
  }

  /**
   * Builds a query for simple mode (no schema, direct column access).
   */
  private buildSimpleWorkingQuery(
    columns: ReadonlyArray<Column> | undefined,
    filters: ReadonlyArray<Filter>,
    pivot?: Pivot,
  ): string {
    const colNames = columns?.map((c) => c.field) ?? ['*'];

    // Include column aggregates in the query string so changes trigger a reload
    const aggregateSuffix = columns
      ?.filter((c) => c.aggregate)
      .map((c) => `${c.field}:${c.aggregate}`)
      .join(',');

    let query: string;

    if (pivot && !pivot.drillDown) {
      // Pivot mode: Build aggregate columns from pivot.aggregates
      const aggregates = pivot.aggregates ?? [];
      const valCols = aggregates
        .map((agg) => {
          if (agg.function === 'COUNT') {
            return `COUNT(*) AS __count__`;
          }
          const field = 'field' in agg ? agg.field : null;
          if (!field) return null;
          const alias = this.pathToAlias(field);
          if (agg.function === 'ANY') {
            return `MIN(${field}) AS ${alias}`;
          }
          return `${agg.function}(${field}) AS ${alias}`;
        })
        .filter(Boolean)
        .join(', ');

      const groupByFields = pivot.groupBy.map(({field}) => field);
      if (groupByFields.length > 0) {
        const groupCols = groupByFields.join(', ');
        const selectCols = valCols ? `${groupCols}, ${valCols}` : groupCols;
        query = `
          SELECT ${selectCols}
          FROM (${this.baseQuery})
          GROUP BY ${groupCols}
        `;
      } else {
        query = `
          SELECT ${valCols}
          FROM (${this.baseQuery})
        `;
      }
    } else if (pivot?.drillDown) {
      // Drill-down mode
      query = `\nSELECT ${colNames.join(', ')} FROM (${this.baseQuery})`;

      const drillDownConditions = pivot.groupBy
        .map((col) => {
          const field = col.field;
          const value = pivot.drillDown![field];
          if (value === null) {
            return `${field} IS NULL`;
          }
          return `${field} = ${sqlValue(value)}`;
        })
        .join(' AND ');

      if (drillDownConditions) {
        query = `SELECT * FROM (${query}) WHERE ${drillDownConditions}`;
      }
    } else {
      query = `\nSELECT ${colNames.join(', ')} FROM (${this.baseQuery})`;
    }

    // Add WHERE clause for filters
    if (filters.length > 0) {
      const whereConditions = filters.map(simpleFilter2Sql).join(' AND ');
      query = `SELECT * FROM (${query}) WHERE ${whereConditions}`;
    }

    // Add ORDER BY clause - find sorted column from columns or pivot
    const sortedColumn = this.findSortedColumn(columns, pivot);
    if (sortedColumn) {
      const {field, direction} = sortedColumn;
      let columnExists: boolean;
      if (pivot && !pivot.drillDown) {
        const groupByFields = pivot.groupBy.map(({field}) => field);
        const aggregateFields = (pivot.aggregates ?? []).map((a) =>
          'field' in a ? a.field : '__count__',
        );
        const pivotColumns = [...groupByFields, ...aggregateFields];
        columnExists = pivotColumns.includes(field);
      } else {
        columnExists = columns === undefined || colNames.includes(field);
      }
      if (columnExists) {
        query += `\nORDER BY ${field} ${direction.toUpperCase()}`;
      }
    }

    // Append aggregate suffix as a comment so changes trigger reload
    if (aggregateSuffix) {
      query += ` /* aggregates: ${aggregateSuffix} */`;
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
   * Builds a query for schema mode (with JOINs based on column paths).
   */
  private buildSchemaWorkingQuery(
    columns: ReadonlyArray<Column> | undefined,
    filters: ReadonlyArray<Filter>,
    pivot?: Pivot,
  ): string {
    const resolver = new SQLSchemaResolver(
      this.sqlSchema!,
      this.rootSchemaName!,
    );

    const baseTable = resolver.getBaseTable();
    const baseAlias = resolver.getBaseAlias();

    // For pivot mode without drill-down, we build aggregates differently
    if (pivot && !pivot.drillDown) {
      return this.buildSchemaPivotQuery(resolver, filters, pivot);
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

    // Add ORDER BY clause - find sorted column
    const sortedColumn = this.findSortedColumn(columns, pivot);
    if (sortedColumn) {
      const {field, direction} = sortedColumn;
      const sqlExpr = resolver.resolveColumnPath(field);
      if (sqlExpr) {
        query += `\nORDER BY ${sqlExpr} ${direction.toUpperCase()}`;
      }
    }

    return query;
  }

  /**
   * Builds a pivot query with GROUP BY and aggregations (schema mode).
   */
  private buildSchemaPivotQuery(
    resolver: SQLSchemaResolver,
    filters: ReadonlyArray<Filter>,
    pivot: Pivot,
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

    const selectClauses = [...groupByExprs, ...aggregateExprs];
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

    // Add ORDER BY - find sorted column from pivot
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

    return query;
  }

  /**
   * Builds a distinct values query based on the current mode.
   */
  private buildDistinctValuesQuery(columnPath: string): string | undefined {
    if (this.useSchema) {
      const resolver = new SQLSchemaResolver(
        this.sqlSchema!,
        this.rootSchemaName!,
      );
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
    } else {
      return `
        SELECT DISTINCT ${columnPath} AS value
        FROM (${this.baseQuery})
        ORDER BY ${columnPath} IS NULL, ${columnPath}
      `;
    }
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

  private async getRowCount(workingQuery: string): Promise<number> {
    const result = await this.engine.query(
      this.wrapQueryWithPrelude(`
      WITH data AS (${workingQuery})
      SELECT COUNT(*) AS total_count
      FROM data
    `),
    );
    return result.firstRow({total_count: NUM}).total_count;
  }

  private async getPivotAggregates(
    _columns: ReadonlyArray<Column> | undefined,
    filters: ReadonlyArray<Filter>,
    pivot: Pivot,
  ): Promise<Row> {
    if (this.useSchema) {
      return this.getSchemaPivotAggregates(filters, pivot);
    } else {
      return this.getSimplePivotAggregates(filters, pivot);
    }
  }

  private async getSimplePivotAggregates(
    filters: ReadonlyArray<Filter>,
    pivot: Pivot,
  ): Promise<Row> {
    let filteredBaseQuery = `SELECT * FROM (${this.baseQuery})`;
    if (filters.length > 0) {
      const whereConditions = filters.map(simpleFilter2Sql).join(' AND ');
      filteredBaseQuery = `SELECT * FROM (${filteredBaseQuery}) WHERE ${whereConditions}`;
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
        return `${agg.function}(${field}) AS ${alias}`;
      })
      .filter(Boolean)
      .join(', ');

    if (!selectClauses) {
      return {};
    }

    const query = `
      SELECT ${selectClauses}
      FROM (${filteredBaseQuery})
    `;

    const result = await runQueryForQueryTable(
      this.wrapQueryWithPrelude(query),
      this.engine,
    );
    return result.rows[0] ?? {};
  }

  private async getSchemaPivotAggregates(
    filters: ReadonlyArray<Filter>,
    pivot: Pivot,
  ): Promise<Row> {
    const resolver = new SQLSchemaResolver(
      this.sqlSchema!,
      this.rootSchemaName!,
    );

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
        this.sqlSchema!,
        this.rootSchemaName!,
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

  private async getColumnAggregates(
    filters: ReadonlyArray<Filter>,
    columns: ReadonlyArray<Column>,
  ): Promise<Row> {
    if (this.useSchema) {
      return this.getSchemaColumnAggregates(filters, columns);
    } else {
      return this.getSimpleColumnAggregates(filters, columns);
    }
  }

  private async getSimpleColumnAggregates(
    filters: ReadonlyArray<Filter>,
    columns: ReadonlyArray<Column>,
  ): Promise<Row> {
    let filteredBaseQuery = `SELECT * FROM (${this.baseQuery})`;
    if (filters.length > 0) {
      const whereConditions = filters.map(simpleFilter2Sql).join(' AND ');
      filteredBaseQuery = `SELECT * FROM (${filteredBaseQuery}) WHERE ${whereConditions}`;
    }

    const selectClauses = columns
      .filter((col) => col.aggregate)
      .map((col) => {
        const func = col.aggregate!;
        if (func === 'ANY') {
          return `MIN(${col.field}) AS ${col.field}`;
        }
        return `${func}(${col.field}) AS ${col.field}`;
      })
      .join(', ');

    if (!selectClauses) {
      return {};
    }

    const query = `
      SELECT ${selectClauses}
      FROM (${filteredBaseQuery})
    `;

    const result = await runQueryForQueryTable(
      this.wrapQueryWithPrelude(query),
      this.engine,
    );
    return result.rows[0] ?? {};
  }

  private async getSchemaColumnAggregates(
    filters: ReadonlyArray<Filter>,
    columns: ReadonlyArray<Column>,
  ): Promise<Row> {
    const resolver = new SQLSchemaResolver(
      this.sqlSchema!,
      this.rootSchemaName!,
    );

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

  private async getRows(
    workingQuery: string,
    pagination?: Pagination,
  ): Promise<{offset: number; rows: Row[]}> {
    let query = `
      WITH data AS (${workingQuery})
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

    return {
      offset: pagination?.offset ?? 0,
      rows: result.rows,
    };
  }
}

function simpleFilter2Sql(filter: Filter): string {
  switch (filter.op) {
    case '=':
    case '!=':
    case '<':
    case '<=':
    case '>':
    case '>=':
      return `${filter.field} ${filter.op} ${sqlValue(filter.value)}`;
    case 'glob':
      return `${filter.field} GLOB ${sqlValue(filter.value)}`;
    case 'not glob':
      return `${filter.field} NOT GLOB ${sqlValue(filter.value)}`;
    case 'is null':
      return `${filter.field} IS NULL`;
    case 'is not null':
      return `${filter.field} IS NOT NULL`;
    case 'in':
      return `${filter.field} IN (${filter.value.map(sqlValue).join(', ')})`;
    case 'not in':
      return `${filter.field} NOT IN (${filter.value.map(sqlValue).join(', ')})`;
    default:
      assertUnreachable(filter);
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

function arePivotsEqual(a?: Pivot, b?: Pivot): boolean {
  if (a === b) return true;
  if (a === undefined || b === undefined) return false;

  // Compare groupBy fields
  const aGroupBy = a.groupBy.map(({field}) => field).join(',');
  const bGroupBy = b.groupBy.map(({field}) => field).join(',');
  if (aGroupBy !== bGroupBy) return false;

  // Compare aggregates
  if (JSON.stringify(a.aggregates) !== JSON.stringify(b.aggregates)) {
    return false;
  }
  if (JSON.stringify(a.drillDown) !== JSON.stringify(b.drillDown)) return false;

  return true;
}
