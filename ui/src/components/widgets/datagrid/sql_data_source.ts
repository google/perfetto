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
import {exists} from '../../../base/utils';
import {Engine} from '../../../trace_processor/engine';
import {NUM, Row, SqlValue} from '../../../trace_processor/query_result';
import {DisposableSqlEntity} from '../../../trace_processor/sql_utils';
import {runQueryForQueryTable} from '../../query_table/queries';
import {
  DataSource,
  DataSourceModel,
  Pagination,
  RowsQueryResult,
} from './data_source';
import {AggregateFunction, Column, Filter, Pivot} from './model';
import {SQLSchemaRegistry, SQLSchemaResolver} from './sql_schema';
import {filterToSql, sqlValue, toAlias} from './sql_utils';
import {
  buildAggregateExpr,
  createPivotTable,
  queryPivotTable,
} from './pivot_operator';

export function ensure<T>(x: T | null | undefined): asserts x is T {
  if (!exists(x)) {
    throw new Error('Value is null or undefined');
  }
}

const pivotTableName = '__intrinsic_pivot_default__';

/**
 * Configuration for SQLDataSource.
 */
export interface SQLDataSourceConfig {
  readonly engine: Engine;
  readonly sqlSchema: SQLSchemaRegistry;
  readonly rootSchemaName: string;
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

interface SortSpec {
  id: string;
  field: string;
  direction: 'ASC' | 'DESC';
}

interface NormalizedRequest {
  columns: readonly Column[];
  filters: readonly Filter[];
  pagination?: Pagination;
  pivot?: Pivot;
  sort?: SortSpec;
}

/**
 * SQL data source for DataGrid.
 *
 * Simplified version: supports flat mode and pivot mode.
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
  private readonly pivotTableSlot: QuerySlot<DisposableSqlEntity>;

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
    this.pivotTableSlot = new QuerySlot(this.taskQueue);
  }

  /**
   * Creates a fresh SQLSchemaResolver for query building.
   * Each query needs its own resolver since it tracks JOIN state.
   */
  private createResolver(): SQLSchemaResolver {
    return new SQLSchemaResolver(this.sqlSchema, this.rootSchemaName);
  }

  /**
   * Fetch rows for the current model state.
   */
  useRows(model: DataSourceModel): RowsQueryResult {
    // Normalize the model into a cleaner working structure
    const req = this.normalizeRequest(model);

    if (req.pivot) {
      if (req.pivot.drillDown) {
        return this.useRowsPivotDrilldownMode(req);
      } else if (req.pivot.collapsibleGroups) {
        return this.useRowsPivotMode(req);
      } else {
        return this.useRowsFlatPivotMode(req);
      }
    } else {
      return this.useRowsFlatMode(req);
    }
  }

  /**
   * Normalized request structure - separates concerns for easier processing.
   */
  private normalizeRequest(model: DataSourceModel): NormalizedRequest {
    const {columns, filters = [], pagination, pivot} = model;

    // Extract sorting from wherever it lives
    let sort: SortSpec | undefined;

    if (pivot?.drillDown) {
      // In drill-down mode, sort comes from columns
      const col = columns?.find((c) => c.sort);
      if (col) {
        sort = {id: col.id, field: col.field, direction: col.sort!};
      }
    } else if (pivot) {
      // In pivot mode, sort comes from groupBy or aggregates
      const groupCol = pivot.groupBy.find((g) => g.sort);
      if (groupCol) {
        sort = {id: groupCol.id, field: groupCol.field, direction: groupCol.sort!};
      } else {
        const agg = pivot.aggregates?.find((a) => a.sort);
        if (agg) {
          sort = {
            id: agg.id,
            field: 'field' in agg ? agg.field : '',
            direction: agg.sort!,
          };
        }
      }
    } else {
      // In flat mode, sort comes from columns
      const col = columns?.find((c) => c.sort);
      if (col) {
        sort = {id: col.id, field: col.field, direction: col.sort!};
      }
    }

    return {
      columns: columns ?? [],
      filters,
      pagination,
      pivot,
      sort,
    };
  }

