// Copyright (C) 2024 The Android Open Source Project
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

import {NUM, Row} from '../../../../trace_processor/query_result';
import {
  tableColumnAlias,
  ColumnOrderClause,
  Filter,
  isSqlColumnEqual,
  SqlColumn,
  sqlColumnId,
  TableColumn,
  tableColumnId,
} from './column';
import {buildSqlQuery} from './query_builder';
import {raf} from '../../../../core/raf_scheduler';
import {SortDirection} from '../../../../base/comparison_utils';
import {assertTrue} from '../../../../base/logging';
import {SqlTableDescription} from './table_description';
import {Trace} from '../../../../public/trace';

const ROW_LIMIT = 100;

interface Request {
  // Select statement, without the includes and the LIMIT and OFFSET clauses.
  selectStatement: string;
  // Query, including the LIMIT and OFFSET clauses.
  query: string;
  // Map of SqlColumn's id to the column name in the query.
  columns: {[key: string]: string};
}

// Result of the execution of the query.
interface Data {
  // Rows to show, including pagination.
  rows: Row[];
  error?: string;
}

interface RowCount {
  // Total number of rows in view, excluding the pagination.
  // Undefined if the query returned an error.
  count: number;
  // Filters which were used to compute this row count.
  // We need to recompute the totalRowCount only when filters change and not
  // when the set of columns / order by changes.
  filters: Filter[];
}

function isFilterEqual(a: Filter, b: Filter) {
  return (
    a.op === b.op &&
    a.columns.length === b.columns.length &&
    a.columns.every((c, i) => isSqlColumnEqual(c, b.columns[i]))
  );
}

function areFiltersEqual(a: Filter[], b: Filter[]) {
  if (a.length !== b.length) return false;
  return a.every((f, i) => isFilterEqual(f, b[i]));
}

export class SqlTableState {
  private readonly additionalImports: string[];

  // Columns currently displayed to the user. All potential columns can be found `this.table.columns`.
  private columns: TableColumn[];
  private filters: Filter[];
  private orderBy: {
    column: TableColumn;
    direction: SortDirection;
  }[];
  private offset = 0;
  private request: Request;
  private data?: Data;
  private rowCount?: RowCount;

  constructor(
    readonly trace: Trace,
    readonly config: SqlTableDescription,
    private readonly args?: {
      initialColumns?: TableColumn[];
      additionalColumns?: TableColumn[];
      imports?: string[];
      filters?: Filter[];
      orderBy?: {
        column: TableColumn;
        direction: SortDirection;
      }[];
    },
  ) {
    this.additionalImports = args?.imports || [];

    this.filters = args?.filters || [];
    this.columns = [];

    if (args?.initialColumns !== undefined) {
      assertTrue(
        args?.additionalColumns === undefined,
        'Only one of `initialColumns` and `additionalColumns` can be set',
      );
      this.columns.push(...args.initialColumns);
    } else {
      for (const column of this.config.columns) {
        if (column instanceof TableColumn) {
          if (column.startsHidden !== true) {
            this.columns.push(column);
          }
        } else {
          const cols = column.initialColumns?.();
          for (const col of cols ?? []) {
            this.columns.push(col);
          }
        }
      }
      if (args?.additionalColumns !== undefined) {
        this.columns.push(...args.additionalColumns);
      }
    }

    this.orderBy = args?.orderBy ?? [];

    this.request = this.buildRequest();
    this.reload();
  }

  clone(): SqlTableState {
    return new SqlTableState(this.trace, this.config, {
      initialColumns: this.columns,
      imports: this.args?.imports,
      filters: this.filters,
      orderBy: this.orderBy,
    });
  }

  private getSQLImports() {
    const tableImports = this.config.imports || [];
    return [...tableImports, ...this.additionalImports]
      .map((i) => `INCLUDE PERFETTO MODULE ${i};`)
      .join('\n');
  }

  private getCountRowsSQLQuery(): string {
    return `
      ${this.getSQLImports()}

      ${this.getSqlQuery({count: 'COUNT()'})}
    `;
  }

  // Return a query which selects the given columns, applying the filters and ordering currently in effect.
  getSqlQuery(columns: {[key: string]: SqlColumn}): string {
    return buildSqlQuery({
      table: this.config.name,
      columns,
      filters: this.filters,
      orderBy: this.getOrderedBy(),
    });
  }

