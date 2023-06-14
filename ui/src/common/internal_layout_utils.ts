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

// Object to facilitate generation of SELECT statement using
// generateSqlWithInternalLayout.
//
// Fields:
// @columns: a string array list of the columns to be selected from the table.
// required by the internal_layout function.
// @sourceTable: the table in the FROM clause, source of the data.
// @whereClause: the WHERE clause to filter data from the source table.
// @orderByClause: the ORDER BY clause for the query data.
interface GenerateSqlArgs {
  columns: string[];
  sourceTable: string;
  ts: string;
  dur: string;
  whereClause?: string;
  orderByClause?: string;
}

// Function to generate a SELECT statement utilizing the internal_layout
// SQL function as a depth field.
export function generateSqlWithInternalLayout(sqlArgs: GenerateSqlArgs):
    string {
  let sql = `SELECT ` + sqlArgs.columns.toString() +
      `, internal_layout(${sqlArgs.ts}, ${sqlArgs.dur}) OVER (ORDER BY ${
                sqlArgs.ts}` +
      ' ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS depth' +
      ' FROM ' + sqlArgs.sourceTable;
  if (sqlArgs.whereClause !== undefined) {
    sql += ' WHERE ' + sqlArgs.whereClause;
  }
  if (sqlArgs.orderByClause !== undefined) {
    sql += ' ORDER BY ' + sqlArgs.orderByClause;
  }
  return sql;
}
