// Copyright (C) 2026 The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use it except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Micro-benchmark for the QueryResult decode hot path. This mirrors what
// SliceTrack / CounterTrack do when loading large tracks (1000s..1Ms of rows):
// take a raw trace_processor.QueryResult protobuf (as produced by TP / the Wasm
// engine) and turn it into fully-processed, per-row typed arrays.
//
// It compares the two processing paths:
//   1. iter() per-row decoding
//   2. decodeColumns() bulk columnar decoding
//
// The timed region covers decode work only. Checksums are used to (a) verify
// that both paths agree and (b) keep the JIT from eliminating the decode, but
// they are computed in a separate, untimed pass so they don't contaminate the
// measured numbers (see bench()).
//
// (The allocation-free varint reader only exists on this branch - to measure
// it, run this same benchmark on `main` and compare the iter() number; see
// the note emitted at the end.)
//
// This is NOT a correctness test. It's a perf harness kept under the
// *_unittest.ts glob purely so it runs in the same Vite/Vitest environment
// (protobufjs init, module resolution). Run it explicitly with:
//
//   RUN_BENCH=1 ui/run-unittests -t 'QueryResultBenchmark'
//
// It is gated behind RUN_BENCH=1 so it doesn't slow down the normal test suite.

import {
  createQueryResult,
  LONG,
  NUM,
  STR,
  type ColumnarResultFor,
  type QueryResult,
  type RowIterator,
  type SpecType,
} from './query_result';

// trace_processor.proto QueryResult.CellsBatch.CellType values. Hard-coded
// (rather than imported from the generated protobufjs module) for clarity.
const CELL_VARINT = 2;
const CELL_FLOAT64 = 3;
const CELL_STRING = 4;
const CELL_INT32 = 6;
const CELL_INT64 = 7;

const INT32_MIN = -2147483648;
const INT32_MAX = 2147483647;

// --- Minimal protobuf wire encoder ---------------------------------------
// We hand-encode the trace_processor.QueryResult bytes rather than going
// through protobufjs because QueryResult's reader hand-parses the wire format
// anyway, so this is exactly what it consumes.

function pushVarint(arr: number[], value: number): void {
  // value is a non-negative integer that may exceed 2**32, so we avoid 32-bit
  // bitwise ops and use arithmetic instead.
  while (value > 127) {
    arr.push((value % 128) + 128);
    value = Math.floor(value / 128);
  }
  arr.push(value);
}

function pushTag(arr: number[], field: number, wireType: number): void {
  pushVarint(arr, field * 8 + wireType);
}

// Appends a length-delimited (wire type 2) field carrying |payload|.
function pushLenDelim(arr: number[], field: number, payload: number[]): void {
  pushTag(arr, field, 2);
  pushVarint(arr, payload.length);
  for (let i = 0; i < payload.length; i++) arr.push(payload[i]);
}

// Appends |value| as a little-endian sfixed32 (4 bytes, two's complement).
function pushFixed32(arr: number[], value: number): void {
  const v = value | 0;
  arr.push(v & 0xff, (v >>> 8) & 0xff, (v >>> 16) & 0xff, (v >>> 24) & 0xff);
}

// Appends |value| as a little-endian sfixed64 (8 bytes, two's complement).
// |value| must fit in a JS number (< 2**53); it is split into two 32-bit halves.
function pushFixed64(arr: number[], value: number): void {
  const lo = value >>> 0;
  const hi = Math.floor(value / 0x100000000) >>> 0;
  arr.push(
    lo & 0xff,
    (lo >>> 8) & 0xff,
    (lo >>> 16) & 0xff,
    (lo >>> 24) & 0xff,
    hi & 0xff,
    (hi >>> 8) & 0xff,
    (hi >>> 16) & 0xff,
    (hi >>> 24) & 0xff,
  );
}

const textEncoder = new TextEncoder();
function utf8(s: string): number[] {
  return Array.from(textEncoder.encode(s));
}

// The columns of a typical ftrace instant / slice track: id, ts, count,
// depth (integers) plus a STR name. ts holds realistic nanosecond
// timestamps (well above INT32_MAX) so it exercises the expensive multi-byte
// varint path; the others are small.
const COLUMN_NAMES = ['id', 'ts', 'dur', 'count', 'depth', 'upid', 'name'];
const NAMES = [
  'sched_wakeup',
  'sched_switch',
  'cpu_idle',
  'irq_handler_entry',
  'workqueue_execute_start',
];

// Per-row integer values, in scan order: [id, ts, dur, count, depth, upid].
function rowInts(r: number): [number, number, number, number, number, number] {
  return [
    r, // id        (small)
    1_500_000_000_000 + r * 7919, // ts  (~1.5ms-base ns, multi-byte varint)
    (r % 100000) + 1, // dur  (medium)
    (r % 7) + 1, // count     (small)
    r % 4, // depth     (small)
    100000 + (r % 500), // upid  (medium)
  ];
}

