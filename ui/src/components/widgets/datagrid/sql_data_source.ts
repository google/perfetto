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
import {DataSource, DataSourceModel, DataSourceResult} from './data_source';
import {
  DataGridColumn,
  Filter,
  SortBy,
  SortByColumn,
  Pagination,
  PivotModel,
} from './model';
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

  private workingQuery = '';
  private pagination?: Pagination;
  private pivot?: PivotModel;
  private cachedResult?: DataSourceResult;
  private isLoadingFlag = false;
  private parameterKeysCache = new Map<string, ReadonlyArray<string>>();

  constructor(config: SQLDataSourceConfig) {
    this.engine = config.engine;
    this.baseQuery = config.baseQuery;
    this.sqlSchema = config.sqlSchema;
    this.rootSchemaName = config.rootSchemaName;

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
   * Notify of parameter changes and trigger data update
   */
  notify({
    columns,
    sorting = {direction: 'UNSORTED'},
    filters = [],
    pagination,
    pivot,
    distinctValuesColumns,
    parameterKeyColumns,
  }: DataSourceModel): void {
    this.limiter.schedule(async () => {
      this.isLoadingFlag = true;

      try {
        const workingQuery = this.buildWorkingQuery(
          columns,
          filters,
          sorting,
          pivot,
        );

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
          if (
            pivot &&
            !pivot.drillDown &&
            Object.keys(pivot.values).length > 0
          ) {
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
          const columnsWithAggregation = columns?.filter((c) => c.aggregation);
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
                const result = await runQueryForQueryTable(query, this.engine);
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
                      query,
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

  /**
   * Export all data with current filters/sorting applied.
   */
  async exportData(): Promise<Row[]> {
    if (!this.workingQuery) {
      return [];
    }

    const query = `SELECT * FROM (${this.workingQuery})`;
    const result = await runQueryForQueryTable(query, this.engine);
    return result.rows;
  }

  /**
   * Builds a complete SQL query based on the current mode.
   */
  private buildWorkingQuery(
    columns: ReadonlyArray<DataGridColumn> | undefined,
    filters: ReadonlyArray<Filter>,
    sorting: SortBy,
    pivot?: PivotModel,
  ): string {
    if (this.useSchema) {
      return this.buildSchemaWorkingQuery(columns, filters, sorting, pivot);
    } else {
      return this.buildSimpleWorkingQuery(columns, filters, sorting, pivot);
    }
  }

  /**
   * Builds a query for simple mode (no schema, direct column access).
   */
  private buildSimpleWorkingQuery(
    columns: ReadonlyArray<DataGridColumn> | undefined,
    filters: ReadonlyArray<Filter>,
    sorting: SortBy,
    pivot?: PivotModel,
  ): string {
    const colNames = columns?.map((c) => c.column) ?? ['*'];

    let query: string;

    if (pivot && !pivot.drillDown) {
      // Pivot mode: Build aggregate columns
      const valCols = Object.entries(pivot.values)
        .sort(([a], [b]) => a.localeCompare(b))
        .map(([key, value]) => {
          const alias = this.pathToAlias(key);
          if (value.func === 'COUNT') {
            return `COUNT(*) AS ${alias}`;
          }
          if (value.func === 'ANY') {
            return `MIN(${value.col}) AS ${alias}`;
          }
          return `${value.func}(${value.col}) AS ${alias}`;
        })
        .join(', ');

      if (pivot.groupBy.length > 0) {
        const groupCols = pivot.groupBy.join(', ');
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
          const value = pivot.drillDown![col];
          if (value === null) {
            return `${col} IS NULL`;
          }
          return `${col} = ${sqlValue(value)}`;
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

    // Add ORDER BY clause
    if (sorting.direction !== 'UNSORTED') {
      const {column, direction} = sorting as SortByColumn;
      let columnExists: boolean;
      if (pivot && !pivot.drillDown) {
        const pivotColumns = [...pivot.groupBy, ...Object.keys(pivot.values)];
        columnExists = pivotColumns.includes(column);
      } else {
        columnExists = columns === undefined || colNames.includes(column);
      }
      if (columnExists) {
        query += `\nORDER BY ${column} ${direction.toUpperCase()}`;
      }
    }

    return query;
  }

  /**
   * Builds a query for schema mode (with JOINs based on column paths).
   */
  private buildSchemaWorkingQuery(
    columns: ReadonlyArray<DataGridColumn> | undefined,
    filters: ReadonlyArray<Filter>,
    sorting: SortBy,
    pivot?: PivotModel,
  ): string {
    const resolver = new SQLSchemaResolver(
      this.sqlSchema!,
      this.rootSchemaName!,
    );

    const baseTable = resolver.getBaseTable();
    const baseAlias = resolver.getBaseAlias();

    // For pivot mode without drill-down, we build aggregates differently
    if (pivot && !pivot.drillDown) {
      return this.buildSchemaPivotQuery(resolver, filters, sorting, pivot);
    }

    // Normal mode or drill-down: select individual columns
    const colPaths = columns?.map((c) => c.column) ?? [];
    const selectExprs: string[] = [];

    for (const path of colPaths) {
      const sqlExpr = resolver.resolveColumnPath(path);
      if (sqlExpr) {
        const alias = this.pathToAlias(path);
        selectExprs.push(`${sqlExpr} AS ${alias}`);
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
        const sqlExpr = resolver.resolveColumnPath(filter.column);
        if (!sqlExpr) {
          return this.filterToSql(filter, filter.column);
        }
        return this.filterToSql(filter, sqlExpr);
      });
      query += `\nWHERE ${whereConditions.join(' AND ')}`;
    }

    // Add drill-down conditions
    if (pivot?.drillDown) {
      const drillDownConditions = pivot.groupBy
        .map((col) => {
          const value = pivot.drillDown![col];
          const sqlExpr = resolver.resolveColumnPath(col) ?? col;
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

    // Add ORDER BY clause
    if (sorting.direction !== 'UNSORTED') {
      const {column, direction} = sorting as SortByColumn;
      const sqlExpr = resolver.resolveColumnPath(column);
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
    sorting: SortBy,
    pivot: PivotModel,
  ): string {
    const baseTable = resolver.getBaseTable();
    const baseAlias = resolver.getBaseAlias();

    // Resolve groupBy columns
    const groupByExprs: string[] = [];
    const groupByAliases: string[] = [];

    for (const col of pivot.groupBy) {
      const sqlExpr = resolver.resolveColumnPath(col);
      if (sqlExpr) {
        const alias = this.pathToAlias(col);
        groupByExprs.push(`${sqlExpr} AS ${alias}`);
        groupByAliases.push(alias);
      }
    }

    // Build aggregate expressions
    const aggregateExprs = Object.entries(pivot.values)
      .sort(([a], [b]) => a.localeCompare(b))
      .map(([key, value]) => {
        const alias = this.pathToAlias(key);
        if (value.func === 'COUNT') {
          return `COUNT(*) AS ${alias}`;
        }
        const colExpr =
          'col' in value ? resolver.resolveColumnPath(value.col) : null;
        if (!colExpr) {
          return `NULL AS ${alias}`;
        }
        if (value.func === 'ANY') {
          return `MIN(${colExpr}) AS ${alias}`;
        }
        return `${value.func}(${colExpr}) AS ${alias}`;
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
        const sqlExpr = resolver.resolveColumnPath(filter.column);
        if (!sqlExpr) {
          return this.filterToSql(filter, filter.column);
        }
        return this.filterToSql(filter, sqlExpr);
      });
      query += `\nWHERE ${whereConditions.join(' AND ')}`;
    }

    // Add GROUP BY
    if (groupByAliases.length > 0) {
      const groupByOrigExprs = pivot.groupBy.map(
        (col) => resolver.resolveColumnPath(col) ?? col,
      );
      query += `\nGROUP BY ${groupByOrigExprs.join(', ')}`;
    }

    // Add ORDER BY
    if (sorting.direction !== 'UNSORTED') {
      const {column, direction} = sorting as SortByColumn;
      const pivotColumns = [...pivot.groupBy, ...Object.keys(pivot.values)];
      if (pivotColumns.includes(column)) {
        const alias = pivot.groupBy.includes(column)
          ? this.pathToAlias(column)
          : column;
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
    const result = await this.engine.query(`
      WITH data AS (${workingQuery})
      SELECT COUNT(*) AS total_count
      FROM data
    `);
    return result.firstRow({total_count: NUM}).total_count;
  }

  private async getPivotAggregates(
    _columns: ReadonlyArray<DataGridColumn> | undefined,
    filters: ReadonlyArray<Filter>,
    pivot: PivotModel,
  ): Promise<Row> {
    if (this.useSchema) {
      return this.getSchemaPivotAggregates(filters, pivot);
    } else {
      return this.getSimplePivotAggregates(filters, pivot);
    }
  }

  private async getSimplePivotAggregates(
    filters: ReadonlyArray<Filter>,
    pivot: PivotModel,
  ): Promise<Row> {
    let filteredBaseQuery = `SELECT * FROM (${this.baseQuery})`;
    if (filters.length > 0) {
      const whereConditions = filters.map(simpleFilter2Sql).join(' AND ');
      filteredBaseQuery = `SELECT * FROM (${filteredBaseQuery}) WHERE ${whereConditions}`;
    }

    const selectClauses = Object.entries(pivot.values)
      .sort(([a], [b]) => a.localeCompare(b))
      .map(([key, value]) => {
        const alias = this.pathToAlias(key);
        if (value.func === 'COUNT') {
          return `COUNT(*) AS ${alias}`;
        }
        if (value.func === 'ANY') {
          return `NULL AS ${alias}`;
        }
        return `${value.func}(${value.col}) AS ${alias}`;
      })
      .join(', ');

    if (!selectClauses) {
      return {};
    }

    const query = `
      SELECT ${selectClauses}
      FROM (${filteredBaseQuery})
    `;

    const result = await runQueryForQueryTable(query, this.engine);
    return result.rows[0] ?? {};
  }

  private async getSchemaPivotAggregates(
    filters: ReadonlyArray<Filter>,
    pivot: PivotModel,
  ): Promise<Row> {
    const resolver = new SQLSchemaResolver(
      this.sqlSchema!,
      this.rootSchemaName!,
    );

    const baseTable = resolver.getBaseTable();
    const baseAlias = resolver.getBaseAlias();

    // Resolve filter columns first to ensure JOINs are added
    for (const filter of filters) {
      resolver.resolveColumnPath(filter.column);
    }

    const selectClauses = Object.entries(pivot.values)
      .sort(([a], [b]) => a.localeCompare(b))
      .map(([key, value]) => {
        const alias = this.pathToAlias(key);
        if (value.func === 'COUNT') {
          return `COUNT(*) AS ${alias}`;
        }
        if (value.func === 'ANY') {
          return `NULL AS ${alias}`;
        }
        const colExpr =
          'col' in value ? resolver.resolveColumnPath(value.col) : null;
        if (!colExpr) {
          return `NULL AS ${alias}`;
        }
        return `${value.func}(${colExpr}) AS ${alias}`;
      })
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
        const sqlExpr = filterResolver.resolveColumnPath(filter.column);
        return this.filterToSql(filter, sqlExpr ?? filter.column);
      });
      query += `\nWHERE ${whereConditions.join(' AND ')}`;
    }

    const result = await runQueryForQueryTable(query, this.engine);
    return result.rows[0] ?? {};
  }

  private async getColumnAggregates(
    filters: ReadonlyArray<Filter>,
    columns: ReadonlyArray<DataGridColumn>,
  ): Promise<Row> {
    if (this.useSchema) {
      return this.getSchemaColumnAggregates(filters, columns);
    } else {
      return this.getSimpleColumnAggregates(filters, columns);
    }
  }

  private async getSimpleColumnAggregates(
    filters: ReadonlyArray<Filter>,
    columns: ReadonlyArray<DataGridColumn>,
  ): Promise<Row> {
    let filteredBaseQuery = `SELECT * FROM (${this.baseQuery})`;
    if (filters.length > 0) {
      const whereConditions = filters.map(simpleFilter2Sql).join(' AND ');
      filteredBaseQuery = `SELECT * FROM (${filteredBaseQuery}) WHERE ${whereConditions}`;
    }

    const selectClauses = columns
      .filter((col) => col.aggregation)
      .map((col) => {
        const func = col.aggregation!;
        if (func === 'COUNT') {
          return `COUNT(*) AS ${col.column}`;
        }
        if (func === 'ANY') {
          return `MIN(${col.column}) AS ${col.column}`;
        }
        return `${func}(${col.column}) AS ${col.column}`;
      })
      .join(', ');

    if (!selectClauses) {
      return {};
    }

    const query = `
      SELECT ${selectClauses}
      FROM (${filteredBaseQuery})
    `;

    const result = await runQueryForQueryTable(query, this.engine);
    return result.rows[0] ?? {};
  }

  private async getSchemaColumnAggregates(
    filters: ReadonlyArray<Filter>,
    columns: ReadonlyArray<DataGridColumn>,
  ): Promise<Row> {
    const resolver = new SQLSchemaResolver(
      this.sqlSchema!,
      this.rootSchemaName!,
    );

    const baseTable = resolver.getBaseTable();
    const baseAlias = resolver.getBaseAlias();

    const selectClauses = columns
      .filter((col) => col.aggregation)
      .map((col) => {
        const func = col.aggregation!;
        const colExpr = resolver.resolveColumnPath(col.column);
        const alias = this.pathToAlias(col.column);

        if (func === 'COUNT') {
          return `COUNT(*) AS ${alias}`;
        }
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

    const joinClauses = resolver.buildJoinClauses();

    let query = `
SELECT ${selectClauses}
FROM ${baseTable} AS ${baseAlias}
${joinClauses}`;

    if (filters.length > 0) {
      const whereConditions = filters.map((filter) => {
        const sqlExpr = resolver.resolveColumnPath(filter.column);
        return this.filterToSql(filter, sqlExpr ?? filter.column);
      });
      query += `\nWHERE ${whereConditions.join(' AND ')}`;
    }

    const result = await runQueryForQueryTable(query, this.engine);
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

    const result = await runQueryForQueryTable(query, this.engine);

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
      return `${filter.column} ${filter.op} ${sqlValue(filter.value)}`;
    case 'glob':
      return `${filter.column} GLOB ${sqlValue(filter.value)}`;
    case 'not glob':
      return `${filter.column} NOT GLOB ${sqlValue(filter.value)}`;
    case 'is null':
      return `${filter.column} IS NULL`;
    case 'is not null':
      return `${filter.column} IS NOT NULL`;
    case 'in':
      return `${filter.column} IN (${filter.value.map(sqlValue).join(', ')})`;
    case 'not in':
      return `${filter.column} NOT IN (${filter.value.map(sqlValue).join(', ')})`;
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

function arePivotsEqual(a?: PivotModel, b?: PivotModel): boolean {
  if (a === b) return true;
  if (a === undefined || b === undefined) return false;

  if (a.groupBy.join(',') !== b.groupBy.join(',')) return false;
  if (!arePivotValuesEqual(a.values, b.values)) return false;
  if (JSON.stringify(a.drillDown) !== JSON.stringify(b.drillDown)) return false;

  return true;
}

function arePivotValuesEqual(
  a: PivotModel['values'],
  b: PivotModel['values'],
): boolean {
  const keysA = Object.keys(a);
  const keysB = Object.keys(b);

  if (keysA.length !== keysB.length) return false;

  for (const key of keysA) {
    if (!(key in b)) return false;
    if (JSON.stringify(a[key]) !== JSON.stringify(b[key])) return false;
  }

  return true;
}
