// Copyright (C) 2021 The Android Open Source Project
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

// This file deals with deserialization and iteration of the proto-encoded
// byte buffer that is returned by TraceProcessor when invoking the
// TPM_QUERY_STREAMING method. The returned |query_result| buffer is optimized
// for being moved cheaply across workers and decoded on-the-flight as we step
// through the iterator.
// See comments around QueryResult in trace_processor.proto for more details.

// The classes in this file are organized as follows:
//
// QueryResultImpl:
// The object returned by the Engine.query(sql) method.
// This object is a holder of row data. Batches of raw get appended
// incrementally as they are received by the remote TraceProcessor instance.
// QueryResultImpl also deals with asynchronicity of queries and allows callers
// to obtain a promise that waits for more (or all) rows.
// At any point in time the following objects hold a reference to QueryResult:
// - The Engine: for appending row batches.
// - UI code, typically controllers, who make queries.
//
// ResultBatch:
// Hold the data, returned by the remote TraceProcessor instance, for a number
// of rows (TP typically chunks the results in batches of 128KB).
// A QueryResultImpl holds exclusively ResultBatches for a given query.
// ResultBatch is not exposed externally, it's just an internal representation
// that helps with proto decoding. ResultBatch is immutable after it gets
// appended and decoded. The iteration state is held by the RowIteratorImpl.
//
// RowIteratorImpl:
// Decouples the data owned by QueryResultImpl (and its ResultBatch(es)) from
// the iteration state. The iterator effectively is the union of a ResultBatch
// and the row number in it. Rows within the batch are decoded as the user calls
// next(). When getting at the end of the batch, it takes care of switching to
// the next batch (if any) within the QueryResultImpl.
// This object is part of the API exposed to tracks / controllers.

// Ensure protobuf is initialized.
import '../base/static_initializers';
import protobuf from 'protobufjs/minimal';
import {defer, type Deferred} from '../base/deferred';
import {ensureExists, assertFalse, assertTrue} from '../base/assert';
import {utf8Decode} from '../base/string_utils';
import {Duration, type duration, Time, type time} from '../base/time';

export type SqlValue =
  string | number | bigint | null | Uint8Array<ArrayBuffer>;

// The column type constants are branded with unique string-literal types so
// that each is uniquely identifiable at the type level (via typeof). This
// allows mapped types like DecodeColumnType to distinguish them. The brands
// are erased at runtime; the constants retain their original values.
export const NUM = {__brand: 'NUM', __nonNullish: true} as const;
export const STR = {__brand: 'STR', __nonNullish: true} as const;
export const BLOB = {__brand: 'BLOB', __nonNullish: true} as const;
export const LONG = {__brand: 'LONG', __nonNullish: true} as const;
export const NUM_NULL = {__brand: 'NUM'} as const;
export const STR_NULL = {__brand: 'STR'} as const;
export const BLOB_NULL = {__brand: 'BLOB'} as const;
export const LONG_NULL = {__brand: 'LONG'} as const;
export const UNKNOWN = {} as const;

// One decoded column produced by QueryResult.decodeColumns(). The runtime type
// depends on the requested column type:
//  - NUM       -> Float64Array
//  - LONG      -> BigInt64Array
//  - NUM_NULL  -> Array<number | null>
//  - LONG_NULL -> Array<bigint | null>
//  - STR/STR_NULL -> Array<string | null>
//  - BLOB/*    -> Array<SqlValue>
export type ColumnarColumn =
  | Float64Array
  | BigInt64Array
  | Array<string>
  | Array<number | null>
  | Array<bigint | null>
  | Array<string | null>
  | Array<SqlValue>;

// Maps a spec value to the concrete column type produced by decodeColumns().
// Uses the branded types on the constants so each is uniquely distinguishable
// regardless of TypeScript literal widening.
export type DecodeColumnType<V> = V extends typeof NUM
  ? Float64Array
  : V extends typeof LONG
    ? BigInt64Array
    : Array<DecodeRowType<V>>;

// The fully-typed result of decodeColumns() for a given spec.
export type ColumnarResultFor<T extends SpecType> = {
  readonly [K in keyof T]: DecodeColumnType<T[K]>;
};

export interface ColumnarResult {
  readonly [columnName: string]: ColumnarColumn;
}

// Maps a spec value to the concrete row type produced by iter().
// Uses the branded types on the constants so each is uniquely distinguishable
// regardless of TypeScript literal widening.
export type DecodeRowType<V> = V extends typeof NUM
  ? number
  : V extends typeof LONG
    ? bigint
    : V extends typeof NUM_NULL
      ? number | null
      : V extends typeof LONG_NULL
        ? bigint | null
        : V extends typeof STR
          ? string
          : V extends typeof STR_NULL
            ? string | null
            : V extends typeof BLOB
              ? Uint8Array<ArrayBuffer>
              : V extends typeof BLOB_NULL
                ? Uint8Array<ArrayBuffer> | null
                : V extends typeof UNKNOWN
                  ? SqlValue
                  : never;

export type InferRowType<T extends SpecType> = {
  readonly [K in keyof T]: DecodeRowType<T[K]>;
};

const SHIFT_32BITS = 32n;

// Describes the inheritance tree of the above types.
const inheritanceTree = new Map<SpecValue, {readonly extends?: SpecValue}>([
  [NUM, {extends: NUM_NULL}],
  [NUM_NULL, {extends: UNKNOWN}],
  [LONG, {extends: LONG_NULL}],
  [LONG_NULL, {extends: UNKNOWN}],
  [BLOB, {extends: BLOB_NULL}],
  [BLOB_NULL, {extends: UNKNOWN}],
  [STR, {extends: STR_NULL}],
  [STR_NULL, {extends: UNKNOWN}],
  [UNKNOWN, {}],
]);

/**
 * Check whether a given type extends another.
 *
 * @param required - The type we want to extend.
 * @param actual - The type to test.
 * @returns - True if `actual` extends `required`.
 */
export function checkExtends(required: SpecValue, actual: SpecValue): boolean {
  // If the types are the same, just return true
  if (required === actual) return true;

  const ancestry = getAncestryPath(actual);
  return ancestry.includes(required);
}

/**
 * Returns the closest common ancestor of two types.
 */
export function unionTypes(
  typeA: SpecValue,
  typeB: SpecValue,
): SpecValue | undefined {
  // If the types are the same, just return the same type
  if (typeA === typeB) return typeA;

  // Get the ancestry path for each type
  const pathA = getAncestryPath(typeA);
  const pathB = getAncestryPath(typeB);

  // Find the first common type in both ancestry paths
  for (const type of pathA) {
    if (pathB.includes(type)) {
      return type;
    }
  }

  return undefined;
}

/**
 * Returns the ancestry path from the given type to the root, inclusive.
 */
function getAncestryPath(type: SpecValue): SpecValue[] {
  const path: SpecValue[] = [type];
  let current = inheritanceTree.get(type);

  while (current && current.extends !== undefined) {
    path.push(current.extends);
    current = inheritanceTree.get(current.extends);
  }

  return path;
}

