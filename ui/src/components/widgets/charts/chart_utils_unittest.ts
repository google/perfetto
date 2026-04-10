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

import {percentile} from './chart_utils';

describe('percentile', () => {
  test('empty array returns NaN', () => {
    expect(percentile([], 50)).toBeNaN();
  });

  test('single element returns that element', () => {
    expect(percentile([42], 50)).toBe(42);
    expect(percentile([42], 0)).toBe(42);
    expect(percentile([42], 100)).toBe(42);
  });

  test('P0 returns minimum', () => {
    expect(percentile([10, 20, 30, 40, 50], 0)).toBe(10);
  });

  test('P100 returns maximum', () => {
    expect(percentile([10, 20, 30, 40, 50], 100)).toBe(50);
  });

  test('P50 of even count interpolates', () => {
    expect(percentile([10, 20], 50)).toBe(15);
  });

  test('P50 of odd count returns middle', () => {
    expect(percentile([10, 20, 30], 50)).toBe(20);
  });

  test('P25 interpolation', () => {
    expect(percentile([1, 2, 3, 4, 5], 25)).toBe(2);
  });

  test('P75 interpolation', () => {
    expect(percentile([1, 2, 3, 4, 5], 75)).toBe(4);
  });

  test('unsorted input is handled correctly', () => {
    expect(percentile([50, 10, 30, 20, 40], 50)).toBe(30);
  });

  test('does not mutate input array', () => {
    const input = [5, 3, 1, 4, 2];
    const copy = [...input];
    percentile(input, 50);
    expect(input).toEqual(copy);
  });
});
