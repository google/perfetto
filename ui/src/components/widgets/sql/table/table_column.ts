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

// Interface which allows TableColumn to interact with the table (e.g. add filters, or run the query).
export interface TableManager {
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
// It is based on the primary SQL column, but also contains additional information needed for displaying it as a part of a table.
export interface TableColumn<
  SupportingColumns extends {[key: string]: SqlColumn} = {},
> {
  readonly column: SqlColumn;

  // Column title to be displayed.
  // If not set, then `alias` will be used if it's unique.
  // If `alias` is not set as well, then `sqlColumnId(primaryColumn())` will be used.
  // TODO(altimin): This should return m.Children, but a bunch of things, including low-level widgets (Button, MenuItem, Anchor) need to be fixed first.
  getTitle?(): string | undefined;

  // In some cases to render a value in a table, we need information from additional columns.
  // For example, args have three related columns: int_value, string_value and real_value. From the user perspective, we want to coalesce them into a single "value" column,
  // but to do this correctly we need to fetch the `type` column.
  supportingColumns?(): SupportingColumns;

  /**
   * Render a table cell. tableManager can be undefined, in which case the cell should provide basic rendering (e.g. for pivot table).
   *
   * @param value The value to be rendered.
   * @param tableManager Optional table manager to allow interaction with the table (e.g. adding filters).
   * @param supportingValues Optional additional values needed to render the cell.
   */
  renderCell(
    value: SqlValue,
    tableManager?: TableManager,
    supportingValues?: {[key in keyof SupportingColumns]: SqlValue},
  ): m.Children;

  // A set of columns to be added when opening this table.
  // It has two primary purposes:
  // - Allow some columns to be hidden by default (by returning an empty array).
  // - Expand some columns (e.g. utid and upid are not meaningful by themselves, so the corresponding columns might add a "name" column by default).
  initialColumns?(): TableColumn[];

  // Some columns / values (arg_set_ids, table ids, etc) are primarily used to reference other data.
  // This method allows showing the user list of additional columns which can be fetched using this column.
  listDerivedColumns?(
    manager: TableManager,
  ): undefined | (() => Promise<Map<string, TableColumn>>);
}

// Returns a unique identifier for the table column.
export function tableColumnId(column: TableColumn): string {
  return sqlColumnId(column.column);
}

export function tableColumnAlias(column: TableColumn): string {
  return tableColumnId(column).replace(/[^a-zA-Z0-9_]/g, '__');
}

export function columnTitle(column: TableColumn): string {
  if (column.getTitle !== undefined) {
    const title = column.getTitle();
    if (title !== undefined) return title;
  }
  return sqlColumnId(column.column);
}