// Decodes an int64 varint as a JS number and advances the cursor. This is the
// same algorithm as protobuf.js's Reader.int64() followed by
// LongBits.toNumber(), but it does not allocate a LongBits object per cell,
// which measured at ~6ns per cell over a large result set. The byte loops are
// bounded (4 + 1 + 5), so malformed input cannot spin.
function readVarIntAsNumber(buf: Uint8Array, cursor: {pos: number}): number {
  let pos = cursor.pos;
  let lo = 0;
  let hi = 0;
  let b = 0;
  let i = 0;
  for (; i < 4; ++i) {
    b = buf[pos++];
    lo = (lo | ((b & 127) << (i * 7))) >>> 0;
    if (b < 128) {
      cursor.pos = pos;
      return lo;
    }
  }
  b = buf[pos++];
  lo = (lo | ((b & 127) << 28)) >>> 0;
  hi = (hi | ((b & 127) >> 4)) >>> 0;
  if (b >= 128) {
    for (i = 0; i < 5; ++i) {
      b = buf[pos++];
      hi = (hi | ((b & 127) << (i * 7 + 3))) >>> 0;
      if (b < 128) break;
    }
  }
  cursor.pos = pos;
  if (hi >>> 31) {
    // Negative: two's complement, matching LongBits.toNumber(false).
    const nlo = (~lo + 1) >>> 0;
    let nhi = ~hi >>> 0;
    if (nlo === 0) nhi = (nhi + 1) >>> 0;
    return -(nlo + nhi * 4294967296);
  }
  return lo + hi * 4294967296;
}

// Writes an int64 varint directly into the backing buffer of a BigInt64Array,
// through an Int32Array view over the same buffer, avoiding the allocation of
// an intermediate bigint per cell. The two int32 stores below are bit-exact
// equivalent to BigInt64Array[i] = BigInt.asIntN(64, (BigInt(hi) << 32n) | BigInt(lo)),
// because typed-array stores apply ToInt32 to the value and the two's-complement
// 64-bit pattern is exactly the (lo, hi) pair. Only valid on little-endian
// platforms (see IS_LITTLE_ENDIAN below).
// As in readVarIntAsNumber, the byte loops are bounded (4 + 1 + 5), so malformed
// input cannot spin.
function readVarIntIntoInt32s(
  buf: Uint8Array,
  cursor: {pos: number},
  lo32: Int32Array,
  outIdx: number,
): void {
  let pos = cursor.pos;
  let lo = 0;
  let hi = 0;
  let b = 0;
  let i = 0;
  for (; i < 4; ++i) {
    b = buf[pos++];
    lo = (lo | ((b & 127) << (i * 7))) >>> 0;
    if (b < 128) {
      cursor.pos = pos;
      lo32[outIdx * 2] = lo;
      lo32[outIdx * 2 + 1] = 0;
      return;
    }
  }
  b = buf[pos++];
  lo = (lo | ((b & 127) << 28)) >>> 0;
  hi = (hi | ((b & 127) >> 4)) >>> 0;
  if (b >= 128) {
    for (i = 0; i < 5; ++i) {
      b = buf[pos++];
      hi = (hi | ((b & 127) << (i * 7 + 3))) >>> 0;
      if (b < 128) break;
    }
  }
  cursor.pos = pos;
  lo32[outIdx * 2] = lo;
  lo32[outIdx * 2 + 1] = hi;
}

// Typed arrays share the platform endianness. All browsers the UI runs on are
// little-endian, but keep a runtime check so that on an exotic big-endian host
// we fall back to the bigint-based path rather than corrupting values.
const IS_LITTLE_ENDIAN =
  new Uint8Array(new Uint32Array([0x04030201]).buffer)[0] === 0x01;

// Fast decode varint int64 into a bigint
// Inspired by
// https://github.com/protobufjs/protobuf.js/blob/56b1e64979dae757b67a21d326e16acee39f2267/src/reader.js#L123
export function decodeInt64Varint(
  buf: Uint8Array,
  cursor: {pos: number},
): bigint {
  let pos = cursor.pos;
  let hi: number = 0;
  let lo: number = 0;
  let i = 0;

  if (buf.length - pos > 4) {
    // fast route (lo)
    for (; i < 4; ++i) {
      // 1st..4th
      lo = (lo | ((buf[pos] & 127) << (i * 7))) >>> 0;
      if (buf[pos++] < 128) {
        cursor.pos = pos;
        return BigInt(lo);
      }
    }
    // 5th
    lo = (lo | ((buf[pos] & 127) << 28)) >>> 0;
    hi = (hi | ((buf[pos] & 127) >> 4)) >>> 0;
    if (buf[pos++] < 128) {
      cursor.pos = pos;
      return (BigInt(hi) << SHIFT_32BITS) | BigInt(lo);
    }
    i = 0;
  } else {
    for (; i < 3; ++i) {
      if (pos >= buf.length) {
        throw Error('Index out of range');
      }
      // 1st..3rd
      lo = (lo | ((buf[pos] & 127) << (i * 7))) >>> 0;
      if (buf[pos++] < 128) {
        cursor.pos = pos;
        return BigInt(lo);
      }
    }
    // 4th
    lo = (lo | ((buf[pos++] & 127) << (i * 7))) >>> 0;
    cursor.pos = pos;
    return (BigInt(hi) << SHIFT_32BITS) | BigInt(lo);
  }
  if (buf.length - pos > 4) {
    // fast route (hi)
    for (; i < 5; ++i) {
      // 6th..10th
      hi = (hi | ((buf[pos] & 127) << (i * 7 + 3))) >>> 0;
      if (buf[pos++] < 128) {
        cursor.pos = pos;
        const big = (BigInt(hi) << SHIFT_32BITS) | BigInt(lo);
        return BigInt.asIntN(64, big);
      }
    }
  } else {
    for (; i < 5; ++i) {
      if (pos >= buf.length) {
        throw Error('Index out of range');
      }
      // 6th..10th
      hi = (hi | ((buf[pos] & 127) << (i * 7 + 3))) >>> 0;
      if (buf[pos++] < 128) {
        cursor.pos = pos;
        const big = (BigInt(hi) << SHIFT_32BITS) | BigInt(lo);
        return BigInt.asIntN(64, big);
      }
    }
  }
  throw Error('invalid varint encoding');
}

// Info that could help debug a query error. For example the query
// in question, the stack where the query was issued, the active
// plugin etc.
export interface QueryErrorInfo {
  query: string;
  tag?: string; // The EngineProxy tag.
}

export class QueryError extends Error {
  readonly queryErrorInfo: QueryErrorInfo;

  constructor(message: string, info: QueryErrorInfo) {
    super(message);
    this.queryErrorInfo = info;
  }

  toString() {
    const info = this.queryErrorInfo;
    return `${super.toString()}\nEngineTag: ${info.tag}\nQuery:\n${info.query}`;
  }
}

// One row extracted from an SQL result:
export interface Row {
  [key: string]: SqlValue;
}

export type SpecValue =
  | typeof UNKNOWN
  | typeof NUM
  | typeof LONG
  | typeof NUM_NULL
  | typeof LONG_NULL
  | typeof STR
  | typeof STR_NULL
  | typeof BLOB
  | typeof BLOB_NULL;

export interface SpecType {
  [key: string]: SpecValue;
}

// The methods that any iterator has to implement.
export interface RowIteratorBase {
  valid(): boolean;
  next(): void;

  // Reflection support for cases where the column names are not known upfront
  // (e.g. the query result table for user-provided SQL queries).
  // It throws if the passed column name doesn't exist.
  // Example usage:
  // for (const it = queryResult.iter({}); it.valid(); it.next()) {
  //   for (const columnName : queryResult.columns()) {
  //      console.log(it.get(columnName));
  get(columnName: string): SqlValue;
}

