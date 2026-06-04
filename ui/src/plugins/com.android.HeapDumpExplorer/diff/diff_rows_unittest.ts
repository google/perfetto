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

// Unit tests for the pure JS-side diff merger. No engine, no Mithril.

import type {DiffRow} from './diff_rows';
import {
  abs,
  baselineCol,
  compareByAbsDeltaDesc,
  compareNum,
  currentCol,
  delta,
  deltaCol,
  KEY_COL,
  mergeRows,
  STATUS_COL,
} from './diff_rows';

describe('delta', () => {
  it('handles two numbers', () => {
    expect(delta(5, 8)).toBe(3);
    expect(delta(8, 5)).toBe(-3);
    expect(delta(0, 0)).toBe(0);
  });

  it('treats null as 0', () => {
    expect(delta(null, 5)).toBe(5);
    expect(delta(5, null)).toBe(-5);
    expect(delta(null, null)).toBe(0);
  });

  it('promotes to bigint when either side is bigint', () => {
    expect(delta(5n, 8n)).toBe(3n);
    expect(delta(5, 8n)).toBe(3n);
    expect(delta(8n, 5)).toBe(-3n);
    expect(delta(null, 5n)).toBe(5n);
    expect(delta(5n, null)).toBe(-5n);
  });

  it('preserves precision above 2^53 for bigint', () => {
    const huge = 1n << 60n;
    expect(delta(0n, huge)).toBe(huge);
    expect(delta(huge, 2n * huge)).toBe(huge);
    // Compare against the bigint result, not number-truncated value.
    expect(delta(huge, huge + 1n)).toBe(1n);
  });
});

describe('compareNum', () => {
  it('orders numbers correctly', () => {
    expect(compareNum(1, 2)).toBe(-1);
    expect(compareNum(2, 1)).toBe(1);
    expect(compareNum(1, 1)).toBe(0);
  });

  it('orders bigints correctly', () => {
    expect(compareNum(1n, 2n)).toBe(-1);
    expect(compareNum(2n, 1n)).toBe(1);
    expect(compareNum(1n, 1n)).toBe(0);
  });

  it('mixes number and bigint without throwing', () => {
    expect(compareNum(1, 2n)).toBe(-1);
    expect(compareNum(2n, 1)).toBe(1);
  });

  it('treats null as smallest', () => {
    expect(compareNum(null, 1)).toBe(-1);
    expect(compareNum(1, null)).toBe(1);
    expect(compareNum(null, null)).toBe(0);
  });
});

describe('abs', () => {
  it('handles number', () => {
    expect(abs(5)).toBe(5);
    expect(abs(-5)).toBe(5);
    expect(abs(0)).toBe(0);
  });
  it('handles bigint', () => {
    expect(abs(5n)).toBe(5n);
    expect(abs(-5n)).toBe(5n);
    expect(abs(0n)).toBe(0n);
  });
});

