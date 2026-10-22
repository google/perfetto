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

import {describe, expect, test} from 'vitest';
import {gopRanges} from './video_frame_player';

// Key frames are marked with 'K', other frames with '.'.
function frames(pattern: string) {
  return [...pattern].map((c) => ({isKey: c === 'K'}));
}

describe('gopRanges', () => {
  test('decodes from the preceding key frame', () => {
    expect(gopRanges(frames('K...K...'), [2, 7])).toEqual([
      [0, 2],
      [4, 7],
    ]);
  });

  test('merges overlapping and adjacent ranges', () => {
    expect(gopRanges(frames('K...K...'), [1, 3, 4, 5])).toEqual([[0, 5]]);
  });

  test('skips frames without a preceding key frame', () => {
    expect(gopRanges(frames('..K..'), [0, 1, 3])).toEqual([[2, 3]]);
  });

  test('skips out of range indices', () => {
    expect(gopRanges(frames('K.'), [-1, 1, 2])).toEqual([[0, 1]]);
  });
});
