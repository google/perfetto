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

import {SqlColumn} from './sql_column';
import {buildSqlQuery} from './query_builder';
import {raf} from '../../../../core/raf_scheduler';
import {assertTrue} from '../../../../base/logging';
import {SqlTableDescription} from './table_description';
import {Trace} from '../../../../public/trace';
import {Filters} from './filters';
import {TableColumn, tableColumnAlias, tableColumnId} from './table_column';
import {moveArrayItem} from '../../../../base/array_utils';
import {uuidv4} from '../../../../base/uuid';

export class SqlTableState {
  public readonly filters: Filters;
  public readonly uuid: string;

  private readonly additionalImports: string[];

  // Columns currently displayed to the user. All potential columns can be found `this.table.columns`.
  private columns: TableColumn[];

  constructor(
    readonly trace: Trace,
    readonly config: SqlTableDescription,
    private readonly args?: {
      initialColumns?: TableColumn[];
      additionalColumns?: TableColumn[];
      imports?: string[];
      filters?: Filters;
    },
  ) {
    this.additionalImports = args?.imports || [];
    this.uuid = uuidv4();

    this.filters = args?.filters || new Filters();
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
      filters: new Filters(this.filters.get()),
    });
  }

  private getSQLImports() {
    const tableImports = this.config.imports || [];
    return [...tableImports, ...this.additionalImports]
      .map((i) => `INCLUDE PERFETTO MODULE ${i};`)
      .join('\n');
  }

  // Return a query which selects the given columns, applying the filters currently in effect.
  getSqlQuery(columns: {[key: string]: SqlColumn}): string {
    return buildSqlQuery({
      table: this.config.name,
      columns,
      prefix: this.config.prefix,
      filters: this.filters.get(),
    });
  }

  // Returns the SQL imports needed for this table's queries.
  // These must be run before any queries that use the base query.
  getSqlImports(): string {
    return this.getSQLImports();
  }

  // Returns the query for the data.
  getBaseQuery(): string {
    const columns: {[key: string]: SqlColumn} = Object.fromEntries(
      this.columns.map((c) => [tableColumnAlias(c), c.column]),
    );

    return buildSqlQuery({
      table: this.config.name,
      columns,
      prefix: this.config.prefix,
      // No filters or orderBy - SQLDataSource will handle those
    });
  }

  addColumn(column: TableColumn, index: number) {
    this.columns.splice(index + 1, 0, column);
    raf.scheduleFullRedraw();
  }

  hideColumnAtIndex(index: number) {
    this.columns.splice(index, 1);
    raf.scheduleFullRedraw();
  }

  replaceColumnAtIndex(index: number, column: TableColumn) {
    this.columns[index] = column;
    raf.scheduleFullRedraw();
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
