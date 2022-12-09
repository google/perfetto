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

import {
  computeIntervals,
  performReordering,
} from './dragndrop_logic';

describe('performReordering', () => {
  test('has the same elements in the result', () => {
    const arr = [1, 2, 3, 4, 5, 6];
    const arrSet = new Set(arr);

    for (let i = 0; i < arr.length; i++) {
      for (let j = 0; j < arr.length; j++) {
        if (i === j) {
          // The function has a precondition that two indices have to be
          // different.
          continue;
        }

        const permutedLeft =
            performReordering(computeIntervals(arr.length, i, j, 'left'), arr);
        expect(new Set(permutedLeft)).toEqual(arrSet);
        expect(permutedLeft.length).toEqual(arr.length);

        const permutedRight =
            performReordering(computeIntervals(arr.length, i, j, 'right'), arr);
        expect(new Set(permutedRight)).toEqual(arrSet);
        expect(permutedRight.length).toEqual(arr.length);
      }
    }
  });
});
