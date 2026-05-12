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

// JS outer-join for diff views: one row per key with baseline / current /
// delta columns plus a status. Cross-engine SQL JOIN isn't possible
// (each baseline trace lives in its own Worker / sqlite), so the merge
// runs in JS over already-aggregated rows.

import type {Row, SqlValue} from '../../../trace_processor/query_result';

export type Num = number | bigint;

export type DiffStatus = 'NEW' | 'REMOVED' | 'GREW' | 'SHRANK' | 'UNCHANGED';

export const KEY_COL = 'key';
export const STATUS_COL = 'status';
export const baselineCol = (field: string) => `_b_${field}`;
export const currentCol = (field: string) => `_c_${field}`;
export const deltaCol = (field: string) => `_d_${field}`;

export interface MergeOptions<T extends Row> {
  readonly baseline: readonly T[];
  readonly current: readonly T[];
  readonly keyOf: (row: T) => string;
  readonly numericFields: readonly string[];
  // Non-numeric columns to copy through (preferring current side).
  readonly passThroughFields?: readonly string[];
  readonly primaryDeltaField: string;
  // |Δ(primaryDeltaField)| ≤ threshold → UNCHANGED. Defaults to 0.
  readonly statusThreshold?: Num;
}

export interface DiffRow extends Row {
  readonly [KEY_COL]: string;
  readonly [STATUS_COL]: DiffStatus;
  readonly [field: string]: SqlValue;
}

// Outer-join `baseline` and `current` by `keyOf`. Throws on duplicate keys
// (inputs must already be aggregated by the join key — see `dedupeByKey`).
export function mergeRows<T extends Row>(opts: MergeOptions<T>): DiffRow[] {
  const {
    baseline,
    current,
    keyOf,
    numericFields,
    passThroughFields = [],
    primaryDeltaField,
    statusThreshold,
  } = opts;

  if (!numericFields.includes(primaryDeltaField)) {
    throw new Error(
      `mergeRows: primaryDeltaField '${primaryDeltaField}' must be in numericFields`,
    );
  }

  const baselineMap = indexByKey(baseline, keyOf, 'baseline');
  const currentMap = indexByKey(current, keyOf, 'current');

  const allKeys = new Set<string>();
  for (const k of baselineMap.keys()) allKeys.add(k);
  for (const k of currentMap.keys()) allKeys.add(k);

  const result: DiffRow[] = [];
  for (const key of allKeys) {
    const b = baselineMap.get(key);
    const c = currentMap.get(key);

    const row: Record<string, SqlValue> = {
      [KEY_COL]: key,
      [STATUS_COL]: classify(b, c, primaryDeltaField, statusThreshold ?? 0),
    };

    for (const field of numericFields) {
      const bv = numericOrNull(b, field);
      const cv = numericOrNull(c, field);
      row[baselineCol(field)] = bv as SqlValue;
      row[currentCol(field)] = cv as SqlValue;
      row[deltaCol(field)] = delta(bv, cv) as SqlValue;
    }

    for (const field of passThroughFields) {
      row[field] = (c?.[field] ?? b?.[field] ?? null) as SqlValue;
    }

    result.push(row as DiffRow);
  }

  return result;
}

// b - a. Coerces to bigint if either side is bigint (heap sizes can
// exceed 2^53). Treats null as 0.
export function delta(a: Num | null, b: Num | null): Num {
  if (a == null && b == null) return 0;
  if (typeof a === 'bigint' || typeof b === 'bigint') {
    return toBigInt(b) - toBigInt(a);
  }
  return ((b as number) ?? 0) - ((a as number) ?? 0);
}

export function abs(v: Num): Num {
  if (typeof v === 'bigint') return v < 0n ? -v : v;
  return Math.abs(v);
}

