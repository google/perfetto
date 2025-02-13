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
import {SimpleColumn} from '../table/table';
import {SqlColumn, sqlColumnId, sqlColumnName} from './sql_column';
import {Filter} from './filters';

// Interface which allows TableColumn to interact with the table (e.g. add filters, or run the query).
export interface LegacyTableManager {
  addFilter(filter: Filter): void;
  removeFilter(filter: Filter): void;

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
export abstract class LegacyTableColumn {
  constructor(params?: TableColumnParams) {
    this.tag = params?.tag;
    this.alias = params?.alias;
  }

  // Column title to be displayed.
  // If not set, then `alias` will be used if it's unique.
  // If `alias` is not set as well, then `sqlColumnId(primaryColumn())` will be used.
  // TODO(altimin): This should return m.Children, but a bunch of things, including low-level widgets (Button, MenuItem, Anchor) need to be fixed first.
  getTitle?(): string | undefined;

  // Some SQL columns can map to multiple table columns. For example, a "utid" can be displayed as an integer column, or as a "thread" column, which displays "$thread_name [$tid]".
  // Each column should have a unique id, so in these cases `tag` is appended to the primary column id to guarantee uniqueness.
  readonly tag?: string;

  // Preferred alias to be used in the SQL query. If omitted, column name will be used instead, including postfixing it with an integer if necessary.
  // However, e.g. explicit aliases like `process_name` and `thread_name` are typically preferred to `name_1`, `name_2`, hence the need for explicit aliasing.
  readonly alias?: string;

  // The SQL column this data corresponds to. Will be also used for sorting and aggregation purposes.
  abstract primaryColumn(): SqlColumn;

  // Sometimes to display an interactive cell more than a single value is needed (e.g. "time range" corresponds to (ts, dur) pair. While we want to show the duration, we would want to highlight the interval on hover, for which both timestamp and duration are needed.
  dependentColumns?(): {[key: string]: SqlColumn};

  // The set of underlying sql columns that should be sorted when this column is sorted.
  sortColumns?(): SqlColumn[];

  // Render a table cell. `value` corresponds to the fetched SQL value for the primary column, `dependentColumns` are the fetched values for the dependent columns.
  abstract renderCell(
    value: SqlValue,
    tableManager: LegacyTableManager,
    dependentColumns: {[key: string]: SqlValue},
  ): m.Children;

  // A set of columns to be added when opening this table.
  // It has two primary purposes:
  // - Allow some columns to be hidden by default (by returning an empty array).
  // - Expand some columns (e.g. utid and upid are not meaningful by themselves, so the corresponding columns might add a "name" column by default).
  initialColumns?(): LegacyTableColumn[];

  // Some columns / values (arg_set_ids, table ids, etc) are primarily used to reference other data.
  // This method allows showing the user list of additional columns which can be fetched using this column.
  listDerivedColumns?(
    manager: LegacyTableManager,
  ): undefined | (() => Promise<Map<string, LegacyTableColumn>>);
}

export class FromSimpleColumn extends LegacyTableColumn {
  readonly simpleCol: SimpleColumn;

  primaryColumn(): SqlColumn {
    return this.simpleCol.name;
  }

  renderCell(
    value: SqlValue,
    tableManager: LegacyTableManager,
    _dependentColumns: {[key: string]: SqlValue},
  ): m.Children {
    return this.simpleCol.renderCell(value, tableManager);
  }

  constructor(simpleCol: SimpleColumn, params?: TableColumnParams) {
    super(params);
    this.simpleCol = simpleCol;
  }
}

// Returns a unique identifier for the table column.
export function tableColumnId(column: LegacyTableColumn): string {
  const primaryColumnName = sqlColumnId(column.primaryColumn());
  if (column.tag) {
    return `${primaryColumnName}#${column.tag}`;
  }
  return primaryColumnName;
}

export function tableColumnAlias(column: LegacyTableColumn): string {
  return column.alias ?? sqlColumnName(column.primaryColumn());
}
