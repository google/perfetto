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

// A column in the SQL query. It can be either be:
// - A column in the table (represented by a string).
// - An expression,computing the value based on other columns (represented by SqlExpression).
// - A derived column: `column` from `source.table`, with `source.joinOn` describing
//   how `source.table` should be joined with the primary table (usually join on ID).
export type SqlColumn =
  | string
  | SqlExpression
  | {
      column: string;
      source: SourceTable;
      // Optional unique id for this column, which will be shown to the user (e.g. in column header and filters).
      id?: string;
    };

// A class representing a SQL column which is computed based on other columns.
export class SqlExpression {
  // op: Operation, which takes the expressions for columns and returns a valid SQL expression.
  // columns: List of columns that the operation references.
  // id: optional unique id for this column, which will be shown to the user (e.g. in column header and filters).
  constructor(
    public op: (cols: string[]) => string,
    public columns: SqlColumn[],
    public id?: string,
  ) {
    this.op = op;
  }
}

// A source table for a derived SQL column, describining the additional table to be joined with the primary source table and the join contraints.
export type SourceTable = {
  table: string;
  joinOn: {[key: string]: SqlColumn};
  // Whether more performant 'INNER JOIN' can be used instead of 'LEFT JOIN'.
  // Special care should be taken to ensure that a) all rows exist in a target table, and b) the source is not null, otherwise the rows will be filtered out.
  // false by default.
  innerJoin?: boolean;
};

// A unique identifier for the SQL column.
export function sqlColumnId(column: SqlColumn): string {
  // For table columns, use the column name as an id.
  if (typeof column === 'string') {
    return column;
  }
  // For expressions, use the specified id, or plug the ids of the columns into the expression.
  if (column instanceof SqlExpression) {
    if (column.id !== undefined) return column.id;
    return `${column.op(column.columns.map(sqlColumnId))}`;
  }
  if (column.id !== undefined) {
    return column.id;
  }
  // Special case: If the join is performed on a single column `id`, we can use a simpler representation (i.e. `table[id].column`).
  if (arrayEquals(Object.keys(column.source.joinOn), ['id'])) {
    return `${column.source.table}[${sqlColumnId(Object.values(column.source.joinOn)[0])}].${column.column}`;
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

// A column order clause, which specifies the column and the direction in which it should be sorted.
export interface ColumnOrderClause {
  column: SqlColumn;
  direction: SortDirection;
}