// Has all the types available on it directly + the column getter
export type RowWithGetter<T extends SpecType> = InferRowType<T> &
  Row & {
    get(columnName: string): SqlValue;
  };

// A RowIterator is a type that has all the fields defined in the query spec
// plus the valid() and next() operators. This is to ultimately allow the
// clients to do:
// const result = await engine.query("select name, surname, id from people;");
// const iter = queryResult.iter({name: STR, surname: STR, id: NUM});
// for (; iter.valid(); iter.next())
//  console.log(iter.name, iter.surname);
export type RowIterator<T extends SpecType> = RowIteratorBase & InferRowType<T>;

function columnTypeToString(t: SpecValue): string {
  switch (t) {
    case NUM:
      return 'NUM';
    case NUM_NULL:
      return 'NUM_NULL';
    case STR:
      return 'STR';
    case STR_NULL:
      return 'STR_NULL';
    case BLOB:
      return 'BLOB';
    case BLOB_NULL:
      return 'BLOB_NULL';
    case LONG:
      return 'LONG';
    case LONG_NULL:
      return 'LONG_NULL';
    case UNKNOWN:
      return 'UNKNOWN';
    default:
      return `INVALID(${t})`;
  }
}

function isCompatible(actual: CellType, expected: SpecValue): boolean {
  switch (actual) {
    case CellType.CELL_NULL:
      return (
        expected === NUM_NULL ||
        expected === STR_NULL ||
        expected === BLOB_NULL ||
        expected === LONG_NULL ||
        expected === UNKNOWN
      );
    case CellType.CELL_VARINT:
      return (
        expected === NUM ||
        expected === NUM_NULL ||
        expected === LONG ||
        expected === LONG_NULL ||
        expected === UNKNOWN
      );
    case CellType.CELL_FLOAT64:
      return expected === NUM || expected === NUM_NULL || expected === UNKNOWN;
    case CellType.CELL_STRING:
      return expected === STR || expected === STR_NULL || expected === UNKNOWN;
    case CellType.CELL_BLOB:
      return (
        expected === BLOB || expected === BLOB_NULL || expected === UNKNOWN
      );
    default:
      throw new Error(`Unknown CellType ${actual}`);
  }
}

// One pass over the packed cell types (bytes[start..end)), building per-column
// bitmasks of the cell types present (seen[c] gets bit 1<<type set) and
// returning the MAXIMUM cell type value seen. Callers treat any type above
// CELL_BLOB as out of the known range. (The max, not the bitwise OR: an OR
// would exceed CELL_BLOB for perfectly valid mixed batches, e.g.
// CELL_VARINT | CELL_STRING == 6, needlessly falling back to the slow path.)
// Shared by RowIteratorImpl.tryMoveToNextBatch() and QueryResultImpl
// .decodeColumns() for cheap spec-vs-actual type validation: the walk body is
// one array read and two ORs, and the expensive per-type compatibility check
// only runs once per column.
function scanCellTypeMasks(
  bytes: Uint8Array,
  start: number,
  end: number,
  numColumns: number,
  seen: Uint32Array,
): number {
  let maxType = 0;
  let col = 0;
  for (let i = start; i < end; i++) {
    const t = bytes[i];
    seen[col] |= 1 << t;
    if (t > maxType) maxType = t;
    if (++col === numColumns) col = 0;
  }
  return maxType;
}

// True if every cell type present in |mask| is compatible with |expType|.
function isMaskCompatible(mask: number, expType: SpecValue): boolean {
  while (mask !== 0) {
    const t = 31 - Math.clz32(mask & -mask);
    mask &= mask - 1;
    if (!isCompatible(t as CellType, expType)) return false;
  }
  return true;
}

// Slow path shared by iter() and decodeColumns(): walks the cells of one
// column to find the first incompatible one, so the thrown error can name the
// offending row and column. |rowOffset| is added to the within-batch row index
// (decodeColumns() passes the number of rows decoded from earlier batches).
// Normally throws; if nothing is found (possible when the masks aliased due
// to a cell type outside the known range) it just returns and the caller's
// own decode loop rejects the invalid cell type.
function throwOnIncompatibleCell(
  bytes: Uint8Array,
  start: number,
  end: number,
  numColumns: number,
  col: number,
  expType: SpecValue,
  colName: string,
  rowOffset: number,
  errorInfo: QueryErrorInfo,
): void {
  for (let i = start + col; i < end; i += numColumns) {
    const actualType = bytes[i] as CellType;
    if (isCompatible(actualType, expType)) continue;
    let err;
    if (actualType === CellType.CELL_NULL) {
      err =
        'SQL value is NULL but that was not expected' +
        ` (expected type: ${columnTypeToString(expType)}). ` +
        'Did you mean NUM_NULL, LONG_NULL, STR_NULL or BLOB_NULL?';
    } else {
      err = `Incompatible cell type. Expected: ${columnTypeToString(
        expType,
      )} actual: ${CELL_TYPE_NAMES[actualType]}`;
    }
    const row = rowOffset + Math.floor((i - start) / numColumns);
    throw new QueryError(
      `Error @ row: ${row} col: '${colName}': ${err}`,
      errorInfo,
    );
  }
}

// This has to match CellType in trace_processor.proto.
enum CellType {
  CELL_NULL = 1,
  CELL_VARINT = 2,
  CELL_FLOAT64 = 3,
  CELL_STRING = 4,
  CELL_BLOB = 5,
}

const CELL_TYPE_NAMES = [
  'UNKNOWN',
  'NULL',
  'VARINT',
  'FLOAT64',
  'STRING',
  'BLOB',
];

const TAG_LEN_DELIM = 2;

// This is the interface exposed to readers (e.g. tracks). The underlying object
// (QueryResultImpl) owns the result data. This allows to obtain iterators on
// that. In future it will allow to wait for incremental updates (new rows being
// fetched) for streaming queries.
export interface QueryResult {
  // Obtains an iterator.
  // TODO(primiano): this should have an option to destruct data as we read. In
  // the case of a long query (e.g. `SELECT * FROM sched` in the query prompt)
  // we don't want to accumulate everything in memory. OTOH UI tracks want to
  // keep the data around so they can redraw them on each animation frame. For
  // now we keep everything in memory in the QueryResultImpl object.
  // iter<T extends Row>(spec: T): RowIterator<T>;
  iter<T extends SpecType>(spec: T): RowIterator<T>;

  // Bulk-decodes the requested columns across all currently-available rows in a
  // single pass, returning one dense array per column (see ColumnarColumn for
  // the per-type representation). This is substantially faster than iter() for
  // large result sets where the caller wants whole columns: it loops over every
  // row inside one function (avoiding a per-row valid()/next() round-trip
  // through the iterator's bound-method wrappers) and writes into typed arrays
  // instead of a generic per-row object keyed by column name. Prefer this over
  // iter() on hot track-loading paths that pull millions of cells.
  //
  // Cell types are validated up-front per batch (same checks and same errors
  // as iter()): a mismatch between the spec and the actual query result types,
  // including NULL cells for the non-nullable spec types (NUM/LONG/STR),
  // throws a QueryError.
  decodeColumns<T extends SpecType>(spec: T): ColumnarResultFor<T>;

