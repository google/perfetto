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

import {timeToCode, TPTime, TPTimeSpan} from './time';

test('seconds to code', () => {
  expect(timeToCode(3)).toEqual('3s');
  expect(timeToCode(60)).toEqual('1m');
  expect(timeToCode(63)).toEqual('1m 3s');
  expect(timeToCode(63.2)).toEqual('1m 3s 200ms');
  expect(timeToCode(63.2221)).toEqual('1m 3s 222ms 100us');
  expect(timeToCode(63.2221111)).toEqual('1m 3s 222ms 111us 100ns');
  expect(timeToCode(0.2221111)).toEqual('222ms 111us 100ns');
  expect(timeToCode(0.000001)).toEqual('1us');
  expect(timeToCode(0.000003)).toEqual('3us');
  expect(timeToCode(1.000001)).toEqual('1s 1us');
  expect(timeToCode(200.00000003)).toEqual('3m 20s 30ns');
  expect(timeToCode(0)).toEqual('0s');
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
