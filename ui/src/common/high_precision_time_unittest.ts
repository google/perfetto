// Copyright (C) 2023 The Android Open Source Project
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

import {Time, time} from '../base/time';

import {
  HighPrecisionTime as HPTime,
  HighPrecisionTimeSpan as HPTimeInterval,
} from './high_precision_time';

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
      return new HPTime(numBase - 1n, 1.0 - numFractions);
    } else {
      return new HPTime(numBase, numFractions);
    }
  } else {
    return new HPTime(numBase);
  }
}

function mkSpan(t1: string, t2: string): HPTimeInterval {
  return new HPTimeInterval(mkTime(t1), mkTime(t2));
}

describe('Time', () => {
  it('should create a new Time object with the given base and offset', () => {
    const time = new HPTime(136n, 0.3);
    expect(time.base).toBe(136n);
    expect(time.offset).toBeCloseTo(0.3);
  });

  it('should normalize when offset is >= 1', () => {
    let time = new HPTime(1n, 2.3);
    expect(time.base).toBe(3n);
    expect(time.offset).toBeCloseTo(0.3);

    time = new HPTime(1n, 1);
    expect(time.base).toBe(2n);
    expect(time.offset).toBeCloseTo(0);
  });

  it('should normalize when offset is < 0', () => {
    const time = new HPTime(1n, -0.4);
    expect(time.base).toBe(0n);
    expect(time.offset).toBeCloseTo(0.6);
  });

  it('should store timestamps without losing precision', () => {
    let time = HPTime.fromTime(123n as time);
    expect(time.toTime()).toBe(123n as time);

    time = HPTime.fromTime(1152921504606846976n as time);
    expect(time.toTime()).toBe(1152921504606846976n as time);
  });

  it('should store and manipulate timestamps without losing precision', () => {
    let time = HPTime.fromTime(123n as time);
    time = time.addTime(Time.fromRaw(456n));
    expect(time.toTime()).toBe(579n);

    time = HPTime.fromTime(2315700508990407843n as time);
    time = time.addTime(2315718101717517451n as time);
    expect(time.toTime()).toBe(4631418610707925294n);
  });

  it('should add time', () => {
    const time1 = mkTime('1.3');
    const time2 = mkTime('3.1');
    const result = time1.add(time2);
    expect(result.base).toEqual(4n);
    expect(result.offset).toBeCloseTo(0.4);
  });

  it('should subtract time', () => {
    const time1 = mkTime('3.1');
    const time2 = mkTime('1.3');
    const result = time1.sub(time2);
    expect(result.base).toEqual(1n);
    expect(result.offset).toBeCloseTo(0.8);
  });

  it('should add nanoseconds', () => {
    const time = mkTime('1.3');
    const result = time.addNanos(0.8);
    expect(result.base).toEqual(2n);
    expect(result.offset).toBeCloseTo(0.1);
  });

  it('should add seconds', () => {
    const time = mkTime('1.3');
    const result = time.addSeconds(0.008);
    expect(result.base).toEqual(8000001n);
    expect(result.offset).toBeCloseTo(0.3);
  });

  it('should perform gte comparisions', () => {
    const time = mkTime('1.2');
    expect(time.gte(mkTime('0.5'))).toBeTruthy();
    expect(time.gte(mkTime('1.1'))).toBeTruthy();
    expect(time.gte(mkTime('1.2'))).toBeTruthy();
    expect(time.gte(mkTime('1.5'))).toBeFalsy();
    expect(time.gte(mkTime('5.5'))).toBeFalsy();
  });

  it('should perform gt comparisions', () => {
    const time = mkTime('1.2');
    expect(time.gt(mkTime('0.5'))).toBeTruthy();
    expect(time.gt(mkTime('1.1'))).toBeTruthy();
    expect(time.gt(mkTime('1.2'))).toBeFalsy();
    expect(time.gt(mkTime('1.5'))).toBeFalsy();
    expect(time.gt(mkTime('5.5'))).toBeFalsy();
  });

  it('should perform lt comparisions', () => {
    const time = mkTime('1.2');
    expect(time.lt(mkTime('0.5'))).toBeFalsy();
    expect(time.lt(mkTime('1.1'))).toBeFalsy();
    expect(time.lt(mkTime('1.2'))).toBeFalsy();
    expect(time.lt(mkTime('1.5'))).toBeTruthy();
    expect(time.lt(mkTime('5.5'))).toBeTruthy();
  });

  it('should perform lte comparisions', () => {
    const time = mkTime('1.2');
    expect(time.lte(mkTime('0.5'))).toBeFalsy();
    expect(time.lte(mkTime('1.1'))).toBeFalsy();
    expect(time.lte(mkTime('1.2'))).toBeTruthy();
    expect(time.lte(mkTime('1.5'))).toBeTruthy();
    expect(time.lte(mkTime('5.5'))).toBeTruthy();
  });

  it('should detect equality', () => {
    const time = new HPTime(1n, 0.2);
    expect(time.eq(new HPTime(1n, 0.2))).toBeTruthy();
    expect(time.eq(new HPTime(0n, 1.2))).toBeTruthy();
    expect(time.eq(new HPTime(-100n, 101.2))).toBeTruthy();
    expect(time.eq(new HPTime(1n, 0.3))).toBeFalsy();
    expect(time.eq(new HPTime(2n, 0.2))).toBeFalsy();
  });

  it('should clamp a time to a range', () => {
    const time1 = mkTime('1.2');
    const time2 = mkTime('5.4');
    const time3 = mkTime('2.8');
    const lower = mkTime('2.3');
    const upper = mkTime('4.5');
    expect(time1.clamp(lower, upper)).toEqual(lower);
    expect(time2.clamp(lower, upper)).toEqual(upper);
    expect(time3.clamp(lower, upper)).toEqual(time3);
  });

  it('should convert to seconds', () => {
    expect(new HPTime(1n, .2).seconds).toBeCloseTo(0.0000000012);
    expect(new HPTime(1000000000n, .0).seconds).toBeCloseTo(1);
  });

  it('should convert to nanos', () => {
    expect(new HPTime(1n, .2).nanos).toBeCloseTo(1.2);
    expect(new HPTime(1000000000n, .0).nanos).toBeCloseTo(1e9);
  });

  it('should convert to timestamps', () => {
    expect(new HPTime(1n, .2).toTime('round')).toBe(1n);
    expect(new HPTime(1n, .5).toTime('round')).toBe(2n);
    expect(new HPTime(1n, .2).toTime('floor')).toBe(1n);
    expect(new HPTime(1n, .5).toTime('floor')).toBe(1n);
    expect(new HPTime(1n, .2).toTime('ceil')).toBe(2n);
    expect(new HPTime(1n, .5).toTime('ceil')).toBe(2n);
  });

  it('should divide', () => {
    let result = mkTime('1').divide(2);
    expect(result.base).toBe(0n);
    expect(result.offset).toBeCloseTo(0.5);

    result = mkTime('1.6').divide(2);
    expect(result.base).toBe(0n);
    expect(result.offset).toBeCloseTo(0.8);

    result = mkTime('-0.5').divide(2);
    expect(result.base).toBe(-1n);
    expect(result.offset).toBeCloseTo(0.75);

    result = mkTime('123.1').divide(123);
    expect(result.base).toBe(1n);
    expect(result.offset).toBeCloseTo(0.000813, 6);
  });

  it('should multiply', () => {
    let result = mkTime('1').multiply(2);
    expect(result.base).toBe(2n);
    expect(result.offset).toBeCloseTo(0);

    result = mkTime('1').multiply(2.5);
    expect(result.base).toBe(2n);
    expect(result.offset).toBeCloseTo(0.5);

    result = mkTime('-0.5').multiply(2);
    expect(result.base).toBe(-1n);
    expect(result.offset).toBeCloseTo(0.0);

    result = mkTime('123.1').multiply(25.5);
    expect(result.base).toBe(3139n);
    expect(result.offset).toBeCloseTo(0.05);
  });

  it('should convert to string', () => {
    expect(mkTime('1.3').toString()).toBe('1.3');
    expect(mkTime('12983423847.332533').toString()).toBe('12983423847.332533');
    expect(new HPTime(234n).toString()).toBe('234');
  });

  it('should calculate absolute', () => {
    let result = mkTime('-0.7').abs();
    expect(result.base).toEqual(0n);
    expect(result.offset).toBeCloseTo(0.7);

    result = mkTime('-1.3').abs();
    expect(result.base).toEqual(1n);
    expect(result.offset).toBeCloseTo(0.3);

    result = mkTime('-100').abs();
    expect(result.base).toEqual(100n);
    expect(result.offset).toBeCloseTo(0);

    result = mkTime('34.5345').abs();
    expect(result.base).toEqual(34n);
    expect(result.offset).toBeCloseTo(0.5345);
  });
});

