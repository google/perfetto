// Copyright (C) 2026 The Android Open Source Project
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

import {Engine} from '../../../../trace_processor/engine';
import {SqlValue} from '../../../../trace_processor/query_result';

/**
 * Data grid row.
 */
export interface DataGridRow {
  [key: string]: SqlValue;
}

/**
 * Data grid result.
 */
export interface DataGridResult {
  readonly columns: readonly string[];
  readonly rows: readonly DataGridRow[];
  readonly totalCount: number;
}

/**
 * Load datagrid data — raw rows from the table with all columns.
 *
 * Note: tableName is interpolated directly into the SQL query. This is safe
 * because it comes from trusted sources (materialized table names from the
 * query execution service), not from user input.
 */
export async function loadDatagridData(
  engine: Engine,
  tableName: string,
  limit: number = 100,
): Promise<DataGridResult> {
  const query = `SELECT * FROM ${tableName} LIMIT ${limit}`;
  const result = await engine.query(query);

  const columns = result.columns();
  const rows: DataGridRow[] = [];

  for (const it = result.iter({}); it.valid(); it.next()) {
    const row: DataGridRow = {};
    for (const col of columns) {
      row[col] = it.get(col);
    }
    rows.push(row);
  }

  return {columns, rows, totalCount: rows.length};
}
