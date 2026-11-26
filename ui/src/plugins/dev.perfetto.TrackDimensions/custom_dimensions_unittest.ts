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
  buildCustomDimensions,
  type CustomDimensionRow,
  getCustomDimensionChips,
  getCustomDimensions,
  getProcessCustomDimensions,
  type ProcessRootTrackRow,
} from './custom_dimensions';

function row(
  trackId: number,
  name: string,
  value: bigint | string,
  displayName: string | null = null,
): CustomDimensionRow {
  return {
    trackId,
    name,
    intValue: typeof value === 'bigint' ? value : null,
    stringValue: typeof value === 'string' ? value : null,
    displayValue: String(value),
    displayName,
  };
}

function chipsFor(
  rows: ReadonlyArray<CustomDimensionRow>,
  trackIds: ReadonlyArray<number>,
  roots: ReadonlyArray<ProcessRootTrackRow> = [],
) {
  const dimensions = buildCustomDimensions(rows, roots);
  return getCustomDimensionChips(getCustomDimensions(dimensions, trackIds));
}

describe('custom dimensions', () => {
  test('hides dimensions uniform across the trace', () => {
    const rows = [
      row(1, 'rank', 0n),
      row(2, 'rank', 1n),
      row(1, 'job', 'train'),
      row(2, 'job', 'train'),
    ];
    expect(chipsFor(rows, [1])).toEqual(['rank 0']);
  });

  test('uses display names and sorts by dimension name', () => {
    const rows = [
      row(1, 'stage', 'fwd', 'forward pass'),
      row(2, 'stage', 'bwd'),
      row(1, 'rank', 0n, ''),
      row(2, 'rank', 1n),
    ];
    expect(chipsFor(rows, [1])).toEqual(['rank 0', 'forward pass']);
    expect(chipsFor(rows, [2])).toEqual(['rank 1', 'stage bwd']);
  });

  test('distinguishes integer and string values', () => {
    const rows = [row(1, 'id', 3n), row(2, 'id', '3')];
    const dimensions = buildCustomDimensions(rows, []);
    expect(dimensions.varyingNames.has('id')).toBe(true);
  });

  test('keeps int64 values beyond number precision distinct', () => {
    const rows = [
      row(1, 'id', 9007199254740992n),
      row(2, 'id', 9007199254740993n),
    ];
    const dimensions = buildCustomDimensions(rows, []);
    expect(dimensions.varyingNames.has('id')).toBe(true);
  });

  test('requires every track to agree on value and label', () => {
    const rows = [
      row(1, 'rank', 0n),
      row(2, 'rank', 0n),
      row(3, 'rank', 1n),
      row(4, 'rank', 0n, 'zero'),
    ];
    expect(chipsFor(rows, [1, 2])).toEqual(['rank 0']);
    expect(chipsFor(rows, [1, 3])).toEqual([]);
    expect(chipsFor(rows, [1, 4])).toEqual([]);
    expect(chipsFor(rows, [1, 5])).toEqual([]);
    expect(chipsFor(rows, [])).toEqual([]);
  });

  test('describes a process by the dimensions of all its root tracks', () => {
    const rows = [row(1, 'rank', 0n), row(2, 'rank', 0n), row(3, 'rank', 1n)];
    const dimensions = buildCustomDimensions(rows, [
      {upid: 10, trackId: 1},
      {upid: 10, trackId: 2},
      {upid: 20, trackId: 3},
      {upid: 20, trackId: 4},
    ]);
    expect(
      getCustomDimensionChips(getProcessCustomDimensions(dimensions, 10)),
    ).toEqual(['rank 0']);
    // Track 4 has no dimensions, so the roots of process 20 don't agree.
    expect(getProcessCustomDimensions(dimensions, 20)).toEqual([]);
    expect(getProcessCustomDimensions(dimensions, 30)).toEqual([]);
  });

  test('omits dimensions an ancestor shows with the same value', () => {
    const rows = [
      row(1, 'rank', 0n),
      row(1, 'stage', 'fwd'),
      row(2, 'rank', 1n),
      row(2, 'stage', 'bwd'),
    ];
    const dimensions = buildCustomDimensions(rows, []);
    const parent = getCustomDimensions(dimensions, [1]);
    const own = getCustomDimensions(dimensions, [2]);
    expect(getCustomDimensionChips(parent, parent)).toEqual([]);
    expect(getCustomDimensionChips(own, parent)).toEqual([
      'rank 1',
      'stage bwd',
    ]);
    expect(
      getCustomDimensionChips(own, [
        ...parent.filter((d) => d.name === 'stage'),
        ...own.filter((d) => d.name === 'rank'),
      ]),
    ).toEqual(['stage bwd']);
  });
});