  // We need column names to pass to the debug track creation logic.
  private buildSqlSelectStatement(): {
    selectStatement: string;
    columns: {[key: string]: string};
  } {
    const columns: {[key: string]: SqlColumn} = {};
    // A set of columnIds for quick lookup.
    const sqlColumnIds: Set<string> = new Set();
    // We want to use the shortest posible name for each column, but we also need to mindful of potential collisions.
    // To avoid collisions, we append a number to the column name if there are multiple columns with the same name.
    const columnNameCount: {[key: string]: number} = {};

    const tableColumns: {column: TableColumn; name: string; alias: string}[] =
      [];

    for (const column of this.columns) {
      // If TableColumn has an alias, use it. Otherwise, use the column name.
      const name = tableColumnAlias(column);
      if (!(name in columnNameCount)) {
        columnNameCount[name] = 0;
      }

      // Note: this can break if the user specifies a column which ends with `__<number>`.
      // We intentionally use two underscores to avoid collisions and will fix it down the line if it turns out to be a problem.
      const alias = `${name}__${++columnNameCount[name]}`;
      tableColumns.push({column, name, alias});
    }

    for (const column of tableColumns) {
      const sqlColumn = column.column.primaryColumn();
      // If we have only one column with this name, we don't need to disambiguate it.
      if (columnNameCount[column.name] === 1) {
        columns[column.name] = sqlColumn;
      } else {
        columns[column.alias] = sqlColumn;
      }
      sqlColumnIds.add(sqlColumnId(sqlColumn));
    }

    // We are going to be less fancy for the dependendent columns can just always suffix them with a unique integer.
    let dependentColumnCount = 0;
    for (const column of tableColumns) {
      const dependentColumns =
        column.column.dependentColumns !== undefined
          ? column.column.dependentColumns()
          : {};
      for (const col of Object.values(dependentColumns)) {
        if (sqlColumnIds.has(sqlColumnId(col))) continue;
        const name = typeof col === 'string' ? col : col.column;
        const alias = `__${name}_${dependentColumnCount++}`;
        columns[alias] = col;
        sqlColumnIds.add(sqlColumnId(col));
      }
    }

    return {
      selectStatement: this.getSqlQuery(columns),
      columns: Object.fromEntries(
        Object.entries(columns).map(([key, value]) => [
          sqlColumnId(value),
          key,
        ]),
      ),
    };
  }

  getNonPaginatedSQLQuery(): string {
    return `
      ${this.getSQLImports()}

      ${this.buildSqlSelectStatement().selectStatement}
    `;
  }

  getPaginatedSQLQuery(): Request {
    return this.request;
  }

  canGoForward(): boolean {
    if (this.data === undefined) return false;
    return this.data.rows.length > ROW_LIMIT;
  }

  canGoBack(): boolean {
    if (this.data === undefined) return false;
    return this.offset > 0;
  }

  goForward() {
    if (!this.canGoForward()) return;
    this.offset += ROW_LIMIT;
    this.reload({offset: 'keep'});
  }

  goBack() {
    if (!this.canGoBack()) return;
    this.offset -= ROW_LIMIT;
    this.reload({offset: 'keep'});
  }

  getDisplayedRange(): {from: number; to: number} | undefined {
    if (this.data === undefined) return undefined;
    return {
      from: this.offset + 1,
      to: this.offset + Math.min(this.data.rows.length, ROW_LIMIT),
    };
  }

  private async loadRowCount(): Promise<RowCount | undefined> {
    const filters = Array.from(this.filters);
    const res = await this.trace.engine.query(this.getCountRowsSQLQuery());
    if (res.error() !== undefined) return undefined;
    return {
      count: res.firstRow({count: NUM}).count,
      filters: filters,
    };
  }

  private buildRequest(): Request {
    const {selectStatement, columns} = this.buildSqlSelectStatement();
    // We fetch one more row to determine if we can go forward.
    const query = `
      ${this.getSQLImports()}
      ${selectStatement}
      LIMIT ${ROW_LIMIT + 1}
      OFFSET ${this.offset}
    `;
    return {selectStatement, query, columns};
  }

