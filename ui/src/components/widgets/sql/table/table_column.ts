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

import m from 'mithril';
import {SqlValue} from '../../../../trace_processor/query_result';
import {Trace} from '../../../../public/trace';
import {SqlColumn, sqlColumnId} from './sql_column';
import {Filters} from './filters';
import {PerfettoSqlType} from '../../../../trace_processor/perfetto_sql_type';

// Interface which allows TableColumn to interact with the table (e.g. add filters, or run the query).
export interface TableManager {
  filters: Filters;
  trace: Trace;
  getSqlQuery(data: {[key: string]: SqlColumn}): string;
}

// Context passed to renderCell to allow interaction with the table.
export interface RenderCellContext {
  filters: Filters;
  trace: Trace;
  getSqlQuery(data: {[key: string]: SqlColumn}): string;
  hasColumn(column: TableColumn): boolean;
  addColumn(column: TableColumn): void;
}

// Context passed to listDerivedColumns to provide information about the table.
export interface ListColumnsContext {
  filters: Filters;
  trace: Trace;
  getSqlQuery(data: {[key: string]: SqlColumn}): string;
}

export interface TableColumnParams {
  // See TableColumn.tag.
  tag?: string;
  // See TableColumn.alias.
  alias?: string;
  // See TableColumn.startsHidden.
  startsHidden?: boolean;
}

// Class which represents a column in a table, which can be displayed to the user.
export interface TableColumn {
  readonly column: SqlColumn;
  readonly type: PerfettoSqlType | undefined;
  // In some cases, the UI needs additional information to be able to render a given cell (e.g. for display arg values,
  // we need to know arg type as well as arg value to generate a correct filter). In these cases, the common solution is fetch a JSON value
  // or a protobuf from the SQL and render it accordingly, so if set, `display` column overrides which value is going to be passed to `renderCell`.
  // `column` is still always going to be used for sorting, aggregation and casting.
  readonly display?: SqlColumn;

  // Column title to be displayed.
  // If not set, then `alias` will be used if it's unique.
  // If `alias` is not set as well, then `sqlColumnId(primaryColumn())` will be used.
  // TODO(altimin): This should return m.Children, but a bunch of things, including low-level widgets (Button, MenuItem, Anchor) need to be fixed first.
  getTitle?(): string | undefined;

  // Get column-specific menu items for this column.
  // This allows columns to provide their own menu items in the table header menu.
  // For example, a CastColumn can provide an "Undo cast" menu item.
  getColumnSpecificMenuItems?(args: {
    replaceColumn: (column: TableColumn) => void;
  }): m.Children;

  /**
   * Render a table cell. context can be undefined, in which case the cell should provide basic rendering (e.g. for pivot table).
   *
   * @param value The value to be rendered.
   * @param context Optional context to allow interaction with the table (e.g. adding filters).
   */
  renderCell(value: SqlValue, context?: RenderCellContext): RenderedCell;

  // A set of columns to be added when opening this table.
  // It has two primary purposes:
  // - Allow some columns to be hidden by default (by returning an empty array).
  // - Expand some columns (e.g. utid and upid are not meaningful by themselves, so the corresponding columns might add a "name" column by default).
  initialColumns?(): TableColumn[];

  // Some columns / values (arg_set_ids, table ids, etc) are primarily used to reference other data.
  // This method allows showing the user list of additional columns which can be fetched using this column.
  listDerivedColumns?(
    context: ListColumnsContext,
  ): undefined | (() => Promise<Map<string, TableColumn>>);
}

// Returns a unique identifier for the table column.
export function tableColumnId(column: TableColumn): string {
  return sqlColumnId(column.column);
}

export function tableColumnAlias(column: TableColumn): string {
  return tableColumnId(column).replace(/[^a-zA-Z0-9_]/g, (char) => {
    if (char === '_') {
      return '__';
    }
    return '_' + char.charCodeAt(0);
  });
}

export function columnTitle(column: TableColumn): string {
  if (column.getTitle !== undefined) {
    const title = column.getTitle();
    if (title !== undefined) return title;
  }
  return sqlColumnId(column.column);
}

export interface RenderedCell {
  readonly content: m.Children;
  readonly menu?: m.Children;
  readonly isNumerical?: boolean;
  readonly isNull?: boolean;
}
