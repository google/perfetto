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

import {
  QueryResult,
  QuerySlot,
  SerialTaskQueue,
} from '../../../base/query_slot';
import {exists, maybeUndefined} from '../../../base/utils';
import {Engine} from '../../../trace_processor/engine';
import {NUM, Row, SqlValue} from '../../../trace_processor/query_result';
import {
  createVirtualTable,
  DisposableSqlEntity,
} from '../../../trace_processor/sql_utils';
import {runQueryForQueryTable} from '../../query_table/queries';
import {
  DataSource,
  DataSourceModel,
  Pagination,
  RowsQueryResult,
} from './data_source';
import {AggregateFunction, Column, Filter, IdBasedTree, Pivot} from './model';
import {
  isSQLExpressionDef,
  SQLSchemaRegistry,
  SQLSchemaResolver,
} from './sql_schema';
import {filterToSql, sqlValue, toAlias} from './sql_utils';

export function ensure<T>(x: T | null | undefined): asserts x is T {
  if (!exists(x)) {
    throw new Error('Value is null or undefined');
  }
}

// Get the virtual table name
const pivotTableName = '__intrinsic_pivot_default__';

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

// Result types for QuerySlots
interface RowsResult {
  rowOffset: number;
  totalRows: number;
  rows: Row[];
  query: string;
}

interface AggregatesResult {
  totals: Map<string, SqlValue>;
}

interface DistinctValuesResult {
  values: Map<string, ReadonlyArray<SqlValue>>;
}

interface DistinctValuesKey {
  columns: ReadonlyArray<string>;
}

// Simplified filter representation for cache keys
// Values are converted to strings for consistent serialization
interface FilterKey {
  field: string;
  op: string;
  value?: string;
}

interface TreeTableKey {
  filters: ReadonlyArray<FilterKey>;
  idColumn: string;
  parentIdColumn: string;
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
  private readonly taskQueue = new SerialTaskQueue();
  private readonly sqlSchema: SQLSchemaRegistry;
  private readonly rootSchemaName: string;
  private readonly prelude?: string;

  // QuerySlots for each data type
  private readonly rowsSlot: QuerySlot<RowsResult>;
  private readonly aggregatesSlot: QuerySlot<AggregatesResult>;
  private readonly distinctValuesSlot: QuerySlot<DistinctValuesResult>;
  private readonly pivotTableSlot: QuerySlot<DisposableSqlEntity>;
  private readonly treeTableSlot: QuerySlot<DisposableSqlEntity>;

  // Cache for parameter keys (simple cache, not using QuerySlot)
  private parameterKeysCache = new Map<string, ReadonlyArray<string>>();

  private currentTreeTableName?: string;

  // Track the last working query for exportData
  private lastWorkingQuery?: string;

  constructor(config: SQLDataSourceConfig) {
    this.engine = config.engine;
    this.sqlSchema = config.sqlSchema;
    this.rootSchemaName = config.rootSchemaName;
    this.prelude = config.preamble;

    // Initialize QuerySlots with shared task queue
    this.rowsSlot = new QuerySlot(this.taskQueue);
    this.aggregatesSlot = new QuerySlot(this.taskQueue);
    this.distinctValuesSlot = new QuerySlot(this.taskQueue);
    this.pivotTableSlot = new QuerySlot(this.taskQueue);
    this.treeTableSlot = new QuerySlot(this.taskQueue);
  }

  /**
   * Fetch rows for the current model state.
   * Call every render with the current model to get rows and trigger updates.
   */
  useRows(model: DataSourceModel): RowsQueryResult {
    const {pivot, idBasedTree} = model;

    // Dispatch to the appropriate mode handler
    if (idBasedTree) {
      return this.useRowsTreeMode(model);
    } else if (pivot) {
      if (pivot.drillDown) {
        return this.useRowsPivotDrilldownMode(model);
      } else if (pivot.collapsibleGroups) {
        return this.useRowsPivotMode(model);
      } else {
        return this.useRowsFlatPivotMode(model);
      }
    } else {
      return this.useRowsFlatMode(model);
    }
  }

