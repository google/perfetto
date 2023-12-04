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

import {lookupPath, shallowEquals} from './object_utils';

test('lookupPath', () => {
  const nested = {baz: 'qux'};
  const value = {
    foo: {
      bar: [1, 2, 3],
    },
    baz: nested,
  };
  expect(lookupPath(value, ['foo', 'bar', 1])).toBe(2);
  expect(lookupPath(value, ['foo', 'bar', 2])).toBe(3);
  expect(lookupPath(value, ['foo', 'bar'])).toStrictEqual([1, 2, 3]);
  expect(lookupPath(value, ['foo'])).toStrictEqual({bar: [1, 2, 3]});
  expect(lookupPath(value, [])).toBe(value);
  expect(lookupPath(value, ['baz'])).toBe(nested);
});

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
