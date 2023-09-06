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
  Duration,
  Time,
  Timecode,
  TimeSpan,
} from '../base/time';

test('Duration.format', () => {
  expect(Duration.format(0n)).toEqual('0s');
  expect(Duration.format(3_000_000_000n)).toEqual('3s');
  expect(Duration.format(60_000_000_000n)).toEqual('1m');
  expect(Duration.format(63_000_000_000n)).toEqual('1m 3s');
  expect(Duration.format(63_200_000_000n)).toEqual('1m 3s 200ms');
  expect(Duration.format(63_222_100_000n)).toEqual('1m 3s 222ms 100us');
  expect(Duration.format(63_222_111_100n)).toEqual('1m 3s 222ms 111us 100ns');
  expect(Duration.format(222_111_100n)).toEqual('222ms 111us 100ns');
  expect(Duration.format(1_000n)).toEqual('1us');
  expect(Duration.format(3_000n)).toEqual('3us');
  expect(Duration.format(1_000_001_000n)).toEqual('1s 1us');
  expect(Duration.format(200_000_000_030n)).toEqual('3m 20s 30ns');
  expect(Duration.format(3_600_000_000_000n)).toEqual('1h');
  expect(Duration.format(3_600_000_000_001n)).toEqual('1h 1ns');
  expect(Duration.format(86_400_000_000_000n)).toEqual('24h');
  expect(Duration.format(86_400_000_000_001n)).toEqual('24h 1ns');
  expect(Duration.format(31_536_000_000_000_000n)).toEqual('8,760h');
  expect(Duration.format(31_536_000_000_000_001n)).toEqual('8,760h 1ns');
});

test('Duration.humanise', () => {
  expect(Duration.humanise(0n)).toEqual('0s');
  expect(Duration.humanise(123n)).toEqual('123ns');
  expect(Duration.humanise(1_234n)).toEqual('1.2us');
  expect(Duration.humanise(12_345n)).toEqual('12.3us');
  expect(Duration.humanise(3_000_000_000n)).toEqual('3s');
  expect(Duration.humanise(60_000_000_000n)).toEqual('60s');
  expect(Duration.humanise(63_000_000_000n)).toEqual('63s');
  expect(Duration.humanise(63_200_000_000n)).toEqual('63.2s');
  expect(Duration.humanise(63_222_100_000n)).toEqual('63.2s');
  expect(Duration.humanise(63_222_111_100n)).toEqual('63.2s');
  expect(Duration.humanise(222_111_100n)).toEqual('222.1ms');
  expect(Duration.humanise(1_000n)).toEqual('1us');
  expect(Duration.humanise(3_000n)).toEqual('3us');
  expect(Duration.humanise(1_000_001_000n)).toEqual('1s');
  expect(Duration.humanise(200_000_000_030n)).toEqual('200s');
  expect(Duration.humanise(3_600_000_000_000n)).toEqual('3600s');
  expect(Duration.humanise(86_400_000_000_000n)).toEqual('86400s');
  expect(Duration.humanise(31_536_000_000_000_000n)).toEqual('31536000s');
});

test('Duration.fromMillis', () => {
  expect(Duration.fromMillis(123.456789)).toEqual(123456789n);
  expect(Duration.fromMillis(123.4567895)).toEqual(123456789n);
  expect(Duration.fromMillis(0.0000001)).toEqual(0n);
});

test('timecode', () => {
  expect(new Timecode(Time.fromRaw(0n)).toString(' '))
      .toEqual('00:00:00.000 000 000');
  expect(new Timecode(Time.fromRaw(123n)).toString(' '))
      .toEqual('00:00:00.000 000 123');
  expect(new Timecode(Time.fromRaw(60_000_000_000n)).toString(' '))
      .toEqual('00:01:00.000 000 000');
  expect(new Timecode(Time.fromRaw(12_345_678_910n)).toString(' '))
      .toEqual('00:00:12.345 678 910');
  expect(new Timecode(Time.fromRaw(86_400_000_000_000n)).toString(' '))
      .toEqual('1d00:00:00.000 000 000');
  expect(new Timecode(Time.fromRaw(31_536_000_000_000_000n)).toString(' '))
      .toEqual('365d00:00:00.000 000 000');
  expect(new Timecode(Time.fromRaw(-123n)).toString(' '))
      .toEqual('-00:00:00.000 000 123');
});

function mkSpan(start: bigint, end: bigint) {
  return new TimeSpan(Time.fromRaw(start), Time.fromRaw(end));
}

describe('TimeSpan', () => {
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

    expect(x.contains(Time.fromRaw(9n))).toBeFalsy();
    expect(x.contains(Time.fromRaw(10n))).toBeTruthy();
    expect(x.contains(Time.fromRaw(15n))).toBeTruthy();
    expect(x.contains(Time.fromRaw(20n))).toBeFalsy();
    expect(x.contains(Time.fromRaw(21n))).toBeFalsy();

    expect(x.contains(mkSpan(12n, 18n))).toBeTruthy();
    expect(x.contains(mkSpan(5n, 25n))).toBeFalsy();
    expect(x.contains(mkSpan(5n, 15n))).toBeFalsy();
    expect(x.contains(mkSpan(15n, 25n))).toBeFalsy();
    expect(x.contains(mkSpan(0n, 10n))).toBeFalsy();
    expect(x.contains(mkSpan(20n, 30n))).toBeFalsy();
  });

  it('checks intersection with span', () => {
    const x = mkSpan(10n, 20n);

    expect(x.intersectsInterval(mkSpan(0n, 10n))).toBeFalsy();
    expect(x.intersectsInterval(mkSpan(5n, 15n))).toBeTruthy();
    expect(x.intersectsInterval(mkSpan(12n, 18n))).toBeTruthy();
    expect(x.intersectsInterval(mkSpan(15n, 25n))).toBeTruthy();
    expect(x.intersectsInterval(mkSpan(20n, 30n))).toBeFalsy();
    expect(x.intersectsInterval(mkSpan(5n, 25n))).toBeTruthy();
  });

  it('checks intersection', () => {
    const x = mkSpan(10n, 20n);

    expect(x.intersects(Time.fromRaw(0n), Time.fromRaw(10n))).toBeFalsy();
    expect(x.intersects(Time.fromRaw(5n), Time.fromRaw(15n))).toBeTruthy();
    expect(x.intersects(Time.fromRaw(12n), Time.fromRaw(18n))).toBeTruthy();
    expect(x.intersects(Time.fromRaw(15n), Time.fromRaw(25n))).toBeTruthy();
    expect(x.intersects(Time.fromRaw(20n), Time.fromRaw(30n))).toBeFalsy();
    expect(x.intersects(Time.fromRaw(5n), Time.fromRaw(25n))).toBeTruthy();
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