  /**
   * Fetch distinct values for filter dropdowns.
   */
  useDistinctValues(
    model: DataSourceModel,
  ): QueryResult<ReadonlyMap<string, readonly SqlValue[]>> {
    const {distinctValuesColumns} = model;

    if (!distinctValuesColumns || distinctValuesColumns.size === 0) {
      return {data: undefined, isPending: false, isFresh: true};
    }

    const columns = Array.from(distinctValuesColumns).sort();
    const result = this.distinctValuesSlot.use({
      key: {columns} as DistinctValuesKey,
      queryFn: async () => this.fetchDistinctValues(model),
    });

    return {
      data: result.data?.values,
      isPending: result.isPending,
      isFresh: result.isFresh,
    };
  }

  /**
   * Fetch parameter keys for parameterized columns.
   */
  useParameterKeys(
    model: DataSourceModel,
  ): QueryResult<ReadonlyMap<string, readonly string[]>> {
    // Resolve parameter keys (synchronously cached)
    this.resolveParameterKeys(model);

    // Return current cache state
    const data =
      this.parameterKeysCache.size > 0 ? this.parameterKeysCache : undefined;
    return {data, isPending: false, isFresh: true};
  }

  /**
   * Fetch aggregate totals (grand totals across all filtered rows).
   */
  useAggregateTotals(
    model: DataSourceModel,
  ): QueryResult<ReadonlyMap<string, SqlValue>> {
    const {pivot, idBasedTree} = model;

    // No aggregates in tree mode or drill-down mode
    if (idBasedTree || pivot?.drillDown) {
      return {data: undefined, isPending: false, isFresh: true};
    }

    const aggregateQueryKey = this.buildAggregateQueryKey(model);
    if (!aggregateQueryKey) {
      return {data: undefined, isPending: false, isFresh: true};
    }

    const result = this.aggregatesSlot.use({
      key: {queryKey: aggregateQueryKey},
      queryFn: async () => this.fetchAggregates(model),
      enabled: true,
    });

    return {
      data: result.data?.totals,
      isPending: result.isPending,
      isFresh: result.isFresh,
    };
  }

  /**
   * Flat mode: Regular table view without pivot or tree structure.
   */
  private useRowsFlatMode(model: DataSourceModel): RowsQueryResult {
    const {pagination} = model;

    const rowsQuery = this.buildQuery(model, {
      includeOrderBy: true,
      pagination: undefined, // Pagination handled in fetchRows
    });

    const result = this.rowsSlot.use({
      key: {query: rowsQuery, pagination},
      queryFn: async () => this.fetchRows(model, rowsQuery),
      enabled: true,
      retainOn: ['pagination'],
    });

    return this.toRowsQueryResult(result, rowsQuery);
  }

  /**
   * Pivot mode with collapsible groups: Uses __intrinsic_pivot virtual table.
   */
  private useRowsPivotMode(model: DataSourceModel): RowsQueryResult {
    const {pagination, pivot, filters = []} = model;
    ensure(pivot);

    // The table key is based on:
    // - Group columns in the pivot (not sort)
    // - Filters
    // - Aggregate columns and functions
    const tableKey = {
      filters: filters.map((f) => ({
        field: f.field,
        op: f.op,
        value: 'value' in f ? String(f.value) : undefined,
      })),
      groupBy: pivot.groupBy.map((g) => g.field),
      aggregates: (pivot.aggregates ?? []).map((a) => ({
        function: a.function,
        field: 'field' in a ? a.field : undefined,
      })),
    };

    const sorting = pivot.groupBy
      .map((c) => ({field: c.field, sort: c.sort}))
      .concat(pivot.aggregates.map((a) => ({field: a.id, sort: a.sort})));

    // The rows key is based on:
    // - The table key
    // - Pagination
    // - Sorting
    // - Expand/collapse state
    const rowsKey = {
      table: tableKey,
      pagination,
      sort: sorting,
      expandedIds: pivot.expandedIds
        ? Array.from(pivot.expandedIds).sort()
        : [],
      collapsedIds: pivot.collapsedIds
        ? Array.from(pivot.collapsedIds).sort()
        : [],
    };

    // First we use the intrinsic pivot virtual table
    const {data: virtualTable, isPending: tableIsPending} =
      this.pivotTableSlot.use({
        key: tableKey,
        queryFn: async () => this.createIntrinsicPivotTable(pivot, filters),
      });

    if (tableIsPending) {
      console.log('Pivot table loading...', tableKey);
    }

    // Build query (pagination handled by virtual table)
    const rowsQuery = this.buildQuery(model, {
      includeOrderBy: true,
      pagination,
    });

    // Schedule rows fetch (depends on virtual table being ready)
    const result = this.rowsSlot.use({
      key: rowsKey,
      queryFn: async () => this.fetchRows(model, rowsQuery),
      enabled: !!virtualTable,
      retainOn: ['pagination', 'expandedIds', 'collapsedIds'],
    });

    if (result.isPending) {
      console.log('Rows query loading...', rowsKey, result.isFresh, result.data);
    }

    return this.toRowsQueryResult(result, rowsQuery);
  }

