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

// Micro-benchmark for the QueryResult row iterator hot path. This mirrors the
// kind of loop that DatasetSliceTrack.getInstantBuffers() runs when loading
// large ftrace instant tracks (1000s..1Ms of rows): valid()/next() plus the
// underlying protobuf cell parsing.
//
// This is NOT a correctness test. It's a perf harness kept under the
// *_unittest.ts glob purely so it runs in the same Vite/Vitest environment
// (protobufjs init, module resolution). Run it explicitly with:
//
//   ui/node ui/node_modules/.bin/vitest run --config ui/vitest.config.mjs \
//       -t 'QueryResultBenchmark'
//
// It is gated behind RUN_BENCH=1 so it doesn't slow down the normal test
// suite.

import protos from '../protos';
import {createQueryResult, NUM, STR, type QueryResult} from './query_result';

const T = protos.QueryResult.CellsBatch.CellType;

// Encode a varint the same way protobuf does, so we can hand-build batches
// with realistic (large, multi-byte) timestamp values.
function encodeVarint(out: number[], value: number) {
  // Only used for non-negative values here (ids, ts, count, depth).
  let v = value;
  while (v > 0x7f) {
    out.push((v & 0x7f) | 0x80);
    v = Math.floor(v / 128);
  }
  out.push(v & 0x7f);
}

// Builds an encoded trace_processor.QueryResult with `numRows` rows split into
// batches of ~`rowsPerBatch` rows, mimicking the columns of an ftrace instant
// track: __id, __ts, __count, __depth (all NUM/varint) plus a STR name.
function buildInstantTrackResult(
  numRows: number,
  rowsPerBatch: number,
): Uint8Array[] {
  const columnNames = ['__id', '__ts', '__count', '__depth', 'name'];
  const names = [
    'sched_wakeup',
    'sched_switch',
    'cpu_idle',
    'irq_handler_entry',
    'workqueue_execute_start',
  ];

  const out: Uint8Array[] = [];
  let row = 0;
  let firstBatch = true;
  while (row < numRows) {
    const n = Math.min(rowsPerBatch, numRows - row);
    const cells: number[] = [];
    const varintBytes: number[] = [];
    const stringCells: string[] = [];
    for (let i = 0; i < n; i++) {
      const r = row + i;
      cells.push(T.CELL_VARINT); // __id
      cells.push(T.CELL_VARINT); // __ts
      cells.push(T.CELL_VARINT); // __count
      cells.push(T.CELL_VARINT); // __depth
      cells.push(T.CELL_STRING); // name
      encodeVarint(varintBytes, r); // id
      encodeVarint(varintBytes, 1_000_000_000 + r * 1234); // ts (big)
      encodeVarint(varintBytes, (r % 7) + 1); // count
      encodeVarint(varintBytes, r % 4); // depth
      stringCells.push(names[r % names.length]);
    }
    const isLast = row + n >= numRows;
    const batch = protos.QueryResult.CellsBatch.create({
      cells,
      varintCells: new Uint8Array(varintBytes),
      stringCells: stringCells.join('\0'),
      isLastBatch: isLast,
    });
    const resProto = protos.QueryResult.create({
      columnNames: firstBatch ? columnNames : [],
      batch: [batch],
    });
    out.push(protos.QueryResult.encode(resProto).finish());
    firstBatch = false;
    row += n;
  }
  return out;
}

function makeResult(encodedBatches: Uint8Array[]): QueryResult {
  const qr = createQueryResult({query: 'benchmark'});
  for (const b of encodedBatches) {
    qr.appendResultBatch(b as Uint8Array<ArrayBuffer>);
  }
  return qr;
}

// Mirrors the work in DatasetSliceTrack.getInstantBuffers(): read every column,
// stash the numerics into typed arrays and the string into an array. Returns a
// checksum so the JIT can't optimise the loop away.
function iterateInstantTrack(qr: QueryResult): number {
  const count = qr.numRows();
  const xs = new Float32Array(count);
  const depths = new Uint16Array(count);
  const titles = new Array<string>(count);
  let checksum = 0;
  const it = qr.iter({
    __id: NUM,
    __ts: NUM,
    __count: NUM,
    __depth: NUM,
    name: STR,
  });
  for (let i = 0; it.valid(); it.next(), ++i) {
    const id = it.__id;
    const ts = it.__ts;
    const cnt = it.__count;
    const depth = it.__depth;
    const title = it.name;
    xs[i] = ts;
    depths[i] = depth;
    titles[i] = title;
    checksum += id + cnt + depth + title.length;
  }
  return checksum;
}

function fmt(n: number): string {
  return n.toLocaleString('en-US');
}

const shouldRun = process.env.RUN_BENCH === '1';
const maybe = shouldRun ? test : test.skip;

describe('QueryResultBenchmark', () => {
  maybe('instant track iteration', () => {
    const NUM_ROWS = 500_000;
    const ROWS_PER_BATCH = 5_000; // ~ production 128KB batches.
    const WARMUP = 5;
    const ITERS = 25;

    // Build once. Reconstruct the QueryResult per iteration (cheap) so we
    // re-run the iterator over the same already-decoded batches.
    const encoded = buildInstantTrackResult(NUM_ROWS, ROWS_PER_BATCH);

    let checksum = 0;
    const samples: number[] = [];
    for (let i = 0; i < WARMUP + ITERS; i++) {
      const qr = makeResult(encoded);
      const t0 = performance.now();
      checksum = iterateInstantTrack(qr);
      const t1 = performance.now();
      if (i >= WARMUP) samples.push(t1 - t0);
    }

    samples.sort((a, b) => a - b);
    const min = samples[0];
    const median = samples[Math.floor(samples.length / 2)];
    const mean = samples.reduce((a, b) => a + b, 0) / samples.length;
    const rowsPerSec = NUM_ROWS / (median / 1000);

    const report =
      `\n[QueryResultBenchmark] instant track, ${fmt(NUM_ROWS)} rows ` +
      `(5 cols), ${ITERS} iters\n` +
      `  min=${min.toFixed(2)}ms median=${median.toFixed(2)}ms ` +
      `mean=${mean.toFixed(2)}ms\n` +
      `  throughput=${fmt(Math.round(rowsPerSec))} rows/s ` +
      `(${((median / NUM_ROWS) * 1e6).toFixed(1)} ns/row)\n` +
      `  checksum=${checksum}\n`;
    // Vitest swallows console.log; write to a file (and stderr) so the result
    // is visible regardless.
    process.stderr.write(report);
    const outFile = process.env.BENCH_OUT;
    if (outFile !== undefined) {
      // eslint-disable-next-line @typescript-eslint/no-var-requires
      const fs = require('node:fs');
      fs.appendFileSync(outFile, report);
    }

    expect(samples.length).toBe(ITERS);
  });
});
