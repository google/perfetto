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
import {SortDirection} from '../../../../base/comparison_utils';
import {arrayEquals} from '../../../../base/array_utils';
import {Trace} from '../../../../public/trace';

// We are dealing with two types of columns here:
// - Column, which is shown to a user in table (high-level, ColumnTable).
// - Column in the underlying SQL data (low-level, SqlColumn).
// They are related, but somewhat separate due to the fact that some table columns need to work with multiple SQL values to display it properly.
// For example, a "time range" column would need both timestamp and duration to display interactive experience (e.g. highlight the time range on hover).
// Each TableColumn has a primary SqlColumn, as well as optional dependent columns.

// A source table for a SQL column, representing the joined table and the join constraints.
export type SourceTable = {
  table: string;
  joinOn: {[key: string]: SqlColumn};
  // Whether more performant 'INNER JOIN' can be used instead of 'LEFT JOIN'.
  // Special care should be taken to ensure that a) all rows exist in a target table, and b) the source is not null, otherwise the rows will be filtered out.
  // false by default.
  innerJoin?: boolean;
};

// A column in the SQL query. It can be either a column from a base table or a "lookup" column from a joined table.
export type SqlColumn =
  | string
  | {
      column: string;
      source: SourceTable;
    };

// List of columns of args, corresponding to arg values, which cause a short-form of the ID to be generated.
// (e.g. arg_set_id[foo].int instead of args[arg_set_id,key=foo].int_value).
const ARG_COLUMN_TO_SUFFIX: {[key: string]: string} = {
  display_value: '',
  int_value: '.int',
  string_value: '.str',
  real_value: '.real',
};

// A unique identifier for the SQL column.
export function sqlColumnId(column: SqlColumn): string {
  if (typeof column === 'string') {
    return column;
  }
  // Special case: If the join is performed on a single column `id`, we can use a simpler representation (i.e. `table[id].column`).
  if (arrayEquals(Object.keys(column.source.joinOn), ['id'])) {
    return `${column.source.table}[${sqlColumnId(Object.values(column.source.joinOn)[0])}].${column.column}`;
  }
  // Special case: args lookup. For it, we can use a simpler representation (i.e. `arg_set_id[key]`).
  if (
    column.column in ARG_COLUMN_TO_SUFFIX &&
    column.source.table === 'args' &&
    arrayEquals(Object.keys(column.source.joinOn).sort(), ['arg_set_id', 'key'])
  ) {
    const key = column.source.joinOn['key'];
    const argSetId = column.source.joinOn['arg_set_id'];
    return `${sqlColumnId(argSetId)}[${sqlColumnId(key)}]${ARG_COLUMN_TO_SUFFIX[column.column]}`;
  }
  // Otherwise, we need to list all the join constraints.
  const lookup = Object.entries(column.source.joinOn)
    .map(([key, value]): string => {
      const valueStr = sqlColumnId(value);
      if (key === valueStr) return key;
      return `${key}=${sqlColumnId(value)}`;
    })
    .join(', ');
  return `${column.source.table}[${lookup}].${column.column}`;
}

export function isSqlColumnEqual(a: SqlColumn, b: SqlColumn): boolean {
  return sqlColumnId(a) === sqlColumnId(b);
}

function sqlColumnName(column: SqlColumn): string {
  if (typeof column === 'string') {
    return column;
  }
  return column.column;
}

// Interface which allows TableColumn and TableColumnSet to interact with the table (e.g. add filters, or run the query).
export interface TableManager {
  addFilter(filter: Filter): void;

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

export interface AggregationConfig {
  dataType?: 'nominal' | 'quantitative';
}

// Class which represents a column in a table, which can be displayed to the user.
// It is based on the primary SQL column, but also contains additional information needed for displaying it as a part of a table.
export abstract class TableColumn {
  constructor(params?: TableColumnParams) {
    this.tag = params?.tag;
    this.alias = params?.alias;
    this.startsHidden = params?.startsHidden ?? false;
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

  // Whether the column should be hidden by default.
  readonly startsHidden: boolean;

  // The SQL column this data corresponds to. Will be also used for sorting and aggregation purposes.
  abstract primaryColumn(): SqlColumn;

  // Sometimes to display an interactive cell more than a single value is needed (e.g. "time range" corresponds to (ts, dur) pair. While we want to show the duration, we would want to highlight the interval on hover, for which both timestamp and duration are needed.
  dependentColumns?(): {[key: string]: SqlColumn};

  // The set of underlying sql columns that should be sorted when this column is sorted.
  sortColumns?(): SqlColumn[];

  // Render a table cell. `value` corresponds to the fetched SQL value for the primary column, `dependentColumns` are the fetched values for the dependent columns.
  abstract renderCell(
    value: SqlValue,
    tableManager: TableManager,
    dependentColumns: {[key: string]: SqlValue},
  ): m.Children;

  // Specifies how this column should be aggregated. If not set, then all
  // numeric columns will be treated as quantitative, and all other columns as
  // nominal.
  aggregation?(): AggregationConfig;
}

// Returns a unique identifier for the table column.
export function tableColumnId(column: TableColumn): string {
  const primaryColumnName = sqlColumnId(column.primaryColumn());
  if (column.tag) {
    return `${primaryColumnName}#${column.tag}`;
  }
  return primaryColumnName;
}

export function tableColumnAlias(column: TableColumn): string {
  return column.alias ?? sqlColumnName(column.primaryColumn());
}

// This class represents a set of columns, from which the user can choose which columns to display. It is typically impossible or impractical to list all possible columns, so this class allows to discover them dynamically.
// Two examples of canonical TableColumnSet usage are:
// - Argument sets, where the set of arguments can be arbitrary large (and can change when the user changes filters on the table).
// - Dependent columns, where the id.
export abstract class TableColumnSet {
  // TODO(altimin): This should return m.Children, same comment as in TableColumn.getTitle applies here.
  abstract getTitle(): string;

  // Returns a list of columns from this TableColumnSet which should be displayed by default.
  initialColumns?(): TableColumn[];

  // Returns a list of columns which can be added to the table from the current TableColumnSet.
  abstract discover(manager: TableManager): Promise<
    {
      key: string;
      column: TableColumn | TableColumnSet;
    }[]
  >;
}

// A filter which can be applied to the table.
export interface Filter {
  // Operation: it takes a list of column names and should return a valid SQL expression for this filter.
  op: (cols: string[]) => string;
  // Columns that the `op` should reference. The number of columns should match the number of interpolations in `op`.
  columns: SqlColumn[];
  // Returns a human-readable title for the filter. If not set, `op` will be used.
  // TODO(altimin): This probably should return m.Children, but currently Button expects its label to be string.
  getTitle?(): string;
}

// Returns a default string representation of the filter.
export function formatFilter(filter: Filter): string {
  return filter.op(filter.columns.map((c) => sqlColumnId(c)));
}

// Returns a human-readable title for the filter.
export function filterTitle(filter: Filter): string {
  if (filter.getTitle !== undefined) {
    return filter.getTitle();
  }
  return formatFilter(filter);
}

// A column order clause, which specifies the column and the direction in which it should be sorted.
export interface ColumnOrderClause {
  column: SqlColumn;
  direction: SortDirection;
}
