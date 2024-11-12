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

import {Time} from '../base/time';
import {UNEXPECTED_PINK} from '../public/lib/colorizer';
import {Slice} from '../public/track';
import {filterVisibleSlicesForTesting as filterVisibleSlices} from './base_slice_track';

function slice(start: number, duration: number, depth: number = 0): Slice {
  const startNs = Time.fromRaw(BigInt(start));
  const durNs = Time.fromRaw(BigInt(duration));
  const endNs = Time.fromRaw(startNs + durNs);
  return {
    id: 42,
    startNs,
    endNs,
    durNs,
    ts: startNs,
    dur: durNs,
    depth,
    flags: 0,
    title: '',
    subTitle: '',
    colorScheme: UNEXPECTED_PINK,
    fillRatio: 1,
    isHighlighted: false,
  };
}

const s = slice;
const t = Time.fromRaw;

test('filterVisibleSlices', () => {
  expect(filterVisibleSlices([], t(0n), t(100n))).toEqual([]);
  expect(filterVisibleSlices([s(10, 80)], t(0n), t(100n))).toEqual([s(10, 80)]);
  expect(filterVisibleSlices([s(0, 20)], t(10n), t(100n))).toEqual([s(0, 20)]);
  expect(filterVisibleSlices([s(0, 10)], t(10n), t(100n))).toEqual([s(0, 10)]);
  expect(filterVisibleSlices([s(100, 10)], t(10n), t(100n))).toEqual([
    s(100, 10),
  ]);
  expect(filterVisibleSlices([s(10, 0)], t(10n), t(100n))).toEqual([s(10, 0)]);
  expect(filterVisibleSlices([s(100, 0)], t(10n), t(100n))).toEqual([
    s(100, 0),
  ]);
  expect(filterVisibleSlices([s(0, 5)], t(10n), t(90n))).toEqual([]);
  expect(filterVisibleSlices([s(95, 5)], t(10n), t(90n))).toEqual([]);
  expect(filterVisibleSlices([s(0, 5), s(95, 5)], t(10n), t(90n))).toEqual([]);
  expect(
    filterVisibleSlices([s(0, 5), s(50, 0), s(95, 5)], t(10n), t(90n)),
  ).toEqual([s(50, 0)]);
  expect(
    filterVisibleSlices([s(0, 5), s(1, 9), s(6, 3)], t(10n), t(90n)),
  ).toContainEqual(s(1, 9));
  expect(
    filterVisibleSlices([s(0, 5), s(1, 9), s(6, 3), s(50, 0)], t(10n), t(90n)),
  ).toContainEqual(s(1, 9));
  expect(filterVisibleSlices([s(85, 10), s(100, 10)], t(10n), t(90n))).toEqual([
    s(85, 10),
  ]);
  expect(filterVisibleSlices([s(0, 100)], t(10n), t(90n))).toEqual([s(0, 100)]);
  expect(
    filterVisibleSlices(
      [
        s(0, 1),
        s(1, 1),
        s(2, 1),
        s(3, 1),
        s(4, 1),
        s(5, 10),
        s(6, 1),
        s(7, 1),
        s(8, 1),
        s(9, 1),
      ],
      t(10n),
      t(90n),
    ),
  ).toContainEqual(s(5, 10));
});

test('filterVisibleSlicesOrderByDepthAndTs', () => {
  expect(
    filterVisibleSlices(
      [
        s(5, 2, 0),
        s(5, 4, 0),
        s(5, 6, 0),
        s(7, 5, 0),
        s(8, 10, 0),
        s(4, 1, 1),
        s(6, 3, 1),
        s(8, 6, 1),
        s(6, 1, 2),
        s(10, 9, 2),
        s(11, 3, 2),
      ],
      t(10n),
      t(90n),
    ),
  ).toEqual([
    s(5, 6, 0),
    s(7, 5, 0),
    s(8, 10, 0),
    s(8, 6, 1),
    s(10, 9, 2),
    s(11, 3, 2),
  ]);
});

test('filterVisibleSlicesOrderByTs', () => {
  expect(
    filterVisibleSlices(
      [s(4, 5), s(4, 3), s(5, 10), s(6, 3), s(7, 2), s(10, 10)],
      t(10n),
      t(90n),
    ),
  ).toEqual([s(5, 10), s(10, 10)]);
});
