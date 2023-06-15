// Copyright (C) 2023 The Android Open Source Project
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

import {arrayEquals} from '../../base/array_utils';
import {SortDirection} from '../../base/comparison_utils';
import {EngineProxy} from '../../common/engine';
import {NUM, Row} from '../../common/query_result';
import {globals} from '../globals';
import {constraintsToQueryFragment} from '../sql_utils';
import {
  Column,
  columnFromSqlTableColumn,
  sqlProjectionsForColumn,
} from './column';
import {SqlTableDescription} from './table_description';

interface ColumnOrderClause {
  // We only allow the table to be sorted by the columns which are displayed to
  // the user to avoid confusion, so we use a reference to the underlying Column
  // here and compare it by reference down the line.
  column: Column;
  direction: SortDirection;
}

const ROW_LIMIT = 100;

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
  filters: string[];
}

export class SqlTableState {
  private readonly engine_: EngineProxy;
  private readonly table_: SqlTableDescription;

  get engine() {
    return this.engine_;
  }
  get table() {
    return this.table_;
  }

  private filters: string[];
  private columns: Column[];
  private orderBy: ColumnOrderClause[];
  private offset = 0;
  private data?: Data;
  private rowCount?: RowCount;

  constructor(
      engine: EngineProxy, table: SqlTableDescription, filters?: string[]) {
    this.engine_ = engine;
    this.table_ = table;

    this.filters = filters || [];
    this.columns = [];
    for (const column of this.table.columns) {
      if (column.startsHidden) continue;
      this.columns.push(columnFromSqlTableColumn(column));
    }
    this.orderBy = [];

    this.reload();
  }

  // Compute the actual columns to fetch.
  private getSQLProjections(): string[] {
    const result = new Set<string>();
    for (const column of this.columns) {
      for (const p of sqlProjectionsForColumn(column)) {
        result.add(p);
      }
    }
    return Array.from(result);
  }

  private getSQLImports() {
    return (this.table.imports || [])
        .map((i) => `SELECT IMPORT("${i}");`)
        .join('\n');
  }

  private getCountRowsSQLQuery(): string {
    return `
      ${this.getSQLImports()}

      SELECT
        COUNT() AS count
      FROM ${this.table.name}
      ${constraintsToQueryFragment({
      filters: this.filters,
    })}
    `;
  }

  getNonPaginatedSQLQuery(): string {
    const orderBy = this.orderBy.map((c) => ({
                                       fieldName: c.column.alias,
                                       direction: c.direction,
                                     }));
    return `
      ${this.getSQLImports()}

      SELECT
        ${this.getSQLProjections().join(',\n')}
      FROM ${this.table.name}
      ${constraintsToQueryFragment({
      filters: this.filters,
      orderBy: orderBy,
    })}
    `;
  }

  getPaginatedSQLQuery():
      string {  // We fetch one more row to determine if we can go forward.
    return `
      ${this.getNonPaginatedSQLQuery()}
      LIMIT ${ROW_LIMIT + 1}
      OFFSET ${this.offset}
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

  getDisplayedRange(): {from: number, to: number}|undefined {
    if (this.data === undefined) return undefined;
    return {
      from: this.offset + 1,
      to: this.offset + Math.min(this.data.rows.length, ROW_LIMIT),
    };
  }

  private async loadRowCount(): Promise<RowCount|undefined> {
    const filters = Array.from(this.filters);
    const res = await this.engine.query(this.getCountRowsSQLQuery());
    if (res.error() !== undefined) return undefined;
    return {
      count: res.firstRow({count: NUM}).count,
      filters: filters,
    };
  }

  private async loadData(): Promise<Data> {
    const queryRes = await this.engine.query(this.getPaginatedSQLQuery());
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

  private async reload(params?: {offset: 'reset'|'keep'}) {
    if ((params?.offset ?? 'reset') === 'reset') {
      this.offset = 0;
    }
    const updateRowCount = !arrayEquals(this.rowCount?.filters, this.filters);
    this.data = undefined;
    if (updateRowCount) {
      this.rowCount = undefined;
    }

    // Delay the visual update by 50ms to avoid flickering (if the query returns
    // before the data is loaded.
    setTimeout(() => globals.rafScheduler.scheduleFullRedraw(), 50);

    if (updateRowCount) {
      this.rowCount = await this.loadRowCount();
    }
    this.data = await this.loadData();

    globals.rafScheduler.scheduleFullRedraw();
  }

  getTotalRowCount(): number|undefined {
    return this.rowCount?.count;
  }

  getDisplayedRows(): Row[] {
    return this.data?.rows || [];
  }

  getQueryError(): string|undefined {
    return this.data?.error;
  }

  isLoading() {
    return this.data === undefined;
  }

  removeFilter(filter: string) {
    this.filters.splice(this.filters.indexOf(filter), 1);
    this.reload();
  }

  addFilter(filter: string) {
    this.filters.push(filter);
    this.reload();
  }

  getFilters(): string[] {
    return this.filters;
  }

  sortBy(clause: ColumnOrderClause) {
    this.orderBy = this.orderBy || [];
    // Remove previous sort by the same column.
    this.orderBy = this.orderBy.filter((c) => c.column !== clause.column);
    // Add the new sort clause to the front, so we effectively stable-sort the
    // data currently displayed to the user.
    this.orderBy.unshift(clause);
    this.reload();
  }

  unsort() {
    this.orderBy = [];
    this.reload();
  }

  isSortedBy(column: Column): SortDirection|undefined {
    if (!this.orderBy) return undefined;
    if (this.orderBy.length === 0) return undefined;
    if (this.orderBy[0].column !== column) return undefined;
    return this.orderBy[0].direction;
  }

  addColumn(column: Column, index: number) {
    this.columns.splice(index + 1, 0, column);
    this.reload({offset: 'keep'});
  }

  hideColumnAtIndex(index: number) {
    const column = this.columns[index];
    this.columns.splice(index, 1);
    // We can only filter by the visibile columns to avoid confusing the user,
    // so we remove order by clauses that refer to the hidden column.
    this.orderBy = this.orderBy.filter((c) => c.column !== column);
    // TODO(altimin): we can avoid the fetch here if the orderBy hasn't changed.
    this.reload({offset: 'keep'});
  }

  getSelectedColumns(): Column[] {
    return this.columns;
  }
};