describe('mergeRows', () => {
  const opts = {
    keyOf: (r: {cls: string}) => r.cls,
    numericFields: ['cnt', 'retained'],
    primaryDeltaField: 'retained',
  };

  it('produces UNCHANGED for identical rows', () => {
    const baseline = [{cls: 'Foo', cnt: 10, retained: 1000}];
    const current = [{cls: 'Foo', cnt: 10, retained: 1000}];
    const out = mergeRows({baseline, current, ...opts});
    expect(out).toHaveLength(1);
    expect(out[0][KEY_COL]).toBe('Foo');
    expect(out[0][STATUS_COL]).toBe('UNCHANGED');
    expect(out[0][deltaCol('retained')]).toBe(0);
    expect(out[0][baselineCol('retained')]).toBe(1000);
    expect(out[0][currentCol('retained')]).toBe(1000);
  });

  it('flags GREW when current > baseline', () => {
    const baseline = [{cls: 'Foo', cnt: 10, retained: 1000}];
    const current = [{cls: 'Foo', cnt: 12, retained: 1500}];
    const out = mergeRows({baseline, current, ...opts});
    expect(out[0][STATUS_COL]).toBe('GREW');
    expect(out[0][deltaCol('retained')]).toBe(500);
    expect(out[0][deltaCol('cnt')]).toBe(2);
  });

  it('flags SHRANK when current < baseline', () => {
    const baseline = [{cls: 'Foo', cnt: 10, retained: 1500}];
    const current = [{cls: 'Foo', cnt: 8, retained: 1000}];
    const out = mergeRows({baseline, current, ...opts});
    expect(out[0][STATUS_COL]).toBe('SHRANK');
    expect(out[0][deltaCol('retained')]).toBe(-500);
  });

  it('flags NEW when row only in current', () => {
    const out = mergeRows({
      baseline: [],
      current: [{cls: 'Foo', cnt: 5, retained: 500}],
      ...opts,
    });
    expect(out[0][STATUS_COL]).toBe('NEW');
    expect(out[0][baselineCol('retained')]).toBeNull();
    expect(out[0][currentCol('retained')]).toBe(500);
    expect(out[0][deltaCol('retained')]).toBe(500);
  });

  it('flags REMOVED when row only in baseline', () => {
    const out = mergeRows({
      baseline: [{cls: 'Foo', cnt: 5, retained: 500}],
      current: [],
      ...opts,
    });
    expect(out[0][STATUS_COL]).toBe('REMOVED');
    expect(out[0][baselineCol('retained')]).toBe(500);
    expect(out[0][currentCol('retained')]).toBeNull();
    expect(out[0][deltaCol('retained')]).toBe(-500);
  });

  it('classifies present-on-both rows by delta, not by zero-on-one-side', () => {
    // A class present in both snapshots with dominated_size_bytes=0 in
    // baseline (common — many classes are reachable but not dominators of
    // anything substantial) must be GREW, not NEW. NEW is reserved for
    // rows that did not exist in the baseline input at all.
    const out = mergeRows({
      baseline: [{cls: 'Foo', cnt: 5, retained: 0}],
      current: [{cls: 'Foo', cnt: 100, retained: 1000}],
      ...opts,
    });
    expect(out[0][STATUS_COL]).toBe('GREW');
  });

  it('handles empty inputs on both sides', () => {
    expect(mergeRows({baseline: [], current: [], ...opts})).toEqual([]);
  });

  it('produces NEW + REMOVED + UNCHANGED in mixed input', () => {
    const baseline = [
      {cls: 'A', cnt: 1, retained: 100},
      {cls: 'B', cnt: 2, retained: 200},
      {cls: 'C', cnt: 3, retained: 300},
    ];
    const current = [
      {cls: 'A', cnt: 1, retained: 100}, // unchanged
      {cls: 'C', cnt: 5, retained: 500}, // grew
      {cls: 'D', cnt: 1, retained: 100}, // new
      // B removed
    ];
    const out = mergeRows({baseline, current, ...opts});
    const byKey = new Map(out.map((r) => [String(r[KEY_COL]), r]));
    expect(byKey.get('A')?.[STATUS_COL]).toBe('UNCHANGED');
    expect(byKey.get('B')?.[STATUS_COL]).toBe('REMOVED');
    expect(byKey.get('C')?.[STATUS_COL]).toBe('GREW');
    expect(byKey.get('D')?.[STATUS_COL]).toBe('NEW');
  });

  it('mixes bigint and number across sides without throwing', () => {
    const baseline = [{cls: 'Foo', cnt: 10n, retained: 1000n}];
    const current = [{cls: 'Foo', cnt: 12, retained: 1500}];
    const out = mergeRows({baseline, current, ...opts});
    expect(out[0][STATUS_COL]).toBe('GREW');
    // Both sides coerced to bigint when one is.
    expect(out[0][deltaCol('retained')]).toBe(500n);
  });

  it('preserves precision above 2^53 with bigint', () => {
    const huge = 1n << 55n; // > 2^53
    const baseline = [{cls: 'Foo', cnt: 1n, retained: huge}];
    const current = [{cls: 'Foo', cnt: 1n, retained: huge + 1n}];
    const out = mergeRows({baseline, current, ...opts});
    expect(out[0][deltaCol('retained')]).toBe(1n);
  });

  it('throws on duplicate keys (defensive)', () => {
    const baseline = [
      {cls: 'Foo', cnt: 1, retained: 100},
      {cls: 'Foo', cnt: 2, retained: 200},
    ];
    expect(() => mergeRows({baseline, current: [], ...opts})).toThrow(
      /duplicate key/,
    );
  });

  it('throws if primaryDeltaField not in numericFields', () => {
    expect(() =>
      mergeRows({
        baseline: [],
        current: [],
        keyOf: (r: {cls: string}) => r.cls,
        numericFields: ['cnt'],
        primaryDeltaField: 'retained',
      }),
    ).toThrow(/primaryDeltaField/);
  });

  it('handles non-ASCII Kotlin lambda class names', () => {
    const baseline = [
      {cls: '$$Lambda$Foo$abc', cnt: 1, retained: 100},
      {cls: 'kotlin.coroutines.intrinsics. Suspended', cnt: 1, retained: 100},
    ];
    const current = [
      {cls: '$$Lambda$Foo$abc', cnt: 2, retained: 200},
      {cls: 'kotlin.coroutines.intrinsics. Suspended', cnt: 1, retained: 100},
    ];
    const out = mergeRows({baseline, current, ...opts});
    expect(out).toHaveLength(2);
    const lambda = out.find((r) => String(r[KEY_COL]).includes('Lambda'));
    expect(lambda?.[STATUS_COL]).toBe('GREW');
    expect(lambda?.[deltaCol('retained')]).toBe(100);
  });

  it('passes through extra string fields preferring current', () => {
    const baseline = [{cls: 'Foo', cnt: 1, retained: 100, label: 'old'}];
    const current = [{cls: 'Foo', cnt: 2, retained: 200, label: 'new'}];
    const out = mergeRows({
      baseline,
      current,
      ...opts,
      passThroughFields: ['label'],
    });
    expect(out[0].label).toBe('new');
  });

  it('passes through extra fields falling back to baseline', () => {
    const baseline = [{cls: 'Foo', cnt: 1, retained: 100, label: 'kept'}];
    const current: typeof baseline = [];
    const out = mergeRows({
      baseline,
      current,
      ...opts,
      passThroughFields: ['label'],
    });
    expect(out[0].label).toBe('kept');
  });
});

