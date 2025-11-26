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

import {Engine} from '../../trace_processor/engine';
import {Row} from '../../trace_processor/query_result';

export interface QueryResponse {
  query: string;
  error?: string;
  totalRowCount: number;
  durationMs: number;
  columns: string[];
  rows: Row[];
  statementCount: number;
  statementWithOutputCount: number;
  lastStatementSql: string;
}

/**
 * Runs a query and pulls out all the columns into a list of 'row' objects,
 * where each row contains a dictionary of [columnName] -> value.
 *
 * This method will not throw if the query fails, instead the error is populated
 * in the return value.
 *
 * This function is designed to be used with table viewers, where the structure
 * of resulting rows is not known up front. Use engine.query() or
 * engine.tryQuery() and use the iterators if the resulting data is to be
 * processed.
 *
 * @param sqlQuery - The query to evaluate.
 * @param engine - The engine to use to run the query.
 */
export async function runQueryForQueryTable(
  sqlQuery: string,
  engine: Engine,
): Promise<QueryResponse> {
  const startMs = performance.now();

  // TODO(primiano): once the controller thread is gone we should pass down
  // the result objects directly to the frontend, iterate over the result
  // and deal with pagination there. For now we keep the old behavior and
  // truncate to 10k rows.

  const maybeResult = await engine.tryQuery(sqlQuery);

  if (maybeResult.ok) {
    const queryRes = maybeResult.value;

    const durationMs = performance.now() - startMs;
    const rows: Row[] = [];
    const columns = queryRes.columns();
    for (const iter = queryRes.iter({}); iter.valid(); iter.next()) {
      const row: Row = {};
      for (const colName of columns) {
        const value = iter.get(colName);
        row[colName] = value;
      }
      rows.push(row);
    }

    const result: QueryResponse = {
      query: sqlQuery,
      durationMs,
      error: queryRes.error(),
      totalRowCount: queryRes.numRows(),
      columns,
      rows,
      statementCount: queryRes.statementCount(),
      statementWithOutputCount: queryRes.statementWithOutputCount(),
      lastStatementSql: queryRes.lastStatementSql(),
    };
    return result;
  } else {
    // In the case of a query error we don't want the exception to bubble up
    // as a crash. The |queryRes| object will be populated anyways.
    // queryRes.error() is used to tell if the query errored or not. If it
    // errored, the frontend will show a graceful message instead.
    return {
      query: sqlQuery,
      durationMs: performance.now() - startMs,
      error: maybeResult.error,
      totalRowCount: 0,
      columns: [],
      rows: [],
      statementCount: 0,
      statementWithOutputCount: 0,
      lastStatementSql: '',
    };
  }
}

export interface ResponseLike {
  readonly columns: ReadonlyArray<string>;
  readonly rows: ReadonlyArray<Row>;
}

export function formatAsDelimited(
  resp: ResponseLike,
  separator: string = '\t',
): string {
  const lines: ReadonlyArray<string>[] = [];
  lines.push(resp.columns);
  for (const row of resp.rows) {
    const line = [];
    for (const col of resp.columns) {
      const value = row[col];
      line.push(value === null ? 'NULL' : `${value}`);
    }
    lines.push(line);
  }
  return lines.map((line) => line.join(separator)).join('\n');
}

export function formatAsMarkdownTable(resp: ResponseLike): string {
  if (resp.columns.length === 0) return '';

  // Convert all values to strings.
  // rows = [header, separators, ...body]
  const rows: ReadonlyArray<string>[] = [];
  rows.push(resp.columns);
  rows.push(resp.columns.map((_) => '---'));
  for (const responseRow of resp.rows) {
    rows.push(
      resp.columns.map((responseCol) => {
        const value = responseRow[responseCol];
        return value === null ? 'NULL' : `${value}`;
      }),
    );
  }

  // Find the maximum width of each column.
  const maxWidths: number[] = Array(resp.columns.length).fill(0);
  for (const row of rows) {
    for (let i = 0; i < resp.columns.length; i++) {
      if (row[i].length > maxWidths[i]) {
        maxWidths[i] = row[i].length;
      }
    }
  }

  const text = rows
    .map((row, rowIndex) => {
      // Pad each column to the maximum width with hyphens (separator row) or
      // spaces (all other rows).
      const expansionChar = rowIndex === 1 ? '-' : ' ';
      const line: string[] = row.map(
        (str, colIndex) =>
          str + expansionChar.repeat(maxWidths[colIndex] - str.length),
      );
      return `| ${line.join(' | ')} |`;
    })
    .join('\n');

  return text;
}
