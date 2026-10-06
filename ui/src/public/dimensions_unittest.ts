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
import {
  type Dimension,
  formatDimensionLabel,
  formatDimensionLabels,
  visibleDimensionNames,
} from './dimensions';

function intDim(name: string, value: number, displayName?: string): Dimension {
  return {name, intValue: value, displayName};
}

function strDim(name: string, value: string): Dimension {
  return {name, stringValue: value};
}

describe('formatDimensionLabel', () => {
  test('renders an integer dimension as "<name> <value>"', () => {
    expect(formatDimensionLabel(intDim('rank', 17))).toBe('rank 17');
  });

  test('renders a string dimension as "<name> <value>"', () => {
    expect(formatDimensionLabel(strDim('stage', 'forward'))).toBe(
      'stage forward',
    );
  });

  test('a producer display name overrides the rendered label', () => {
    expect(formatDimensionLabel(intDim('rank', 7, 'worker-east'))).toBe(
      'worker-east',
    );
  });

  test('joins multiple labels with a middot', () => {
    expect(
      formatDimensionLabels([intDim('rank', 3), strDim('stage', 'forward')]),
    ).toBe('rank 3 · stage forward');
  });
});

describe('visibleDimensionNames', () => {
  test('hides integer dimensions with a single distinct value', () => {
    const visible = visibleDimensionNames([
      intDim('rank', 3),
      intDim('rank', 3),
    ]);
    expect(Array.from(visible)).toEqual([]);
  });

  test('shows integer dimensions which disambiguate tracks', () => {
    const visible = visibleDimensionNames([
      intDim('rank', 3),
      intDim('rank', 7),
    ]);
    expect(Array.from(visible)).toEqual(['rank']);
  });

  test('integer dimensions collapse independently', () => {
    const visible = visibleDimensionNames([
      intDim('rank', 3),
      intDim('rank', 7),
      intDim('stream', 0),
      intDim('stream', 0),
    ]);
    expect(Array.from(visible)).toEqual(['rank']);
  });

  test('always shows string dimensions, even with a single value', () => {
    // A Chrome frame title on the single renderer of a trace: one value, but
    // it is emitted precisely to be shown.
    const visible = visibleDimensionNames([
      strDim('chrome.process_label', 'IMDb: Ratings, Reviews'),
      strDim('stage', 'forward'),
      strDim('stage', 'forward'),
    ]);
    expect(Array.from(visible).sort()).toEqual([
      'chrome.process_label',
      'stage',
    ]);
  });

  test('hides the rank of a trace with a single rank', () => {
    // A trace recorded by a single training process: the rank is inherited by
    // all of its tracks but has a single value, so it tells nothing apart.
    const visible = visibleDimensionNames([
      intDim('rank', 0),
      intDim('rank', 0),
      intDim('rank', 0),
      intDim('shard', 0),
      intDim('shard', 1),
    ]);
    expect(Array.from(visible)).toEqual(['shard']);
  });

  test('dimensions presented elsewhere are never labelled', () => {
    const visible = visibleDimensionNames([
      {name: 'machine', intValue: 0},
      {name: 'machine', intValue: 1},
      {name: 'gpu', intValue: 0},
      {name: 'gpu', intValue: 1},
    ]);
    expect(Array.from(visible)).toEqual([]);
  });
});
