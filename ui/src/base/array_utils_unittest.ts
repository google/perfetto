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

import {allUnique, range} from './array_utils';

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
