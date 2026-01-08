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

import {assertUnreachable} from '../../../base/logging';
import {maybeUndefined} from '../../../base/utils';
import {Engine} from '../../../trace_processor/engine';
import {
  createQueryCache,
  UseQueryResult,
} from '../../../trace_processor/query_cache';
import {
  NUM,
  QueryResult,
  Row,
  SqlValue,
} from '../../../trace_processor/query_result';
import {runQueryForQueryTable} from '../../query_table/queries';
import {DataSource, DataSourceModel, PivotRollups} from './data_source';
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
  private readonly sqlSchema: SQLSchemaRegistry;
  private readonly rootSchemaName: string;
  private readonly prelude?: string;
  private readonly queryCache: ReturnType<typeof createQueryCache>;

  // Track the last built query for exportData
  private lastRowsQuery?: string;

  constructor(config: SQLDataSourceConfig) {
    this.engine = config.engine;
    this.sqlSchema = config.sqlSchema;
    this.rootSchemaName = config.rootSchemaName;
    this.prelude = config.preamble;
    this.queryCache = createQueryCache(this.engine);
  }

  getRows(
    model: DataSourceModel,
  ): UseQueryResult<{totalRows: number; offset: number; rows: Row[]}> {
    const {pagination} = model;

    // Build base query (without ORDER BY for counting)
    const baseQuery = this.buildQuery(model, {includeOrderBy: false});

    // Count query
    const countQuery = this.wrapQueryWithPrelude(`
      WITH data AS (${baseQuery})
      SELECT COUNT(*) AS total_count
      FROM data
    `);
    const countResult = this.queryCache.useQuery(countQuery);
    if (countResult.isLoading) {
      return {isLoading: true};
    }

    const rowCount = countResult.result.firstRow({
      total_count: NUM,
    }).total_count;

    // Rows query (with ORDER BY for proper pagination)
    const rowsQuery = this.buildQuery(model, {includeOrderBy: true});
    this.lastRowsQuery = rowsQuery; // Track for exportData
    let query = `
      WITH data AS (${rowsQuery})
      SELECT *
      FROM data
    `;
    if (pagination) {
      query += `LIMIT ${pagination.limit} OFFSET ${pagination.offset}`;
    }

    const rowsResult = this.queryCache.useQuery(
      this.wrapQueryWithPrelude(query),
    );
    if (rowsResult.isLoading) {
      return {
        result: {
          totalRows: rowCount,
          offset: 0,
          rows: [],
        },
        isLoading: true,
      };
    }

    const offset = pagination?.offset ?? 0;
    const rows = queryResultToRows(rowsResult.result);

    return {
      result: {totalRows: rowCount, offset, rows},
      isLoading: countResult.isLoading || rowsResult.isLoading,
    };
  }

  getDistinctValues(columnPath: string): UseQueryResult<readonly SqlValue[]> {
    const query = this.buildDistinctValuesQuery(columnPath);
    if (!query) {
      return {result: [], isLoading: false};
    }

    const {result, isLoading} = this.queryCache.useQuery(
      this.wrapQueryWithPrelude(query),
    );

    if (isLoading) {
      return {isLoading: true};
    }

    const values = queryResultToRows(result).map((r) => r['value']);
    return {result: values, isLoading: false};
  }

  getAggregateTotals(
    model: DataSourceModel,
  ): UseQueryResult<ReadonlyMap<string, SqlValue>> {
    const {columns, filters = [], pivot} = model;

    // Pivot aggregates (but not drill-down mode)
    const pivotAggregates = pivot?.aggregates ?? [];
    if (pivot && !pivot.drillDown && pivotAggregates.length > 0) {
      const query = this.buildPivotAggregateQuery(filters, pivot);
      if (!query) {
        return {result: new Map(), isLoading: false};
      }

      const {result, isLoading} = this.queryCache.useQuery(
        this.wrapQueryWithPrelude(query),
      );

      if (isLoading) {
        return {isLoading: true};
      }

      const row = queryResultToRows(result)[0] ?? {};
      const totals = new Map<string, SqlValue>(Object.entries(row));
      return {result: totals, isLoading: false};
    }

    // Column-level aggregations (non-pivot mode)
    const columnsWithAggregation = columns?.filter((c) => c.aggregate);
    if (columnsWithAggregation && columnsWithAggregation.length > 0 && !pivot) {
      const query = this.buildColumnAggregateQuery(
        filters,
        columnsWithAggregation,
      );
      if (!query) {
        return {result: new Map(), isLoading: false};
      }

      const {result, isLoading} = this.queryCache.useQuery(
        this.wrapQueryWithPrelude(query),
      );

      if (isLoading) {
        return {isLoading: true};
      }

      const row = queryResultToRows(result)[0] ?? {};
      const totals = new Map<string, SqlValue>(Object.entries(row));
      return {result: totals, isLoading: false};
    }

    return {result: new Map(), isLoading: false};
  }

  getPivotRollups(model: DataSourceModel): UseQueryResult<PivotRollups> {
    const {pivot, filters = []} = model;

    // Only resolve rollups for hierarchical pivot mode (2+ groupBy columns)
    if (!pivot || pivot.drillDown || pivot.groupBy.length < 2) {
      return {result: {byLevel: new Map()}, isLoading: false};
    }

    const byLevel = new Map<number, Row[]>();
    let anyLoading = false;

    // For N groupBy columns, we need rollups for levels 0 to N-2
    for (let level = 0; level < pivot.groupBy.length - 1; level++) {
      const query = this.buildRollupQuery(filters, pivot, level);
      const {result, isLoading} = this.queryCache.useQuery(
        this.wrapQueryWithPrelude(query),
      );

      if (isLoading) {
        anyLoading = true;
      } else {
        byLevel.set(level, queryResultToRows(result));
      }
    }

    if (anyLoading) {
      return {result: {byLevel}, isLoading: true};
    }

    return {result: {byLevel}, isLoading: false};
  }

  getParameterKeys(prefix: string): UseQueryResult<readonly string[]> {
    const schema = this.sqlSchema[this.rootSchemaName];
    const colDef = maybeUndefined(schema?.columns[prefix]);

    if (colDef && isSQLExpressionDef(colDef) && colDef.parameterized) {
      if (colDef.parameterKeysQuery) {
        const baseTable = schema.table;
        const baseAlias = `${baseTable}_0`;
        const query = colDef.parameterKeysQuery(baseTable, baseAlias);

        const {result, isLoading} = this.queryCache.useQuery(
          this.wrapQueryWithPrelude(query),
        );

        if (isLoading) {
          return {isLoading: true};
        }

        const keys = queryResultToRows(result).map((r) => String(r['key']));
        return {result: keys, isLoading: false};
      }
    }

    return {result: [], isLoading: false};
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
    if (!this.lastRowsQuery) {
      return [];
    }

    const query = `SELECT * FROM (${this.lastRowsQuery})`;
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

    // Add drill-down conditions (only for fields present in drillDown)
    if (pivot?.drillDown) {
      const drillDownConditions = pivot.groupBy
        .filter((col) => col.field in pivot.drillDown!)
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
   * Builds a pivot aggregate query.
   */
  private buildPivotAggregateQuery(
    filters: ReadonlyArray<Filter>,
    pivot: Pivot,
  ): string | undefined {
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
      return undefined;
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

    return query;
  }

  /**
   * Builds a column aggregate query.
   */
  private buildColumnAggregateQuery(
    filters: ReadonlyArray<Filter>,
    columns: ReadonlyArray<Column>,
  ): string | undefined {
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
      return undefined;
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

    return query;
  }

  /**
   * Builds a rollup query for a specific grouping level.
   */
  private buildRollupQuery(
    filters: ReadonlyArray<Filter>,
    pivot: Pivot,
    level: number,
  ): string {
    const resolver = new SQLSchemaResolver(this.sqlSchema, this.rootSchemaName);

    const baseTable = resolver.getBaseTable();
    const baseAlias = resolver.getBaseAlias();

    // Get groupBy columns for this level (first N+1 columns for level N)
    const groupByColumnsForLevel = pivot.groupBy.slice(0, level + 1);

    // Build groupBy expressions
    const groupByExprs: string[] = [];
    const groupByFields: string[] = [];

    for (const col of groupByColumnsForLevel) {
      const field = col.field;
      groupByFields.push(field);
      const sqlExpr = resolver.resolveColumnPath(field);
      if (sqlExpr) {
        const alias = this.pathToAlias(field);
        groupByExprs.push(`${sqlExpr} AS ${alias}`);
      }
    }

    // Build aggregate expressions
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

    // Resolve filter columns to ensure JOINs are added
    for (const filter of filters) {
      resolver.resolveColumnPath(filter.field);
    }

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
    if (groupByFields.length > 0) {
      const groupByOrigExprs = groupByFields.map(
        (field) => resolver.resolveColumnPath(field) ?? field,
      );
      query += `\nGROUP BY ${groupByOrigExprs.join(', ')}`;
    }

    // Add ORDER BY based on the groupBy columns' sort settings
    const sortedCol = groupByColumnsForLevel.find((col) => col.sort);
    if (sortedCol) {
      const alias = this.pathToAlias(sortedCol.field);
      query += `\nORDER BY ${alias} ${sortedCol.sort}`;
    }

    return query;
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

function queryResultToRows(queryResult: QueryResult): Row[] {
  const rows: Row[] = [];
  const columns = queryResult.columns();
  for (const iter = queryResult.iter({}); iter.valid(); iter.next()) {
    const row: Row = {};
    for (const colName of columns) {
      row[colName] = iter.get(colName);
    }
    rows.push(row);
  }
  return rows;
}