  // Like iter() for queries that expect only one row. It embeds the valid()
  // check (i.e. throws if no rows are available) and returns directly the
  // first result.
  firstRow<T extends SpecType>(spec: T): RowWithGetter<T>;

  // Like firstRow() but returns undefined if no rows are available.
  maybeFirstRow<T extends SpecType>(spec: T): RowWithGetter<T> | undefined;

  // If != undefined the query errored out and error() contains the message.
  error(): string | undefined;

  // Returns the number of rows accumulated so far. Note that this number can
  // change over time as more batches are received. It becomes stable only
  // when isComplete() returns true or after waitAllRows() is resolved.
  numRows(): number;

  // If true all rows have been fetched. Calling iter() will iterate through the
  // last row. If false, iter() will return an iterator which might iterate
  // through some rows (or none) but will surely not reach the end.
  isComplete(): boolean;

  // Returns a promise that is resolved only when all rows (i.e. all batches)
  // have been fetched. The promise return value is always the object itself.
  waitAllRows(): Promise<QueryResult>;

  // Returns a promise that is resolved when either:
  // - more rows are available
  // - all rows are available
  // The promise return value is always the object iself.
  waitMoreRows(): Promise<QueryResult>;

  // Can return an empty array if called before the first batch is resolved.
  // This should be called only after having awaited for at least one batch.
  columns(): string[];

  // Returns the number of SQL statements in the query
  // (e.g. 2 'if SELECT 1; SELECT 2;')
  statementCount(): number;

  // Returns the number of SQL statement that produced output rows. This number
  // is <= statementCount().
  statementWithOutputCount(): number;

  // Returns the last SQL statement.
  lastStatementSql(): string;

  // Returns the time spend processing the query so far in milliseconds. This is
  // timed by trace processor to avoid any time spend in the queue. It's updated
  // every time we get a new batch to convey the time taken do far, so it should
  // be monotonically increasing and stable when isComplete() is true.
  elapsedTimeMs(): number;
}

// Drains a QueryResult into an array of typed rows described by `spec`. This is
// the array-materializing counterpart to iter(): use it when you want every row
// up front rather than streaming through them.
// Example: const rows = materializeRows(result, {id: NUM, name: STR});
export function materializeRows<T extends SpecType>(
  result: QueryResult,
  spec: T,
): InferRowType<T>[] {
  const rows: InferRowType<T>[] = [];
  const cols = Object.keys(spec);
  for (const it = result.iter(spec); it.valid(); it.next()) {
    const row: Record<string, SqlValue> = {};
    for (const col of cols) row[col] = it.get(col);
    rows.push(row as InferRowType<T>);
  }
  return rows;
}

// Interface exposed to engine.ts to pump in the data as new row batches arrive.
export interface WritableQueryResult {
  // |resBytes| is a proto-encoded trace_processor.QueryResult message.
  //  The overall flow looks as follows:
  // - The user calls engine.query('select ...') and gets a QueryResult back.
  // - The query call posts a message to the worker that runs the SQL engine (
  //   or sends a HTTP request in case of the RPC+HTTP interface).
  // - The returned QueryResult object is initially empty.
  // - Over time, the sql engine will postMessage() back results in batches.
  // - Each bach will end up calling this appendResultBatch() method.
  // - If there is any pending promise (e.g. the caller called
  //   queryResult.waitAllRows()), this call will awake them (if this is the
  //   last batch).
  appendResultBatch(resBytes: Uint8Array): void;

  // If true all rows have been fetched. If false, more appendResultBatch()
  // calls are expected.
  isComplete(): boolean;
}

// The actual implementation, which bridges together the reader side and the
// writer side (the one exposed to the Engine). This is the same object so that
// when the engine pumps new row batches we can resolve pending promises that
// readers (e.g. track code) are waiting for.
class QueryResultImpl implements QueryResult, WritableQueryResult {
  columnNames: string[] = [];
  private _error?: string;
  private _numRows = 0;
  private _isComplete = false;
  private _errorInfo: QueryErrorInfo;
  private _statementCount = 0;
  private _statementWithOutputCount = 0;
  private _lastStatementSql = '';
  private _elapsedTimeMs = 0;

  constructor(errorInfo: QueryErrorInfo) {
    this._errorInfo = errorInfo;
  }

  // --- QueryResult implementation.

  // TODO(primiano): for the moment new batches are appended but old batches
  // are never removed. This won't work with abnormally large result sets, as
  // it will stash all rows in memory. We could switch to a model where the
  // iterator is destructive and deletes batch objects once iterating past the
  // end of each batch. If we do that, than we need to assign monotonic IDs to
  // batches. Also if we do that, we should prevent creating more than one
  // iterator for a QueryResult.
  batches: ResultBatch[] = [];

  // Promise awaiting on waitAllRows(). This should be resolved only when the
  // last result batch has been been retrieved.
  private allRowsPromise?: Deferred<QueryResult>;

  // Promise awaiting on waitMoreRows(). This resolved when the next
  // batch is appended via appendResultBatch.
  private moreRowsPromise?: Deferred<QueryResult>;

  isComplete(): boolean {
    return this._isComplete;
  }
  numRows(): number {
    return this._numRows;
  }
  error(): string | undefined {
    return this._error;
  }
  columns(): string[] {
    return this.columnNames;
  }
  statementCount(): number {
    return this._statementCount;
  }
  statementWithOutputCount(): number {
    return this._statementWithOutputCount;
  }
  lastStatementSql(): string {
    return this._lastStatementSql;
  }
  elapsedTimeMs(): number {
    return this._elapsedTimeMs;
  }

  iter<T extends SpecType>(spec: T): RowIterator<T> {
    const impl = new RowIteratorImplWithRowData(spec, this);
    return impl as {} as RowIterator<T>;
  }

