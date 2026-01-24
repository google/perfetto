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
import {filterToSql, toAlias} from '../sql_utils';

/**
 * Flat pivot engine for DataGrid.
 *
 * Handles grouped/aggregated views without hierarchical expand/collapse.
 * Shows aggregated groups using a simple GROUP BY query.
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
}

/**
 * Filters can contain values that are not JSON friendly - e.g. Uint8Arrays.
 * Returns a serialized version of the filters suitable for use as a cache key.
 */
function serializeFilters(filters: PivotModel['filters']) {
  return filters?.map((filter) => filterToSql(filter, filter.field));
}
