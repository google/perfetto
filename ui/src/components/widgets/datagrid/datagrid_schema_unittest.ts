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

import {escapePath, splitPath} from './datagrid_schema';

describe('splitPath', () => {
  test('empty string returns single empty part', () => {
    expect(splitPath('')).toStrictEqual(['']);
  });

  test('single segment with no dots', () => {
    expect(splitPath('foo')).toStrictEqual(['foo']);
  });

  test('two segments separated by dot', () => {
    expect(splitPath('parent.child')).toStrictEqual(['parent', 'child']);
  });

  test('three segments', () => {
    expect(splitPath('a.b.c')).toStrictEqual(['a', 'b', 'c']);
  });

  test('trailing dot produces empty last part', () => {
    expect(splitPath('foo.')).toStrictEqual(['foo', '']);
  });

  test('leading dot produces empty first part', () => {
    expect(splitPath('.foo')).toStrictEqual(['', 'foo']);
  });

  test('escaped period is preserved in the part', () => {
    expect(splitPath('parent.name..with..dots')).toStrictEqual([
      'parent',
      'name.with.dots',
    ]);
  });

  test('escaped period at the start', () => {
    expect(splitPath('foo..bar.baz')).toStrictEqual(['foo.bar', 'baz']);
  });

  test('multiple consecutive escaped periods', () => {
    expect(splitPath('a..b..c..d')).toStrictEqual(['a.b.c.d']);
  });

  test('mix of escaped and unescaped periods', () => {
    expect(splitPath('a.b..c.d..e.f')).toStrictEqual(['a', 'b.c', 'd.e', 'f']);
  });

  test('escaped period between unescaped separators', () => {
    expect(splitPath('x.y..z.w')).toStrictEqual(['x', 'y.z', 'w']);
  });

  test('only escaped periods', () => {
    expect(splitPath('a..b..c')).toStrictEqual(['a.b.c']);
  });

  test('odd number of dots ends the part on a literal dot', () => {
    // "foo." followed by a separator then "bar".
    expect(splitPath('foo...bar')).toStrictEqual(['foo.', 'bar']);
  });
});

describe('escapePath', () => {
  test('leaves plain names unchanged', () => {
    expect(escapePath('foo')).toBe('foo');
  });

  test('doubles a single dot', () => {
    expect(escapePath('foo.bar')).toBe('foo..bar');
  });

  test('doubles every dot', () => {
    expect(escapePath('a.b.c')).toBe('a..b..c');
  });

  test('round-trips through splitPath as a single part', () => {
    const name = 'weird.name.with.dots';
    expect(splitPath(escapePath(name))).toStrictEqual([name]);
  });
});
