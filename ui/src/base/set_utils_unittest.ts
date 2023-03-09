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

import {intersect, isSetEqual, union} from './set_utils';

// Just to make the tests easier to read:
function s<T>(xs: T[]): Set<T> {
  return new Set<T>(xs);
}

test('union', () => {
  expect(union(s([]), s([]))).toEqual(s([]));
  expect(union(s([1]), s([2]))).toEqual(s([1, 2]));
  expect(union(s([1]), s([]))).toEqual(s([1]));
  expect(union(s([]), s([2]))).toEqual(s([2]));
  expect(union(s([1]), s([1]))).toEqual(s([1]));
  expect(union(s([1, 2]), s([2, 3]))).toEqual(s([1, 2, 3]));
});

test('intersect', () => {
  expect(intersect(s([]), s([]))).toEqual(s([]));
  expect(intersect(s([1]), s([2]))).toEqual(s([]));
  expect(intersect(s([1]), s([]))).toEqual(s([]));
  expect(intersect(s([]), s([2]))).toEqual(s([]));
  expect(intersect(s([1]), s([1]))).toEqual(s([1]));
  expect(intersect(s([1, 2]), s([2, 3]))).toEqual(s([2]));
});

test('isSetEqual', () => {
  expect(isSetEqual(s([]), s([]))).toEqual(true);
  expect(isSetEqual(s([1]), s([2]))).toEqual(false);
  expect(isSetEqual(s([1]), s([]))).toEqual(false);
  expect(isSetEqual(s([]), s([2]))).toEqual(false);
  expect(isSetEqual(s([1]), s([1]))).toEqual(true);
  expect(isSetEqual(s([1, 2]), s([2, 3]))).toEqual(false);
});