  decodeColumns<T extends SpecType>(spec: T): ColumnarResultFor<T> {
    const colNames = this.columnNames;
    const numColumns = colNames.length;
    const total = this._numRows;

    // Per-column plan, indexed by column position. `kind` selects how the cell
    // is materialised and into which output array; ignored columns (kind 0)
    // still advance the per-type cursors but store nothing.
    const enum Kind {
      Ignore = 0,
      Num = 1, // -> Float64Array
      Long = 2, // -> BigInt64Array
      Str = 3, // -> Array<string | null>
      Blob = 4, // -> Array<SqlValue>
      NumNull = 5, // -> Array<number | null>
      LongNull = 6, // -> Array<bigint | null>
    }
    const kinds = new Uint8Array(numColumns);
    // The original spec value per column (undefined = column not requested),
    // used by the type validation pre-pass below and for its error messages.
    const specValues = new Array<SpecValue | undefined>(numColumns);
    // For LONG columns backed by a BigInt64Array (little-endian only), an
    // Int32Array view over the same buffer, used to store varints without
    // materializing a bigint. Indexed by column position; undefined otherwise.
    const longLo32s = new Array<Int32Array | undefined>(numColumns);
    // dests is indexed by column position; entries are typed/plain arrays whose
    // element type is governed by kinds[c]. The stores below cast per-kind.
    const dests = new Array<ColumnarColumn | undefined>(numColumns);
    const out: {[k: string]: ColumnarColumn} = {};

    for (const name of Object.keys(spec)) {
      const c = colNames.indexOf(name);
      if (c < 0) {
        throw new QueryError(
          `Column ${name} not found in the SQL result ` +
            `set {${colNames.join(' ')}}`,
          this._errorInfo,
        );
      }
      const t = spec[name];
      let kind: Kind;
      let dest: ColumnarColumn;
      if (t === NUM) {
        kind = Kind.Num;
        dest = new Float64Array(total);
      } else if (t === NUM_NULL) {
        kind = Kind.NumNull;
        dest = new Array<number | null>(total);
      } else if (t === LONG) {
        kind = Kind.Long;
        const longArr = new BigInt64Array(total);
        dest = longArr;
        if (IS_LITTLE_ENDIAN) {
          longLo32s[c] = new Int32Array(longArr.buffer, 0, total * 2);
        }
      } else if (t === LONG_NULL) {
        kind = Kind.LongNull;
        dest = new Array<bigint | null>(total);
      } else if (t === STR || t === STR_NULL) {
        kind = Kind.Str;
        dest = new Array<string | null>(total);
      } else {
        kind = Kind.Blob;
        dest = new Array<SqlValue>(total);
      }
      kinds[c] = kind;
      // Row's index signature is SqlValue, but spec objects only ever hold
      // SpecValues (see the checks above).
      specValues[c] = t as SpecValue;
      dests[c] = dest;
      out[name] = dest;
    }

    let row = 0;
    const batches = this.batches;
    for (let b = 0; b < batches.length; b++) {
      const batch = batches[b];
      if (batch.numCells === 0) continue;
      const bytes = batch.batchBytes;
      const f64 = batch.float64Cells;
      const strs = batch.stringCells;
      const blobs = batch.blobCells;
      const ctEnd = batch.cellTypesOff + batch.cellTypesLen;
      let ctOff = batch.cellTypesOff;

      // Type validation pre-pass, shared with iter(): one cheap byte scan per
      // batch building per-column cell-type bitmasks; the spec compatibility
      // check then only runs once per column. On mismatch this throws the
      // same QueryError that iter() would (same message format), including
      // NULL cells for the non-nullable spec types (NUM/LONG/STR), which
      // previously decoded as sentinels (NaN / 0n).
      const seen = new Uint32Array(numColumns);
      const maxCellType = scanCellTypeMasks(
        bytes,
        ctOff,
        ctEnd,
        numColumns,
        seen,
      );
      // A type outside the known range would alias in the bitmask (JS shifts
      // are mod 32), so fall back to walking every cell in that case.
      const trustMasks = maxCellType <= CellType.CELL_BLOB;
      for (let c = 0; c < numColumns; c++) {
        const expType = specValues[c];
        if (expType === undefined) continue;
        if (trustMasks && isMaskCompatible(seen[c], expType)) continue;
        // Slow path: throws, naming the first offending row and column.
        throwOnIncompatibleCell(
          bytes,
          ctOff,
          ctEnd,
          numColumns,
          c,
          expType,
          colNames[c],
          row, // Rows already decoded from earlier batches.
          this._errorInfo,
        );
      }

      const varintCursor = {pos: batch.varintOff};
      let fIdx = 0;
      let sIdx = 0;
      let bIdx = 0;

      while (ctOff < ctEnd) {
        for (let c = 0; c < numColumns; c++) {
          const cellType = bytes[ctOff++];
          const kind = kinds[c];
          switch (cellType) {
            case CellType.CELL_VARINT:
              if (kind === Kind.Num || kind === Kind.NumNull) {
                const v = readVarIntAsNumber(bytes, varintCursor);
                (dests[c] as Array<number | null>)[row] = v;
              } else if (kind === Kind.Long) {
                const lo32 = longLo32s[c];
                if (lo32 !== undefined) {
                  readVarIntIntoInt32s(bytes, varintCursor, lo32, row);
                } else {
                  // Big-endian fallback: go through a bigint.
                  const v = decodeInt64Varint(bytes, varintCursor);
                  (dests[c] as BigInt64Array)[row] = v;
                }
              } else {
                const v = decodeInt64Varint(bytes, varintCursor);
                if (dests[c] !== undefined) {
                  (dests[c] as Array<bigint | null>)[row] = v;
                }
              }
              break;
            case CellType.CELL_FLOAT64: {
              const v = f64[fIdx++];
              if (dests[c] !== undefined) {
                (dests[c] as Array<number | null>)[row] = v;
              }
              break;
            }
            case CellType.CELL_STRING: {
              const v = strs[sIdx++];
              if (kind === Kind.Str) {
                (dests[c] as Array<string | null>)[row] = v;
              }
              break;
            }
            case CellType.CELL_NULL:
              // The validation pre-pass above rejects NULLs for the
              // non-nullable spec types, so this is normally only reachable
              // for nullable columns (which store null). The sentinel stores
              // below are defensive: they can only run for batches containing
              // out-of-range cell types, where the switch default below throws
              // on the invalid cell anyway.
              if (kind === Kind.Num) {
                (dests[c] as Float64Array)[row] = NaN;
              } else if (kind === Kind.Long) {
                (dests[c] as BigInt64Array)[row] = 0n;
              } else if (kind !== Kind.Ignore) {
                (dests[c] as Array<SqlValue>)[row] = null;
              }
              break;
            case CellType.CELL_BLOB: {
              const v = blobs[bIdx++];
              if (kind === Kind.Blob) {
                (dests[c] as Array<SqlValue>)[row] = new Uint8Array(v);
              }
              break;
            }
            default:
              throw new QueryError(
                `Invalid cell type ${cellType}`,
                this._errorInfo,
              );
          }
        }
        row++;
      }
    }
    return out as ColumnarResultFor<T>;
  }

  firstRow<T extends SpecType>(spec: T): RowWithGetter<T> {
    const impl = new RowIteratorImplWithRowData(spec, this);
    assertTrue(impl.valid());
    return impl as {} as Row as RowWithGetter<T>;
  }

  maybeFirstRow<T extends SpecType>(spec: T): RowWithGetter<T> | undefined {
    const impl = new RowIteratorImplWithRowData(spec, this);
    if (!impl.valid()) {
      return undefined;
    }
    return impl as {} as Row as RowWithGetter<T>;
  }

  // Can be called only once.
  waitAllRows(): Promise<QueryResult> {
    assertTrue(this.allRowsPromise === undefined);
    this.allRowsPromise = defer<QueryResult>();
    if (this._isComplete) {
      this.resolveOrReject(this.allRowsPromise, this);
    }
    return this.allRowsPromise;
  }

  waitMoreRows(): Promise<QueryResult> {
    if (this.moreRowsPromise !== undefined) {
      return this.moreRowsPromise;
    }

    const moreRowsPromise = defer<QueryResult>();
    if (this._isComplete) {
      this.resolveOrReject(moreRowsPromise, this);
    } else {
      this.moreRowsPromise = moreRowsPromise;
    }
    return moreRowsPromise;
  }

  // --- WritableQueryResult implementation.

