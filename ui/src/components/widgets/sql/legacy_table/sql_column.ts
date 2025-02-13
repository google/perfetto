// Copyright (C) 2025 The Android Open Source Project
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

import {arrayEquals} from '../../../../base/array_utils';
import {SortDirection} from '../../../../base/comparison_utils';

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

export function sqlColumnName(column: SqlColumn): string {
  if (typeof column === 'string') {
    return column;
  }
  return column.column;
}

// A column order clause, which specifies the column and the direction in which it should be sorted.
export interface ColumnOrderClause {
  column: SqlColumn;
  direction: SortDirection;
}
