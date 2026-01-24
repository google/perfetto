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

import {QueryResult, QuerySlot, SerialTaskQueue} from '../../../../base/query_slot';
import {Engine} from '../../../../trace_processor/engine';
import {Row, SqlValue} from '../../../../trace_processor/query_result';
import {DisposableSqlEntity} from '../../../../trace_processor/sql_utils';
import {DataSourceRows, PivotModel} from '../datagrid_engine';
import {
  buildAggregateExpr,
  createPivotTable,
  queryPivotTable,
} from '../pivot_operator';
import {SQLSchemaRegistry, SQLSchemaResolver} from '../sql_schema';
import {filterToSql, toAlias} from '../sql_utils';

/**
 * Pivot engine for DataGrid using __intrinsic_pivot virtual table.
 *
 * Handles grouped/aggregated views with hierarchical expand/collapse.
 */
export class PivotEngine {
  private readonly rowCountSlot: QuerySlot<number>;
  private readonly rowsSlot: QuerySlot<{
    readonly rows: readonly Row[];
    readonly rowOffset: number;
  }>;
  private readonly pivotTableSlot: QuerySlot<DisposableSqlEntity>;
  private readonly totalsSlot: QuerySlot<ReadonlyMap<string, SqlValue>>;

  constructor(
    // A short SQL safe UUID to use for naming temporary tables
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
    this.totalsSlot = new QuerySlot<ReadonlyMap<string, SqlValue>>(queue);
  }

  get(model: PivotModel): DataSourceRows {
    const {
      groupBy,
      aggregates,
      filters = [],
      pagination,
      expandedIds,
      collapsedIds,
      sort,
    } = model;

    // Build the source subquery with filters applied
    const sourceQuery = this.buildSourceQuery(filters);

    // Build groupBy column names (raw field names for the pivot table)
    const groupByColumns = groupBy.map((col) => col.field);

    // Build aggregate expressions
    const aggregateExprs = aggregates.map((agg) => {
      if (agg.function === 'COUNT') {
        return 'COUNT(*)';
      } else {
        return buildAggregateExpr(agg.function, agg.field);
      }
    });

    // Create/get the pivot virtual table
    const pivotTableResult = this.pivotTableSlot.use({
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

    // Don't proceed until the pivot table is ready
    if (pivotTableResult.isPending || !pivotTableResult.data) {
      return {isPending: true};
    }

    const pivotTableName = pivotTableResult.data.name;

    // Build column aliases for the result rows
    // Maps pivot table output columns to model aliases
    const columnAliases: Record<string, string> = {};
    // Also build reverse mapping: alias -> pivot column name (for sorting)
    const aliasToColumn: Record<string, string> = {};
    for (let i = 0; i < groupBy.length; i++) {
      columnAliases[groupBy[i].field] = toAlias(groupBy[i].alias);
      aliasToColumn[groupBy[i].alias] = groupBy[i].field;
    }
    for (let i = 0; i < aggregates.length; i++) {
      columnAliases[`agg_${i}`] = toAlias(aggregates[i].alias);
      aliasToColumn[aggregates[i].alias] = `agg_${i}`;
    }

    // Build sort string for pivot table (e.g., 'agg_0 DESC')
    const sortStr = sort
      ? `${aliasToColumn[sort.alias] ?? sort.alias} ${sort.direction}`
      : undefined;

    // Query rows from the pivot table
    const rowsResult = this.rowsSlot.use({
      key: {
        sourceQuery,
        groupByColumns,
        aggregateExprs,
        expandedIds: expandedIds ? Array.from(expandedIds) : undefined,
        collapsedIds: collapsedIds ? Array.from(collapsedIds) : undefined,
        pagination,
        sortStr,
      },
      retainOn: ['pagination', 'expandedIds', 'collapsedIds'],
      queryFn: async () => {
        const result = await queryPivotTable(this.engine, pivotTableName, {
          expandedIds,
          collapsedIds,
          sort: sortStr,
          offset: pagination?.offset,
          limit: pagination?.limit,
          columnAliases,
        });
        return {
          rows: result.rows,
          rowOffset: pagination?.offset ?? 0,
        };
      },
    });

    // Get total row count (based on expansion state only, not pagination/sort)
    const rowCountResult = this.rowCountSlot.use({
      key: {
        sourceQuery,
        groupByColumns,
        aggregateExprs,
        expandedIds: expandedIds ? Array.from(expandedIds) : undefined,
        collapsedIds: collapsedIds ? Array.from(collapsedIds) : undefined,
      },
      retainOn: ['expandedIds', 'collapsedIds'],
      queryFn: async () => {
        // Query with same expansion state but no pagination to get total
        const result = await queryPivotTable(this.engine, pivotTableName, {
          expandedIds,
          collapsedIds,
          sort: sortStr,
          columnAliases,
        });
        return result.totalRows;
      },
    });

    // Merge the results into a single bundle
    return {
      totalRows: rowCountResult.data,
      rowOffset: rowsResult.data?.rowOffset,
      rows: rowsResult.data?.rows,
      isPending: rowCountResult.isPending || rowsResult.isPending,
    };
  }

  /**
   * Get grand totals across all filtered rows (no grouping).
   */
  getTotals(model: PivotModel): QueryResult<ReadonlyMap<string, SqlValue>> {
    const {aggregates, filters = []} = model;

    return this.totalsSlot.use({
      key: {
        aggregates,
        filters: filters.map((f) => filterToSql(f, f.field)),
      },
      queryFn: async () => {
        const query = this.buildTotalsQuery(model);
        const result = await this.engine.query(query);
        const row = result.firstRow({}) as Row;
        const totals = new Map<string, SqlValue>();
        for (const agg of aggregates) {
          totals.set(agg.alias, row[agg.alias]);
        }
        return totals;
      },
    });
  }

  /**
   * Builds a query to get grand totals (aggregates without GROUP BY).
   */
  private buildTotalsQuery(model: PivotModel): string {
    const {aggregates, filters = []} = model;
    const resolver = new SQLSchemaResolver(this.sqlSchema, this.rootSchemaName);

    // Build SELECT clause with only aggregates
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

    return sql;
  }

  /**
   * Builds the source subquery with filters applied.
   * This becomes the input to the pivot virtual table.
   */
  private buildSourceQuery(filters: PivotModel['filters']): string {
    const resolver = new SQLSchemaResolver(this.sqlSchema, this.rootSchemaName);
    const baseTable = resolver.getBaseTable();
    const baseAlias = resolver.getBaseAlias();

    let sql = `SELECT ${baseAlias}.* FROM ${baseTable} AS ${baseAlias}`;

    // Add JOIN clauses if any filters reference joined columns
    if (filters && filters.length > 0) {
      // Resolve filter columns to trigger JOIN creation
      for (const filter of filters) {
        resolver.resolveColumnPath(filter.field);
      }

      const joinClauses = resolver.buildJoinClauses();
      if (joinClauses) {
        sql += `\n${joinClauses}`;
      }

      // Build WHERE clause
      const whereConditions = filters.map((filter) => {
        const sqlExpr = resolver.resolveColumnPath(filter.field);
        return filterToSql(filter, sqlExpr ?? filter.field);
      });
      sql += `\nWHERE ${whereConditions.join(' AND ')}`;
    }

    // Wrap in parentheses to make it a valid subquery for the pivot table
    return `(${sql})`;
  }

  dispose(): void {
    this.rowCountSlot.dispose();
    this.rowsSlot.dispose();
    this.pivotTableSlot.dispose();
    this.totalsSlot.dispose();
  }
}
