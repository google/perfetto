// Copyright (C) 2024 The Android Open Source Project
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

import {HighPrecisionTime as HPTime} from './high_precision_time';
import {HighPrecisionTimeSpan as HPTimeSpan} from './high_precision_time_span';
import {Time} from './time';

const t = Time.fromRaw;

// Quick 'n' dirty function to convert a string to a HPtime
// Used to make tests more readable
// E.g. '1.3' -> {base: 1, offset: 0.3}
// E.g. '-0.3' -> {base: -1, offset: 0.7}
function hptime(time: string): HPTime {
  const array = time.split('.');
  if (array.length > 2) throw new Error(`Bad time format ${time}`);
  const [base, fractions] = array;
  const negative = time.startsWith('-');
  const numBase = BigInt(base);

  if (fractions) {
    const numFractions = Number(`0.${fractions}`);
    if (negative) {
      return new HPTime(t(numBase - 1n), 1.0 - numFractions);
    } else {
      return new HPTime(t(numBase), numFractions);
    }
  } else {
    return new HPTime(t(numBase));
  }
}

describe('HighPrecisionTimeSpan', () => {
  it('can be constructed from integer time', () => {
    const span = HPTimeSpan.fromTime(t(10n), t(20n));
    expect(span.start.integral).toEqual(10n);
    expect(span.start.fractional).toBeCloseTo(0);
    expect(span.duration).toBeCloseTo(10);
  });

  test('end', () => {
    const span = HPTimeSpan.fromTime(t(10n), t(20n));
    expect(span.end.integral).toEqual(20n);
    expect(span.end.fractional).toBeCloseTo(0);
  });

  test('midpoint', () => {
    const span = HPTimeSpan.fromTime(t(10n), t(20n));
    expect(span.midpoint.integral).toEqual(15n);
    expect(span.midpoint.fractional).toBeCloseTo(0);
  });

  test('translate', () => {
    const span = HPTimeSpan.fromTime(t(10n), t(20n));
    expect(span.translate(10).start.integral).toEqual(20n);
    expect(span.translate(10).start.fractional).toEqual(0);
    expect(span.translate(10).duration).toBeCloseTo(10);
  });

  test('pad', () => {
    const span = HPTimeSpan.fromTime(t(10n), t(20n));
    expect(span.pad(10).start.integral).toEqual(0n);
    expect(span.pad(10).start.fractional).toEqual(0);
    expect(span.pad(10).duration).toBeCloseTo(30);
  });

  test('scale', () => {
    const span = HPTimeSpan.fromTime(t(10n), t(20n));
    const zoomed = span.scale(2, 0.5, 0);
    expect(zoomed.start.integral).toEqual(5n);
    expect(zoomed.start.fractional).toBeCloseTo(0);
    expect(zoomed.duration).toBeCloseTo(20);
  });

  test('intersect', () => {
    const span = new HPTimeSpan(hptime('5'), 3);

    let result = span.intersect(t(7n), t(10n));
    expect(result.start.integral).toBe(7n);
    expect(result.start.fractional).toBeCloseTo(0);
    expect(result.duration).toBeCloseTo(1);

    result = span.intersect(t(1n), t(6n));
    expect(result.start.integral).toBe(5n);
    expect(result.start.fractional).toBeCloseTo(0);
    expect(result.duration).toBeCloseTo(1);

    // Non overlapping time spans should return 0
    result = span.intersect(t(100n), t(200n));
    expect(result.start.integral).toBe(0n);
    expect(result.start.fractional).toBeCloseTo(0);
    expect(result.duration).toBeCloseTo(0);
  });

  test('fitWithin', () => {
    const span = new HPTimeSpan(hptime('5'), 3);

    let result = span.fitWithin(t(10n), t(20n));
    expect(result.start.integral).toBe(10n);
    expect(result.start.fractional).toBeCloseTo(0);
    expect(result.duration).toBeCloseTo(3);

    result = span.fitWithin(t(-10n), t(-5n));
    expect(result.start.integral).toBe(-8n);
    expect(result.start.fractional).toBeCloseTo(0);
    expect(result.duration).toBeCloseTo(3);

    result = span.fitWithin(t(1n), t(2n));
    expect(result.start.integral).toBe(1n);
    expect(result.start.fractional).toBeCloseTo(0);
    expect(result.duration).toBeCloseTo(1);
  });

  test('clampDuration', () => {
    const span = new HPTimeSpan(hptime('5'), 1);
    const clamped = span.clampDuration(10);

    expect(clamped.start.integral).toBe(5n);
    expect(clamped.start.fractional).toBeCloseTo(0);
    expect(clamped.duration).toBeCloseTo(10);
  });

  test('equality', () => {
    const span = new HPTimeSpan(hptime('10'), 10);
    expect(span.equals(span)).toBe(true);
    expect(span.equals(new HPTimeSpan(hptime('10'), 10.5))).toBe(false);
    expect(span.equals(new HPTimeSpan(hptime('10.1'), 10))).toBe(false);
  });

  test('contains', () => {
    const span = new HPTimeSpan(hptime('10'), 10);
    expect(span.contains(t(9n))).toBe(false);
    expect(span.contains(t(10n))).toBe(true);
    expect(span.contains(t(19n))).toBe(true);
    expect(span.contains(t(20n))).toBe(false);
  });

  test('containsSpan', () => {
    const span = new HPTimeSpan(hptime('10'), 10);
    expect(span.containsSpan(t(9n), t(15n))).toBe(false);
    expect(span.containsSpan(t(10n), t(15n))).toBe(true);
    expect(span.containsSpan(t(15n), t(20n))).toBe(true);
    expect(span.containsSpan(t(15n), t(21n))).toBe(false);
    expect(span.containsSpan(t(30n), t(40n))).toBe(false);
  });

  test('overlapsSpan', () => {
    const span = new HPTimeSpan(hptime('10'), 10);
    expect(span.overlaps(t(9n), t(10n))).toBe(false);
    expect(span.overlaps(t(9n), t(11n))).toBe(true);
    expect(span.overlaps(t(19n), t(21n))).toBe(true);
    expect(span.overlaps(t(20n), t(21n))).toBe(false);
  });
});
