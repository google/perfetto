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
import {thumbnailIndices, tileLayout} from './video_minimap';

// A stream of `n` frames with a key frame every `gop` frames from `offset`.
function stream(n: number, gop: number, offset = 0) {
  return Array.from({length: n}, (_, i) => ({
    isKey: i >= offset && (i - offset) % gop === 0,
  }));
}

describe('thumbnailIndices', () => {
  test('keeps every decodable frame of short streams', () => {
    expect(thumbnailIndices(stream(5, 60))).toEqual([0, 1, 2, 3, 4]);
  });

  test('skips frames before the first key frame', () => {
    expect(thumbnailIndices(stream(6, 60, 3))).toEqual([3, 4, 5]);
  });

  test('returns nothing without key frames', () => {
    expect(thumbnailIndices([{isKey: false}, {isKey: false}])).toEqual([]);
  });

  test('uses key frames on long streams with frequent key frames', () => {
    const frames = stream(23040, 48);
    const indices = thumbnailIndices(frames);
    expect(indices.length).toBe(256);
    expect(indices.slice(0, -1).every((i) => frames[i]!.isKey)).toBe(true);
  });

  test('ends with the last frame', () => {
    for (const frames of [stream(23040, 48), stream(10000, 5000)]) {
      const indices = thumbnailIndices(frames);
      expect(indices[indices.length - 1]).toBe(frames.length - 1);
    }
  });

  test('falls back to evenly spaced frames when key frames are sparse', () => {
    const frames = stream(10000, 5000);
    const indices = thumbnailIndices(frames);
    expect(indices.length).toBe(256);
    expect(indices[0]).toBe(0);
    expect(indices.filter((i) => frames[i]!.isKey)).toEqual([0, 5000]);
  });

  test('returns strictly increasing indices', () => {
    for (const frames of [
      stream(23040, 48),
      stream(1000, 7, 3),
      stream(300, 1),
    ]) {
      const indices = thumbnailIndices(frames);
      for (let i = 1; i < indices.length; i++) {
        expect(indices[i]).toBeGreaterThan(indices[i - 1]!);
      }
    }
  });
});

describe('tileLayout', () => {
  test('starts at the first frame and shows the last frame whole', () => {
    const tiles = tileLayout(100, 500, 1000, 50);
    expect(tiles[0]).toBe(100);
    expect(tiles[tiles.length - 1]).toBe(500);
  });

  test('spreads tiles evenly without overlap', () => {
    const tiles = tileLayout(100, 500, 1000, 50);
    const steps = tiles.slice(1).map((x, i) => x - tiles[i]!);
    expect(Math.max(...steps) - Math.min(...steps)).toBeLessThanOrEqual(1);
    expect(Math.min(...steps)).toBeGreaterThan(50);
  });

  test('keeps the last tile within the minimap', () => {
    const tiles = tileLayout(0, 1000, 1000, 50);
    expect(tiles[0]).toBe(0);
    expect(tiles[tiles.length - 1]).toBe(950);
  });

  test('shows a single whole tile for very short videos', () => {
    expect(tileLayout(990, 995, 1000, 50)).toEqual([950]);
  });
});