  /**
   * Flat pivot mode: Simple GROUP BY aggregation without hierarchy.
   */
  private useRowsFlatPivotMode(model: DataSourceModel): RowsQueryResult {
    const {pagination} = model;

    const rowsQuery = this.buildQuery(model, {
      includeOrderBy: true,
      pagination: undefined, // Pagination handled in fetchRows
    });

    const result = this.rowsSlot.use({
      key: {query: rowsQuery, pagination},
      queryFn: async () => this.fetchRows(model, rowsQuery),
      enabled: true,
      retainOn: ['pagination'],
    });

    return this.toRowsQueryResult(result, rowsQuery);
  }

  /**
   * Pivot drill-down mode: Shows individual rows filtered by pivot group.
   */
  private useRowsPivotDrilldownMode(model: DataSourceModel): RowsQueryResult {
    const {pagination} = model;

    const rowsQuery = this.buildQuery(model, {
      includeOrderBy: true,
      pagination: undefined, // Pagination handled in fetchRows
    });

    const result = this.rowsSlot.use({
      key: {query: rowsQuery, pagination},
      queryFn: async () => this.fetchRows(model, rowsQuery),
      enabled: true,
      retainOn: ['pagination'],
    });

    return this.toRowsQueryResult(result, rowsQuery);
  }

  /**
   * Tree mode: Uses __intrinsic_tree virtual table.
   */
  private useRowsTreeMode(model: DataSourceModel): RowsQueryResult {
    const {pagination} = model;

    // Ensure tree table is created
    this.ensureTreeTable(model);

    // Build query (pagination handled by virtual table)
    const rowsQuery = this.buildQuery(model, {
      includeOrderBy: true,
      pagination,
    });

    // Schedule rows fetch (depends on virtual table being ready)
    const virtualTableReady = this.currentTreeTableName !== undefined;
    const result = this.rowsSlot.use({
      key: {query: rowsQuery, pagination},
      queryFn: async () => this.fetchRows(model, rowsQuery),
      enabled: virtualTableReady,
      retainOn: ['pagination'],
    });

    return this.toRowsQueryResult(result, rowsQuery);
  }

  /**
   * Ensures the tree virtual table is created/updated.
   */
  private ensureTreeTable(model: DataSourceModel): void {
    const {filters = [], idBasedTree} = model;
    if (!idBasedTree) return;

    const treeKey: TreeTableKey = {
      filters: filters.map((f) => ({
        field: f.field,
        op: f.op,
        value: 'value' in f ? String(f.value) : undefined,
      })),
      idColumn: idBasedTree.idColumn,
      parentIdColumn: idBasedTree.parentIdColumn,
    };
    const treeResult = this.treeTableSlot.use({
      key: treeKey,
      queryFn: async () => this.createIntrinsicTreeTable(model),
    });
    this.currentTreeTableName = treeResult.data?.name;
  }

  /**
   * Converts internal QueryResult to RowsQueryResult.
   */
  private toRowsQueryResult(
    result: QueryResult<RowsResult>,
    query: string,
  ): RowsQueryResult {
    const data = result.data
      ? {
          rowOffset: result.data.rowOffset,
          totalRows: result.data.totalRows,
          rows: result.data.rows,
        }
      : undefined;

    const workingQuery = result.data?.query ?? query;

    // Track for exportData
    if (result.data?.query) {
      this.lastWorkingQuery = result.data.query;
    }

    return {
      data,
      isPending: result.isPending,
      isFresh: result.isFresh,
      query: workingQuery,
    };
  }

