// Copyright (C) 2018 The Android Open Source Project
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

import {Engine} from './engine';
import {Row} from './query_result';

const MAX_DISPLAY_ROWS = 10000;

export interface QueryResponse {
  id: string;
  query: string;
  error?: string;
  totalRowCount: number;
  durationMs: number;
  columns: string[];
  rows: Row[];
  statementCount: number;
  statementWithOutputCount: number;
}

export async function runQuery(
    queryId: string, sqlQuery: string, engine: Engine): Promise<QueryResponse> {
  const startMs = performance.now();
  const queryRes = engine.query(sqlQuery);

  // TODO(primiano): once the controller thread is gone we should pass down
  // the result objects directly to the frontend, iterate over the result
  // and deal with pagination there. For now we keep the old behavior and
  // truncate to 10k rows.

  try {
    await queryRes.waitAllRows();
  } catch {
    // In the case of a query error we don't want the exception to bubble up
    // as a crash. The |queryRes| object will be populated anyways.
    // queryRes.error() is used to tell if the query errored or not. If it
    // errored, the frontend will show a graceful message instead.
  }

  const durationMs = performance.now() - startMs;
  const rows: Row[] = [];
  const columns = queryRes.columns();
  let numRows = 0;
  for (const iter = queryRes.iter({}); iter.valid(); iter.next()) {
    const row: Row = {};
    for (const colName of columns) {
      const value = iter.get(colName);
      row[colName] = value === null ? 'NULL' : value;
    }
    rows.push(row);
    if (++numRows >= MAX_DISPLAY_ROWS) break;
  }

  const result: QueryResponse = {
    id: queryId,
    query: sqlQuery,
    durationMs,
    error: queryRes.error(),
    totalRowCount: queryRes.numRows(),
    columns,
    rows,
    statementCount: queryRes.statementCount(),
    statementWithOutputCount: queryRes.statementWithOutputCount(),
  };
  return result;
}
