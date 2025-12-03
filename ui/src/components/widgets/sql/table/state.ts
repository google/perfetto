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

import {SqlColumn, sqlColumnId} from './sql_column';
import {assertTrue} from '../../../../base/logging';
import {SqlTableDescription} from './table_description';
import {Trace} from '../../../../public/trace';
import {TableColumn, tableColumnAlias, tableColumnId} from './table_column';
import {moveArrayItem} from '../../../../base/array_utils';
import {uuidv4} from '../../../../base/uuid';
import {buildSqlQuery} from './query_builder';

export class SqlTableState {
  public readonly uuid: string;

  private readonly additionalImports: string[];
  private readonly columnObservers: Array<() => void> = [];

  // Columns currently displayed to the user. All potential columns can be found in this.config.columns.
  private columns: TableColumn[];

  constructor(
    readonly trace: Trace,
    readonly config: SqlTableDescription,
    private readonly args?: {
      initialColumns?: TableColumn[];
      additionalColumns?: TableColumn[];
      imports?: string[];
    },
  ) {
    this.additionalImports = args?.imports || [];
    this.uuid = uuidv4();
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
  }

  clone(): SqlTableState {
    return new SqlTableState(this.trace, this.config, {
      initialColumns: this.columns,
      imports: this.args?.imports,
    });
  }

  /**
   * Register a callback to be called when columns change
   */
  onColumnsChanged(callback: () => void): void {
    this.columnObservers.push(callback);
  }

  /**
   * Notify all observers that columns have changed
   */
  private notifyColumnsChanged(): void {
    this.columnObservers.forEach((cb) => cb());
  }

  /**
   * Build the base SQL query for the data source.
   * This uses the query builder to handle joins and column expressions properly.
   * WITHOUT imports (those should be run separately)
   * and WITHOUT filters/sorting/pagination (those are handled by SQLDataSource)
   */
  buildBaseQuery(): string {
    // Build columns map for query builder
    const columns: {[key: string]: SqlColumn} = Object.fromEntries(
      this.columns.map((c) => [tableColumnAlias(c), c.column]),
    );

    // Use the query builder which handles joins and complex expressions
    return buildSqlQuery({
      table: this.config.name,
      columns,
      prefix: this.config.prefix,
      // No filters, groupBy, or orderBy - those are handled by SQLDataSource
    });
  }

  /**
   * Get SQL imports that need to be run before any queries.
   * These should be executed once when creating the data source.
   */
  getSQLImports(): string {
    const tableImports = this.config.imports || [];
    const allImports = [...tableImports, ...this.additionalImports];
    return allImports.length > 0
      ? allImports.map((i) => `INCLUDE PERFETTO MODULE ${i};`).join('\n')
      : '';
  }

  /**
   * Get mapping from SqlColumn IDs to their aliases in the query results
   */
  getColumnAliasMap(): {[key: string]: string} {
    const map: {[key: string]: string} = {};
    for (const column of this.columns) {
      map[sqlColumnId(column.column)] = tableColumnAlias(column);
    }
    return map;
  }

  addColumn(column: TableColumn, index: number) {
    this.columns.splice(index + 1, 0, column);
    this.notifyColumnsChanged();
  }

  hideColumnAtIndex(index: number) {
    this.columns.splice(index, 1);
    this.notifyColumnsChanged();
  }

  replaceColumnAtIndex(index: number, column: TableColumn) {
    this.columns[index] = column;
    this.notifyColumnsChanged();
  }

  moveColumn(fromIndex: number, toIndex: number) {
    moveArrayItem(this.columns, fromIndex, toIndex);
    this.notifyColumnsChanged();
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