  /**
   * Fetches rows for the current model.
   */
  private async fetchRows(
    model: DataSourceModel,
    rowsQuery: string,
  ): Promise<RowsResult> {
    const {pagination, pivot, idBasedTree} = model;

    // Get row count
    const countQuery = this.buildQuery(model, {includeOrderBy: false});
    const countResult = await this.engine.query(
      this.wrapQueryWithPrelude(`
      WITH data AS (${countQuery})
      SELECT COUNT(*) AS total_count
      FROM data
    `),
    );
    const totalRows = countResult.firstRow({total_count: NUM}).total_count;

    // Fetch rows
    const usesVirtualTable =
      (pivot !== undefined && !pivot.drillDown && pivot.collapsibleGroups) ||
      idBasedTree !== undefined;

    let query = `
      WITH data AS (${rowsQuery})
      SELECT *
      FROM data
    `;

    if (pagination && !usesVirtualTable) {
      query += `LIMIT ${pagination.limit} OFFSET ${pagination.offset}`;
    }

    const result = await runQueryForQueryTable(
      this.wrapQueryWithPrelude(query),
      this.engine,
    );

    return {
      rowOffset: pagination?.offset ?? 0,
      totalRows,
      rows: result.rows as Row[],
      query: rowsQuery,
    };
  }

  /**
   * Fetches aggregate totals for the current model.
   */
  private async fetchAggregates(
    model: DataSourceModel,
  ): Promise<AggregatesResult> {
    const {columns, filters = [], pivot} = model;
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

    return {totals};
  }

  /**
   * Fetches distinct values for requested columns.
   */
  private async fetchDistinctValues(
    model: DataSourceModel,
  ): Promise<DistinctValuesResult> {
    const {distinctValuesColumns} = model;
    const values = new Map<string, ReadonlyArray<SqlValue>>();

    if (!distinctValuesColumns) {
      return {values};
    }

    for (const columnPath of distinctValuesColumns) {
      const query = this.buildDistinctValuesQuery(columnPath);
      if (!query) continue;

      const queryResult = await runQueryForQueryTable(
        this.wrapQueryWithPrelude(query),
        this.engine,
      );
      values.set(
        columnPath,
        queryResult.rows.map((r) => r['value']),
      );
    }

    return {values};
  }

  /**
   * Builds a unique string representing the aggregate query for cache comparison.
   */
  private buildAggregateQueryKey(model: DataSourceModel): string | undefined {
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
   * Resolves parameter keys for parameterized columns.
   */
  private resolveParameterKeys(model: DataSourceModel): void {
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

          // Schedule async fetch through task queue
          this.taskQueue.schedule(this, async () => {
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
          });
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
    if (!this.lastWorkingQuery) {
      return [];
    }

    const query = `SELECT * FROM (${this.lastWorkingQuery})`;
    const result = await runQueryForQueryTable(
      this.wrapQueryWithPrelude(query),
      this.engine,
    );
    return result.rows as Row[];
  }

  /**
   * Creates the intrinsic pivot virtual table.
   * Returns a DisposableSqlEntity that will drop the table when disposed.
   */
  private async createIntrinsicPivotTable(
    pivot: Pivot,
    filters: readonly Filter[],
  ): Promise<DisposableSqlEntity> {
    const resolver = new SQLSchemaResolver(this.sqlSchema, this.rootSchemaName);
    const baseTable = resolver.getBaseTable();
    const baseAlias = resolver.getBaseAlias();

    // Build the base table expression (with filters as subquery if needed)
    let sourceTable = baseTable;
    if (filters.length > 0) {
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
        return aggregateFunctionToSql(a.function, field);
      })
      .join(', ');

    // Build the USING clause for the virtual table
    const usingClause = `__intrinsic_pivot(
  "${sourceTable.replace(/"/g, '""')}",
  '${hierarchyCols}',
  '${aggExprs}'
)`;

    return await createVirtualTable({
      engine: this.engine,
      using: usingClause,
      name: pivotTableName,
    });
  }

