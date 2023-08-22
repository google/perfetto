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

import {FuzzyFinder} from './fuzzy';

describe('FuzzyFinder', () => {
  const items = [
    'aaa',
    'aba',
    'zzz',
    'c z d z e',
    'Foo',
    'ababc',
  ];
  const finder = new FuzzyFinder(items, (x) => x);

  it('finds all for empty search term', () => {
    const result = finder.find('');
    // Expect all results are returned in original order.
    expect(result).toEqual([
      {item: 'aaa', segments: [{matching: false, value: 'aaa'}]},
      {item: 'aba', segments: [{matching: false, value: 'aba'}]},
      {item: 'zzz', segments: [{matching: false, value: 'zzz'}]},
      {item: 'c z d z e', segments: [{matching: false, value: 'c z d z e'}]},
      {item: 'Foo', segments: [{matching: false, value: 'Foo'}]},
      {item: 'ababc', segments: [{matching: false, value: 'ababc'}]},
    ]);
  });

  it('finds exact match', () => {
    const result = finder.find('aaa');
    expect(result).toEqual(expect.arrayContaining([
      {item: 'aaa', segments: [{matching: true, value: 'aaa'}]},
    ]));
  });

  it('finds approx matches', () => {
    const result = finder.find('aa');
    // Allow finding results in any order.
    expect(result).toEqual(expect.arrayContaining([
      {
        item: 'aaa',
        // Either |aa|a or a|aa| is valid.
        segments: expect.arrayContaining([
          {matching: true, value: 'aa'},
          {matching: false, value: 'a'},
        ]),
      },
      {
        item: 'aba',
        segments: [
          {matching: true, value: 'a'},
          {matching: false, value: 'b'},
          {matching: true, value: 'a'},
        ],
      },
    ]));
  });

  it('does not find completely unrelated items', () => {
    // |zzz| looks nothing like |aa| and should not be returned.
    const result = finder.find('aa');
    expect(result).not.toEqual(expect.arrayContaining([
      expect.objectContaining({item: 'zzz'}),
    ]));
  });

  it('finds non-consecutive matches', () => {
    const result = finder.find('cde');
    expect(result).toEqual(expect.arrayContaining([
      {
        item: 'c z d z e',
        segments: [
          {matching: true, value: 'c'},
          {matching: false, value: ' z '},
          {matching: true, value: 'd'},
          {matching: false, value: ' z '},
          {matching: true, value: 'e'},
        ],
      },
    ]));
  });

  it('finds case insensitive match', () => {
    const result = finder.find('foO');
    expect(result).toEqual(expect.arrayContaining([
      {item: 'Foo', segments: [{matching: true, value: 'Foo'}]},
    ]));
  });

  it('finds match with false start', () => {
    const result = finder.find('abc');
    expect(result).toEqual(expect.arrayContaining([
      {
        item: 'ababc',
        segments: [
          {matching: true, value: 'ab'},
          {matching: false, value: 'ab'},
          {matching: true, value: 'c'},
        ],
      },
    ]));
  });
});
