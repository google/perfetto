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

import {flattenArgs} from './metatracing';

describe('flattenArgs', () => {
  test('flattens nested object', () => {
    const value = {
      foo: {
        bar: [1, 2, 3],
      },
      baz: {baz: 'qux'},
    };
    expect(flattenArgs(value)).toStrictEqual({
      'foo.bar[0]': '1',
      'foo.bar[1]': '2',
      'foo.bar[2]': '3',
      'baz.baz': 'qux',
    });
  });

  test('flattens single value', () => {
    const value = 123;
    expect(flattenArgs(value)).toStrictEqual({
      '': '123',
    });
  });

  test('flattens array', () => {
    const value = [1, 2, 3];
    expect(flattenArgs(value)).toStrictEqual({
      '[0]': '1',
      '[1]': '2',
      '[2]': '3',
    });
  });

  test('flattens array of objects', () => {
    const value = [{foo: 'bar'}, {baz: 123}];
    expect(flattenArgs(value)).toStrictEqual({
      '[0].foo': 'bar',
      '[1].baz': '123',
    });
  });
});
