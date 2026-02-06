// Copyright (C) 2026 The Android Open Source Project
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
} from '../../../../base/query_slot';
import {Engine} from '../../../../trace_processor/engine';
import {NUM, Row} from '../../../../trace_processor/query_result';
import {runQueryForQueryTable} from '../../../query_table/queries';
import {DataSourceRows, PivotModel} from '../data_source';
import {AggregateFunction} from '../model';
import {SQLSchemaRegistry, SQLSchemaResolver} from '../sql_schema';
import {filterToSql, toAlias} from '../sql_utils';

/**
 * Builds an aggregate expression string from function and field.
 * Note: COUNT is not part of AggregateFunction - use 'COUNT(*)' directly.
 */
function buildAggregateExpr(func: AggregateFunction, field: string): string {
  if (func === 'ANY') {
    return `MIN(${field})`; // ANY maps to MIN
  }
  return `${func}(${field})`;
}

// Flat GROUP BY datasource - uses simple GROUP BY queries without hierarchy.
export class SQLDataSourceGroupBy {
  private readonly rowCountSlot: QuerySlot<number>;
  private readonly rowsSlot: QuerySlot<{
    readonly rows: readonly Row[];
    readonly rowOffset: number;
  }>;
  private readonly summariesSlot: QuerySlot<Row>;

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
    this.summariesSlot = new QuerySlot<Row>(queue);
  }

  getRows(model: PivotModel): DataSourceRows {
    const {groupBy, aggregates, filters = [], pagination, sort} = model;

    const queryKey = {
      groupBy,
      aggregates,
      filters: serializeFilters(filters),
    };

    const rowCountResult = this.rowCountSlot.use({
      key: queryKey,
      queryFn: async () => {
        const query = this.buildGroupByQuery(model, {countOnly: true});
        const result = await this.engine.query(query);
        return result.firstRow({count: NUM}).count;
      },
    });

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

  getSummaries(model: PivotModel): QueryResult<Row> {
    const {aggregates, filters = []} = model;

    return this.summariesSlot.use({
      key: {
        aggregates,
        filters: serializeFilters(filters),
      },
      queryFn: async () => {
        const query = this.buildSummariesQuery(model);
        const result = await this.engine.query(query);
        return result.firstRow({}) as Row;
      },
    });
  }

  async exportData(model: PivotModel): Promise<readonly Row[]> {
    const query = this.buildGroupByQuery({
      ...model,
      pagination: undefined,
    });
    const result = await runQueryForQueryTable(query, this.engine);
    return result.rows;
  }

  private buildSummariesQuery(model: PivotModel): string {
    const {aggregates, filters = []} = model;
    const resolver = new SQLSchemaResolver(this.sqlSchema, this.rootSchemaName);

    const selectExprs: string[] = [];
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

    const baseTable = resolver.getBaseTable();
    const baseAlias = resolver.getBaseAlias();
    const joinClauses = resolver.buildJoinClauses();

    let sql = `SELECT ${selectExprs.join(', ')}`;
    sql += `\nFROM ${baseTable} AS ${baseAlias}`;
    if (joinClauses) {
      sql += `\n${joinClauses}`;
    }

    if (filters.length > 0) {
      const whereConditions = filters.map((filter) => {
        const sqlExpr = resolver.resolveColumnPath(filter.field);
        return filterToSql(filter, sqlExpr ?? filter.field);
      });
      sql += `\nWHERE ${whereConditions.join(' AND ')}`;
    }

    return sql;
  }

  private buildGroupByQuery(
    model: PivotModel,
    options?: {countOnly?: boolean},
  ): string {
    const {groupBy, aggregates, filters = [], pagination, sort} = model;
    const resolver = new SQLSchemaResolver(this.sqlSchema, this.rootSchemaName);

    const selectExprs: string[] = [];

    for (const col of groupBy) {
      const sqlExpr = resolver.resolveColumnPath(col.field);
      if (sqlExpr) {
        selectExprs.push(`${sqlExpr} AS ${toAlias(col.alias)}`);
      }
    }

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

    const baseTable = resolver.getBaseTable();
    const baseAlias = resolver.getBaseAlias();
    const joinClauses = resolver.buildJoinClauses();

    let sql = `SELECT ${selectExprs.join(', ')}`;
    sql += `\nFROM ${baseTable} AS ${baseAlias}`;
    if (joinClauses) {
      sql += `\n${joinClauses}`;
    }

    if (filters.length > 0) {
      const whereConditions = filters.map((filter) => {
        const sqlExpr = resolver.resolveColumnPath(filter.field);
        return filterToSql(filter, sqlExpr ?? filter.field);
      });
      sql += `\nWHERE ${whereConditions.join(' AND ')}`;
    }

    const groupByExprs = groupBy.map((col) => {
      return resolver.resolveColumnPath(col.field) ?? col.field;
    });
    sql += `\nGROUP BY ${groupByExprs.join(', ')}`;

    if (options?.countOnly) {
      return `SELECT COUNT(*) as count FROM (${sql})`;
    }

    if (sort) {
      // Sort strings case insensitively
      sql += `\nORDER BY ${toAlias(sort.alias)} COLLATE NOCASE ${sort.direction} `;
    }

    if (pagination) {
      sql += `\nLIMIT ${pagination.limit} OFFSET ${pagination.offset ?? 0}`;
    }

    return sql;
  }

  dispose(): void {
    this.rowCountSlot.dispose();
    this.rowsSlot.dispose();
    this.summariesSlot.dispose();
  }
}

// Serialize filters for use as cache keys (handles non-JSON values like Uint8Array).
export function serializeFilters(filters: PivotModel['filters']) {
  return filters?.map((filter) => filterToSql(filter, filter.field));
}