  // Called by the engine when a new QueryResult is available. Note that a
  // single Query() call can yield >1 QueryResult due to result batching
  // if more than ~64K of data are returned, e.g. when returning O(M) rows.
  // |resBytes| is a proto-encoded trace_processor.QueryResult message.
  // It is fine to retain the resBytes without slicing a copy, because
  // ProtoRingBuffer does the slice() for us (or passes through the buffer
  // coming from postMessage() (Wasm case) of fetch() (HTTP+RPC case).
  appendResultBatch(resBytes: Uint8Array<ArrayBuffer>) {
    const reader = protobuf.Reader.create(resBytes);
    assertTrue(reader.pos === 0);
    const columnNamesEmptyAtStartOfBatch = this.columnNames.length === 0;
    const columnNamesSet = new Set<string>();
    while (reader.pos < reader.len) {
      const tag = reader.uint32();
      switch (tag >>> 3) {
        case 1: // column_names
          // Only the first batch should contain the column names. If this fires
          // something is going wrong in the handling of the batch stream.
          assertTrue(columnNamesEmptyAtStartOfBatch);
          const origColName = reader.string();
          let colName = origColName;
          // In some rare cases two columns can have the same name (b/194891824)
          // e.g. `select 1 as x, 2 as x`. These queries don't happen in the
          // UI code, but they can happen when the user types a query (e.g.
          // with a join). The most practical thing we can do here is renaming
          // the columns with a suffix. Keeping the same name will break when
          // iterating, because column names become iterator object keys.
          for (let i = 1; columnNamesSet.has(colName); ++i) {
            colName = `${origColName}_${i}`;
            assertTrue(i < 100); // Give up at some point;
          }
          columnNamesSet.add(colName);
          this.columnNames.push(colName);
          break;
        case 2: // error
          // The query has errored only if the |error| field is non-empty.
          // In protos, we don't distinguish between non-present and empty.
          // Make sure we don't propagate ambiguous empty strings to JS.
          const err = reader.string();
          this._error = err !== undefined && err.length ? err : undefined;
          break;
        case 3: // batch
          const batchLen = reader.uint32();
          const batchRaw = resBytes.subarray(reader.pos, reader.pos + batchLen);
          reader.pos += batchLen;

          // The ResultBatch ctor parses the CellsBatch submessage.
          const parsedBatch = new ResultBatch(batchRaw);
          this.batches.push(parsedBatch);
          this._isComplete = parsedBatch.isLastBatch;

          // In theory one could construct a valid proto serializing the column
          // names after the cell batches. In practice the QueryResultSerializer
          // doesn't do that so it's not worth complicating the code.
          const numColumns = this.columnNames.length;
          if (numColumns !== 0) {
            assertTrue(parsedBatch.numCells % numColumns === 0);
            this._numRows += parsedBatch.numCells / numColumns;
          } else {
            // numColumns == 0 is  plausible for queries like CREATE TABLE ... .
            assertTrue(parsedBatch.numCells === 0);
          }
          break;

        case 4:
          this._statementCount = reader.uint32();
          break;

        case 5:
          this._statementWithOutputCount = reader.uint32();
          break;

        case 6:
          this._lastStatementSql = reader.string();
          break;

        case 7:
          this._elapsedTimeMs = reader.double();
          break;

        default:
          console.warn(`Unexpected QueryResult field ${tag >>> 3}`);
          reader.skipType(tag & 7);
          break;
      } // switch (tag)
    } // while (pos < end)

    if (this.moreRowsPromise !== undefined) {
      this.resolveOrReject(this.moreRowsPromise, this);
      this.moreRowsPromise = undefined;
    }

    if (this._isComplete && this.allRowsPromise !== undefined) {
      this.resolveOrReject(this.allRowsPromise, this);
    }
  }

  ensureAllRowsPromise(): Promise<QueryResult> {
    if (this.allRowsPromise === undefined) {
      this.waitAllRows(); // Will populate |this.allRowsPromise|.
    }
    return ensureExists(this.allRowsPromise);
  }

  get errorInfo(): QueryErrorInfo {
    return this._errorInfo;
  }

  private resolveOrReject(promise: Deferred<QueryResult>, arg: QueryResult) {
    if (this._error === undefined) {
      promise.resolve(arg);
    } else {
      promise.reject(new QueryError(this._error, this._errorInfo));
    }
  }
}

// This class holds onto a received result batch (a Uint8Array) and does some
// partial parsing to tokenize the various cell groups. This parsing mainly
// consists of identifying and caching the offsets of each cell group and
// initializing the varint decoders. This half parsing is done to keep the
// iterator's next() fast, without decoding everything into memory.
// This is an internal implementation detail and is not exposed outside. The
// RowIteratorImpl uses this class to iterate through batches (this class takes
// care of iterating within a batch, RowIteratorImpl takes care of switching
// batches when needed).
// Note: at any point in time there can be more than one ResultIterator
// referencing the same batch. The batch must be immutable.
class ResultBatch {
  readonly isLastBatch: boolean = false;
  readonly batchBytes: Uint8Array<ArrayBuffer>;
  readonly cellTypesOff: number = 0;
  readonly cellTypesLen: number = 0;
  readonly varintOff: number = 0;
  readonly varintLen: number = 0;
  readonly float64Cells = new Float64Array();
  readonly blobCells: Uint8Array<ArrayBuffer>[] = [];
  readonly stringCells: string[] = [];

  // batchBytes is a trace_processor.QueryResult.CellsBatch proto.
  constructor(batchBytes: Uint8Array<ArrayBuffer>) {
    this.batchBytes = batchBytes;
    const reader = protobuf.Reader.create(batchBytes);
    assertTrue(reader.pos === 0);
    const end = reader.len;

    // Here we deconstruct the proto by hand. The CellsBatch is carefully
    // designed to allow a very fast parsing from the TS side. We pack all cells
    // of the same types together, so we can do only one call (per batch) to
    // TextDecoder.decode(), we can overlay a memory-aligned typedarray for
    // float values and can quickly tell and type-check the cell types.
    // One row = N cells (we know the number upfront from the outer message).
    // Each bach contains always an integer multiple of N cells (i.e. rows are
    // never fragmented across different batches).
    while (reader.pos < end) {
      const tag = reader.uint32();
      switch (tag >>> 3) {
        case 1: // cell types, a packed array containing one CellType per cell.
          assertTrue((tag & 7) === TAG_LEN_DELIM); // Must be packed varint.
          this.cellTypesLen = reader.uint32();
          this.cellTypesOff = reader.pos;
          reader.pos += this.cellTypesLen;
          break;

        case 2: // varint_cells, a packed varint buffer.
          assertTrue((tag & 7) === TAG_LEN_DELIM); // Must be packed varint.
          const packLen = reader.uint32();
          this.varintOff = reader.pos;
          this.varintLen = packLen;
          assertTrue(reader.buf === batchBytes);
          assertTrue(
            this.varintOff + this.varintLen <=
              batchBytes.byteOffset + batchBytes.byteLength,
          );
          reader.pos += packLen;
          break;

        case 3: // float64_cells, a 64-bit aligned packed fixed64 buffer.
          assertTrue((tag & 7) === TAG_LEN_DELIM); // Must be packed varint.
          const f64Len = reader.uint32();
          assertTrue(f64Len % 8 === 0);
          // Float64Array's constructor is evil: the offset is in bytes but the
          // length is in 8-byte words.
          const f64Words = f64Len / 8;
          const f64Off = batchBytes.byteOffset + reader.pos;
          if (f64Off % 8 === 0) {
            this.float64Cells = new Float64Array(
              batchBytes.buffer,
              f64Off,
              f64Words,
            );
          } else {
            // When using the production code in trace_processor's rpc.cc, the
            // float64 should be 8-bytes aligned. The slow-path case is only for
            // tests.
            const slice = batchBytes.buffer.slice(f64Off, f64Off + f64Len);
            this.float64Cells = new Float64Array(slice);
          }
          reader.pos += f64Len;
          break;

        case 4: // blob_cells: one entry per blob.
          assertTrue((tag & 7) === TAG_LEN_DELIM);
          // protobufjs's bytes() under the hoods calls slice() and creates
          // a copy. Fine here as blobs are rare and not a fastpath.
          this.blobCells.push(new Uint8Array(reader.bytes()));
          break;

        case 5: // string_cells: all the string cells concatenated with \0s.
          assertTrue((tag & 7) === TAG_LEN_DELIM);
          const strLen = reader.uint32();
          assertTrue(reader.pos + strLen <= end);
          const subArr = batchBytes.subarray(reader.pos, reader.pos + strLen);
          assertTrue(subArr.length === strLen);
          // The reason why we do this split rather than creating one string
          // per entry is that utf8 decoding has some non-negligible cost. See
          // go/postmessage-benchmark .
          this.stringCells = utf8Decode(subArr).split('\0');
          reader.pos += strLen;
          break;

        case 6: // is_last_batch (boolean).
          this.isLastBatch = !!reader.bool();
          break;

        case 7: // padding for realignment, skip silently.
          reader.skipType(tag & 7);
          break;

        default:
          console.warn(`Unexpected QueryResult.CellsBatch field ${tag >>> 3}`);
          reader.skipType(tag & 7);
          break;
      } // switch(tag)
    } // while (pos < end)
  }

