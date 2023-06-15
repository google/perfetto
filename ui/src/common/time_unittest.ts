// Copyright (C) 2019 The Android Open Source Project
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

import {globals} from '../frontend/globals';
import {createEmptyState} from './empty_state';
import {
  formatDuration,
  formatDurationShort,
  Timecode,
  TPTime,
  TPTimeSpan,
} from './time';

beforeAll(() => {
  globals.state = createEmptyState();
  globals.state.traceTime.start = 0n;
});

test('formatDuration', () => {
  expect(formatDuration(0n)).toEqual('0s');
  expect(formatDuration(3_000_000_000n)).toEqual('3s');
  expect(formatDuration(60_000_000_000n)).toEqual('1m');
  expect(formatDuration(63_000_000_000n)).toEqual('1m 3s');
  expect(formatDuration(63_200_000_000n)).toEqual('1m 3s 200ms');
  expect(formatDuration(63_222_100_000n)).toEqual('1m 3s 222ms 100us');
  expect(formatDuration(63_222_111_100n)).toEqual('1m 3s 222ms 111us 100ns');
  expect(formatDuration(222_111_100n)).toEqual('222ms 111us 100ns');
  expect(formatDuration(1_000n)).toEqual('1us');
  expect(formatDuration(3_000n)).toEqual('3us');
  expect(formatDuration(1_000_001_000n)).toEqual('1s 1us');
  expect(formatDuration(200_000_000_030n)).toEqual('3m 20s 30ns');
  expect(formatDuration(3_600_000_000_000n)).toEqual('60m');
  expect(formatDuration(3_600_000_000_001n)).toEqual('60m 1ns');
  expect(formatDuration(86_400_000_000_000n)).toEqual('1,440m');
  expect(formatDuration(86_400_000_000_001n)).toEqual('1,440m 1ns');
  expect(formatDuration(31_536_000_000_000_000n)).toEqual('525,600m');
  expect(formatDuration(31_536_000_000_000_001n)).toEqual('525,600m 1ns');
});

test('formatDurationShort', () => {
  expect(formatDurationShort(0n)).toEqual('0s');
  expect(formatDurationShort(123n)).toEqual('123ns');
  expect(formatDurationShort(1_234n)).toEqual('1.2us');
  expect(formatDurationShort(12_345n)).toEqual('12.3us');
  expect(formatDurationShort(3_000_000_000n)).toEqual('3s');
  expect(formatDurationShort(60_000_000_000n)).toEqual('60s');
  expect(formatDurationShort(63_000_000_000n)).toEqual('63s');
  expect(formatDurationShort(63_200_000_000n)).toEqual('63.2s');
  expect(formatDurationShort(63_222_100_000n)).toEqual('63.2s');
  expect(formatDurationShort(63_222_111_100n)).toEqual('63.2s');
  expect(formatDurationShort(222_111_100n)).toEqual('222.1ms');
  expect(formatDurationShort(1_000n)).toEqual('1us');
  expect(formatDurationShort(3_000n)).toEqual('3us');
  expect(formatDurationShort(1_000_001_000n)).toEqual('1s');
  expect(formatDurationShort(200_000_000_030n)).toEqual('200s');
  expect(formatDurationShort(3_600_000_000_000n)).toEqual('3600s');
  expect(formatDurationShort(86_400_000_000_000n)).toEqual('86400s');
  expect(formatDurationShort(31_536_000_000_000_000n)).toEqual('31536000s');
});

test('timecode', () => {
  expect(new Timecode(0n).toString(' ')).toEqual('00:00:00.000 000 000');
  expect(new Timecode(123n).toString(' ')).toEqual('00:00:00.000 000 123');
  expect(new Timecode(60_000_000_000n).toString(' '))
      .toEqual('00:01:00.000 000 000');
  expect(new Timecode(12_345_678_910n).toString(' '))
      .toEqual('00:00:12.345 678 910');
  expect(new Timecode(86_400_000_000_000n).toString(' '))
      .toEqual('1d00:00:00.000 000 000');
  expect(new Timecode(31_536_000_000_000_000n).toString(' '))
      .toEqual('365d00:00:00.000 000 000');
  expect(new Timecode(-123n).toString(' ')).toEqual('-00:00:00.000 000 123');
});

function mkSpan(start: TPTime, end: TPTime) {
  return new TPTimeSpan(start, end);
}

describe('TPTimeSpan', () => {
  it('throws when start is later than end', () => {
    expect(() => mkSpan(1n, 0n)).toThrow();
  });

  it('can calc duration', () => {
    expect(mkSpan(10n, 20n).duration).toBe(10n);
  });

  it('can calc midpoint', () => {
    expect(mkSpan(10n, 20n).midpoint).toBe(15n);
    expect(mkSpan(10n, 19n).midpoint).toBe(14n);
    expect(mkSpan(10n, 10n).midpoint).toBe(10n);
  });

  it('can be compared', () => {
    const x = mkSpan(10n, 20n);
    expect(x.equals(mkSpan(10n, 20n))).toBeTruthy();
    expect(x.equals(mkSpan(11n, 20n))).toBeFalsy();
    expect(x.equals(mkSpan(10n, 19n))).toBeFalsy();
  });

  it('checks containment', () => {
    const x = mkSpan(10n, 20n);

    expect(x.contains(9n)).toBeFalsy();
    expect(x.contains(10n)).toBeTruthy();
    expect(x.contains(15n)).toBeTruthy();
    expect(x.contains(20n)).toBeFalsy();
    expect(x.contains(21n)).toBeFalsy();

    expect(x.contains(mkSpan(12n, 18n))).toBeTruthy();
    expect(x.contains(mkSpan(5n, 25n))).toBeFalsy();
    expect(x.contains(mkSpan(5n, 15n))).toBeFalsy();
    expect(x.contains(mkSpan(15n, 25n))).toBeFalsy();
    expect(x.contains(mkSpan(0n, 10n))).toBeFalsy();
    expect(x.contains(mkSpan(20n, 30n))).toBeFalsy();
  });

  it('checks intersection', () => {
    const x = mkSpan(10n, 20n);

    expect(x.intersects(mkSpan(0n, 10n))).toBeFalsy();
    expect(x.intersects(mkSpan(5n, 15n))).toBeTruthy();
    expect(x.intersects(mkSpan(12n, 18n))).toBeTruthy();
    expect(x.intersects(mkSpan(15n, 25n))).toBeTruthy();
    expect(x.intersects(mkSpan(20n, 30n))).toBeFalsy();
    expect(x.intersects(mkSpan(5n, 25n))).toBeTruthy();
  });

  it('can add', () => {
    const x = mkSpan(10n, 20n);
    expect(x.add(5n)).toEqual(mkSpan(15n, 25n));
  });

  it('can pad', () => {
    const x = mkSpan(10n, 20n);
    expect(x.pad(5n)).toEqual(mkSpan(5n, 25n));
  });
});
