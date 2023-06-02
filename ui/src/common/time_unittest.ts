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

import {
  formatTPTime,
  TPTime,
  TPTimeSpan,
  tpTimeToCode,
  tpTimeToString,
} from './time';

test('tpTimeToCode', () => {
  expect(tpTimeToCode(0n)).toEqual('0s');
  expect(tpTimeToCode(3_000_000_000n)).toEqual('3s');
  expect(tpTimeToCode(60_000_000_000n)).toEqual('1m');
  expect(tpTimeToCode(63_000_000_000n)).toEqual('1m 3s');
  expect(tpTimeToCode(63_200_000_000n)).toEqual('1m 3s 200ms');
  expect(tpTimeToCode(63_222_100_000n)).toEqual('1m 3s 222ms 100us');
  expect(tpTimeToCode(63_222_111_100n)).toEqual('1m 3s 222ms 111us 100ns');
  expect(tpTimeToCode(222_111_100n)).toEqual('222ms 111us 100ns');
  expect(tpTimeToCode(1_000n)).toEqual('1us');
  expect(tpTimeToCode(3_000n)).toEqual('3us');
  expect(tpTimeToCode(1_000_001_000n)).toEqual('1s 1us');
  expect(tpTimeToCode(200_000_000_030n)).toEqual('3m 20s 30ns');
  expect(tpTimeToCode(3_600_000_000_000n)).toEqual('60m');
  expect(tpTimeToCode(3_600_000_000_001n)).toEqual('60m 1ns');
  expect(tpTimeToCode(86_400_000_000_000n)).toEqual('1,440m');
  expect(tpTimeToCode(86_400_000_000_001n)).toEqual('1,440m 1ns');
  expect(tpTimeToCode(31_536_000_000_000_000n)).toEqual('525,600m');
  expect(tpTimeToCode(31_536_000_000_000_001n)).toEqual('525,600m 1ns');
});

test('formatTPTime', () => {
  expect(formatTPTime(0n)).toEqual('0.000 000 000');
  expect(formatTPTime(3_000_000_000n)).toEqual('3.000 000 000');
  expect(formatTPTime(60_000_000_000n)).toEqual('60.000 000 000');
  expect(formatTPTime(63_000_000_000n)).toEqual('63.000 000 000');
  expect(formatTPTime(63_200_000_000n)).toEqual('63.200 000 000');
  expect(formatTPTime(63_222_100_000n)).toEqual('63.222 100 000');
  expect(formatTPTime(63_222_111_100n)).toEqual('63.222 111 100');
  expect(formatTPTime(222_111_100n)).toEqual('0.222 111 100');
  expect(formatTPTime(1_000n)).toEqual('0.000 001 000');
  expect(formatTPTime(3_000n)).toEqual('0.000 003 000');
  expect(formatTPTime(1_000_001_000n)).toEqual('1.000 001 000');
  expect(formatTPTime(200_000_000_030n)).toEqual('200.000 000 030');
  expect(formatTPTime(3_600_000_000_000n)).toEqual('3600.000 000 000');
  expect(formatTPTime(86_400_000_000_000n)).toEqual('86400.000 000 000');
  expect(formatTPTime(86_400_000_000_001n)).toEqual('86400.000 000 001');
  expect(formatTPTime(31_536_000_000_000_000n)).toEqual('31536000.000 000 000');
  expect(formatTPTime(31_536_000_000_000_001n)).toEqual('31536000.000 000 001');
});

test('tpTimeToString', () => {
  expect(tpTimeToString(0n)).toEqual('0 s');
  expect(tpTimeToString(3_000_000_000n)).toEqual('3 s');
  expect(tpTimeToString(60_000_000_000n)).toEqual('60 s');
  expect(tpTimeToString(63_000_000_000n)).toEqual('63 s');
  expect(tpTimeToString(63_200_000_000n)).toEqual('63.2 s');
  expect(tpTimeToString(63_222_100_000n)).toEqual('63.2 s');
  expect(tpTimeToString(63_222_111_100n)).toEqual('63.2 s');
  expect(tpTimeToString(222_111_100n)).toEqual('222.1 ms');
  expect(tpTimeToString(1_000n)).toEqual('1 us');
  expect(tpTimeToString(3_000n)).toEqual('3 us');
  expect(tpTimeToString(1_000_001_000n)).toEqual('1 s');
  expect(tpTimeToString(200_000_000_030n)).toEqual('200 s');
  expect(tpTimeToString(3_600_000_000_000n)).toEqual('3600 s');
  expect(tpTimeToString(86_400_000_000_000n)).toEqual('86400 s');
  expect(tpTimeToString(31_536_000_000_000_000n)).toEqual('31536000 s');
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