describe('HighPrecisionTimeSpan', () => {
  it('can be constructed from HP time', () => {
    const span = new HPTimeInterval(mkTime('10'), mkTime('20'));
    expect(span.start).toEqual(mkTime('10'));
    expect(span.end).toEqual(mkTime('20'));
  });

  it('can be constructed from integer time', () => {
    const span = new HPTimeInterval(Time.fromRaw(10n), Time.fromRaw(20n));
    expect(span.start).toEqual(mkTime('10'));
    expect(span.end).toEqual(mkTime('20'));
  });

  it('throws when start is later than end', () => {
    expect(() => new HPTimeInterval(mkTime('0.1'), mkTime('0'))).toThrow();
    expect(() => new HPTimeInterval(mkTime('1124.0001'), mkTime('1124')))
        .toThrow();
  });

  it('can calc duration', () => {
    let dur = mkSpan('10', '20').duration;
    expect(dur.base).toBe(10n);
    expect(dur.offset).toBeCloseTo(0);

    dur = mkSpan('10.123', '20.456').duration;
    expect(dur.base).toBe(10n);
    expect(dur.offset).toBeCloseTo(0.333);
  });

  it('can calc midpoint', () => {
    let mid = mkSpan('10', '20').midpoint;
    expect(mid.base).toBe(15n);
    expect(mid.offset).toBeCloseTo(0);

    mid = mkSpan('10.25', '16.75').midpoint;
    expect(mid.base).toBe(13n);
    expect(mid.offset).toBeCloseTo(0.5);
  });

  it('can be compared', () => {
    expect(mkSpan('0.1', '34.2').equals(mkSpan('0.1', '34.2'))).toBeTruthy();
    expect(mkSpan('0.1', '34.5').equals(mkSpan('0.1', '34.2'))).toBeFalsy();
    expect(mkSpan('0.9', '34.2').equals(mkSpan('0.1', '34.2'))).toBeFalsy();
  });

  it('checks if span contains another span', () => {
    const x = mkSpan('10', '20');

    expect(x.contains(mkTime('9'))).toBeFalsy();
    expect(x.contains(mkTime('10'))).toBeTruthy();
    expect(x.contains(mkTime('15'))).toBeTruthy();
    expect(x.contains(mkTime('20'))).toBeFalsy();
    expect(x.contains(mkTime('21'))).toBeFalsy();

    expect(x.contains(mkSpan('12', '18'))).toBeTruthy();
    expect(x.contains(mkSpan('5', '25'))).toBeFalsy();
    expect(x.contains(mkSpan('5', '15'))).toBeFalsy();
    expect(x.contains(mkSpan('15', '25'))).toBeFalsy();
    expect(x.contains(mkSpan('0', '10'))).toBeFalsy();
    expect(x.contains(mkSpan('20', '30'))).toBeFalsy();
  });

  it('checks if span intersects another span', () => {
    const x = mkSpan('10', '20');

    expect(x.intersectsInterval(mkSpan('0', '10'))).toBeFalsy();
    expect(x.intersectsInterval(mkSpan('5', '15'))).toBeTruthy();
    expect(x.intersectsInterval(mkSpan('12', '18'))).toBeTruthy();
    expect(x.intersectsInterval(mkSpan('15', '25'))).toBeTruthy();
    expect(x.intersectsInterval(mkSpan('20', '30'))).toBeFalsy();
    expect(x.intersectsInterval(mkSpan('5', '25'))).toBeTruthy();
  });

  it('checks intersection', () => {
    const x = mkSpan('10', '20');

    expect(x.intersects(mkTime('0'), mkTime('10'))).toBeFalsy();
    expect(x.intersects(mkTime('5'), mkTime('15'))).toBeTruthy();
    expect(x.intersects(mkTime('12'), mkTime('18'))).toBeTruthy();
    expect(x.intersects(mkTime('15'), mkTime('25'))).toBeTruthy();
    expect(x.intersects(mkTime('20'), mkTime('30'))).toBeFalsy();
    expect(x.intersects(mkTime('5'), mkTime('25'))).toBeTruthy();
  });
});
