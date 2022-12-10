// Copyright (C) 2022 The Android Open Source Project
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

import {assertTrue} from '../base/logging';

export type DropDirection = 'left'|'right';

export interface Interval {
  from: number;
  to: number;
}

/*
 * When a drag'n'drop is performed in a linear sequence, the resulting reordered
 * array will consist of several contiguous subarrays of the original glued
 * together.
 *
 * This function implements the computation of these intervals.
 *
 * The drag'n'drop operation performed is as follows: in the sequence with given
 * length, the element with index `dragFrom` is dropped on the `direction` to
 * the element `dragTo`.
 */
export function computeIntervals(
    length: number, dragFrom: number, dragTo: number, direction: DropDirection):
    Interval[] {
  assertTrue(dragFrom !== dragTo);

  if (dragTo < dragFrom) {
    const prefixLen = direction == 'left' ? dragTo : dragTo + 1;
    return [
      // First goes unchanged prefix.
      {from: 0, to: prefixLen},
      // Then goes dragged element.
      {from: dragFrom, to: dragFrom + 1},
      // Then goes suffix up to dragged element (which has already been moved).
      {from: prefixLen, to: dragFrom},
      // Then the rest of an array.
      {from: dragFrom + 1, to: length},
    ];
  }

  // Other case: dragTo > dragFrom
  const prefixLen = direction == 'left' ? dragTo : dragTo + 1;
  return [
    {from: 0, to: dragFrom},
    {from: dragFrom + 1, to: prefixLen},
    {from: dragFrom, to: dragFrom + 1},
    {from: prefixLen, to: length},
  ];
}

export function performReordering<T>(intervals: Interval[], arr: T[]): T[] {
  const result: T[] = [];

  for (const interval of intervals) {
    result.push(...arr.slice(interval.from, interval.to));
  }

  return result;
}