  /**
   * Creates the intrinsic tree virtual table.
   * Returns a DisposableSqlEntity that will drop the table when disposed.
   */
  private async createIntrinsicTreeTable(
    model: DataSourceModel,
  ): Promise<DisposableSqlEntity> {
    const {filters = [], idBasedTree} = model;
    if (!idBasedTree) throw new Error('idBasedTree required');

    const resolver = new SQLSchemaResolver(this.sqlSchema, this.rootSchemaName);
    const baseTable = resolver.getBaseTable();
    const baseAlias = resolver.getBaseAlias();

    // Build the base table expression (with filters as subquery if needed)
    let sourceTable = baseTable;
    if (filters.length > 0) {
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

    // Build the USING clause for the virtual table
    const usingClause = `__intrinsic_tree(
  "${sourceTable.replace(/"/g, '""')}",
  '${idBasedTree.idColumn}',
  '${idBasedTree.parentIdColumn}'
)`;

    return await createVirtualTable({
      engine: this.engine,
      using: usingClause,
    });
  }

  /**
   * Builds a complete SQL query from the model.
   */
  private buildQuery(
    model: DataSourceModel,
    options: {includeOrderBy: boolean; pagination?: Pagination},
  ): string {
    const {columns, filters = [], pivot, idBasedTree} = model;

    const resolver = new SQLSchemaResolver(this.sqlSchema, this.rootSchemaName);

    const baseTable = resolver.getBaseTable();
    const baseAlias = resolver.getBaseAlias();

    // For pivot mode without drill-down, we build aggregates differently
    if (pivot && !pivot.drillDown) {
      // Flat mode (no collapsible groups) uses simple GROUP BY
      // Tree mode (collapsible groups) uses the intrinsic pivot virtual table
      if (pivot.collapsibleGroups) {
        return this.buildPivotQuery(resolver, filters, pivot, options, columns);
      } else {
        return this.buildFlatPivotQuery(resolver, filters, pivot, options);
      }
    }

    // ID-based tree mode: uses __intrinsic_tree virtual table
    if (idBasedTree) {
      return this.buildIdBasedTreeQuery(idBasedTree, columns, options);
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
    options: {includeOrderBy: boolean; pagination?: Pagination},
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
   * Builds a flat pivot query using simple GROUP BY (no tree structure).
   * Used when collapsibleGroups is false - just aggregation without hierarchy.
   */
  private buildFlatPivotQuery(
    resolver: SQLSchemaResolver,
    filters: ReadonlyArray<Filter>,
    pivot: Pivot,
    options: {includeOrderBy: boolean; pagination?: Pagination},
  ): string {
    const baseTable = resolver.getBaseTable();
    const baseAlias = resolver.getBaseAlias();

    // Build SELECT clause with groupBy columns and aggregates
    const selectClauses: string[] = [];
    const groupByClauses: string[] = [];

    // Add groupBy columns
    for (const col of pivot.groupBy) {
      const sqlExpr = resolver.resolveColumnPath(col.field);
      if (sqlExpr) {
        const alias = toAlias(col.id);
        selectClauses.push(`${sqlExpr} AS ${alias}`);
        groupByClauses.push(sqlExpr);
      }
    }

    // Add aggregate columns
    const aggregates = pivot.aggregates ?? [];
    for (const agg of aggregates) {
      const alias = toAlias(agg.id);
      if (agg.function === 'COUNT') {
        selectClauses.push(`COUNT(*) AS ${alias}`);
      } else {
        const sqlExpr = resolver.resolveColumnPath(agg.field);
        if (sqlExpr) {
          selectClauses.push(
            `${aggregateFunctionToSql(agg.function, sqlExpr)} AS ${alias}`,
          );
        }
      }
    }

    // Resolve filter paths to ensure JOINs are added
    for (const filter of filters) {
      resolver.resolveColumnPath(filter.field);
    }

    const joinClauses = resolver.buildJoinClauses();

    // Build WHERE clause from filters
    const whereConditions = filters
      .map((f) => {
        const sqlExpr = resolver.resolveColumnPath(f.field);
        return sqlExpr ? filterToSql(f, sqlExpr) : null;
      })
      .filter((c): c is string => c !== null);

    let query = `
SELECT ${selectClauses.join(',\n       ')}
FROM ${baseTable} AS ${baseAlias}
${joinClauses}`;

    if (whereConditions.length > 0) {
      query += `\nWHERE ${whereConditions.join('\n  AND ')}`;
    }

    if (groupByClauses.length > 0) {
      query += `\nGROUP BY ${groupByClauses.join(', ')}`;
    }

    // Add ORDER BY if requested
    if (options.includeOrderBy) {
      const sortedColumn = this.findSortedColumn(undefined, pivot);
      if (sortedColumn) {
        // Find the alias for this column
        const groupByCol = pivot.groupBy.find((g) => g.id === sortedColumn.id);
        const aggCol = aggregates.find((a) => a.id === sortedColumn.id);
        const alias = toAlias(sortedColumn.id);
        if (groupByCol || aggCol) {
          query += `\nORDER BY ${alias} ${sortedColumn.direction}`;
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
   */
  private buildIntrinsicPivotQuery(
    _resolver: SQLSchemaResolver,
    _filters: ReadonlyArray<Filter>,
    pivot: Pivot,
    options: {includeOrderBy: boolean; pagination?: Pagination},
    dependencyColumns?: ReadonlyArray<Column>,
  ): string {
    // Build expansion constraint - collapsedIds takes precedence if both set
    // collapsedIds = denylist mode (all expanded except listed)
    // expandedIds = allowlist mode (only listed are expanded)
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
    selectClauses.push('__depth__');
    selectClauses.push('__has_children__');
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
FROM ${pivotTableName}
WHERE ${expansionConstraint}
  AND __sort__ = '${sortSpec}'`;

    // Add pagination constraints - virtual table handles these efficiently
    if (options.pagination) {
      query += `\n  AND __offset__ = ${options.pagination.offset}`;
      query += `\n  AND __limit__ = ${options.pagination.limit}`;
    }

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
   * Builds a query using the __intrinsic_tree virtual table.
   * This passes through all source columns plus tree metadata columns.
   */
  private buildIdBasedTreeQuery(
    tree: IdBasedTree,
    columns: ReadonlyArray<Column> | undefined,
    options: {includeOrderBy: boolean; pagination?: Pagination},
  ): string {
    // Get the virtual table name
    const tableName = this.currentTreeTableName ?? '__intrinsic_tree_default__';

    // Build expansion constraint - collapsedIds takes precedence if both set
    let expansionConstraint: string;
    if ('collapsedIds' in tree && tree.collapsedIds !== undefined) {
      const collapsedIdsStr = Array.from(tree.collapsedIds).join(',');
      expansionConstraint = `__collapsed_ids__ = '${collapsedIdsStr}'`;
    } else if ('expandedIds' in tree && tree.expandedIds !== undefined) {
      const expandedIdsStr = Array.from(tree.expandedIds).join(',');
      expansionConstraint = `__expanded_ids__ = '${expandedIdsStr}'`;
    } else {
      // Default: all collapsed (allowlist mode with empty set)
      expansionConstraint = `__expanded_ids__ = ''`;
    }

    // Build sort spec from columns
    let sortSpec = '';
    if (columns) {
      for (const col of columns) {
        if (col.sort) {
          sortSpec = `${col.field} ${col.sort}`;
          break;
        }
      }
    }

    // Build the SELECT clause
    // The virtual table returns: source columns + __depth__ + __has_children__ + __child_count__
    const selectClauses: string[] = ['*'];

    // Build the query
    let query = `SELECT ${selectClauses.join(', ')}
FROM ${tableName}
WHERE ${expansionConstraint}`;

    // Add sort constraint if specified
    if (sortSpec) {
      query += `\n  AND __sort__ = '${sortSpec}'`;
    }

    // Add pagination constraints - virtual table handles these efficiently
    if (options.pagination) {
      query += `\n  AND __offset__ = ${options.pagination.offset}`;
      query += `\n  AND __limit__ = ${options.pagination.limit}`;
    }

    // Add ORDER BY if requested (virtual table handles sorting, but we preserve tree order)
    if (options.includeOrderBy) {
      query += '\nORDER BY rowid'; // Preserve tree order from virtual table
    }

    return query;
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

function aggregateFunctionToSql(
  func: AggregateFunction,
  fieldExpr: string,
): string {
  if (func === 'ANY') {
    return `MIN(${fieldExpr})`; // ANY maps to MIN
  }
  return `${func}(${fieldExpr})`;
}