  get numCells() {
    return this.cellTypesLen;
  }
}

class RowIteratorImpl implements RowIteratorBase {
  // The spec passed to the iter call containing the expected types, e.g.:
  // {'colA': NUM, 'colB': NUM_NULL, 'colC': STRING}.
  // This doesn't ever change.
  readonly rowSpec: SpecType;

  // The object that holds the current row. This points to the parent
  // RowIteratorImplWithRowData instance that created this class.
  rowData: Row;

  // The QueryResult object we are reading data from. The engine will pump
  // batches over time into this object.
  private resultObj: QueryResultImpl;

  // All the member variables in the group below point to the identically-named
  // members in result.batch[batchIdx]. This is to avoid indirection layers in
  // the next() hotpath, so we can do this.float64Cells vs
  // this.resultObj.batch[this.batchIdx].float64Cells.
  // These are re-set every time tryMoveToNextBatch() is called (and succeeds).
  private batchIdx = -1; // The batch index within |result.batches[]|.
  private batchBytes = new Uint8Array();
  private columnNames: string[] = [];
  // rowSpec resolved by column position; see tryMoveToNextBatch().
  private colTypes: Array<SpecValue | undefined> = [];
  private numColumns = 0;
  private cellTypesEnd = -1; // -1 so the 1st next() hits tryMoveToNextBatch().
  private float64Cells = new Float64Array();
  // Cursor into |batchBytes| over the packed varint_cells region. This replaces
  // a protobuf.Reader, whose int64() allocates per cell.
  private varintCursor = {pos: 0};
  private blobCells: Uint8Array<ArrayBuffer>[] = [];
  private stringCells: string[] = [];

  // These members instead are incremented as we read cells from next(). They
  // are the mutable state of the iterator.
  private nextCellTypeOff = 0;
  private nextFloat64Cell = 0;
  private nextStringCell = 0;
  private nextBlobCell = 0;
  private isValid = false;

  constructor(querySpec: SpecType, rowData: Row, res: QueryResultImpl) {
    Object.assign(this, querySpec);
    this.rowData = rowData;
    this.rowSpec = {...querySpec}; // ... -> Copy all the key/value pairs.
    this.resultObj = res;
    this.next();
  }

  valid(): boolean {
    return this.isValid;
  }

  private makeError(message: string): QueryError {
    return new QueryError(message, this.resultObj.errorInfo);
  }

  get(columnName: string): SqlValue {
    const res = this.rowData[columnName];
    if (res === undefined) {
      throw this.makeError(
        `Column '${columnName}' doesn't exist. ` +
          `Actual columns: [${this.columnNames.join(',')}]`,
      );
    }
    return res;
  }

  // Moves the cursor next by one row and updates |isValid|.
  // When this fails to move, two cases are possible:
  // 1. We reached the end of the result set (this is the case if
  //    QueryResult.isComplete() == true when this fails).
  // 2. We reached the end of the current batch, but more rows might come later
  //    (if QueryResult.isComplete() == false).
  next() {
    // At some point we might reach the end of the current batch, but the next
    // batch might be available already. In this case we want next() to
    // transparently move on to the next batch.
    while (this.nextCellTypeOff + this.numColumns > this.cellTypesEnd) {
      // If TraceProcessor is behaving well, we should never end up in a
      // situation where we have leftover cells. TP is expected to serialize
      // whole rows in each QueryResult batch and NOT truncate them midway.
      // If this assert fires the TP RPC logic has a bug.
      assertTrue(
        this.nextCellTypeOff === this.cellTypesEnd || this.cellTypesEnd === -1,
      );
      if (!this.tryMoveToNextBatch()) {
        this.isValid = false;
        return;
      }
    }

    const rowData = this.rowData;
    const numColumns = this.numColumns;
    const batchBytes = this.batchBytes;
    const colTypes = this.colTypes;
    const varintCursor = this.varintCursor;

    // Read the current row.
    for (let i = 0; i < numColumns; i++) {
      const cellType = batchBytes[this.nextCellTypeOff++];
      const colName = this.columnNames[i];
      const expType = colTypes[i];

      switch (cellType) {
        case CellType.CELL_NULL:
          rowData[colName] = null;
          break;

        case CellType.CELL_VARINT:
          if (expType === NUM || expType === NUM_NULL) {
            rowData[colName] = readVarIntAsNumber(batchBytes, varintCursor);
          } else {
            // LONG, LONG_NULL, or unspecified - return as bigint
            rowData[colName] = decodeInt64Varint(batchBytes, varintCursor);
          }
          break;

        case CellType.CELL_FLOAT64:
          rowData[colName] = this.float64Cells[this.nextFloat64Cell++];
          break;

        case CellType.CELL_STRING:
          rowData[colName] = this.stringCells[this.nextStringCell++];
          break;

        case CellType.CELL_BLOB:
          const blob = this.blobCells[this.nextBlobCell++];
          rowData[colName] = blob;
          break;

        default:
          throw this.makeError(`Invalid cell type ${cellType}`);
      }
    } // For (cells)
    this.isValid = true;
  }

