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
} from '../../../../base/query_slot';
import {Engine} from '../../../../trace_processor/engine';
import {Row} from '../../../../trace_processor/query_result';
import {DisposableSqlEntity} from '../../../../trace_processor/sql_utils';
import {DataSourceRows, TreeModel} from '../data_source';
import {SQLSchemaRegistry, SQLSchemaResolver} from '../sql_schema';
import {filterToSql, toAlias} from '../sql_utils';
import {createTreeTable, queryTreeTable} from '../tree_operator';

/**
 * Tree datasource for DataGrid.
 *
 * Handles hierarchical display using id/parent_id columns via __intrinsic_tree.
 */
export class SQLDataSourceTree {
  private readonly rowCountSlot: QuerySlot<number>;
  private readonly rowsSlot: QuerySlot<{
    readonly rows: readonly Row[];
    readonly rowOffset: number;
  }>;
  private readonly tempTableSlot: QuerySlot<DisposableSqlEntity>;

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
    this.tempTableSlot = new QuerySlot<DisposableSqlEntity>(queue);
  }

  /**
   * Tree mode doesn't aggregate, so no summaries.
   */
  getSummaries(_model: TreeModel): QueryResult<Row> {
    return {data: undefined, isPending: false, isFresh: true};
  }

  getRows(model: TreeModel): DataSourceRows {
    const {
      columns,
      filters = [],
      pagination,
      idColumn,
      parentIdColumn,
      expandedIds,
      collapsedIds,
      sort,
    } = model;

    // Build the source subquery with filters applied
    const sourceQuery = this.buildSourceQuery(
      columns,
      filters,
      idColumn,
      parentIdColumn,
    );

    // Create/get the tree virtual table
    const treeTableResult = this.tempTableSlot.use({
      key: {
        sourceQuery,
        idColumn,
        parentIdColumn,
      },
      queryFn: async () => {
        return await createTreeTable(this.engine, {
          sourceTable: sourceQuery,
          idColumn,
          parentIdColumn,
          tableName: `tree_${this.uuid}`,
        });
      },
    });

    // Don't proceed until the tree table is ready
    if (treeTableResult.isPending || !treeTableResult.data) {
      return {isPending: true};
    }

    const treeTableName = treeTableResult.data.name;

    // Build column aliases for the result rows
    const columnAliases: Record<string, string> = {};
    const aliasToColumn: Record<string, string> = {};
    for (const col of columns) {
      columnAliases[col.field] = toAlias(col.alias);
      aliasToColumn[col.alias] = col.field;
    }

    // Ensure idColumn is always included in the result (needed for expand/collapse)
    if (!columnAliases[idColumn]) {
      columnAliases[idColumn] = idColumn;
    }

    // Build sort string for tree table
    const sortStr = sort
      ? `${aliasToColumn[sort.alias] ?? sort.alias} ${sort.direction}`
      : undefined;

    // Query rows from the tree table
    const rowsResult = this.rowsSlot.use({
      key: {
        sourceQuery,
        idColumn,
        parentIdColumn,
        expandedIds: expandedIds ? Array.from(expandedIds) : undefined,
        collapsedIds: collapsedIds ? Array.from(collapsedIds) : undefined,
        pagination,
        sortStr,
      },
      retainOn: ['pagination', 'expandedIds', 'collapsedIds'],
      queryFn: async () => {
        const result = await queryTreeTable(this.engine, treeTableName, {
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
        idColumn,
        parentIdColumn,
        expandedIds: expandedIds ? Array.from(expandedIds) : undefined,
        collapsedIds: collapsedIds ? Array.from(collapsedIds) : undefined,
      },
      retainOn: ['expandedIds', 'collapsedIds'],
      queryFn: async () => {
        const result = await queryTreeTable(this.engine, treeTableName, {
          expandedIds,
          collapsedIds,
          sort: sortStr,
          columnAliases,
        });
        return result.totalRows;
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
   * Export all data with current filters applied (no pagination).
   */
  async exportData(model: TreeModel): Promise<readonly Row[]> {
    const {
      columns,
      filters = [],
      idColumn,
      parentIdColumn,
      collapsedIds,
    } = model;

    // Build the source subquery with filters
    const sourceQuery = this.buildSourceQuery(
      columns,
      filters,
      idColumn,
      parentIdColumn,
    );

    // Create tree table
    const treeTable = await createTreeTable(this.engine, {
      sourceTable: sourceQuery,
      idColumn,
      parentIdColumn,
      tableName: `tree_export_${this.uuid}`,
    });

    try {
      // Build column aliases
      const columnAliases: Record<string, string> = {};
      for (const col of columns) {
        columnAliases[col.field] = toAlias(col.alias);
      }

      // Ensure idColumn is always included
      if (!columnAliases[idColumn]) {
        columnAliases[idColumn] = idColumn;
      }

      // Query all rows (use collapsedIds = empty set for fully expanded)
      const result = await queryTreeTable(this.engine, treeTable.name, {
        collapsedIds: collapsedIds ?? new Set(),
        columnAliases,
      });
      return result.rows;
    } finally {
      await treeTable[Symbol.asyncDispose]();
    }
  }

  /**
   * Builds the source subquery with columns and filters applied.
   * Ensures idColumn and parentIdColumn are always included.
   */
  private buildSourceQuery(
    columns: TreeModel['columns'],
    filters: TreeModel['filters'],
    idColumn: string,
    parentIdColumn: string,
  ): string {
    const resolver = new SQLSchemaResolver(this.sqlSchema, this.rootSchemaName);
    const baseTable = resolver.getBaseTable();
    const baseAlias = resolver.getBaseAlias();

    // Track which columns we've added to avoid duplicates
    const addedColumns = new Set<string>();

    // Build SELECT clause with requested columns
    const selectExprs: string[] = [];
    for (const col of columns) {
      const sqlExpr = resolver.resolveColumnPath(col.field);
      if (sqlExpr) {
        selectExprs.push(`${sqlExpr} AS ${col.field}`);
        addedColumns.add(col.field);
      }
    }

    // Ensure idColumn is included (required by __intrinsic_tree)
    if (!addedColumns.has(idColumn)) {
      const sqlExpr = resolver.resolveColumnPath(idColumn);
      if (sqlExpr) {
        selectExprs.push(`${sqlExpr} AS ${idColumn}`);
        addedColumns.add(idColumn);
      }
    }

    // Ensure parentIdColumn is included (required by __intrinsic_tree)
    if (!addedColumns.has(parentIdColumn)) {
      const sqlExpr = resolver.resolveColumnPath(parentIdColumn);
      if (sqlExpr) {
        selectExprs.push(`${sqlExpr} AS ${parentIdColumn}`);
        addedColumns.add(parentIdColumn);
      }
    }

    // If no columns, select all
    if (selectExprs.length === 0) {
      selectExprs.push(`${baseAlias}.*`);
    }

    let sql = `SELECT ${selectExprs.join(', ')} FROM ${baseTable} AS ${baseAlias}`;

    // Add JOIN clauses if any filters reference joined columns
    if (filters && filters.length > 0) {
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

    // Wrap in parentheses for use as subquery
    return `(${sql})`;
  }

  dispose(): void {
    this.rowCountSlot.dispose();
    this.rowsSlot.dispose();
    this.tempTableSlot.dispose();
  }
}