  private async loadData(): Promise<Data> {
    const queryRes = await this.trace.engine.query(this.request.query);
    const rows: Row[] = [];
    for (const it = queryRes.iter({}); it.valid(); it.next()) {
      const row: Row = {};
      for (const column of queryRes.columns()) {
        row[column] = it.get(column);
      }
      rows.push(row);
    }

    return {
      rows,
      error: queryRes.error(),
    };
  }

  private async reload(params?: {offset: 'reset' | 'keep'}) {
    if ((params?.offset ?? 'reset') === 'reset') {
      this.offset = 0;
    }

    const newFilters = this.rowCount?.filters;
    const filtersMatch =
      newFilters && areFiltersEqual(newFilters, this.filters);
    this.data = undefined;
    const request = this.buildRequest();
    this.request = request;
    if (!filtersMatch) {
      this.rowCount = undefined;
    }

    // Run a delayed UI update to avoid flickering if the query returns quickly.
    raf.scheduleDelayedFullRedraw();

    if (!filtersMatch) {
      this.rowCount = await this.loadRowCount();
    }

    const data = await this.loadData();

    // If the request has changed since we started loading the data, do not update the state.
    if (this.request !== request) return;
    this.data = data;

    raf.scheduleFullRedraw();
  }

  getTotalRowCount(): number | undefined {
    return this.rowCount?.count;
  }

  getCurrentRequest(): Request {
    return this.request;
  }

  getDisplayedRows(): Row[] {
    return this.data?.rows || [];
  }

  getQueryError(): string | undefined {
    return this.data?.error;
  }

  isLoading() {
    return this.data === undefined;
  }

  addFilter(filter: Filter) {
    this.filters.push(filter);
    this.reload();
  }

  removeFilter(filter: Filter) {
    this.filters = this.filters.filter((f) => !isFilterEqual(f, filter));
    this.reload();
  }

  getFilters(): Filter[] {
    return this.filters;
  }

  sortBy(clause: {column: TableColumn; direction: SortDirection}) {
    // Remove previous sort by the same column.
    this.orderBy = this.orderBy.filter(
      (c) => tableColumnId(c.column) != tableColumnId(clause.column),
    );
    // Add the new sort clause to the front, so we effectively stable-sort the
    // data currently displayed to the user.
    this.orderBy.unshift(clause);
    this.reload();
  }

  unsort() {
    this.orderBy = [];
    this.reload();
  }

  isSortedBy(column: TableColumn): SortDirection | undefined {
    if (this.orderBy.length === 0) return undefined;
    if (tableColumnId(this.orderBy[0].column) !== tableColumnId(column)) {
      return undefined;
    }
    return this.orderBy[0].direction;
  }

  getOrderedBy(): ColumnOrderClause[] {
    const result: ColumnOrderClause[] = [];
    for (const orderBy of this.orderBy) {
      const sortColumns = orderBy.column.sortColumns?.() ?? [
        orderBy.column.primaryColumn(),
      ];
      for (const column of sortColumns) {
        result.push({column, direction: orderBy.direction});
      }
    }
    return result;
  }

  addColumn(column: TableColumn, index: number) {
    this.columns.splice(index + 1, 0, column);
    this.reload({offset: 'keep'});
  }

  hideColumnAtIndex(index: number) {
    const column = this.columns[index];
    this.columns.splice(index, 1);
    // We can only filter by the visibile columns to avoid confusing the user,
    // so we remove order by clauses that refer to the hidden column.
    this.orderBy = this.orderBy.filter(
      (c) => tableColumnId(c.column) !== tableColumnId(column),
    );
    // TODO(altimin): we can avoid the fetch here if the orderBy hasn't changed.
    this.reload({offset: 'keep'});
  }

  moveColumn(fromIndex: number, toIndex: number) {
    if (fromIndex === toIndex) return;
    const column = this.columns[fromIndex];
    this.columns.splice(fromIndex, 1);
    if (fromIndex < toIndex) {
      // We have deleted a column, therefore we need to adjust the target index.
      --toIndex;
    }
    this.columns.splice(toIndex, 0, column);
    raf.scheduleFullRedraw();
  }

  getSelectedColumns(): TableColumn[] {
    return this.columns;
  }
}
