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

import {pairObjects, type ObjectRowRaw} from './object_pairing';

function row(
  opts: Partial<ObjectRowRaw> & Pick<ObjectRowRaw, 'id'>,
): ObjectRowRaw {
  return {
    className: 'C',
    heapType: 'app',
    valueString: null,
    arrayLength: null,
    shallow: 16,
    shallowNative: 0,
    retained: 16,
    retainedNative: 0,
    retainedCount: 1,
    ...opts,
  };
}

describe('pairObjects', () => {
  test('empty inputs → no rows', () => {
    expect(pairObjects([], [])).toEqual([]);
  });

  test('paired in one bucket: equal retained → UNCHANGED', () => {
    const out = pairObjects(
      [row({id: 1, retained: 100})],
      [row({id: 2, retained: 100})],
    );
    expect(out).toHaveLength(1);
    expect(out[0].status).toBe('UNCHANGED');
    expect(out[0].c_id).toBe(1);
    expect(out[0].b_id).toBe(2);
    expect(out[0].delta_retained).toBe(0);
  });

  test('paired in one bucket: current > baseline → GREW', () => {
    const out = pairObjects(
      [row({id: 1, retained: 200})],
      [row({id: 2, retained: 100})],
    );
    expect(out[0].status).toBe('GREW');
    expect(out[0].delta_retained).toBe(100);
  });

  test('paired in one bucket: current < baseline → SHRANK', () => {
    const out = pairObjects(
      [row({id: 1, retained: 80})],
      [row({id: 2, retained: 100})],
    );
    expect(out[0].status).toBe('SHRANK');
    expect(out[0].delta_retained).toBe(-20);
  });

  test('excess on current side → NEW', () => {
    const out = pairObjects(
      [row({id: 1, retained: 100}), row({id: 2, retained: 50})],
      [row({id: 9, retained: 100})],
    );
    expect(out).toHaveLength(2);
    const news = out.filter((r) => r.status === 'NEW');
    expect(news).toHaveLength(1);
    expect(news[0].c_id).toBe(2);
    expect(news[0].b_id).toBeNull();
    expect(news[0].delta_retained).toBe(50);
  });

  test('excess on baseline side → REMOVED', () => {
    const out = pairObjects(
      [row({id: 1, retained: 100})],
      [row({id: 8, retained: 100}), row({id: 9, retained: 50})],
    );
    expect(out).toHaveLength(2);
    const gone = out.filter((r) => r.status === 'REMOVED');
    expect(gone).toHaveLength(1);
    expect(gone[0].c_id).toBeNull();
    expect(gone[0].b_id).toBe(9);
    expect(gone[0].delta_retained).toBe(-50);
  });

  test('different className → distinct buckets, no pairing', () => {
    const out = pairObjects(
      [row({id: 1, className: 'Foo', retained: 100})],
      [row({id: 2, className: 'Bar', retained: 100})],
    );
    expect(out).toHaveLength(2);
    expect(out.map((r) => r.status).sort()).toEqual(['NEW', 'REMOVED']);
  });

  test('different heapType → distinct buckets', () => {
    const out = pairObjects(
      [row({id: 1, heapType: 'app', retained: 100})],
      [row({id: 2, heapType: 'zygote', retained: 100})],
    );
    expect(out).toHaveLength(2);
    expect(out.map((r) => r.status).sort()).toEqual(['NEW', 'REMOVED']);
  });

  test('different valueString → distinct buckets', () => {
    const out = pairObjects(
      [row({id: 1, valueString: 'hello', retained: 40})],
      [row({id: 2, valueString: 'world', retained: 40})],
    );
    expect(out.map((r) => r.status).sort()).toEqual(['NEW', 'REMOVED']);
  });

  test('different arrayLength → distinct buckets', () => {
    const out = pairObjects(
      [row({id: 1, arrayLength: 4, retained: 40})],
      [row({id: 2, arrayLength: 8, retained: 40})],
    );
    expect(out.map((r) => r.status).sort()).toEqual(['NEW', 'REMOVED']);
  });

  test('zip is by retained-desc within a bucket', () => {
    // Three current, two baseline. Sort desc on each side, zip positions
    // 0,1; current[2] is the leftover NEW.
    const out = pairObjects(
      [
        row({id: 1, retained: 50}),
        row({id: 2, retained: 200}),
        row({id: 3, retained: 100}),
      ],
      [row({id: 8, retained: 80}), row({id: 9, retained: 150})],
    );
    expect(out).toHaveLength(3);
    const byPair = (cId: number) => out.find((r) => r.c_id === cId);
    // Position 0: current=200, baseline=150 → paired, GREW
    expect(byPair(2)?.status).toBe('GREW');
    expect(byPair(2)?.b_id).toBe(9);
    // Position 1: current=100, baseline=80 → paired, GREW
    expect(byPair(3)?.status).toBe('GREW');
    expect(byPair(3)?.b_id).toBe(8);
    // Leftover current at pos 2: id=1, retained=50 → NEW
    expect(byPair(1)?.status).toBe('NEW');
    expect(byPair(1)?.b_id).toBeNull();
  });

  test('tie on retained → deterministic by id', () => {
    // Two current with the same retained, two baseline same retained.
    // Both sides sort by id ascending as tiebreaker → consistent pairing
    // across runs.
    const out1 = pairObjects(
      [row({id: 5, retained: 100}), row({id: 1, retained: 100})],
      [row({id: 9, retained: 100}), row({id: 2, retained: 100})],
    );
    const out2 = pairObjects(
      [row({id: 1, retained: 100}), row({id: 5, retained: 100})],
      [row({id: 2, retained: 100}), row({id: 9, retained: 100})],
    );
    const sig = (rs: ReturnType<typeof pairObjects>) =>
      rs
        .map((r) => `${r.c_id}/${r.b_id}`)
        .sort()
        .join(',');
    expect(sig(out1)).toBe(sig(out2));
    expect(sig(out1)).toBe('1/2,5/9');
  });

  test('null heapType matches null heapType', () => {
    const out = pairObjects(
      [row({id: 1, heapType: null, retained: 50})],
      [row({id: 2, heapType: null, retained: 80})],
    );
    expect(out).toHaveLength(1);
    expect(out[0].status).toBe('SHRANK');
  });

  test('valueString containing pipe does not collide with empty bucket', () => {
    // Regression check: the bucket separator must be unambiguous so a
    // user-supplied string can never look like a different bucket.
    const out = pairObjects(
      [row({id: 1, valueString: 'a|b', retained: 10})],
      [row({id: 2, valueString: 'a', retained: 10, arrayLength: null})],
    );
    expect(out).toHaveLength(2);
    expect(out.map((r) => r.status).sort()).toEqual(['NEW', 'REMOVED']);
  });

  test('all-zero deltas after pairing → all UNCHANGED', () => {
    const out = pairObjects(
      [row({id: 1}), row({id: 2})],
      [row({id: 8}), row({id: 9})],
    );
    expect(out.every((r) => r.status === 'UNCHANGED')).toBe(true);
  });
});
