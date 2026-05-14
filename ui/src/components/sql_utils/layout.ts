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
interface GenerateSqlArgs {
  // A string array list of the columns to be selected from the table.
  columns: string[];

  // The table or select statement in the FROM clause.
  source: string;

  // The expression returning the timestamp of a slice in the source table.
  ts: string;

  // The expression returning the duration of a slice in the source table.
  dur: string;

  // The WHERE clause to filter data from the source table (optional).
  whereClause?: string;

  // The ORDER BY clause for the results (optional).
  orderByClause?: string;

  // The PARTITION BY clause for the internal_layout window function (optional).
  partitionByClause?: string;
}

// Function to generate a SELECT statement utilizing the internal_layout
// SQL function as a depth field.
export function generateSqlWithInternalLayout(
  sqlArgs: GenerateSqlArgs,
): string {
  const maybePartitionBy =
    sqlArgs.partitionByClause === undefined
      ? ''
      : `PARTITION BY ${sqlArgs.partitionByClause} `;
  let sql =
    `SELECT ` +
    sqlArgs.columns.toString() +
    `, internal_layout(${sqlArgs.ts}, ${sqlArgs.dur}) OVER (` +
    `${maybePartitionBy}ORDER BY ${sqlArgs.ts}` +
    ' ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS depth' +
    ` FROM (${sqlArgs.source})`;
  if (sqlArgs.whereClause !== undefined) {
    sql += ' WHERE ' + sqlArgs.whereClause;
  }
  if (sqlArgs.orderByClause !== undefined) {
    sql += ' ORDER BY ' + sqlArgs.orderByClause;
  }
  return sql;
}
