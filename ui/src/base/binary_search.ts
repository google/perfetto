// Copyright (C) 2018 The Android Open Source Project
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

type Numeric = number|bigint;
type Range = [number, number];

function searchImpl<T extends Numeric>(
    haystack: ArrayLike<T>, needle: T, i: number, j: number): number {
  if (i === j) return -1;
  if (i + 1 === j) {
    return (needle >= haystack[i]) ? i : -1;
  }

  const mid = Math.floor((j - i) / 2) + i;
  const midValue = haystack[mid];
  if (needle < midValue) {
    return searchImpl(haystack, needle, i, mid);
  } else {
    return searchImpl(haystack, needle, mid, j);
  }
}

function searchRangeImpl<T extends Numeric>(
    haystack: ArrayLike<T>, needle: T, i: number, j: number): Range {
  if (i === j) return [i, j];
  if (i + 1 === j) {
    if (haystack[i] <= needle) {
      return [i, j];
    } else {
      return [i, i];
    }
  }

  const mid = Math.floor((j - i) / 2) + i;
  const midValue = haystack[mid];

  if (needle < midValue) {
    return searchRangeImpl(haystack, needle, i, mid);
  } else if (needle > midValue) {
    return searchRangeImpl(haystack, needle, mid, j);
  } else {
    while (haystack[i] !== needle) i++;
    while (haystack[j - 1] !== needle) j--;
    return [i, j];
  }
}

// Given a sorted array of numeric values |haystack| and a |needle|, return the
// index of the last element of |haystack| which is less than or equal to
// |needle|, or -1 if all elements of |haystack| are greater than |needle|.
export function search<T extends Numeric>(
    haystack: ArrayLike<T>, needle: T): number {
  return searchImpl(haystack, needle, 0, haystack.length);
}

// Given a sorted array of numeric values (|haystack|) return the half open
// range [i, j) of indices where |haystack| is equal to needle.
export function searchEq<T extends Numeric>(
    haystack: ArrayLike<T>, needle: T, optRange?: Range): Range {
  const range = searchRange(haystack, needle, optRange);
  const [i, j] = range;
  if (haystack[i] === needle) return range;
  return [j, j];
}

// Given a sorted array of numeric values (|haystack|) and a |needle| return the
// smallest half open range [i, j) of indexes which contains |needle|.
export function searchRange<T extends Numeric>(
    haystack: ArrayLike<T>, needle: T, optRange?: Range): Range {
  const [left, right] = optRange ? optRange : [0, haystack.length];
  return searchRangeImpl(haystack, needle, left, right);
}

// Given a sorted array of numeric values (|haystack|) and a |needle| return a
// pair of indexes [i, j] such that:
// If there is at least one element in |haystack| smaller than |needle|
// i is the index of the largest such number otherwise -1;
// If there is at least one element in |haystack| larger than |needle|
// j is the index of the smallest such element otherwise -1.
//
// So we try to get the indexes of the two data points around needle
// or -1 if there is no such datapoint.
export function searchSegment<T extends Numeric>(
    haystack: ArrayLike<T>, needle: T): Range {
  if (!haystack.length) return [-1, -1];

  const left = search(haystack, needle);
  if (left === -1) {
    return [left, 0];
  } else if (left + 1 === haystack.length) {
    return [left, -1];
  } else {
    return [left, left + 1];
  }
}
