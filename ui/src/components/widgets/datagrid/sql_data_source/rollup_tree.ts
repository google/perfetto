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
import {DisposableSqlEntity} from '../../../../trace_processor/sql_utils';
import {runQueryForQueryTable} from '../../../query_table/queries';
import {DataSourceRows, PivotModel} from '../data_source';
import {
  buildAggregateExpr,
  buildPivotQuery,
  createPivotTable,
} from '../rollup_tree_operator';
import {serializeFilters} from './group_by';
import {SQLSchemaRegistry, SQLSchemaResolver} from '../sql_schema';
import {filterToSql, toAlias} from '../sql_utils';

// Rollup tree datasource - uses __intrinsic_rollup_tree virtual table.
export class SQLDataSourceRollupTree {
  private readonly rowCountSlot: QuerySlot<number>;
  private readonly rowsSlot: QuerySlot<{
    readonly rows: readonly Row[];
    readonly rowOffset: number;
  }>;
  private readonly pivotTableSlot: QuerySlot<DisposableSqlEntity>;
  private readonly summariesSlot: QuerySlot<Row>;

  constructor(
    private readonly uuid: string,
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
    this.pivotTableSlot = new QuerySlot<DisposableSqlEntity>(queue);
    this.summariesSlot = new QuerySlot<Row>(queue);
  }

  getRows(model: PivotModel): DataSourceRows {
    const {
      groupBy,
      aggregates,
      filters = [],
      pagination,
      expandedIds,
      collapsedIds,
      sort,
    } = model;

    const pivotTableResult = this.usePivotTable(model);
    if (pivotTableResult.isPending || !pivotTableResult.data) {
      return {isPending: true};
    }

    const pivotTableName = pivotTableResult.data.name;

    // Build column alias mappings
    // Operator outputs __group_N and __agg_N, we map to user-facing aliases
    const columnAliases: Record<string, string> = {};
    const aliasToColumn: Record<string, string> = {};
    for (let i = 0; i < groupBy.length; i++) {
      columnAliases[`__group_${i}`] = toAlias(groupBy[i].alias);
      aliasToColumn[groupBy[i].alias] = `__group_${i}`;
    }
    for (let i = 0; i < aggregates.length; i++) {
      columnAliases[`__agg_${i}`] = toAlias(aggregates[i].alias);
      aliasToColumn[aggregates[i].alias] = `__agg_${i}`;
    }

    // Build sort string for the rollup tree operator.
    // Aggregate columns use __agg_N format, groupBy columns use __group_N
    // to sort that level by hierarchy value (other levels sort by __agg_0).
    const sortStr = sort
      ? (() => {
          const column = aliasToColumn[sort.alias];
          if (column?.startsWith('__agg_')) {
            return `${column} ${sort.direction}`;
          } else {
            // GroupBy column - find its index and use __group_N format
            const groupIndex = groupBy.findIndex(
              (col) => col.alias === sort.alias,
            );
            if (groupIndex >= 0) {
              return `__group_${groupIndex} ${sort.direction}`;
            }
            // Fallback to first aggregate
            return `__agg_0 ${sort.direction}`;
          }
        })()
      : undefined;

    const pivotKey = {
      groupBy,
      aggregates,
      filters: serializeFilters(filters),
    };

    const rowCountResult = this.rowCountSlot.use({
      key: {
        ...pivotKey,
        expandedIds: expandedIds ? Array.from(expandedIds) : undefined,
        collapsedIds: collapsedIds ? Array.from(collapsedIds) : undefined,
      },
      retainOn: ['expandedIds', 'collapsedIds'],
      queryFn: async () => {
        const query = buildPivotQuery(pivotTableName, {
          expandedIds,
          collapsedIds,
          minDepth: 1,
          countOnly: true,
        });
        const result = await this.engine.query(query);
        return result.firstRow({count: NUM}).count;
      },
    });

    const rowsResult = this.rowsSlot.use({
      key: {
        ...pivotKey,
        expandedIds: expandedIds ? Array.from(expandedIds) : undefined,
        collapsedIds: collapsedIds ? Array.from(collapsedIds) : undefined,
        pagination,
        sortStr,
      },
      retainOn: ['pagination', 'expandedIds', 'collapsedIds'],
      queryFn: async () => {
        const query = buildPivotQuery(pivotTableName, {
          expandedIds,
          collapsedIds,
          sort: sortStr,
          offset: pagination?.offset,
          limit: pagination?.limit,
          minDepth: 1,
          columnAliases,
        });
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
    const {groupBy, aggregates, filters = []} = model;

    const pivotTableResult = this.usePivotTable(model);
    if (pivotTableResult.isPending || !pivotTableResult.data) {
      return {isPending: true, data: undefined, isFresh: false};
    }

    const pivotTableName = pivotTableResult.data.name;

    const columnAliases: Record<string, string> = {};
    for (let i = 0; i < groupBy.length; i++) {
      columnAliases[`__group_${i}`] = toAlias(groupBy[i].alias);
    }
    for (let i = 0; i < aggregates.length; i++) {
      columnAliases[`__agg_${i}`] = toAlias(aggregates[i].alias);
    }

    return this.summariesSlot.use({
      key: {
        groupBy,
        aggregates,
        filters: serializeFilters(filters),
      },
      queryFn: async () => {
        const query = buildPivotQuery(pivotTableName, {
          maxDepth: 0,
          columnAliases,
        });
        const result = await runQueryForQueryTable(query, this.engine);
        return (result.rows[0] as Row) ?? ({} as Row);
      },
    });
  }

  private usePivotTable(model: PivotModel): QueryResult<DisposableSqlEntity> {
    const {groupBy, aggregates, filters = []} = model;

    const sourceQuery = this.buildSourceQuery(filters);
    const groupByColumns = groupBy.map((col) => col.field);
    const aggregateExprs = aggregates.map((agg) => {
      if (agg.function === 'COUNT') {
        return 'COUNT(*)';
      } else {
        return buildAggregateExpr(agg.function, agg.field);
      }
    });

    return this.pivotTableSlot.use({
      key: {
        sourceQuery,
        groupByColumns,
        aggregateExprs,
      },
      queryFn: async () => {
        return await createPivotTable(this.engine, {
          sourceTable: sourceQuery,
          groupByColumns,
          aggregateExprs,
          tableName: `pivot_${this.uuid}`,
        });
      },
    });
  }

  private buildSourceQuery(filters: PivotModel['filters']): string {
    const resolver = new SQLSchemaResolver(this.sqlSchema, this.rootSchemaName);
    const baseTable = resolver.getBaseTable();
    const baseAlias = resolver.getBaseAlias();

    let sql = `SELECT ${baseAlias}.* FROM ${baseTable} AS ${baseAlias}`;

    if (filters && filters.length > 0) {
      for (const filter of filters) {
        resolver.resolveColumnPath(filter.field);
      }

      const joinClauses = resolver.buildJoinClauses();
      if (joinClauses) {
        sql += `\n${joinClauses}`;
      }

      const whereConditions = filters.map((filter) => {
        const sqlExpr = resolver.resolveColumnPath(filter.field);
        return filterToSql(filter, sqlExpr ?? filter.field);
      });
      sql += `\nWHERE ${whereConditions.join(' AND ')}`;
    }

    return `(${sql})`;
  }

  dispose(): void {
    this.rowCountSlot.dispose();
    this.rowsSlot.dispose();
    this.pivotTableSlot.dispose();
    this.summariesSlot.dispose();
  }
}
