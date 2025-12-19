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

import {Engine} from '../../trace_processor/engine';
import {QueryResult, SqlValue} from 'src/trace_processor/query_result';

export async function runQueryForMcp(
  engine: Engine,
  query: string,
): Promise<string> {
  const result = await engine.query(query);
  return resultToJson(result);
}

export async function resultToJson(result: QueryResult): Promise<string> {
  const columns = result.columns();
  const rows: unknown[] = [];
  for (const it = result.iter({}); it.valid(); it.next()) {
    if (rows.length > 5000) {
      throw new Error(
        'Query returned too many results, max 5000 rows. Results should be aggregates rather than raw data.',
      );
    }

    const row: {[key: string]: SqlValue} = {};
    for (const name of columns) {
      let value = it.get(name);
      if (typeof value === 'bigint') {
        value = Number(value);
      }
      row[name] = value;
    }
    rows.push(row);
  }
  return JSON.stringify(rows);
}
