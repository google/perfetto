// Copyright (C) 2018 The Android Open Source Project
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

import {Time, TimeSpan} from '../base/time';
import {getPattern, generateTicks, TickType} from './gridline_helper';

test('gridline helper to have sensible step sizes', () => {
  expect(getPattern(1n)).toEqual([1n, '|']);
  expect(getPattern(2n)).toEqual([2n, '|:']);
  expect(getPattern(3n)).toEqual([5n, '|....']);
  expect(getPattern(4n)).toEqual([5n, '|....']);
  expect(getPattern(5n)).toEqual([5n, '|....']);
  expect(getPattern(7n)).toEqual([10n, '|....:....']);

  expect(getPattern(10n)).toEqual([10n, '|....:....']);
  expect(getPattern(20n)).toEqual([20n, '|.:.']);
  expect(getPattern(50n)).toEqual([50n, '|....']);

  expect(getPattern(100n)).toEqual([100n, '|....:....']);
});

describe('generateTicks', () => {
  it('can generate ticks with span starting at origin', () => {
    const tickGen = generateTicks(
      new TimeSpan(Time.fromRaw(0n), Time.fromRaw(10n)),
      1,
    );
    const expected = [
      {type: TickType.MAJOR, time: 0n},
      {type: TickType.MINOR, time: 1n},
      {type: TickType.MINOR, time: 2n},
      {type: TickType.MINOR, time: 3n},
      {type: TickType.MINOR, time: 4n},
      {type: TickType.MEDIUM, time: 5n},
      {type: TickType.MINOR, time: 6n},
      {type: TickType.MINOR, time: 7n},
      {type: TickType.MINOR, time: 8n},
      {type: TickType.MINOR, time: 9n},
    ];
    const actual = Array.from(tickGen!);
    expect(actual).toStrictEqual(expected);
  });

  it('can generate ticks when span has an offset', () => {
    const tickGen = generateTicks(
      new TimeSpan(Time.fromRaw(10n), Time.fromRaw(20n)),
      1,
    );
    const expected = [
      {type: TickType.MAJOR, time: 10n},
      {type: TickType.MINOR, time: 11n},
      {type: TickType.MINOR, time: 12n},
      {type: TickType.MINOR, time: 13n},
      {type: TickType.MINOR, time: 14n},
      {type: TickType.MEDIUM, time: 15n},
      {type: TickType.MINOR, time: 16n},
      {type: TickType.MINOR, time: 17n},
      {type: TickType.MINOR, time: 18n},
      {type: TickType.MINOR, time: 19n},
    ];
    const actual = Array.from(tickGen!);
    expect(actual).toStrictEqual(expected);
  });

  it('can generate ticks when span is large', () => {
    const tickGen = generateTicks(
      new TimeSpan(Time.fromRaw(1000000000n), Time.fromRaw(2000000000n)),
      1,
    );
    const expected = [
      {type: TickType.MAJOR, time: 1000000000n},
      {type: TickType.MINOR, time: 1100000000n},
      {type: TickType.MINOR, time: 1200000000n},
      {type: TickType.MINOR, time: 1300000000n},
      {type: TickType.MINOR, time: 1400000000n},
      {type: TickType.MEDIUM, time: 1500000000n},
      {type: TickType.MINOR, time: 1600000000n},
      {type: TickType.MINOR, time: 1700000000n},
      {type: TickType.MINOR, time: 1800000000n},
      {type: TickType.MINOR, time: 1900000000n},
    ];
    const actual = Array.from(tickGen!);
    expect(actual).toStrictEqual(expected);
  });

  it('throws an error when timespan duration is 0', () => {
    expect(() => {
      Array.from(generateTicks(TimeSpan.ZERO, 1));
    }).toThrow(Error);
  });

  it('throws an error when max ticks is 0', () => {
    const nonZeroTimeSpan = new TimeSpan(Time.fromRaw(0n), Time.fromRaw(1n));
    expect(() => {
      Array.from(generateTicks(nonZeroTimeSpan, 0));
    }).toThrow(Error);
  });
});