describe('compareByAbsDeltaDesc', () => {
  it('sorts by |delta| descending, ties broken by key', () => {
    const rows = [
      {[KEY_COL]: 'small', [deltaCol('retained')]: 100, [STATUS_COL]: 'GREW'},
      {
        [KEY_COL]: 'big-grew',
        [deltaCol('retained')]: 1000,
        [STATUS_COL]: 'GREW',
      },
      {
        [KEY_COL]: 'big-shrank',
        [deltaCol('retained')]: -1000,
        [STATUS_COL]: 'SHRANK',
      },
      {[KEY_COL]: 'zero', [deltaCol('retained')]: 0, [STATUS_COL]: 'UNCHANGED'},
    ] as DiffRow[];
    rows.sort(compareByAbsDeltaDesc('retained'));
    expect(rows.map((r) => String(r[KEY_COL]))).toEqual([
      'big-grew', // ties at |1000|; localeCompare puts 'big-grew' before 'big-shrank'
      'big-shrank',
      'small',
      'zero',
    ]);
  });

  it('handles bigint deltas', () => {
    const huge = 1n << 60n;
    const rows = [
      {[KEY_COL]: 'a', [deltaCol('retained')]: huge, [STATUS_COL]: 'GREW'},
      {[KEY_COL]: 'b', [deltaCol('retained')]: huge - 1n, [STATUS_COL]: 'GREW'},
    ] as DiffRow[];
    rows.sort(compareByAbsDeltaDesc('retained'));
    expect(String(rows[0][KEY_COL])).toBe('a');
  });
});
