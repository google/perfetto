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

import {Memo} from './memo';

test('returns computed value on first call', () => {
  const memo = new Memo<number>();
  const compute = vi.fn(() => 42);

  const result = memo.use({key: {id: 1}, compute});

  expect(result).toBe(42);
  expect(compute).toHaveBeenCalledTimes(1);
});

test('caches result for same key', () => {
  const memo = new Memo<string>();
  const compute = vi.fn(() => 'hello');

  memo.use({key: {id: 1}, compute});
  memo.use({key: {id: 1}, compute});
  memo.use({key: {id: 1}, compute});

  expect(compute).toHaveBeenCalledTimes(1);
});

test('recomputes when key changes', () => {
  const memo = new Memo<string>();
  let count = 0;
  const compute = vi.fn(() => `v${++count}`);

  expect(memo.use({key: {id: 1}, compute})).toBe('v1');
  expect(memo.use({key: {id: 2}, compute})).toBe('v2');
  expect(memo.use({key: {id: 3}, compute})).toBe('v3');

  expect(compute).toHaveBeenCalledTimes(3);
});

test('different object references with same values are treated as equal', () => {
  const memo = new Memo<number>();
  const compute = vi.fn(() => 42);

  memo.use({key: {a: 1, b: 'x'}, compute});
  memo.use({key: {a: 1, b: 'x'}, compute});

  expect(compute).toHaveBeenCalledTimes(1);
});

test('supports nested objects and arrays in key', () => {
  const memo = new Memo<number>();
  const compute = vi.fn(() => 1);

  const key = {
    filters: {name: 'foo', values: [1, 2, 3]},
    nested: {deep: {value: true}},
  };

  memo.use({key, compute});
  memo.use({key: {...key}, compute});
  memo.use({key: structuredClone(key), compute});

  expect(compute).toHaveBeenCalledTimes(1);
});

test('supports bigints in key', () => {
  const memo = new Memo<string>();
  const compute = vi.fn(() => 'ok');

  memo.use({key: {ts: 123n, id: 1}, compute});
  memo.use({key: {ts: 123n, id: 1}, compute});

  expect(compute).toHaveBeenCalledTimes(1);

  // Different bigint triggers recompute
  memo.use({key: {ts: 456n, id: 1}, compute});
  expect(compute).toHaveBeenCalledTimes(2);
});

test('invalidate clears the cache', () => {
  const memo = new Memo<number>();
  let count = 0;
  const compute = vi.fn(() => ++count);

  expect(memo.use({key: {id: 1}, compute})).toBe(1);
  memo.invalidate();
  expect(memo.use({key: {id: 1}, compute})).toBe(2);

  expect(compute).toHaveBeenCalledTimes(2);
});

test('returns undefined when compute returns undefined', () => {
  const memo = new Memo<string | undefined>();
  const compute = vi.fn(() => undefined);

  const result = memo.use({key: {id: 1}, compute});

  expect(result).toBeUndefined();
  expect(compute).toHaveBeenCalledTimes(1);

  // Should still cache the undefined value
  memo.use({key: {id: 1}, compute});
  expect(compute).toHaveBeenCalledTimes(1);
});

test('uses latest compute function on key change', () => {
  const memo = new Memo<number>();

  let multiplier = 1;
  expect(
    memo.use({
      key: {id: 1},
      compute: () => 10 * multiplier,
    }),
  ).toBe(10);

  multiplier = 2;
  expect(
    memo.use({
      key: {id: 2},
      compute: () => 10 * multiplier,
    }),
  ).toBe(20);
});

test('primitive keys work', () => {
  const memo = new Memo<number>();
  const compute = vi.fn(() => 42);

  memo.use({key: 'simple', compute});
  memo.use({key: 'simple', compute});
  memo.use({key: 'different', compute});

  expect(compute).toHaveBeenCalledTimes(2);
});

test('array keys work', () => {
  const memo = new Memo<number>();
  const compute = vi.fn(() => 42);

  memo.use({key: [1, 2, 3], compute});
  memo.use({key: [1, 2, 3], compute});
  memo.use({key: [1, 2, 4], compute});

  expect(compute).toHaveBeenCalledTimes(2);
});

test('null and undefined key fields are distinguished', () => {
  const memo = new Memo<number>();
  const compute = vi.fn(() => 42);

  memo.use({key: {val: null}, compute});
  memo.use({key: {val: null}, compute});
  memo.use({key: {val: undefined}, compute});

  expect(compute).toHaveBeenCalledTimes(2);
});

test('undefined key does not crash', () => {
  const memo = new Memo();
  memo.use({
    key: undefined,
    compute: () => {},
  });
});

test('null key does not crash', () => {
  const memo = new Memo();
  memo.use({
    key: null,
    compute: () => {},
  });
});

test('disposes cached value when key changes', () => {
  const memo = new Memo<Disposable>();
  const firstDispose = vi.fn();
  const secondDispose = vi.fn();
  const first = {[Symbol.dispose]: firstDispose};
  const second = {[Symbol.dispose]: secondDispose};

  memo.use({key: 1, compute: () => first});
  memo.use({key: 1, compute: () => first});
  expect(firstDispose).not.toHaveBeenCalled();

  memo.use({key: 2, compute: () => second});
  expect(firstDispose).toHaveBeenCalledOnce();
  expect(secondDispose).not.toHaveBeenCalled();
});

test('invalidate disposes the cached value', () => {
  const memo = new Memo<Disposable>();
  const dispose = vi.fn();

  memo.use({key: 1, compute: () => ({[Symbol.dispose]: dispose})});
  memo.invalidate();
  memo.invalidate();

  expect(dispose).toHaveBeenCalledOnce();
});

test('dispose clears the cache and allows reuse', () => {
  const memo = new Memo<Disposable>();
  const firstDispose = vi.fn();
  const second = {[Symbol.dispose]: vi.fn()};

  memo.use({key: 1, compute: () => ({[Symbol.dispose]: firstDispose})});
  memo.dispose();
  memo.dispose();

  expect(firstDispose).toHaveBeenCalledOnce();
  expect(memo.use({key: 1, compute: () => second})).toBe(second);
});
