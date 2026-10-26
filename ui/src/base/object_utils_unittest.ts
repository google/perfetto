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

import {deepFreeze, shallowEquals} from './object_utils';

test('shallowEquals', () => {
  const one = 1;
  const foo = 'Foo!';
  const nestedFoo = {
    foo,
  };
  const nestedFooDupe = {
    foo,
  };

  expect(shallowEquals({}, {})).toBe(true);
  expect(shallowEquals({one}, {})).toBe(false);
  expect(shallowEquals({}, {one})).toBe(false);
  expect(shallowEquals({one}, {one})).toBe(true);
  expect(shallowEquals({nestedFoo}, {nestedFoo})).toBe(true);
  expect(shallowEquals({nestedFoo}, {nestedFooDupe})).toBe(false);
});

describe('deepFreeze', () => {
  test('returns primitives and null unchanged', () => {
    expect(deepFreeze(42)).toBe(42);
    expect(deepFreeze('hello')).toBe('hello');
    expect(deepFreeze(true)).toBe(true);
    expect(deepFreeze(null)).toBe(null);
    expect(deepFreeze(undefined)).toBe(undefined);
  });

  test('recursively freezes nested objects and arrays', () => {
    const obj = {
      a: 1,
      nested: {
        b: 'two',
        arr: [{c: 3}, 4],
      },
    };

    const frozen = deepFreeze(obj);
    expect(frozen).toBe(obj);
    expect(Object.isFrozen(frozen)).toBe(true);
    expect(Object.isFrozen(frozen.nested)).toBe(true);
    expect(Object.isFrozen(frozen.nested.arr)).toBe(true);
    expect(Object.isFrozen(frozen.nested.arr[0])).toBe(true);

    expect(() => {
      frozen.a = 2;
    }).toThrow(TypeError);
    expect(() => {
      frozen.nested.b = 'three';
    }).toThrow(TypeError);
    expect(() => {
      frozen.nested.arr.push(5);
    }).toThrow(TypeError);
    expect(() => {
      (frozen.nested.arr[0] as {c: number}).c = 99;
    }).toThrow(TypeError);
  });
});
