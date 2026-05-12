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

// Per-instance pairing for the Objects diff. Within a single call this is
// AHAT's Diff.java kernel (heapdump/Diff.java): partition both sides by an
// equivalence-class Key (class + heap_type + value_string + array_length),
// sort each bucket by retained size desc, then zip positionally. Leftovers
// become NEW / REMOVED. heap_graph_object id is parser-internal — never
// stable across traces — so we cannot use it as cross-snapshot identity.
//
// AHAT additionally walks the dominator tree top-down, applying this
// kernel at each level so siblings only match if their dominators
// matched. We approximate that recursively by user navigation: callers
// invoke pairObjects once per scope (per-class for the Objects tab,
// per-parent for "Immediately Dominated Objects") and the drill-down
// chain reproduces AHAT's level-by-level walk on demand.

import type {DiffStatus} from './diff_rows';

export interface ObjectRowRaw {
  readonly id: number;
  readonly className: string;
  readonly heapType: string | null;
  readonly valueString: string | null;
  readonly arrayLength: number | null;
  readonly shallow: number;
  readonly shallowNative: number;
  readonly retained: number;
  readonly retainedNative: number;
  readonly retainedCount: number;
}

export interface ObjectPairRow {
  readonly key: string;
  readonly status: DiffStatus;
  readonly className: string;
  readonly heapType: string | null;
  readonly valueString: string | null;
  readonly c_id: number | null;
  readonly b_id: number | null;
  readonly c_shallow: number | null;
  readonly b_shallow: number | null;
  readonly c_shallow_native: number | null;
  readonly b_shallow_native: number | null;
  readonly c_retained: number | null;
  readonly b_retained: number | null;
  readonly c_retained_native: number | null;
  readonly b_retained_native: number | null;
  readonly c_retained_count: number | null;
  readonly b_retained_count: number | null;
  readonly delta_retained: number;
  readonly delta_shallow: number;
}

function bucketKey(r: ObjectRowRaw): string {
  // \x1f as field separator avoids collisions when any field contains the
  // empty string, '|', or other characters a user-controlled value_string
  // could include.
  return [
    r.className,
    r.heapType ?? '',
    r.valueString ?? '',
    r.arrayLength ?? '',
  ].join('\x1f');
}

function compareByRetainedDesc(a: ObjectRowRaw, b: ObjectRowRaw): number {
  // Tie-break by id for deterministic order within an engine. Cross-trace
  // ties remain inherently unstable (different parser ids).
  if (b.retained !== a.retained) return b.retained - a.retained;
  return a.id - b.id;
}

function classifyDelta(c: number, b: number): DiffStatus {
  if (c > b) return 'GREW';
  if (c < b) return 'SHRANK';
  return 'UNCHANGED';
}

/**
 * Pair instances from `current` and `baseline`. Within each Key bucket,
 * sort both sides by retained-size desc and zip positionally. Returns a
 * flat list of pair rows — one per pair, plus one per leftover.
 */
export function pairObjects(
  current: ReadonlyArray<ObjectRowRaw>,
  baseline: ReadonlyArray<ObjectRowRaw>,
): ObjectPairRow[] {
  const cBuckets = new Map<string, ObjectRowRaw[]>();
  const bBuckets = new Map<string, ObjectRowRaw[]>();
  for (const r of current) {
    const k = bucketKey(r);
    let bucket = cBuckets.get(k);
    if (!bucket) {
      bucket = [];
      cBuckets.set(k, bucket);
    }
    bucket.push(r);
  }
  for (const r of baseline) {
    const k = bucketKey(r);
    let bucket = bBuckets.get(k);
    if (!bucket) {
      bucket = [];
      bBuckets.set(k, bucket);
    }
    bucket.push(r);
  }
  const allKeys = new Set<string>();
  for (const k of cBuckets.keys()) allKeys.add(k);
  for (const k of bBuckets.keys()) allKeys.add(k);

  const out: ObjectPairRow[] = [];
  for (const k of allKeys) {
    const cs = cBuckets.get(k) ?? [];
    const bs = bBuckets.get(k) ?? [];
    cs.sort(compareByRetainedDesc);
    bs.sort(compareByRetainedDesc);
    const common = Math.min(cs.length, bs.length);
    for (let i = 0; i < common; i++) {
      const c = cs[i];
      const b = bs[i];
      out.push({
        key: `${k}\x1f${i}`,
        status: classifyDelta(c.retained, b.retained),
        className: c.className,
        heapType: c.heapType,
        valueString: c.valueString,
        c_id: c.id,
        b_id: b.id,
        c_shallow: c.shallow,
        b_shallow: b.shallow,
        c_shallow_native: c.shallowNative,
        b_shallow_native: b.shallowNative,
        c_retained: c.retained,
        b_retained: b.retained,
        c_retained_native: c.retainedNative,
        b_retained_native: b.retainedNative,
        c_retained_count: c.retainedCount,
        b_retained_count: b.retainedCount,
        delta_retained: c.retained - b.retained,
        delta_shallow: c.shallow - b.shallow,
      });
    }
    for (let i = common; i < cs.length; i++) {
      const c = cs[i];
      out.push({
        key: `${k}\x1f${i}`,
        status: 'NEW',
        className: c.className,
        heapType: c.heapType,
        valueString: c.valueString,
        c_id: c.id,
        b_id: null,
        c_shallow: c.shallow,
        b_shallow: null,
        c_shallow_native: c.shallowNative,
        b_shallow_native: null,
        c_retained: c.retained,
        b_retained: null,
        c_retained_native: c.retainedNative,
        b_retained_native: null,
        c_retained_count: c.retainedCount,
        b_retained_count: null,
        delta_retained: c.retained,
        delta_shallow: c.shallow,
      });
    }
    for (let i = common; i < bs.length; i++) {
      const b = bs[i];
      out.push({
        key: `${k}\x1f${i}`,
        status: 'REMOVED',
        className: b.className,
        heapType: b.heapType,
        valueString: b.valueString,
        c_id: null,
        b_id: b.id,
        c_shallow: null,
        b_shallow: b.shallow,
        c_shallow_native: null,
        b_shallow_native: b.shallowNative,
        c_retained: null,
        b_retained: b.retained,
        c_retained_native: null,
        b_retained_native: b.retainedNative,
        c_retained_count: null,
        b_retained_count: b.retainedCount,
        delta_retained: -b.retained,
        delta_shallow: -b.shallow,
      });
    }
  }
  return out;
}