  private tryMoveToNextBatch(): boolean {
    const nextBatchIdx = this.batchIdx + 1;
    if (nextBatchIdx >= this.resultObj.batches.length) {
      return false;
    }

    this.columnNames = this.resultObj.columnNames;
    this.numColumns = this.columnNames.length;
    // Resolve the expected type once per column rather than doing a
    // string-keyed lookup into rowSpec for every cell, both here and in next().
    this.colTypes = this.columnNames.map((name) => this.rowSpec[name]);

    this.batchIdx = nextBatchIdx;
    const batch = ensureExists(this.resultObj.batches[nextBatchIdx]);
    this.batchBytes = batch.batchBytes;
    this.nextCellTypeOff = batch.cellTypesOff;
    this.cellTypesEnd = batch.cellTypesOff + batch.cellTypesLen;
    this.float64Cells = batch.float64Cells;
    this.blobCells = batch.blobCells;
    this.stringCells = batch.stringCells;
    this.varintCursor.pos = batch.varintOff;
    this.nextFloat64Cell = 0;
    this.nextStringCell = 0;
    this.nextBlobCell = 0;

    // Check that all the expected columns are present.
    for (const expectedCol of Object.keys(this.rowSpec)) {
      if (this.columnNames.indexOf(expectedCol) < 0) {
        throw this.makeError(
          `Column ${expectedCol} not found in the SQL result ` +
            `set {${this.columnNames.join(' ')}}`,
        );
      }
    }

    // Check that the cells types are consistent.
    const numColumns = this.numColumns;
    if (batch.numCells === 0) {
      // This can happen if the query result contains just an error. In this
      // an empty batch with isLastBatch=true is appended as an EOF marker.
      // In theory TraceProcessor could return an empty batch in the middle and
      // that would be fine from a protocol viewpoint. In practice, no code path
      // does that today so it doesn't make sense trying supporting it with a
      // recursive call to tryMoveToNextBatch().
      assertTrue(batch.isLastBatch);
      return false;
    }

    assertTrue(numColumns > 0);

    // Collect, per column, the set of cell types present in this batch and
    // check them against the spec (scanCellTypeMasks() /
    // throwOnIncompatibleCell() are shared with decodeColumns()).
    const seen = new Uint32Array(numColumns);
    const maxCellType = scanCellTypeMasks(
      this.batchBytes,
      this.nextCellTypeOff,
      this.cellTypesEnd,
      numColumns,
      seen,
    );

    // A type outside the known range would alias in the bitmask above (JS
    // shifts are mod 32), so fall back to checking every cell in that case.
    const trustMasks = maxCellType <= CellType.CELL_BLOB;
    for (let c = 0; c < numColumns; c++) {
      const expType = this.colTypes[c];
      // If undefined, the caller doesn't want to read this column at all, so
      // it can be whatever.
      if (expType === undefined) continue;
      if (trustMasks && isMaskCompatible(seen[c], expType)) continue;
      this.checkColumnCellTypes(c, expType, numColumns);
    }
    return true;
  }

  // Slow path: walks the cells of one column to find the first incompatible
  // one, so the thrown error can name the offending row. Delegates to the
  // shared throwOnIncompatibleCell() (also used by decodeColumns()).
  private checkColumnCellTypes(
    col: number,
    expType: SpecValue,
    numColumns: number,
  ): void {
    throwOnIncompatibleCell(
      this.batchBytes,
      this.nextCellTypeOff,
      this.cellTypesEnd,
      numColumns,
      col,
      expType,
      this.columnNames[col],
      0,
      this.resultObj.errorInfo,
    );
  }
}

// This is the object ultimately returned to the client when calling
// QueryResult.iter(...).
// The only reason why this is disjoint from RowIteratorImpl is to avoid
// naming collisions between the members variables required by RowIteratorImpl
// and the column names returned by the iterator.
class RowIteratorImplWithRowData implements RowIteratorBase {
  private _impl: RowIteratorImpl;

  next: () => void;
  valid: () => boolean;
  get: (columnName: string) => SqlValue;

  constructor(querySpec: SpecType, res: QueryResultImpl) {
    const thisAsRow = this as {} as Row;
    Object.assign(thisAsRow, querySpec);
    this._impl = new RowIteratorImpl(querySpec, thisAsRow, res);
    this.next = this._impl.next.bind(this._impl);
    this.valid = this._impl.valid.bind(this._impl);
    this.get = this._impl.get.bind(this._impl);
  }
}

// This is a proxy object that wraps QueryResultImpl, adding await-ability.
// This is so that:
// 1. Clients that just want to await for the full result set can just call
//    await engine.query('...') and will get a QueryResult that is guaranteed
//    to be complete.
// 2. Clients that know how to handle the streaming can use it straight away.
class WaitableQueryResultImpl
  implements QueryResult, WritableQueryResult, PromiseLike<QueryResult>
{
  private impl: QueryResultImpl;
  private thenCalled = false;

  constructor(errorInfo: QueryErrorInfo) {
    this.impl = new QueryResultImpl(errorInfo);
  }

  // QueryResult implementation. Proxies all calls to the impl object.
  iter<T extends SpecType>(spec: T) {
    return this.impl.iter(spec);
  }
  decodeColumns<T extends SpecType>(spec: T) {
    return this.impl.decodeColumns(spec);
  }
  firstRow<T extends SpecType>(spec: T) {
    return this.impl.firstRow(spec);
  }
  maybeFirstRow<T extends SpecType>(spec: T) {
    return this.impl.maybeFirstRow(spec);
  }
  waitAllRows() {
    return this.impl.waitAllRows();
  }
  waitMoreRows() {
    return this.impl.waitMoreRows();
  }
  isComplete() {
    return this.impl.isComplete();
  }
  numRows() {
    return this.impl.numRows();
  }
  columns() {
    return this.impl.columns();
  }
  error() {
    return this.impl.error();
  }
  statementCount() {
    return this.impl.statementCount();
  }
  statementWithOutputCount() {
    return this.impl.statementWithOutputCount();
  }
  lastStatementSql() {
    return this.impl.lastStatementSql();
  }
  elapsedTimeMs() {
    return this.impl.elapsedTimeMs();
  }

  // WritableQueryResult implementation.
  appendResultBatch(resBytes: Uint8Array<ArrayBuffer>) {
    return this.impl.appendResultBatch(resBytes);
  }

  // PromiseLike<QueryResult> implementaton.

  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  then(onfulfilled: any, onrejected: any): any {
    assertFalse(this.thenCalled);
    this.thenCalled = true;
    return this.impl.ensureAllRowsPromise().then(onfulfilled, onrejected);
  }

  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  catch(error: any): any {
    return this.impl.ensureAllRowsPromise().catch(error);
  }

  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  finally(callback: () => void): any {
    return this.impl.ensureAllRowsPromise().finally(callback);
  }

  // eslint and clang-format disagree on how to format get[foo](). Let
  // clang-format win:
  get [Symbol.toStringTag](): string {
    return 'Promise<WaitableQueryResult>';
  }
}

export function createQueryResult(
  errorInfo: QueryErrorInfo,
): QueryResult & Promise<QueryResult> & WritableQueryResult {
  return new WaitableQueryResultImpl(errorInfo);
}

// Throws if the value cannot be reasonably converted to a bigint.
// Assumes value is in native time units.
export function timeFromSql(value: SqlValue): time {
  if (typeof value === 'bigint') {
    return Time.fromRaw(value);
  } else if (typeof value === 'number') {
    return Time.fromRaw(BigInt(Math.floor(value)));
  } else if (value === null) {
    return Time.ZERO;
  } else {
    throw Error(`Refusing to create time from unrelated type ${value}`);
  }
}

// Throws if the value cannot be reasonably converted to a bigint.
// Assumes value is in nanoseconds.
export function durationFromSql(value: SqlValue): duration {
  if (typeof value === 'bigint') {
    return value;
  } else if (typeof value === 'number') {
    return BigInt(Math.floor(value));
  } else if (value === null) {
    return Duration.ZERO;
  } else {
    throw Error(`Refusing to create duration from unrelated type ${value}`);
  }
}
