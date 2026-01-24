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

import {QuerySlot, SerialTaskQueue} from '../../../../base/query_slot';
import {Engine} from '../../../../trace_processor/engine';
import {NUM, Row} from '../../../../trace_processor/query_result';
import {runQueryForQueryTable} from '../../../query_table/queries';
import {DataSourceRows, PivotModel} from '../datagrid_engine';
import {buildAggregateExpr} from '../pivot_operator';
import {SQLSchemaRegistry, SQLSchemaResolver} from '../sql_schema';
import {filterToSql, sqlValue, toAlias} from '../sql_utils';

/**
 * Flat pivot engine for DataGrid.
 *
 * Handles grouped/aggregated views without hierarchical expand/collapse:
 * - When no drillDown: shows aggregated groups (GROUP BY query)
 * - When drillDown is set: shows raw rows filtered to specific group values
 */
export class PivotFlatEngine {
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
    this.rowCountSlot = new QuerySlot<number>(queue);
    this.rowsSlot = new QuerySlot<{
      readonly rows: readonly Row[];
      readonly rowOffset: number;
    }>(queue);
  }

  get(model: PivotModel): DataSourceRows {
    const {drillDown} = model;

    // When drillDown is set, show raw rows filtered to specific group values
    if (drillDown && drillDown.length > 0) {
      return this.getDrillDownRows(model);
    }

    // Otherwise show aggregated groups
    return this.getGroupedRows(model);
  }

  /**
   * Get aggregated group rows (GROUP BY query).
   */
  private getGroupedRows(model: PivotModel): DataSourceRows {
    const {groupBy, aggregates, filters = [], pagination, sort} = model;

    const queryKey = {
      groupBy,
      aggregates,
      filters: serializeFilters(filters),
    };

    // Get row count (doesn't depend on pagination or sort)
    const rowCountResult = this.rowCountSlot.use({
      key: queryKey,
      queryFn: async () => {
        const query = this.buildGroupByQuery(model, {countOnly: true});
        const result = await this.engine.query(query);
        return result.firstRow({count: NUM}).count;
      },
    });

    // Get rows with pagination and sort
    const rowsResult = this.rowsSlot.use({
      key: {...queryKey, pagination, sort},
      retainOn: ['pagination', 'sort'],
      queryFn: async () => {
        const query = this.buildGroupByQuery(model);
        const result = await runQueryForQueryTable(query, this.engine);
        return {
          rows: result.rows as Row[],
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
   * Get raw rows filtered to specific group values (drillDown mode).
   */
  private getDrillDownRows(model: PivotModel): DataSourceRows {
    const {drillDown, filters = [], pagination, sort} = model;

    const queryKey = {
      drillDown: serializeDrillDown(drillDown),
      filters: serializeFilters(filters),
    };

    // Get row count
    const rowCountResult = this.rowCountSlot.use({
      key: queryKey,
      queryFn: async () => {
        const query = this.buildDrillDownQuery(model, {countOnly: true});
        const result = await this.engine.query(query);
        return result.firstRow({count: NUM}).count;
      },
    });

    // Get rows
    const rowsResult = this.rowsSlot.use({
      key: {...queryKey, pagination, sort},
      retainOn: ['pagination', 'sort'],
      queryFn: async () => {
        const query = this.buildDrillDownQuery(model);
        const result = await runQueryForQueryTable(query, this.engine);
        return {
          rows: result.rows as Row[],
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
   * Builds a GROUP BY query for aggregated groups.
   */
  private buildGroupByQuery(
    model: PivotModel,
    options?: {countOnly?: boolean},
  ): string {
    const {groupBy, aggregates, filters = [], pagination, sort} = model;
    const resolver = new SQLSchemaResolver(this.sqlSchema, this.rootSchemaName);

    // Build SELECT clause
    const selectExprs: string[] = [];

    // Add groupBy columns
    for (const col of groupBy) {
      const sqlExpr = resolver.resolveColumnPath(col.field);
      if (sqlExpr) {
        selectExprs.push(`${sqlExpr} AS ${toAlias(col.alias)}`);
      }
    }

    // Add aggregate expressions
    for (const agg of aggregates) {
      let aggExpr: string;
      if (agg.function === 'COUNT') {
        aggExpr = 'COUNT(*)';
      } else {
        const fieldExpr = resolver.resolveColumnPath(agg.field);
        aggExpr = buildAggregateExpr(agg.function, fieldExpr ?? agg.field);
      }
      selectExprs.push(`${aggExpr} AS ${toAlias(agg.alias)}`);
    }

    // Build FROM clause
    const baseTable = resolver.getBaseTable();
    const baseAlias = resolver.getBaseAlias();
    const joinClauses = resolver.buildJoinClauses();

    let sql = `SELECT ${selectExprs.join(', ')}`;
    sql += `\nFROM ${baseTable} AS ${baseAlias}`;
    if (joinClauses) {
      sql += `\n${joinClauses}`;
    }

    // Add WHERE clause for filters
    if (filters.length > 0) {
      const whereConditions = filters.map((filter) => {
        const sqlExpr = resolver.resolveColumnPath(filter.field);
        return filterToSql(filter, sqlExpr ?? filter.field);
      });
      sql += `\nWHERE ${whereConditions.join(' AND ')}`;
    }

    // Add GROUP BY clause
    const groupByExprs = groupBy.map((col) => {
      return resolver.resolveColumnPath(col.field) ?? col.field;
    });
    sql += `\nGROUP BY ${groupByExprs.join(', ')}`;

    if (options?.countOnly) {
      return `SELECT COUNT(*) as count FROM (${sql})`;
    }

    // Add ORDER BY
    if (sort) {
      sql += `\nORDER BY ${toAlias(sort.alias)} ${sort.direction}`;
    }

    // Add pagination
    if (pagination) {
      sql += `\nLIMIT ${pagination.limit} OFFSET ${pagination.offset ?? 0}`;
    }

    return sql;
  }

  /**
   * Builds a query for raw rows filtered to drillDown values.
   */
  private buildDrillDownQuery(
    model: PivotModel,
    options?: {countOnly?: boolean},
  ): string {
    const {drillDown = [], filters = [], pagination, sort} = model;
    const resolver = new SQLSchemaResolver(this.sqlSchema, this.rootSchemaName);

    // Resolve drillDown fields to trigger JOIN creation
    for (const dd of drillDown) {
      resolver.resolveColumnPath(dd.field);
    }

    // Also resolve filter fields
    for (const filter of filters) {
      resolver.resolveColumnPath(filter.field);
    }

    // Build FROM clause
    const baseTable = resolver.getBaseTable();
    const baseAlias = resolver.getBaseAlias();
    const joinClauses = resolver.buildJoinClauses();

    let sql = `SELECT ${baseAlias}.*`;
    sql += `\nFROM ${baseTable} AS ${baseAlias}`;
    if (joinClauses) {
      sql += `\n${joinClauses}`;
    }

    // Build WHERE clause combining drillDown and filters
    const whereConditions: string[] = [];

    // Add drillDown conditions
    for (const dd of drillDown) {
      const sqlExpr = resolver.resolveColumnPath(dd.field);
      const expr = sqlExpr ?? dd.field;
      if (dd.value === null) {
        whereConditions.push(`${expr} IS NULL`);
      } else {
        whereConditions.push(`${expr} = ${sqlValue(dd.value)}`);
      }
    }

    // Add filter conditions
    for (const filter of filters) {
      const sqlExpr = resolver.resolveColumnPath(filter.field);
      whereConditions.push(filterToSql(filter, sqlExpr ?? filter.field));
    }

    if (whereConditions.length > 0) {
      sql += `\nWHERE ${whereConditions.join(' AND ')}`;
    }

    if (options?.countOnly) {
      return `SELECT COUNT(*) as count FROM (${sql})`;
    }

    // Add ORDER BY
    if (sort) {
      sql += `\nORDER BY ${toAlias(sort.alias)} ${sort.direction}`;
    }

    // Add pagination
    if (pagination) {
      sql += `\nLIMIT ${pagination.limit} OFFSET ${pagination.offset ?? 0}`;
    }

    return sql;
  }
}

/**
 * Filters can contain values that are not JSON friendly - e.g. Uint8Arrays.
 * Returns a serialized version of the filters suitable for use as a cache key.
 */
function serializeFilters(filters: PivotModel['filters']) {
  return filters?.map((filter) => filterToSql(filter, filter.field));
}

/**
 * DrillDown values can contain Uint8Arrays which are not JSON friendly.
 * Returns a serialized version suitable for use as a cache key.
 */
function serializeDrillDown(drillDown: PivotModel['drillDown']) {
  return drillDown?.map((dd) => `${dd.field}=${sqlValue(dd.value)}`);
}
