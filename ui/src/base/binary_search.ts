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


function searchImpl(
    haystack: Float64Array, needle: number, i: number, j: number): number {
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

export function search(haystack: Float64Array, needle: number): number {
  return searchImpl(haystack, needle, 0, haystack.length);
}


export function searchSegment(
    haystack: Float64Array, needle: number): [number, number] {
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