export function compareNum(a: Num | null, b: Num | null): number {
  if (a == null && b == null) return 0;
  if (a == null) return -1;
  if (b == null) return 1;
  if (typeof a === 'bigint' || typeof b === 'bigint') {
    const ab = toBigInt(a);
    const bb = toBigInt(b);
    return ab < bb ? -1 : ab > bb ? 1 : 0;
  }
  const an = a as number;
  const bn = b as number;
  return an < bn ? -1 : an > bn ? 1 : 0;
}

function toBigInt(v: Num | null): bigint {
  if (v == null) return 0n;
  if (typeof v === 'bigint') return v;
  if (Number.isInteger(v)) return BigInt(v);
  return BigInt(Math.trunc(v));
}

function numericOrNull(row: Row | undefined, field: string): Num | null {
  if (!row) return null;
  const v = row[field];
  if (v == null) return null;
  if (typeof v === 'number' || typeof v === 'bigint') return v;
  if (typeof v === 'string') {
    const n = Number(v);
    if (!Number.isNaN(n)) return n;
  }
  return null;
}

// Sum-merges duplicate keys before `mergeRows`. Use when SQL GROUP BY
// can't fully unique-ify the join key (e.g. type_name shared across
// classloaders).
export function dedupeByKey<T extends Row>(
  rows: readonly T[],
  keyOf: (r: T) => string,
  numericFields: readonly string[],
): T[] {
  const map = new Map<string, T>();
  for (const r of rows) {
    const k = keyOf(r);
    const existing = map.get(k);
    if (existing === undefined) {
      map.set(k, {...r});
      continue;
    }
    const merged = existing as Record<string, SqlValue>;
    for (const f of numericFields) {
      const a = numericOrNull(existing, f);
      const b = numericOrNull(r, f);
      if (a == null && b == null) {
        merged[f] = null;
      } else if (typeof a === 'bigint' || typeof b === 'bigint') {
        merged[f] = (toBigInt(a) + toBigInt(b)) as SqlValue;
      } else {
        merged[f] = ((a ?? 0) as number) + ((b ?? 0) as number);
      }
    }
  }
  return Array.from(map.values());
}

function indexByKey<T extends Row>(
  rows: readonly T[],
  keyOf: (r: T) => string,
  side: string,
): Map<string, T> {
  const map = new Map<string, T>();
  for (const r of rows) {
    const k = keyOf(r);
    if (map.has(k)) {
      throw new Error(
        `mergeRows: duplicate key '${k}' on ${side} side. Both inputs must ` +
          `be pre-aggregated by the join key.`,
      );
    }
    map.set(k, r);
  }
  return map;
}

function classify(
  b: Row | undefined,
  c: Row | undefined,
  field: string,
  threshold: Num,
): DiffStatus {
  // NEW/REMOVED comes from row presence, never value==0 — a class with
  // delta=0 that exists on both sides is UNCHANGED, not NEW.
  if (b === undefined && c === undefined) return 'UNCHANGED';
  if (b === undefined) return 'NEW';
  if (c === undefined) return 'REMOVED';
  const d = delta(numericOrNull(b, field), numericOrNull(c, field));
  if (compareAbs(d, threshold) <= 0) return 'UNCHANGED';
  return compareNum(d, 0) > 0 ? 'GREW' : 'SHRANK';
}

function compareAbs(a: Num, b: Num): number {
  return compareNum(abs(a), abs(b));
}

// Sort by |Δ| desc, ties broken by key.
export function compareByAbsDeltaDesc(
  primaryDeltaField: string,
): (a: DiffRow, b: DiffRow) => number {
  const col = deltaCol(primaryDeltaField);
  return (a, b) => {
    const av = a[col] as Num | null;
    const bv = b[col] as Num | null;
    const aa = av == null ? 0n : abs(av as Num);
    const bb = bv == null ? 0n : abs(bv as Num);
    const cmp = compareNum(bb, aa);
    if (cmp !== 0) return cmp;
    return String(a[KEY_COL]).localeCompare(String(b[KEY_COL]));
  };
}
