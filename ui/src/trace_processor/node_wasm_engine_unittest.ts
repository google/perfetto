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

// Runs the real trace_processor WASM in-process (via NodeWasmEngine) using the
// bare_sql_engine config, then defines a minimal "slice" table and runs queries
// against it. This exercises the full JS<>WASM RPC path without loading a trace.
//
// The test needs the trace_processor.wasm artifact to be built (e.g. via
// ui/build.mjs or `ninja -C out/ui trace_processor_wasm`). If it isn't present
// the whole suite is skipped rather than failing.

import {NUM, STR} from './query_result';
import {
  NodeWasmEngine,
  locateTraceProcessorWasm,
} from './node_wasm_engine_for_testing';
import {createPerfettoTable} from './sql_utils';

const wasmPath = locateTraceProcessorWasm();

describe('NodeWasmEngine (bare SQL engine)', () => {
  let engine: NodeWasmEngine;

  beforeAll(async () => {
    engine = await NodeWasmEngine.create(wasmPath);
    await engine.resetTraceProcessor({bareSqlEngine: true});
  });

  afterAll(() => {
    engine?.[Symbol.dispose]();
  });

  test('built-in slice table is absent in bare mode', async () => {
    const res = await engine.tryQuery('SELECT count(*) FROM slice');
    expect(res.ok).toBe(false);
  });

  test('can create a slice table and query it', async () => {
    await createPerfettoTable({
      engine,
      name: 'slice',
      as: [
        {id: 1, ts: 100n, dur: 10, name: 'foo'},
        {id: 2, ts: 200n, dur: 50, name: 'bar'},
        {id: 3, ts: 300n, dur: 5, name: 'foo'},
      ],
    });

    const agg = await engine.query(
      'SELECT count(*) AS cnt, sum(dur) AS total_dur FROM slice',
    );
    const aggRow = agg.firstRow({cnt: NUM, total_dur: NUM});
    expect(aggRow.cnt).toBe(3);
    expect(aggRow.total_dur).toBe(65);

    const filtered = await engine.query(
      "SELECT count(*) AS cnt FROM slice WHERE name = 'foo'",
    );
    expect(filtered.firstRow({cnt: NUM}).cnt).toBe(2);

    const longest = await engine.query(
      'SELECT name FROM slice ORDER BY dur DESC LIMIT 1',
    );
    expect(longest.firstRow({name: STR}).name).toBe('bar');
  });

  test('can iterate multiple rows', async () => {
    const result = await engine.query('SELECT id, name FROM slice ORDER BY id');
    const names: string[] = [];
    for (const it = result.iter({id: NUM, name: STR}); it.valid(); it.next()) {
      names.push(it.name);
    }
    expect(names).toEqual(['foo', 'bar', 'foo']);
  });
});
