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
import {ColumnOrderClause, SqlColumn, sqlColumnId} from './sql_column';
import {buildSqlQuery} from './query_builder';
import {raf} from '../../../../core/raf_scheduler';
import {SortDirection} from '../../../../base/comparison_utils';
import {assertTrue} from '../../../../base/logging';
import {SqlTableDescription} from './table_description';
import {Trace} from '../../../../public/trace';
import {areFiltersEqual, Filter, Filters} from './filters';
import {TableColumn, tableColumnAlias, tableColumnId} from './table_column';
import {moveArrayItem} from '../../../../base/array_utils';
import {uuidv4} from '../../../../base/uuid';

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

export class SqlTableState {
  public readonly filters: Filters;
  public readonly uuid: string;

  private readonly additionalImports: string[];

  // Columns currently displayed to the user. All potential columns can be found `this.table.columns`.
  private columns: TableColumn[];
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
      filters?: Filters;
      orderBy?: {
        column: TableColumn;
        direction: SortDirection;
      }[];
    },
  ) {
    this.additionalImports = args?.imports || [];
    this.uuid = uuidv4();

    this.filters = args?.filters || new Filters();
    this.filters.addObserver(() => this.reload());
    this.columns = [];

    if (args?.initialColumns !== undefined) {
      assertTrue(
        args?.additionalColumns === undefined,
        'Only one of `initialColumns` and `additionalColumns` can be set',
      );
      this.columns.push(...args.initialColumns);
    } else {
      for (const column of this.config.columns) {
        const columns = column.initialColumns?.() ?? [column];
        this.columns.push(...columns);
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
      filters: new Filters(this.filters.get()),
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
      prefix: this.config.prefix,
      filters: this.filters.get(),
      orderBy: this.getOrderedBy(),
    });
  }

  // We need column names to pass to the debug track creation logic.
  private buildSqlSelectStatement(mode: 'display' | 'data'): {
    selectStatement: string;
    columns: {[key: string]: string};
  } {
    const columns: {[key: string]: SqlColumn} = Object.fromEntries(
      this.columns.map((c) => [
        tableColumnAlias(c),
        mode === 'data' ? c.column : c.display ?? c.column,
      ]),
    );

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

      ${this.buildSqlSelectStatement('data').selectStatement}
    `;
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
    const filters = Array.from(this.filters.get());
    const res = await this.trace.engine.query(this.getCountRowsSQLQuery());
    if (res.error() !== undefined) return undefined;
    return {
      count: res.firstRow({count: NUM}).count,
      filters: filters,
    };
  }

  private buildRequest(): Request {
    const {selectStatement, columns} = this.buildSqlSelectStatement('display');
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
      newFilters && areFiltersEqual(newFilters, this.filters.get());
    this.data = undefined;
    const request = this.buildRequest();
    this.request = request;
    if (!filtersMatch) {
      this.rowCount = undefined;
    }

    // Schedule a full redraw to happen after a short delay (50 ms).
    // This is done to prevent flickering / visual noise and allow the UI to fetch
    // the initial data from the Trace Processor.
    // There is a chance that someone else schedules a full redraw in the
    // meantime, forcing the flicker, but in practice it works quite well and
    // avoids a lot of complexity for the callers.
    // 50ms is half of the responsiveness threshold (100ms):
    // https://web.dev/rail/#response-process-events-in-under-50ms
    setTimeout(() => raf.scheduleFullRedraw(), 50);

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

  sortBy(clause: {column: TableColumn; direction: SortDirection | undefined}) {
    // Remove previous sort by the same column.
    this.orderBy = this.orderBy.filter(
      (c) => tableColumnId(c.column) != tableColumnId(clause.column),
    );
    if (clause.direction === undefined) return;
    // Add the new sort clause to the front, so we effectively stable-sort the
    // data currently displayed to the user.
    this.orderBy.unshift({column: clause.column, direction: clause.direction});
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
      result.push({
        column: orderBy.column.column,
        direction: orderBy.direction,
      });
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
    this.willRemoveColumn(column);
    // TODO(altimin): we can avoid the fetch here if the orderBy hasn't changed.
    this.reload({offset: 'keep'});
  }

  replaceColumnAtIndex(index: number, column: TableColumn) {
    this.willRemoveColumn(this.columns[index]);
    this.columns[index] = column;
    this.reload({offset: 'keep'});
  }

  private willRemoveColumn(column: TableColumn) {
    // We can only filter by the visible columns to avoid confusing the user,
    // so we remove order by clauses that refer to the hidden column.
    this.orderBy = this.orderBy.filter(
      (c) => tableColumnId(c.column) !== tableColumnId(column),
    );
  }

  moveColumn(fromIndex: number, toIndex: number) {
    moveArrayItem(this.columns, fromIndex, toIndex);
  }

  getSelectedColumns(): readonly TableColumn[] {
    return this.columns;
  }
}

export function getSelectableColumns(state: SqlTableState): TableColumn[] {
  const columns = [...state.getSelectedColumns()];
  const existingColumnIds = new Set<string>(columns.map(tableColumnId));
  columns.concat(
    state.config.columns.filter(
      (c) => !existingColumnIds.has(tableColumnId(c)),
    ),
  );
  return columns;
}