  /**
   * Fetch aggregate totals (grand totals across all filtered rows).
   */
  useAggregateTotals(
    model: DataSourceModel,
  ): QueryResult<ReadonlyMap<string, SqlValue>> {
    const {pivot} = model;

    // No aggregates in drill-down mode
    if (pivot?.drillDown) {
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

  // Stub implementations for interface compliance
  useDistinctValues(): QueryResult<ReadonlyMap<string, readonly SqlValue[]>> {
    return {data: undefined, isPending: false, isFresh: true};
  }

  useParameterKeys(): QueryResult<ReadonlyMap<string, readonly string[]>> {
    return {data: undefined, isPending: false, isFresh: true};
  }

  /**
   * Flat mode: Regular table view without pivot.
   */
  private useRowsFlatMode(req: NormalizedRequest): RowsQueryResult {
    const {pagination} = req;

    const rowsQuery = this.buildQuery(req);

    const result = this.rowsSlot.use({
      key: {query: rowsQuery, pagination},
      queryFn: async () => this.fetchRows(req, rowsQuery),
      enabled: true,
      retainOn: ['pagination'],
    });

    return this.toRowsQueryResult(result, rowsQuery);
  }

  /**
   * Pivot mode with collapsible groups: Uses __intrinsic_pivot virtual table.
   */
  private useRowsPivotMode(req: NormalizedRequest): RowsQueryResult {
    const {pagination, pivot, filters, sort} = req;
    ensure(pivot);

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

    const rowsKey = {
      table: tableKey,
      pagination,
      sort,
      expandedIds: pivot.expandedIds
        ? Array.from(pivot.expandedIds).sort()
        : [],
      collapsedIds: pivot.collapsedIds
        ? Array.from(pivot.collapsedIds).sort()
        : [],
    };

    const {data: virtualTable} = this.pivotTableSlot.use({
      key: tableKey,
      queryFn: async () => this.createIntrinsicPivotTable(pivot, filters),
    });

    const result = this.rowsSlot.use({
      key: rowsKey,
      queryFn: async () => this.fetchPivotRows(req),
      enabled: !!virtualTable,
      retainOn: ['pagination', 'expandedIds', 'collapsedIds', 'sort'],
    });

    const queryForDisplay = `SELECT * FROM ${pivotTableName}`;
    return this.toRowsQueryResult(result, queryForDisplay);
  }

  /**
   * Flat pivot mode: Simple GROUP BY aggregation without hierarchy.
   */
  private useRowsFlatPivotMode(req: NormalizedRequest): RowsQueryResult {
    const {pagination} = req;

    const rowsQuery = this.buildQuery(req);

    const result = this.rowsSlot.use({
      key: {query: rowsQuery, pagination},
      queryFn: async () => this.fetchRows(req, rowsQuery),
      enabled: true,
      retainOn: ['pagination'],
    });

    return this.toRowsQueryResult(result, rowsQuery);
  }

  /**
   * Pivot drill-down mode: Shows individual rows filtered by pivot group.
   */
  private useRowsPivotDrilldownMode(req: NormalizedRequest): RowsQueryResult {
    const {pagination} = req;

    const rowsQuery = this.buildQuery(req);

    const result = this.rowsSlot.use({
      key: {query: rowsQuery, pagination},
      queryFn: async () => this.fetchRows(req, rowsQuery),
      enabled: true,
      retainOn: ['pagination'],
    });

    return this.toRowsQueryResult(result, rowsQuery);
  }

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

  private async fetchRows(
    req: NormalizedRequest,
    rowsQuery: string,
  ): Promise<RowsResult> {
    const {pagination, pivot} = req;

    // Get row count
    const countQuery = this.buildQuery({...req, sort: undefined});
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
      pivot !== undefined && !pivot.drillDown && pivot.collapsibleGroups;

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

  private async fetchPivotRows(req: NormalizedRequest): Promise<RowsResult> {
    const {pivot, pagination, sort} = req;
    ensure(pivot);

    // Convert sort spec to pivot table format
    let sortStr = 'agg_0 DESC';
    if (sort) {
      const aggregates = pivot.aggregates ?? [];
      const aggIndex = aggregates.findIndex((a) => a.id === sort.id);
      if (aggIndex >= 0) {
        sortStr = `agg_${aggIndex} ${sort.direction}`;
      } else {
        sortStr = `name ${sort.direction}`;
      }
    }

    const columnAliases: Record<string, string> = {};
    for (const col of pivot.groupBy) {
      columnAliases[col.field] = toAlias(col.id);
    }
    const aggregates = pivot.aggregates ?? [];
    for (let i = 0; i < aggregates.length; i++) {
      columnAliases[`agg_${i}`] = toAlias(aggregates[i].id);
    }

    const result = await queryPivotTable(this.engine, pivotTableName, {
      expandedIds: pivot.expandedIds,
      collapsedIds: pivot.collapsedIds,
      sort: sortStr,
      offset: pagination?.offset,
      limit: pagination?.limit,
      columnAliases,
    });

    return {
      rowOffset: pagination?.offset ?? 0,
      totalRows: result.totalRows,
      rows: result.rows,
      query: `SELECT * FROM ${pivotTableName}`,
    };
  }

  private async fetchAggregates(
    model: DataSourceModel,
  ): Promise<AggregatesResult> {
    const {columns, filters = [], pivot} = model;
    const totals = new Map<string, SqlValue>();

    // Pivot aggregates
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

  private buildAggregateQueryKey(model: DataSourceModel): string | undefined {
    const {columns, filters = [], pivot} = model;

    const parts: string[] = [];

    if (pivot && !pivot.drillDown && Boolean(pivot.aggregates?.length)) {
      parts.push(`pivot:${JSON.stringify(pivot.aggregates)}`);
    }

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

    const filterKey = filters.map((f) => {
      const value = 'value' in f ? f.value : '';
      return `${f.field}:${f.op}:${value}`;
    });
    parts.push(`filters:${filterKey.join(',')}`);

    return parts.join('|');
  }

  private wrapQueryWithPrelude(query: string): string {
    if (this.prelude) {
      return `${this.prelude};\n${query}`;
    }
    return query;
  }

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

  private async createIntrinsicPivotTable(
    pivot: Pivot,
    filters: readonly Filter[],
  ): Promise<DisposableSqlEntity> {
    const sourceTable = this.buildFilteredSourceTable(filters);

    const aggregateExprs = (pivot.aggregates ?? []).map((a) => {
      if (a.function === 'COUNT') return 'COUNT(*)';
      const field = 'field' in a ? a.field : '';
      return buildAggregateExpr(a.function, field);
    });

    return createPivotTable(this.engine, {
      sourceTable,
      groupByColumns: pivot.groupBy.map((g) => g.field),
      aggregateExprs,
      tableName: pivotTableName,
    });
  }

  private buildFilteredSourceTable(filters: readonly Filter[]): string {
    const resolver = this.createResolver();
    const baseTable = resolver.getBaseTable();
    const baseAlias = resolver.getBaseAlias();

    if (filters.length === 0) {
      return baseTable;
    }

    for (const filter of filters) {
      resolver.resolveColumnPath(filter.field);
    }
    const joinClauses = resolver.buildJoinClauses();

    const whereConditions = filters.map((filter) => {
      const sqlExpr = resolver.resolveColumnPath(filter.field);
      return filterToSql(filter, sqlExpr ?? filter.field);
    });

    return `(
      SELECT ${baseAlias}.*
      FROM ${baseTable} AS ${baseAlias} ${joinClauses}
      WHERE ${whereConditions.join(' AND ')})
    `;
  }

  private buildQuery(req: NormalizedRequest): string {
    const {columns, filters, pivot, sort} = req;

    const resolver = this.createResolver();
    const baseTable = resolver.getBaseTable();
    const baseAlias = resolver.getBaseAlias();

    // Pivot mode without drill-down
    if (pivot && !pivot.drillDown) {
      if (pivot.collapsibleGroups) {
        return this.buildIntrinsicPivotQuery(req);
      } else {
        return this.buildFlatPivotQuery(resolver, req);
      }
    }

    // Normal mode or drill-down
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

    if (pivot?.drillDown) {
      for (const col of pivot.groupBy) {
        resolver.resolveColumnPath(col.field);
      }
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
        return filterToSql(filter, sqlExpr ?? filter.field);
      });
      query += `\nWHERE ${whereConditions.join(' AND ')}`;
    }

    if (pivot?.drillDown) {
      const drillDownConditions = pivot.drillDown
        .map(({field, value}) => {
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

    if (sort) {
      const sqlExpr = resolver.resolveColumnPath(sort.field);
      if (sqlExpr) {
        query += `\nORDER BY ${sqlExpr} ${sort.direction}`;
      }
    }

    const aggregateSuffix = columns
      ?.filter((c) => c.aggregate)
      .map((c) => `${c.id}:${c.aggregate}`)
      .join(',');
    if (aggregateSuffix) {
      query += ` /* aggregates: ${aggregateSuffix} */`;
    }

    return query;
  }

  private buildFlatPivotQuery(
    resolver: SQLSchemaResolver,
    req: NormalizedRequest,
  ): string {
    const {filters, pivot, sort} = req;
    ensure(pivot);

    const baseTable = resolver.getBaseTable();
    const baseAlias = resolver.getBaseAlias();

    const selectClauses: string[] = [];
    const groupByClauses: string[] = [];

    for (const col of pivot.groupBy) {
      const sqlExpr = resolver.resolveColumnPath(col.field);
      if (sqlExpr) {
        const alias = toAlias(col.id);
        selectClauses.push(`${sqlExpr} AS ${alias}`);
        groupByClauses.push(sqlExpr);
      }
    }

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

    for (const filter of filters) {
      resolver.resolveColumnPath(filter.field);
    }

    const joinClauses = resolver.buildJoinClauses();

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

    if (sort) {
      const alias = toAlias(sort.id);
      query += `\nORDER BY ${alias} ${sort.direction}`;
    }

    return query;
  }

  private buildIntrinsicPivotQuery(req: NormalizedRequest): string {
    const {columns, pivot, pagination, sort} = req;
    ensure(pivot);

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

    // Convert sort to pivot table format
    let sortSpec = 'agg_0 DESC';
    if (sort) {
      const aggregates = pivot.aggregates ?? [];
      const aggIndex = aggregates.findIndex((a) => a.id === sort.id);
      if (aggIndex >= 0) {
        sortSpec = `agg_${aggIndex} ${sort.direction}`;
      } else {
        sortSpec = `name ${sort.direction}`;
      }
    }

    const selectClauses: string[] = [];

    for (const col of pivot.groupBy) {
      const alias = toAlias(col.id);
      selectClauses.push(`${col.field} AS ${alias}`);
    }

    selectClauses.push('__id__');
    selectClauses.push('__parent_id__');
    selectClauses.push('__depth__');
    selectClauses.push('__has_children__');
    selectClauses.push('__child_count__');

    const aggregates = pivot.aggregates ?? [];
    for (let i = 0; i < aggregates.length; i++) {
      const agg = aggregates[i];
      const alias = toAlias(agg.id);
      selectClauses.push(`agg_${i} AS ${alias}`);
    }

    // Add dependency columns (they won't be in the pivot output, so use NULL)
    if (columns.length > 0) {
      const existingFields = new Set([
        ...pivot.groupBy.map((g) => g.field),
        ...aggregates
          .filter((a) => 'field' in a)
          .map((a) => (a as {field: string}).field),
      ]);

      for (const col of columns) {
        if (!existingFields.has(col.field)) {
          const alias = toAlias(col.id);
          selectClauses.push(`NULL AS ${alias}`);
        }
      }
    }

    let query = `SELECT ${selectClauses.join(',\n       ')}
FROM ${pivotTableName}
WHERE ${expansionConstraint}
  AND __sort__ = '${sortSpec}'`;

    if (pagination) {
      query += `\n  AND __offset__ = ${pagination.offset}`;
      query += `\n  AND __limit__ = ${pagination.limit}`;
    }

    if (sort) {
      query += '\nORDER BY rowid';
    }

    return query;
  }

  private async fetchPivotAggregates(
    filters: ReadonlyArray<Filter>,
    pivot: Pivot,
  ): Promise<Row> {
    const resolver = this.createResolver();
    const baseTable = resolver.getBaseTable();
    const baseAlias = resolver.getBaseAlias();

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
      const filterResolver = this.createResolver();
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
    const resolver = this.createResolver();
    const baseTable = resolver.getBaseTable();
    const baseAlias = resolver.getBaseAlias();

    const selectClauses = columns
      .filter((col) => col.aggregate)
      .map((col) => {
        const func = col.aggregate!;
        const colExpr = resolver.resolveColumnPath(col.field);
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
    return `MIN(${fieldExpr})`;
  }
  return `${func}(${fieldExpr})`;
}
