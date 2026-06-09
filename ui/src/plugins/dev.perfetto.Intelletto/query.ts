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

import type {Engine} from '../../trace_processor/engine';
import type {SqlValue} from '../../trace_processor/query_result';

// A hard row cap on what a single query can return to the model. A stray
// `SELECT *` over a multi-million-row table would otherwise blow the context
// window; we truncate and tell the model the result is partial so it doesn't
// reason over it as complete (and is steered toward aggregation instead).
const MAX_ROWS = 1000;

// Run a PerfettoSQL query and serialise the result to a compact JSON string the
// model can read. bigints are narrowed to numbers (JSON can't carry bigint);
// this loses precision above 2^53 but trace ids/timestamps the model reasons
// over are well within range, and it keeps the payload small.
export async function runQueryForModel(
  engine: Engine,
  query: string,
): Promise<string> {
  const result = await engine.query(query);
  const columns = result.columns();
  const rows: Array<Record<string, SqlValue | number>> = [];
  let truncated = false;
  for (const it = result.iter({}); it.valid(); it.next()) {
    if (rows.length >= MAX_ROWS) {
      truncated = true;
      break;
    }
    const row: Record<string, SqlValue | number> = {};
    for (const name of columns) {
      const value = it.get(name);
      row[name] = typeof value === 'bigint' ? Number(value) : value;
    }
    rows.push(row);
  }
  const payload = JSON.stringify({columns, rows});
  if (truncated) {
    return (
      payload +
      `\n... ${MAX_ROWS}+ rows; result truncated. Prefer COUNT/GROUP BY/` +
      `LIMIT or aggregate rather than pulling raw rows.`
    );
  }
  return payload;
}
