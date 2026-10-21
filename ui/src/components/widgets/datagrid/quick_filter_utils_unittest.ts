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

import {
  isQuickFilterActive,
  valueMatchesQuickFilter,
} from './quick_filter_utils';

describe('valueMatchesQuickFilter', () => {
  test('substring match', () => {
    expect(valueMatchesQuickFilter('foobar', 'oba')).toBe(true);
    expect(valueMatchesQuickFilter('foobar', 'baz')).toBe(false);
  });

  test('ignores A-Z case', () => {
    expect(valueMatchesQuickFilter('FooBar', 'foob')).toBe(true);
    expect(valueMatchesQuickFilter('foobar', 'FOOB')).toBe(true);
  });

  test('does not ignore case for non-ASCII letters', () => {
    expect(valueMatchesQuickFilter('é', 'é')).toBe(true);
    expect(valueMatchesQuickFilter('É', 'é')).toBe(false);
  });

  test('numbers and bigints match their text form', () => {
    expect(valueMatchesQuickFilter(1258291, '1258')).toBe(true);
    expect(valueMatchesQuickFilter(1.5, '1.5')).toBe(true);
    expect(valueMatchesQuickFilter(123456789012345678n, '9012')).toBe(true);
    expect(valueMatchesQuickFilter(42, '43')).toBe(false);
  });

  test('null and blobs never match', () => {
    expect(valueMatchesQuickFilter(null, 'null')).toBe(false);
    expect(valueMatchesQuickFilter(new Uint8Array([49]), '1')).toBe(false);
  });

  test('empty text matches nothing', () => {
    expect(valueMatchesQuickFilter('foo', '')).toBe(false);
  });

  test('special characters are matched literally', () => {
    expect(valueMatchesQuickFilter('a*b', '*')).toBe(true);
    expect(valueMatchesQuickFilter('ab', '*')).toBe(false);
    expect(valueMatchesQuickFilter('a[b', '[')).toBe(true);
    expect(valueMatchesQuickFilter("it's", "'")).toBe(true);
    expect(valueMatchesQuickFilter('a?b', '?')).toBe(true);
    expect(valueMatchesQuickFilter('ab', '?')).toBe(false);
  });
});

describe('isQuickFilterActive', () => {
  test('inactive when undefined or empty', () => {
    expect(isQuickFilterActive(undefined)).toBe(false);
    expect(isQuickFilterActive({text: '', fields: ['a']})).toBe(false);
  });

  test('active with text', () => {
    expect(isQuickFilterActive({text: 'x', fields: []})).toBe(true);
  });
});