// Builds an encoded trace_processor.QueryResult with `numRows` rows split into
// batches of ~`rowsPerBatch`. Integer cells are CELL_VARINT by default (what
// the HTTP+RPC path emits, and what `main` always emits); pass
// useFixedWidthInts to emit them as CELL_INT32 / CELL_INT64 instead (what the
// Wasm UI opts into).
function buildInstantTrackResult(
  numRows: number,
  rowsPerBatch: number,
  useFixedWidthInts = false,
): Uint8Array[] {
  const out: Uint8Array[] = [];
  let row = 0;
  let firstBatch = true;
  while (row < numRows) {
    const n = Math.min(rowsPerBatch, numRows - row);
    const cellTypes: number[] = []; // packed CellType per cell.
    const varints: number[] = []; // packed varint payload.
    const int32s: number[] = []; // packed sfixed32 payload.
    const int64s: number[] = []; // packed sfixed64 payload.
    const stringCells: string[] = [];

    const pushInt = (v: number) => {
      if (useFixedWidthInts) {
        // Mirror the serializer: values that fit in 32 bits go to int32_cells,
        // the rest to int64_cells.
        if (v >= INT32_MIN && v <= INT32_MAX) {
          cellTypes.push(CELL_INT32);
          pushFixed32(int32s, v);
        } else {
          cellTypes.push(CELL_INT64);
          pushFixed64(int64s, v);
        }
      } else {
        cellTypes.push(CELL_VARINT);
        pushVarint(varints, v);
      }
    };

    for (let i = 0; i < n; i++) {
      const r = row + i;
      const [id, ts, dur, count, depth, upid] = rowInts(r);
      pushInt(id);
      pushInt(ts);
      pushInt(dur);
      pushInt(count);
      pushInt(depth);
      pushInt(upid);
      cellTypes.push(CELL_STRING);
      stringCells.push(NAMES[r % NAMES.length]);
    }
    const isLast = row + n >= numRows;

    // CellsBatch payload. Field numbers: cells=1, varint_cells=2,
    // string_cells=5, is_last_batch=6.
    const batch: number[] = [];
    const cellsBuf: number[] = [];
    for (const ct of cellTypes) pushVarint(cellsBuf, ct);
    pushLenDelim(batch, 1, cellsBuf);
    if (varints.length) pushLenDelim(batch, 2, varints);
    if (int32s.length) pushLenDelim(batch, 8, int32s);
    if (int64s.length) pushLenDelim(batch, 9, int64s);
    pushLenDelim(batch, 5, utf8(stringCells.join('\0')));
    pushTag(batch, 6, 0);
    pushVarint(batch, isLast ? 1 : 0);

    // QueryResult wrapper. Field numbers: column_names=1, batch=3.
    const top: number[] = [];
    if (firstBatch) {
      for (const name of COLUMN_NAMES) pushLenDelim(top, 1, utf8(name));
    }
    pushLenDelim(top, 3, batch);

    out.push(Uint8Array.from(top));
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

const SPEC = {
  id: NUM,
  ts: LONG,
  dur: LONG,
  count: NUM,
  depth: NUM,
  upid: NUM,
  name: STR,
} as const;

// --- Timed decode wrappers -----------------------------------------------
// These run ONLY the decode work inside the timed region (see bench()): no
// checksum arithmetic, no copying into secondary buffers. The result is
// returned (and stashed into the benchSink black hole outside the timed
// window) so the JIT cannot dead-code-eliminate the decode itself:
//  - decodeColumns() allocates the output arrays and they escape to the caller;
//  - iter() writes every cell into the iterator object, which also escapes.
function decodeOnly<T extends SpecType>(
  qr: QueryResult,
  spec: T,
): ColumnarResultFor<T> {
  return qr.decodeColumns(spec);
}

function iterOnly<T extends SpecType>(
  qr: QueryResult,
  spec: T,
): RowIterator<T> {
  const it = qr.iter(spec);
  for (; it.valid(); it.next()) {
    // Decode only: next() stores every cell value into `it`.
  }
  return it; // Escapes into benchSink: forces all per-cell stores to be kept.
}

// --- Untimed verification passes for the instant-track workload ------------
// Both must produce the same value; % 65521 keeps the accumulator bounded and
// the per-row work identical between the two paths.
function instantChecksumIter(qr: QueryResult): number {
  let checksum = 0;
  const it = qr.iter(SPEC);
  for (; it.valid(); it.next()) {
    checksum +=
      (it.id +
        Number(it.ts) +
        Number(it.dur) +
        it.count +
        it.depth +
        it.upid +
        it.name.length) %
      65521;
  }
  return checksum;
}

function instantChecksumColumns(qr: QueryResult): number {
  const count = qr.numRows();
  const cols = qr.decodeColumns(SPEC);
  const ids = cols.id;
  const tss = cols.ts;
  const durs = cols.dur;
  const counts = cols.count;
  const depths = cols.depth;
  const upids = cols.upid;
  const names = cols.name;
  let checksum = 0;
  for (let i = 0; i < count; i++) {
    checksum +=
      (ids[i] +
        Number(tss[i]) +
        Number(durs[i]) +
        counts[i] +
        depths[i] +
        upids[i] +
        (names[i] ?? '').length) %
      65521;
  }
  return checksum;
}

// A LONG-only variant: this exercises the int64 varint -> bigint hot path
// (and the BigInt64Array direct-store path in decodeColumns()) without the
// small-integer NUM cells and strings diluting the measurements. The values
// are realistic: nanosecond timestamps (6-byte varints), durations, track ids
// and process upids (medium varints).
const LONG_COLUMN_NAMES = ['ts', 'dur', '__track_id', 'upid'];
const LONG_SPEC = {
  ts: LONG,
  dur: LONG,
  __track_id: LONG,
  upid: LONG,
} as const;

function buildLongOnlyResult(
  numRows: number,
  rowsPerBatch: number,
): Uint8Array[] {
  const out: Uint8Array[] = [];
  let row = 0;
  let firstBatch = true;
  while (row < numRows) {
    const n = Math.min(rowsPerBatch, numRows - row);
    const cellTypes: number[] = [];
    const varints: number[] = [];

    const pushInt = (v: number) => {
      cellTypes.push(CELL_VARINT);
      pushVarint(varints, v);
    };

    for (let i = 0; i < n; i++) {
      const r = row + i;
      pushInt(1_500_000_000_000 + r * 7919); // ts
      pushInt((r % 100000) + 1); // dur
      pushInt(100000 + (r % 500)); // track_id
      pushInt(200000 + (r % 1000)); // upid
    }
    const isLast = row + n >= numRows;

    // CellsBatch payload. Field numbers: cells=1, varint_cells=2,
    // is_last_batch=6.
    const batch: number[] = [];
    const cellsBuf: number[] = [];
    for (const ct of cellTypes) pushVarint(cellsBuf, ct);
    pushLenDelim(batch, 1, cellsBuf);
    if (varints.length) pushLenDelim(batch, 2, varints);
    pushTag(batch, 6, 0);
    pushVarint(batch, isLast ? 1 : 0);

    // QueryResult wrapper. Field numbers: column_names=1, batch=3.
    const top: number[] = [];
    if (firstBatch) {
      for (const name of LONG_COLUMN_NAMES) pushLenDelim(top, 1, utf8(name));
    }
    pushLenDelim(top, 3, batch);

    out.push(Uint8Array.from(top));
    firstBatch = false;
    row += n;
  }
  return out;
}

// A NUM-varint-only variant: same shape as the long-only one, but every column
// is NUM and fed by CELL_VARINT cells (varint -> JS number -> Float64Array),
// which exercises the allocation-free number varint reader
// (readVarIntAsNumber) on the same realistic value ranges without any bigint
// work at all. NOTE: unlike the LONG case, the varint bits cannot be written
// directly into the Float64Array here, because a double's bit pattern is the
// IEEE754 encoding of the integer, not the integer itself; an int->double
// conversion is inherent to this path.
const NUM_COLUMN_NAMES = ['ts', 'dur', 'count', 'depth'];
const NUM_SPEC = {
  ts: NUM,
  dur: NUM,
  count: NUM,
  depth: NUM,
} as const;

function buildVarintOnlyResult(
  numRows: number,
  rowsPerBatch: number,
): Uint8Array[] {
  const out: Uint8Array[] = [];
  let row = 0;
  let firstBatch = true;
  while (row < numRows) {
    const n = Math.min(rowsPerBatch, numRows - row);
    const cellTypes: number[] = [];
    const varints: number[] = [];

    const pushInt = (v: number) => {
      cellTypes.push(CELL_VARINT);
      pushVarint(varints, v);
    };

    for (let i = 0; i < n; i++) {
      const r = row + i;
      pushInt(1_500_000_000_000 + r * 7919); // ts
      pushInt((r % 100000) + 1); // dur
      pushInt((r % 7) + 1); // count
      pushInt(r % 4); // depth
    }
    const isLast = row + n >= numRows;

    // CellsBatch payload. Field numbers: cells=1, varint_cells=2,
    // is_last_batch=6.
    const batch: number[] = [];
    const cellsBuf: number[] = [];
    for (const ct of cellTypes) pushVarint(cellsBuf, ct);
    pushLenDelim(batch, 1, cellsBuf);
    if (varints.length) pushLenDelim(batch, 2, varints);
    pushTag(batch, 6, 0);
    pushVarint(batch, isLast ? 1 : 0);

    // QueryResult wrapper. Field numbers: column_names=1, batch=3.
    const top: number[] = [];
    if (firstBatch) {
      for (const name of NUM_COLUMN_NAMES) pushLenDelim(top, 1, utf8(name));
    }
    pushLenDelim(top, 3, batch);

    out.push(Uint8Array.from(top));
    firstBatch = false;
    row += n;
  }
  return out;
}

// Untimed verification passes (see bench()). Both must produce the same
// value; the % 65521 keeps the accumulator bounded and the per-row work
// identical between the two paths.
function numVarintChecksumIter(qr: QueryResult): number {
  let checksum = 0;
  const it = qr.iter(NUM_SPEC);
  for (; it.valid(); it.next()) {
    checksum += (it.ts + it.dur + it.count + it.depth) % 65521;
  }
  return checksum;
}

function numVarintChecksumColumns(qr: QueryResult): number {
  const count = qr.numRows();
  const cols = qr.decodeColumns(NUM_SPEC);
  const tss = cols.ts;
  const durs = cols.dur;
  const counts = cols.count;
  const depths = cols.depth;
  let checksum = 0;
  for (let i = 0; i < count; i++) {
    checksum += (tss[i] + durs[i] + counts[i] + depths[i]) % 65521;
  }
  return checksum;
}

// A NUM-float64-only variant: every column is NUM, fed by CELL_FLOAT64 cells
// (the fixed64-packed field 3), which is what TraceProcessor emits for REAL
// and float results. Note: an Int32Array word-pair bit-copy (as done for the
// LONG varint path) was tried here and measured ~26% SLOWER than the plain
// Float64Array->Float64Array store (two loads+stores vs one movsd), so the
// simple copy is kept; it is already allocation-free in optimized code.
const F64_COLUMN_NAMES = ['ts', 'dur', '__value', '__scale'];
const F64_SPEC = {
  ts: NUM,
  dur: NUM,
  __value: NUM,
  __scale: NUM,
} as const;

// Scratch buffer for little-endian double encoding (the proto wire format
// mandates little-endian for fixed64 fields).
const f64Scratch = new DataView(new ArrayBuffer(8));

function pushF64(arr: number[], value: number): void {
  f64Scratch.setFloat64(0, value, true);
  for (let i = 0; i < 8; i++) arr.push(f64Scratch.getUint8(i));
}

function f64RowValues(r: number): [number, number, number, number] {
  return [
    1_500_000_000_000.25 + r * 7919.5, // ts  (ns, multi-byte mantissa)
    (r % 100000) + 0.5, // dur
    ((r % 7) + 1) * 1.5, // value
    (r % 4) * 0.25, // scale
  ];
}

function buildFloat64OnlyResult(
  numRows: number,
  rowsPerBatch: number,
): Uint8Array[] {
  const out: Uint8Array[] = [];
  let row = 0;
  let firstBatch = true;
  while (row < numRows) {
    const n = Math.min(rowsPerBatch, numRows - row);
    const cellTypes: number[] = [];
    const f64Cells: number[] = [];

    for (let i = 0; i < n; i++) {
      const r = row + i;
      const [ts, dur, value, scale] = f64RowValues(r);
      for (let c = 0; c < 4; c++) cellTypes.push(CELL_FLOAT64);
      pushF64(f64Cells, ts);
      pushF64(f64Cells, dur);
      pushF64(f64Cells, value);
      pushF64(f64Cells, scale);
    }
    const isLast = row + n >= numRows;

    // CellsBatch payload. Field numbers: cells=1, float64_cells=3,
    // is_last_batch=6.
    const batch: number[] = [];
    const cellsBuf: number[] = [];
    for (const ct of cellTypes) pushVarint(cellsBuf, ct);
    pushLenDelim(batch, 1, cellsBuf);
    pushLenDelim(batch, 3, f64Cells);
    pushTag(batch, 6, 0);
    pushVarint(batch, isLast ? 1 : 0);

    // QueryResult wrapper. Field numbers: column_names=1, batch=3.
    const top: number[] = [];
    if (firstBatch) {
      for (const name of F64_COLUMN_NAMES) pushLenDelim(top, 1, utf8(name));
    }
    pushLenDelim(top, 3, batch);

    out.push(Uint8Array.from(top));
    firstBatch = false;
    row += n;
  }
  return out;
}

// A single-column NUM variant where every value is a 64-bit integer (above
// INT32_MAX, so the fixed-width encoding uses CELL_INT64). This isolates the
// varint-vs-fixed-width int64 decode into a NUM (Float64Array) column: it is
// the direct analog of the num-varint-only benchmark, but the fixed-width side
// exercises CELL_INT64 (sfixed64) cells instead of CELL_VARINT.
const INT64_COLUMN_NAMES = ['x'];
const INT64_SPEC = {x: NUM} as const;
const INT64_LONG_SPEC = {x: LONG} as const;

function buildInt64OnlyResult(
  numRows: number,
  rowsPerBatch: number,
  useFixedWidthInts: boolean,
): Uint8Array[] {
  const out: Uint8Array[] = [];
  let row = 0;
  let firstBatch = true;
  while (row < numRows) {
    const n = Math.min(rowsPerBatch, numRows - row);
    const cellTypes: number[] = [];
    const varints: number[] = [];
    const int64s: number[] = [];

    for (let i = 0; i < n; i++) {
      // 64-bit int, well above INT32_MAX but < 2**53 (exact in a double).
      const v = 1_500_000_000_000 + (row + i) * 7919;
      if (useFixedWidthInts) {
        cellTypes.push(CELL_INT64);
        pushFixed64(int64s, v);
      } else {
        cellTypes.push(CELL_VARINT);
        pushVarint(varints, v);
      }
    }
    const isLast = row + n >= numRows;

    // CellsBatch payload. Field numbers: cells=1, varint_cells=2,
    // int64_cells=9, is_last_batch=6.
    const batch: number[] = [];
    const cellsBuf: number[] = [];
    for (const ct of cellTypes) pushVarint(cellsBuf, ct);
    pushLenDelim(batch, 1, cellsBuf);
    if (varints.length) pushLenDelim(batch, 2, varints);
    if (int64s.length) pushLenDelim(batch, 9, int64s);
    pushTag(batch, 6, 0);
    pushVarint(batch, isLast ? 1 : 0);

    // QueryResult wrapper. Field numbers: column_names=1, batch=3.
    const top: number[] = [];
    if (firstBatch) {
      for (const name of INT64_COLUMN_NAMES) pushLenDelim(top, 1, utf8(name));
    }
    pushLenDelim(top, 3, batch);

    out.push(Uint8Array.from(top));
    firstBatch = false;
    row += n;
  }
  return out;
}

// A single-column NUM variant where every value fits in a signed 32-bit int.
// The varint encoding stores them as CELL_VARINT; the fixed-width encoding
// stores them as CELL_INT32 (sfixed32). This is the direct int32 analog of the
// int64 fixed-vs-varint benchmarks above, and it models the most common real
// case: ID-like / small-counter columns that the Wasm UI routes to the int32
// bucket. Unlike the int64 case (where a 64-bit varint is already 5-10 bytes,
// so sfixed64 is both smaller and faster), here a 3-5 byte varint is often
// smaller on the wire than the flat 4-byte sfixed32, so this isolates the
// decode-speed-vs-wire-size tradeoff.
const INT32_COLUMN_NAMES = ['x'];
const INT32_SPEC = {x: NUM} as const;
const INT32_LONG_SPEC = {x: LONG} as const;

function buildInt32OnlyResult(
  numRows: number,
  rowsPerBatch: number,
  useFixedWidthInts: boolean,
): Uint8Array[] {
  const out: Uint8Array[] = [];
  let row = 0;
  let firstBatch = true;
  while (row < numRows) {
    const n = Math.min(rowsPerBatch, numRows - row);
    const cellTypes: number[] = [];
    const varints: number[] = [];
    const int32s: number[] = [];

    for (let i = 0; i < n; i++) {
      // 32-bit int, well within [INT32_MIN, INT32_MAX] but large enough that
      // the varint encoding is 4-5 bytes (a strong case for the varint path).
      // Max value = 100_000_000 + (numRows-1) * 2000 = 2_099_998_000 < INT32_MAX.
      const v = 100_000_000 + (row + i) * 2000;
      if (useFixedWidthInts) {
        cellTypes.push(CELL_INT32);
        pushFixed32(int32s, v);
      } else {
        cellTypes.push(CELL_VARINT);
        pushVarint(varints, v);
      }
    }
    const isLast = row + n >= numRows;

    // CellsBatch payload. Field numbers: cells=1, varint_cells=2,
    // int32_cells=8, is_last_batch=6.
    const batch: number[] = [];
    const cellsBuf: number[] = [];
    for (const ct of cellTypes) pushVarint(cellsBuf, ct);
    pushLenDelim(batch, 1, cellsBuf);
    if (varints.length) pushLenDelim(batch, 2, varints);
    if (int32s.length) pushLenDelim(batch, 8, int32s);
    pushTag(batch, 6, 0);
    pushVarint(batch, isLast ? 1 : 0);

    // QueryResult wrapper. Field numbers: column_names=1, batch=3.
    const top: number[] = [];
    if (firstBatch) {
      for (const name of INT32_COLUMN_NAMES) pushLenDelim(top, 1, utf8(name));
    }
    pushLenDelim(top, 3, batch);

    out.push(Uint8Array.from(top));
    firstBatch = false;
    row += n;
  }
  return out;
}

// Untimed verification passes (see bench()). The % 65521 keeps the accumulator
// bounded and the per-row work identical between the two paths.
function int64ChecksumIter(qr: QueryResult): number {
  let checksum = 0;
  const it = qr.iter(INT64_SPEC);
  for (; it.valid(); it.next()) {
    checksum += it.x % 65521;
  }
  return checksum;
}

function int64ChecksumColumns(qr: QueryResult): number {
  const count = qr.numRows();
  const cols = qr.decodeColumns(INT64_SPEC);
  const xs = cols.x;
  let checksum = 0;
  for (let i = 0; i < count; i++) {
    checksum += xs[i] % 65521;
  }
  return checksum;
}

// Untimed verification passes for the int64→LONG benchmark. The % 65521n keeps
// the bigint accumulator bounded.
function int64LongChecksumIter(qr: QueryResult): bigint {
  let checksum = 0n;
  const it = qr.iter(INT64_LONG_SPEC);
  for (; it.valid(); it.next()) {
    checksum += (it.x as bigint) % 65521n;
  }
  return checksum;
}

function int64LongChecksumColumns(qr: QueryResult): bigint {
  const count = qr.numRows();
  const cols = qr.decodeColumns(INT64_LONG_SPEC);
  const xs = cols.x as BigInt64Array;
  let checksum = 0n;
  for (let i = 0; i < count; i++) {
    checksum += xs[i] % 65521n;
  }
  return checksum;
}

function int32ChecksumIter(qr: QueryResult): number {
  let checksum = 0;
  const it = qr.iter(INT32_SPEC);
  for (; it.valid(); it.next()) {
    checksum += it.x % 65521;
  }
  return checksum;
}

function int32ChecksumColumns(qr: QueryResult): number {
  const count = qr.numRows();
  const cols = qr.decodeColumns(INT32_SPEC);
  const xs = cols.x;
  let checksum = 0;
  for (let i = 0; i < count; i++) {
    checksum += xs[i] % 65521;
  }
  return checksum;
}

function int32LongChecksumIter(qr: QueryResult): bigint {
  let checksum = 0n;
  const it = qr.iter(INT32_LONG_SPEC);
  for (; it.valid(); it.next()) {
    checksum += (it.x as bigint) % 65521n;
  }
  return checksum;
}

function int32LongChecksumColumns(qr: QueryResult): bigint {
  const count = qr.numRows();
  const cols = qr.decodeColumns(INT32_LONG_SPEC);
  const xs = cols.x as BigInt64Array;
  let checksum = 0n;
  for (let i = 0; i < count; i++) {
    checksum += xs[i] % 65521n;
  }
  return checksum;
}

// Plain sums (no modulo: the identical operation order in both paths keeps
// the float results bit-identical, which doubles as a stricter equality
// check than an integer checksum). Untimed verification passes (see bench()).
function float64ChecksumIter(qr: QueryResult): number {
  let checksum = 0;
  const it = qr.iter(F64_SPEC);
  for (; it.valid(); it.next()) {
    checksum += it.ts + it.dur + it.__value + it.__scale;
  }
  return checksum;
}

function float64ChecksumColumns(qr: QueryResult): number {
  const count = qr.numRows();
  const cols = qr.decodeColumns(F64_SPEC);
  const tss = cols.ts;
  const durs = cols.dur;
  const values = cols.__value;
  const scales = cols.__scale;
  let checksum = 0;
  for (let i = 0; i < count; i++) {
    checksum += tss[i] + durs[i] + values[i] + scales[i];
  }
  return checksum;
}

// Untimed verification passes (see bench()). The % 65521 keeps the checksum
// bounded and avoids bigint growth; the work is identical in both paths.
function longChecksumIter(qr: QueryResult): bigint {
  let checksum = 0n;
  const it = qr.iter(LONG_SPEC);
  for (; it.valid(); it.next()) {
    checksum += (it.ts + it.dur + it.__track_id + it.upid) % 65521n;
  }
  return checksum;
}

function longChecksumColumns(qr: QueryResult): bigint {
  const count = qr.numRows();
  const cols = qr.decodeColumns(LONG_SPEC);
  const tss = cols.ts;
  const durs = cols.dur;
  const trackIds = cols.__track_id;
  const upids = cols.upid;
  let checksum = 0n;
  for (let i = 0; i < count; i++) {
    checksum += (tss[i] + durs[i] + trackIds[i] + upids[i]) % 65521n;
  }
  return checksum;
}

function fmt(n: number): string {
  return n.toLocaleString('en-US');
}

const shouldRun = process.env.RUN_BENCH === '1';
const maybe = shouldRun ? test : test.skip;

const NUM_ROWS = 1_000_000;
const ROWS_PER_BATCH = 5_000; // ~ production 128KB batches.
const WARMUP = 5;
const ITERS = 25;

interface BenchStats {
  readonly label: string;
  readonly min: number;
  readonly median: number;
  readonly mean: number;
  readonly checksum: number | bigint;
}

// Black hole for timed results. Assigning to a module-level variable after
// t1 (outside the timed window) keeps the decode outputs observable for the
// JIT without adding any consumption work to the measured loop. Wrapped in
// an object so tsc's noUnusedLocals doesn't flag it as write-only.
const benchSink: {v: unknown} = {v: undefined};

// Times `timed(qr)` over a freshly-rebuilt QueryResult each iteration. The
// checksum is NOT part of the timed region: after the timed loop, `checksumOf`
// runs once, untimed, on a fresh result, so the verification work cannot
// contaminate the measurements (it exists to prove both paths agree and to
// keep the decode from being dead-code-eliminated, not to be measured).
function bench<R>(
  label: string,
  encoded: Uint8Array[],
  timed: (qr: QueryResult) => R,
  checksumOf: (qr: QueryResult) => number | bigint,
): BenchStats {
  const samples: number[] = [];
  for (let i = 0; i < WARMUP + ITERS; i++) {
    const qr = makeResult(encoded);
    const t0 = performance.now();
    const res = timed(qr);
    const t1 = performance.now();
    benchSink.v = res; // Consume outside the timed window: no DCE, no cost.
    if (i >= WARMUP) samples.push(t1 - t0);
  }
  const checksum = checksumOf(makeResult(encoded));
  samples.sort((a, b) => a - b);
  return {
    label,
    min: samples[0],
    median: samples[Math.floor(samples.length / 2)],
    mean: samples.reduce((a, b) => a + b, 0) / samples.length,
    checksum,
  };
}

function report(stats: BenchStats): string {
  const rowsPerSec = NUM_ROWS / (stats.median / 1000);
  return (
    `  ${stats.label.padEnd(20)} ` +
    `min=${stats.min.toFixed(1)}ms median=${stats.median.toFixed(1)}ms ` +
    `mean=${stats.mean.toFixed(1)}ms  ` +
    `${fmt(Math.round(rowsPerSec))} rows/s ` +
    `(${((stats.median / NUM_ROWS) * 1e6).toFixed(1)} ns/row)\n`
  );
}

function emit(text: string) {
  process.stderr.write(text);
  const outFile = process.env.BENCH_OUT;
  if (outFile !== undefined) {
    const fs = require('node:fs');
    fs.appendFileSync(outFile, text);
  }
}

// Lazily-built benchmark datasets, shared across tests so each encoding is
// produced only once (building 1M rows of wire bytes is not free).
let varintEncodedCache: Uint8Array[] | undefined;
function varintEncoded(): Uint8Array[] {
  if (varintEncodedCache === undefined) {
    varintEncodedCache = buildInstantTrackResult(
      NUM_ROWS,
      ROWS_PER_BATCH,
      false,
    );
  }
  return varintEncodedCache;
}

describe('QueryResultBenchmark', () => {
  maybe(
    'instant track decode: iter() vs decodeColumns()',
    () => {
      const encoded = varintEncoded();

      const iterStats = bench(
        'iter()',
        encoded,
        (qr) => iterOnly(qr, SPEC),
        instantChecksumIter,
      );
      const colStats = bench(
        'decodeColumns()',
        encoded,
        (qr) => decodeOnly(qr, SPEC),
        instantChecksumColumns,
      );

      // Both paths must produce identical results.
      expect(colStats.checksum).toBe(iterStats.checksum);

      const m = (s: BenchStats) => s.median;
      const speedup = (m(iterStats) / m(colStats)).toFixed(2);

      emit(
        `\n[QueryResultBenchmark] instant track, ${fmt(NUM_ROWS)} rows ` +
          `(7 cols: 4 NUM + 2 LONG + 1 STR), ${ITERS} iters, median ms\n\n` +
          report(iterStats) +
          report(colStats) +
          `\n  Speedup (iter -> decodeColumns): ${speedup}x\n`,
      );

      expect(iterStats.median).toBeGreaterThan(0);
    },
    600_000,
  );

  maybe(
    'num-varint-only decode: iter() vs decodeColumns()',
    () => {
      const encoded = buildVarintOnlyResult(NUM_ROWS, ROWS_PER_BATCH);

      const iterStats = bench(
        'iter()',
        encoded,
        (qr) => iterOnly(qr, NUM_SPEC),
        numVarintChecksumIter,
      );
      const colStats = bench(
        'decodeColumns()',
        encoded,
        (qr) => decodeOnly(qr, NUM_SPEC),
        numVarintChecksumColumns,
      );

      // Both paths must produce identical results.
      expect(colStats.checksum).toBe(iterStats.checksum);

      const m = (s: BenchStats) => s.median;
      const speedup = (m(iterStats) / m(colStats)).toFixed(2);

      emit(
        `\n[QueryResultBenchmark] num-varint-only, ${fmt(NUM_ROWS)} rows ` +
          `(4 NUM varint columns, no strings), ${ITERS} iters, median ms\n\n` +
          report(iterStats) +
          report(colStats) +
          `\n  Speedup (iter -> decodeColumns): ${speedup}x\n`,
      );

      expect(iterStats.median).toBeGreaterThan(0);
    },
    600_000,
  );

  maybe(
    'num-float64-only decode: iter() vs decodeColumns()',
    () => {
      const encoded = buildFloat64OnlyResult(NUM_ROWS, ROWS_PER_BATCH);

      const iterStats = bench(
        'iter()',
        encoded,
        (qr) => iterOnly(qr, F64_SPEC),
        float64ChecksumIter,
      );
      const colStats = bench(
        'decodeColumns()',
        encoded,
        (qr) => decodeOnly(qr, F64_SPEC),
        float64ChecksumColumns,
      );

      // Both paths must produce identical results.
      expect(colStats.checksum).toBe(iterStats.checksum);

      const m = (s: BenchStats) => s.median;
      const speedup = (m(iterStats) / m(colStats)).toFixed(2);

      emit(
        `\n[QueryResultBenchmark] num-float64-only, ${fmt(NUM_ROWS)} rows ` +
          `(4 NUM float64 columns, no strings), ${ITERS} iters, median ms\n\n` +
          report(iterStats) +
          report(colStats) +
          `\n  Speedup (iter -> decodeColumns): ${speedup}x\n`,
      );

      expect(iterStats.median).toBeGreaterThan(0);
    },
    600_000,
  );

  maybe(
    'long-only decode: iter() vs decodeColumns()',
    () => {
      const encoded = buildLongOnlyResult(NUM_ROWS, ROWS_PER_BATCH);

      const iterStats = bench(
        'iter()',
        encoded,
        (qr) => iterOnly(qr, LONG_SPEC),
        longChecksumIter,
      );
      const colStats = bench(
        'decodeColumns()',
        encoded,
        (qr) => decodeOnly(qr, LONG_SPEC),
        longChecksumColumns,
      );

      // Both paths must produce identical results.
      expect(colStats.checksum).toBe(iterStats.checksum);

      const m = (s: BenchStats) => s.median;
      const speedup = (m(iterStats) / m(colStats)).toFixed(2);

      emit(
        `\n[QueryResultBenchmark] long-only, ${fmt(NUM_ROWS)} rows ` +
          `(4 LONG columns, no strings), ${ITERS} iters, median ms\n\n` +
          report(iterStats) +
          report(colStats) +
          `\n  Speedup (iter -> decodeColumns): ${speedup}x\n`,
      );

      expect(iterStats.median).toBeGreaterThan(0);
    },
    600_000,
  );

  maybe(
    'fixed-width ints vs varint: iter() and decodeColumns()',
    () => {
      // Single NUM column, 64-bit int values. The varint encoding stores them
      // as CELL_VARINT; the fixed-width encoding stores them as CELL_INT64
      // (sfixed64). Both must decode to identical numbers.
      const varint = buildInt64OnlyResult(NUM_ROWS, ROWS_PER_BATCH, false);
      const fixed = buildInt64OnlyResult(NUM_ROWS, ROWS_PER_BATCH, true);

      const iterVarint = bench(
        'iter() [varint]',
        varint,
        (qr) => iterOnly(qr, INT64_SPEC),
        int64ChecksumIter,
      );
      const iterFixed = bench(
        'iter() [fixed]',
        fixed,
        (qr) => iterOnly(qr, INT64_SPEC),
        int64ChecksumIter,
      );
      const colVarint = bench(
        'decodeColumns() [varint]',
        varint,
        (qr) => decodeOnly(qr, INT64_SPEC),
        int64ChecksumColumns,
      );
      const colFixed = bench(
        'decodeColumns() [fixed]',
        fixed,
        (qr) => decodeOnly(qr, INT64_SPEC),
        int64ChecksumColumns,
      );

      // Both encodings must decode to identical values.
      expect(iterFixed.checksum).toBe(iterVarint.checksum);
      expect(colFixed.checksum).toBe(colVarint.checksum);

      const m = (s: BenchStats) => s.median;
      const iterSpeedup = (m(iterVarint) / m(iterFixed)).toFixed(2);
      const colSpeedup = (m(colVarint) / m(colFixed)).toFixed(2);

      emit(
        `\n[QueryResultBenchmark] fixed-width ints vs varint, ` +
          `${fmt(NUM_ROWS)} rows (1 NUM column, 64-bit ints), ` +
          `${ITERS} iters, median ms\n\n` +
          report(iterVarint) +
          report(iterFixed) +
          `\n  Speedup varint -> fixed (iter): ${iterSpeedup}x\n\n` +
          report(colVarint) +
          report(colFixed) +
          `\n  Speedup varint -> fixed (decodeColumns): ${colSpeedup}x\n`,
      );

      expect(iterFixed.median).toBeGreaterThan(0);
      expect(colFixed.median).toBeGreaterThan(0);
    },
    600_000,
  );

  maybe(
    'fixed-width ints vs varint (LONG): iter() and decodeColumns()',
    () => {
      // Single LONG column, 64-bit int values. The varint encoding stores them
      // as CELL_VARINT; the fixed-width encoding stores them as CELL_INT64
      // (sfixed64). Both must decode to identical bigints. The fixed-width
      // path decodes by bit-copying two 32-bit words into the BigInt64Array
      // backing buffer, avoiding per-cell BigInt allocation.
      const varint = buildInt64OnlyResult(NUM_ROWS, ROWS_PER_BATCH, false);
      const fixed = buildInt64OnlyResult(NUM_ROWS, ROWS_PER_BATCH, true);

      const iterVarint = bench(
        'iter() [varint]',
        varint,
        (qr) => iterOnly(qr, INT64_LONG_SPEC),
        int64LongChecksumIter,
      );
      const iterFixed = bench(
        'iter() [fixed]',
        fixed,
        (qr) => iterOnly(qr, INT64_LONG_SPEC),
        int64LongChecksumIter,
      );
      const colVarint = bench(
        'decodeColumns() [varint]',
        varint,
        (qr) => decodeOnly(qr, INT64_LONG_SPEC),
        int64LongChecksumColumns,
      );
      const colFixed = bench(
        'decodeColumns() [fixed]',
        fixed,
        (qr) => decodeOnly(qr, INT64_LONG_SPEC),
        int64LongChecksumColumns,
      );

      // Both encodings must decode to identical values.
      expect(iterFixed.checksum).toBe(iterVarint.checksum);
      expect(colFixed.checksum).toBe(colVarint.checksum);

      const m = (s: BenchStats) => s.median;
      const iterSpeedup = (m(iterVarint) / m(iterFixed)).toFixed(2);
      const colSpeedup = (m(colVarint) / m(colFixed)).toFixed(2);

      emit(
        `\n[QueryResultBenchmark] fixed-width ints vs varint (LONG), ` +
          `${fmt(NUM_ROWS)} rows (1 LONG column, 64-bit ints), ` +
          `${ITERS} iters, median ms\n\n` +
          report(iterVarint) +
          report(iterFixed) +
          `\n  Speedup varint -> fixed (iter): ${iterSpeedup}x\n\n` +
          report(colVarint) +
          report(colFixed) +
          `\n  Speedup varint -> fixed (decodeColumns): ${colSpeedup}x\n`,
      );

      expect(iterFixed.median).toBeGreaterThan(0);
      expect(colFixed.median).toBeGreaterThan(0);
    },
    600_000,
  );

  maybe(
    'fixed-width ints vs varint (INT32): iter() and decodeColumns()',
    () => {
      // Single NUM column, 32-bit int values. The varint encoding stores them
      // as CELL_VARINT; the fixed-width encoding stores them as CELL_INT32
      // (sfixed32). Both must decode to identical numbers.
      const varint = buildInt32OnlyResult(NUM_ROWS, ROWS_PER_BATCH, false);
      const fixed = buildInt32OnlyResult(NUM_ROWS, ROWS_PER_BATCH, true);

      const iterVarint = bench(
        'iter() [varint]',
        varint,
        (qr) => iterOnly(qr, INT32_SPEC),
        int32ChecksumIter,
      );
      const iterFixed = bench(
        'iter() [fixed]',
        fixed,
        (qr) => iterOnly(qr, INT32_SPEC),
        int32ChecksumIter,
      );
      const colVarint = bench(
        'decodeColumns() [varint]',
        varint,
        (qr) => decodeOnly(qr, INT32_SPEC),
        int32ChecksumColumns,
      );
      const colFixed = bench(
        'decodeColumns() [fixed]',
        fixed,
        (qr) => decodeOnly(qr, INT32_SPEC),
        int32ChecksumColumns,
      );

      // Both encodings must decode to identical values.
      expect(iterFixed.checksum).toBe(iterVarint.checksum);
      expect(colFixed.checksum).toBe(colVarint.checksum);

      const m = (s: BenchStats) => s.median;
      const iterSpeedup = (m(iterVarint) / m(iterFixed)).toFixed(2);
      const colSpeedup = (m(colVarint) / m(colFixed)).toFixed(2);

      emit(
        `\n[QueryResultBenchmark] fixed-width ints vs varint (INT32), ` +
          `${fmt(NUM_ROWS)} rows (1 NUM column, 32-bit ints), ` +
          `${ITERS} iters, median ms\n\n` +
          report(iterVarint) +
          report(iterFixed) +
          `\n  Speedup varint -> fixed (iter): ${iterSpeedup}x\n\n` +
          report(colVarint) +
          report(colFixed) +
          `\n  Speedup varint -> fixed (decodeColumns): ${colSpeedup}x\n`,
      );

      expect(iterFixed.median).toBeGreaterThan(0);
      expect(colFixed.median).toBeGreaterThan(0);
    },
    600_000,
  );

  maybe(
    'fixed-width ints vs varint (INT32, LONG): iter() and decodeColumns()',
    () => {
      // Single LONG column, 32-bit int values. The varint encoding stores them
      // as CELL_VARINT; the fixed-width encoding stores them as CELL_INT32
      // (sfixed32). Both must decode to identical bigints. The fixed-width
      // path decodes by writing the sign-extended 32-bit word into the
      // BigInt64Array backing buffer, avoiding per-cell BigInt allocation.
      const varint = buildInt32OnlyResult(NUM_ROWS, ROWS_PER_BATCH, false);
      const fixed = buildInt32OnlyResult(NUM_ROWS, ROWS_PER_BATCH, true);

      const iterVarint = bench(
        'iter() [varint]',
        varint,
        (qr) => iterOnly(qr, INT32_LONG_SPEC),
        int32LongChecksumIter,
      );
      const iterFixed = bench(
        'iter() [fixed]',
        fixed,
        (qr) => iterOnly(qr, INT32_LONG_SPEC),
        int32LongChecksumIter,
      );
      const colVarint = bench(
        'decodeColumns() [varint]',
        varint,
        (qr) => decodeOnly(qr, INT32_LONG_SPEC),
        int32LongChecksumColumns,
      );
      const colFixed = bench(
        'decodeColumns() [fixed]',
        fixed,
        (qr) => decodeOnly(qr, INT32_LONG_SPEC),
        int32LongChecksumColumns,
      );

      // Both encodings must decode to identical values.
      expect(iterFixed.checksum).toBe(iterVarint.checksum);
      expect(colFixed.checksum).toBe(colVarint.checksum);

      const m = (s: BenchStats) => s.median;
      const iterSpeedup = (m(iterVarint) / m(iterFixed)).toFixed(2);
      const colSpeedup = (m(colVarint) / m(colFixed)).toFixed(2);

      emit(
        `\n[QueryResultBenchmark] fixed-width ints vs varint (INT32, LONG), ` +
          `${fmt(NUM_ROWS)} rows (1 LONG column, 32-bit ints), ` +
          `${ITERS} iters, median ms\n\n` +
          report(iterVarint) +
          report(iterFixed) +
          `\n  Speedup varint -> fixed (iter): ${iterSpeedup}x\n\n` +
          report(colVarint) +
          report(colFixed) +
          `\n  Speedup varint -> fixed (decodeColumns): ${colSpeedup}x\n`,
      );

      expect(iterFixed.median).toBeGreaterThan(0);
      expect(colFixed.median).toBeGreaterThan(0);
    },
    600_000,
  );
});
