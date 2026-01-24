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

import {QuerySlot, SerialTaskQueue} from '../../../../base/query_slot';
import {Engine} from '../../../../trace_processor/engine';
import {NUM, Row} from '../../../../trace_processor/query_result';
import {runQueryForQueryTable} from '../../../query_table/queries';
import {DataSourceRows, FlatModel} from '../datagrid_engine';
import {SQLSchemaRegistry, SQLSchemaResolver} from '../sql_schema';
import {filterToSql, toAlias} from '../sql_utils';

export class FlatEngine {
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

  get(model: FlatModel): DataSourceRows {
    const {columns, filters = [], pagination, sort} = model;

    // Load the row count first
    const rowCountResult = this.rowCountSlot.use({
      key: {
        // The row count doesn't depend on pagination or sort
        columns,
        filters: serializeFilters(filters),
      },
      queryFn: async () => {
        const query = buildQuery(this.sqlSchema, this.rootSchemaName, {
          mode: 'flat',
          columns,
          filters,
        });
        const result = await this.engine.query(
          `SELECT COUNT(*) as count FROM (${query})`,
        );
        return result.firstRow({count: NUM}).count;
      },
    });

    const rowsResult = this.rowsSlot.use({
      key: {
        columns: model.columns,
        filters: serializeFilters(filters),
        pagination,
        sort,
      },
      retainOn: ['pagination', 'sort'],
      queryFn: async () => {
        const query = buildQuery(this.sqlSchema, this.rootSchemaName, model);
        const result = await runQueryForQueryTable(query, this.engine);
        const rows = result.rows;
        return {rows, rowOffset: pagination?.offset ?? 0};
      },
    });

    // Merge the two results into a single bundle
    return {
      totalRows: rowCountResult.data,
      rowOffset: rowsResult.data?.rowOffset,
      rows: rowsResult.data?.rows,
      isPending: rowCountResult.isPending || rowsResult.isPending,
    };
  }

  dispose(): void {
    this.rowCountSlot.dispose();
    this.rowsSlot.dispose();
  }
}

/**
 * Filters can contain values that are not JSON friendly - e.g. Uint8Arrays.
 * This function returns a serialized version of the filters suitable for use as
 * a cache key.
 */
function serializeFilters(filters: FlatModel['filters']) {
  return filters?.map((filter) => filterToSql(filter, filter.field));
}

function buildQuery(
  sqlSchema: SQLSchemaRegistry,
  rootSchemaName: string,
  {columns, filters, pagination, sort}: FlatModel,
): string {
  const resolver = new SQLSchemaResolver(sqlSchema, rootSchemaName);

  let sql = buildSelectClause(resolver, columns);
  sql = addFromClause(sql, resolver);
  sql = addFilterClause(sql, resolver, filters);
  sql = addOrderByClause(sql, sort);
  sql = addPaginationClause(sql, pagination);

  return sql;
}

function buildSelectClause(
  resolver: SQLSchemaResolver,
  columns: FlatModel['columns'],
): string {
  const selectExprs: string[] = [];

  if (columns) {
    for (const col of columns) {
      const sqlExpr = resolver.resolveColumnPath(col.field);
      if (sqlExpr) {
        const alias = toAlias(col.alias);
        selectExprs.push(`${sqlExpr} AS ${alias}`);
      }
    }
  }

  if (selectExprs.length === 0) {
    selectExprs.push(`${resolver.getBaseAlias()}.*`);
  }

  return `SELECT ${selectExprs.join(',\n       ')}`;
}

function addFromClause(sql: string, resolver: SQLSchemaResolver): string {
  const baseTable = resolver.getBaseTable();
  const baseAlias = resolver.getBaseAlias();
  const joinClauses = resolver.buildJoinClauses();

  sql += `\nFROM ${baseTable} AS ${baseAlias}`;
  if (joinClauses) {
    sql += `\n${joinClauses}`;
  }
  return sql;
}

function addFilterClause(
  sql: string,
  resolver: SQLSchemaResolver,
  filters: FlatModel['filters'],
): string {
  if (filters && filters.length > 0) {
    const whereConditions = filters.map((filter) => {
      const sqlExpr = resolver.resolveColumnPath(filter.field);
      return filterToSql(filter, sqlExpr ?? filter.field);
    });
    sql += `\nWHERE ${whereConditions.join(' AND ')}`;
  }
  return sql;
}

function addOrderByClause(sql: string, sort: FlatModel['sort']): string {
  if (sort) {
    sql += `\nORDER BY ${sort.alias} ${sort.direction}`;
  }
  return sql;
}

function addPaginationClause(
  sql: string,
  pagination: FlatModel['pagination'],
): string {
  if (pagination) {
    sql += `\nLIMIT ${pagination.limit} OFFSET ${pagination.offset ?? 0}`;
  }
  return sql;
}
