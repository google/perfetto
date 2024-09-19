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

import {Time, time} from './time';
import {HighPrecisionTime as HPTime} from './high_precision_time';

const t = Time.fromRaw;

// Quick 'n' dirty function to convert a string to a HPtime
// Used to make tests more readable
// E.g. '1.3' -> {base: 1, offset: 0.3}
// E.g. '-0.3' -> {base: -1, offset: 0.7}
function mkTime(time: string): HPTime {
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

describe('Time', () => {
  it('should create a new Time object with the given base and offset', () => {
    const time = new HPTime(t(136n), 0.3);
    expect(time.integral).toBe(136n);
    expect(time.fractional).toBeCloseTo(0.3);
  });

  it('should normalize when offset is >= 1', () => {
    let time = new HPTime(t(1n), 2.3);
    expect(time.integral).toBe(3n);
    expect(time.fractional).toBeCloseTo(0.3);

    time = new HPTime(t(1n), 1);
    expect(time.integral).toBe(2n);
    expect(time.fractional).toBeCloseTo(0);
  });

  it('should normalize when offset is < 0', () => {
    const time = new HPTime(t(1n), -0.4);
    expect(time.integral).toBe(0n);
    expect(time.fractional).toBeCloseTo(0.6);
  });

  it('should store timestamps without losing precision', () => {
    const time = new HPTime(t(1152921504606846976n));
    expect(time.toTime()).toBe(1152921504606846976n as time);
  });

  it('should store and manipulate timestamps without losing precision', () => {
    let time = new HPTime(t(2315700508990407843n));
    time = time.addTime(2315718101717517451n as time);
    expect(time.toTime()).toBe(4631418610707925294n);
  });

  test('add', () => {
    const result = mkTime('1.3').add(mkTime('3.1'));
    expect(result.integral).toEqual(4n);
    expect(result.fractional).toBeCloseTo(0.4);
  });

  test('addTime', () => {
    const result = mkTime('200.334').addTime(t(150n));
    expect(result.integral).toBe(350n);
    expect(result.fractional).toBeCloseTo(0.334);
  });

  test('addNumber', () => {
    const result = mkTime('200.334').addNumber(150.5);
    expect(result.integral).toBe(350n);
    expect(result.fractional).toBeCloseTo(0.834);
  });

  test('sub', () => {
    const result = mkTime('1.3').sub(mkTime('3.1'));
    expect(result.integral).toEqual(-2n);
    expect(result.fractional).toBeCloseTo(0.2);
  });

  test('addTime', () => {
    const result = mkTime('200.334').subTime(t(150n));
    expect(result.integral).toBe(50n);
    expect(result.fractional).toBeCloseTo(0.334);
  });

  test('subNumber', () => {
    const result = mkTime('200.334').subNumber(150.5);
    expect(result.integral).toBe(49n);
    expect(result.fractional).toBeCloseTo(0.834);
  });

  test('gte', () => {
    expect(mkTime('1.0').gte(t(1n))).toBe(true);
    expect(mkTime('1.0').gte(t(2n))).toBe(false);
    expect(mkTime('1.2').gte(t(1n))).toBe(true);
    expect(mkTime('1.2').gte(t(2n))).toBe(false);
  });

  test('gt', () => {
    expect(mkTime('1.0').gt(t(1n))).toBe(false);
    expect(mkTime('1.0').gt(t(2n))).toBe(false);
    expect(mkTime('1.2').gt(t(1n))).toBe(true);
    expect(mkTime('1.2').gt(t(2n))).toBe(false);
  });

  test('lte', () => {
    expect(mkTime('1.0').lte(t(0n))).toBe(false);
    expect(mkTime('1.0').lte(t(1n))).toBe(true);
    expect(mkTime('1.0').lte(t(2n))).toBe(true);
    expect(mkTime('1.2').lte(t(1n))).toBe(false);
    expect(mkTime('1.2').lte(t(2n))).toBe(true);
  });

  test('lt', () => {
    expect(mkTime('1.0').lt(t(0n))).toBe(false);
    expect(mkTime('1.0').lt(t(1n))).toBe(false);
    expect(mkTime('1.0').lt(t(2n))).toBe(true);
    expect(mkTime('1.2').lt(t(1n))).toBe(false);
    expect(mkTime('1.2').lt(t(2n))).toBe(true);
  });

  test('equals', () => {
    const time = new HPTime(t(1n), 0.2);
    expect(time.equals(new HPTime(t(1n), 0.2))).toBeTruthy();
    expect(time.equals(new HPTime(t(0n), 1.2))).toBeTruthy();
    expect(time.equals(new HPTime(t(-100n), 101.2))).toBeTruthy();
    expect(time.equals(new HPTime(t(1n), 0.3))).toBeFalsy();
    expect(time.equals(new HPTime(t(2n), 0.2))).toBeFalsy();
  });

  test('containedWithin', () => {
    expect(mkTime('0.9').containedWithin(t(1n), t(2n))).toBe(false);
    expect(mkTime('1.0').containedWithin(t(1n), t(2n))).toBe(true);
    expect(mkTime('1.2').containedWithin(t(1n), t(2n))).toBe(true);
    expect(mkTime('2.0').containedWithin(t(1n), t(2n))).toBe(false);
    expect(mkTime('2.1').containedWithin(t(1n), t(2n))).toBe(false);
  });

  test('clamp', () => {
    let result = mkTime('1.2').clamp(t(1n), t(2n));
    expect(result.integral).toBe(1n);
    expect(result.fractional).toBeCloseTo(0.2);

    result = mkTime('2.2').clamp(t(1n), t(2n));
    expect(result.integral).toBe(2n);
    expect(result.fractional).toBeCloseTo(0);

    result = mkTime('0.2').clamp(t(1n), t(2n));
    expect(result.integral).toBe(1n);
    expect(result.fractional).toBeCloseTo(0);
  });

  test('toNumber', () => {
    expect(new HPTime(t(1n), 0.2).toNumber()).toBeCloseTo(1.2);
    expect(new HPTime(t(1000000000n), 0.0).toNumber()).toBeCloseTo(1e9);
  });

  test('toTime', () => {
    expect(new HPTime(t(1n), 0.2).toTime('round')).toBe(1n);
    expect(new HPTime(t(1n), 0.5).toTime('round')).toBe(2n);
    expect(new HPTime(t(1n), 0.2).toTime('floor')).toBe(1n);
    expect(new HPTime(t(1n), 0.5).toTime('floor')).toBe(1n);
    expect(new HPTime(t(1n), 0.2).toTime('ceil')).toBe(2n);
    expect(new HPTime(t(1n), 0.5).toTime('ceil')).toBe(2n);
  });

  test('toString', () => {
    expect(mkTime('1.3').toString()).toBe('1.3');
    expect(mkTime('12983423847.332533').toString()).toBe('12983423847.332533');
    expect(new HPTime(t(234n)).toString()).toBe('234');
  });

  test('abs', () => {
    let result = mkTime('-0.7').abs();
    expect(result.integral).toEqual(0n);
    expect(result.fractional).toBeCloseTo(0.7);

    result = mkTime('-1.3').abs();
    expect(result.integral).toEqual(1n);
    expect(result.fractional).toBeCloseTo(0.3);

    result = mkTime('-100').abs();
    expect(result.integral).toEqual(100n);
    expect(result.fractional).toBeCloseTo(0);

    result = mkTime('34.5345').abs();
    expect(result.integral).toEqual(34n);
    expect(result.fractional).toBeCloseTo(0.5345);
  });
});
