// Copyright (C) 2023 The Android Open Source Project
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

import {
  allUnique,
  arrayEquals,
  removeFalsyValues,
  range,
  moveArrayItem,
  min,
  max,
} from './array_utils';

describe('range', () => {
  it('returns array of elements in range [0; n)', () => {
    expect(range(3)).toEqual([0, 1, 2]);
    expect(range(5)).toEqual([0, 1, 2, 3, 4]);
  });

  it('returns empty array on n = 0', () => {
    expect(range(0)).toEqual([]);
  });

  it('throws an error on negative input', () => {
    expect(() => {
      range(-10);
    }).toThrowError();
  });
});

describe('allUnique', () => {
  it('returns true on array with unique elements', () => {
    expect(allUnique(['a', 'b', 'c'])).toBeTruthy();
  });

  it('returns false on array with repeated elements', () => {
    expect(allUnique(['a', 'a', 'b'])).toBeFalsy();
  });

  // Couple of corner cases
  it('returns true on an empty array', () => {
    expect(allUnique([])).toBeTruthy();
  });

  it('returns true on an array with one element', () => {
    expect(allUnique(['test'])).toBeTruthy();
  });
});

describe('arrayEquals', () => {
  it('returns true when two arrays are the same', () => {
    expect(arrayEquals(['a', 'b', 'c'], ['a', 'b', 'c'])).toBeTruthy();
  });

  it('returns false when two arrays differ', () => {
    expect(arrayEquals(['a', 'b', 'c'], ['a', 'c', 'b'])).toBeFalsy();
  });

  it('returns false when arrays have differing lengths', () => {
    expect(arrayEquals(['a', 'b', 'c'], ['a'])).toBeFalsy();
  });
});

test('removeFalsyValues', () => {
  const input = [
    'a',
    false,
    undefined,
    null,
    '',
    123,
    123n,
    true,
    {foo: 'bar'},
  ];
  const expected = ['a', 123, 123n, true, {foo: 'bar'}];
  expect(removeFalsyValues(input)).toEqual(expected);
});

test('moveArrayItem.moveForward', () => {
  const input = range(3);
  moveArrayItem(input, 0, 2);
  expect(input).toEqual([1, 0, 2]);
});

test('moveArrayItem.moveBackward', () => {
  const input = range(3);
  moveArrayItem(input, 2, 0);
  expect(input).toEqual([2, 0, 1]);
});

describe('min', () => {
  it('returns the smallest element', () => {
    expect(min([3, 1, 4, 1, 5, 9, 2, 6])).toBe(1);
  });

  it('handles negative numbers', () => {
    expect(min([-3, -1, -4, -1])).toBe(-4);
  });

  it('returns the only element for a singleton array', () => {
    expect(min([42])).toBe(42);
  });

  it('returns undefined for an empty array', () => {
    expect(min([])).toBeUndefined();
  });

  it('returns NaN if any element is NaN', () => {
    expect(min([1, NaN, 3])).toBeNaN();
    expect(min([NaN])).toBeNaN();
  });

  it('handles Infinity', () => {
    expect(min([Infinity, 1, 2])).toBe(1);
    expect(min([-Infinity, 1, 2])).toBe(-Infinity);
  });
});

describe('max', () => {
  it('returns the largest element', () => {
    expect(max([3, 1, 4, 1, 5, 9, 2, 6])).toBe(9);
  });

  it('handles negative numbers', () => {
    expect(max([-3, -1, -4, -1])).toBe(-1);
  });

  it('returns the only element for a singleton array', () => {
    expect(max([42])).toBe(42);
  });

  it('returns undefined for an empty array', () => {
    expect(max([])).toBeUndefined();
  });

  it('returns NaN if any element is NaN', () => {
    expect(max([1, NaN, 3])).toBeNaN();
    expect(max([NaN])).toBeNaN();
  });

  it('handles Infinity', () => {
    expect(max([Infinity, 1, 2])).toBe(Infinity);
    expect(max([-Infinity, 1, 2])).toBe(2);
  });
});
