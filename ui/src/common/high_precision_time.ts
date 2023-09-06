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

import {assertTrue} from '../base/logging';
import {Span, Time, time} from '../base/time';

export type RoundMode = 'round'|'floor'|'ceil';
export type Timeish = HighPrecisionTime|time;

// Stores a time as a bigint and an offset which is capable of:
// - Storing and reproducing "Time"s without losing precision.
// - Storing time with sub-nanosecond precision.
// This class is immutable - each operation returns a new object.
export class HighPrecisionTime {
  // Time in nanoseconds == base + offset
  // offset is kept in the range 0 <= x < 1 to avoid losing precision
  readonly base: bigint;
  readonly offset: number;

  static get ZERO(): HighPrecisionTime {
    return new HighPrecisionTime(0n);
  }

  constructor(base: bigint = 0n, offset: number = 0) {
    // Normalize offset to sit in the range 0.0 <= x < 1.0
    const offsetFloor = Math.floor(offset);
    this.base = base + BigInt(offsetFloor);
    this.offset = offset - offsetFloor;
  }

  static fromTime(timestamp: time): HighPrecisionTime {
    return new HighPrecisionTime(timestamp, 0);
  }

  static fromNanos(nanos: number|bigint) {
    if (typeof nanos === 'number') {
      return new HighPrecisionTime(0n, nanos);
    } else if (typeof nanos === 'bigint') {
      return new HighPrecisionTime(nanos);
    } else {
      const value: never = nanos;
      throw new Error(`Value ${value} is neither a number nor a bigint`);
    }
  }

  static fromSeconds(seconds: number) {
    const nanos = seconds * 1e9;
    const offset = nanos - Math.floor(nanos);
    return new HighPrecisionTime(BigInt(Math.floor(nanos)), offset);
  }

  static max(a: HighPrecisionTime, b: HighPrecisionTime): HighPrecisionTime {
    return a.gt(b) ? a : b;
  }

  static min(a: HighPrecisionTime, b: HighPrecisionTime): HighPrecisionTime {
    return a.lt(b) ? a : b;
  }

  toTime(roundMode: RoundMode = 'floor'): time {
    switch (roundMode) {
      case 'round':
        return Time.fromRaw(this.base + BigInt(Math.round(this.offset)));
      case 'floor':
        return Time.fromRaw(this.base);
      case 'ceil':
        return Time.fromRaw(this.base + BigInt(Math.ceil(this.offset)));
      default:
        const exhaustiveCheck: never = roundMode;
        throw new Error(`Unhandled roundMode case: ${exhaustiveCheck}`);
    }
  }

  get nanos(): number {
    // WARNING: Number(bigint) can be surprisingly slow.
    // WARNING: Precision may be lost here.
    return Number(this.base) + this.offset;
  }

  get seconds(): number {
    // WARNING: Number(bigint) can be surprisingly slow.
    // WARNING: Precision may be lost here.
    return (Number(this.base) + this.offset) / 1e9;
  }

  add(other: HighPrecisionTime): HighPrecisionTime {
    return new HighPrecisionTime(
        this.base + other.base, this.offset + other.offset);
  }

  addNanos(nanos: number|bigint): HighPrecisionTime {
    return this.add(HighPrecisionTime.fromNanos(nanos));
  }

  addSeconds(seconds: number): HighPrecisionTime {
    return new HighPrecisionTime(this.base, this.offset + seconds * 1e9);
  }

  addTime(ts: time): HighPrecisionTime {
    return new HighPrecisionTime(this.base + ts, this.offset);
  }

  sub(other: HighPrecisionTime): HighPrecisionTime {
    return new HighPrecisionTime(
        this.base - other.base, this.offset - other.offset);
  }

  subTime(ts: time): HighPrecisionTime {
    return new HighPrecisionTime(this.base - ts, this.offset);
  }

  subNanos(nanos: number|bigint): HighPrecisionTime {
    return this.add(HighPrecisionTime.fromNanos(-nanos));
  }

  divide(divisor: number): HighPrecisionTime {
    return this.multiply(1 / divisor);
  }

  multiply(factor: number): HighPrecisionTime {
    const factorFloor = Math.floor(factor);
    const newBase = this.base * BigInt(factorFloor);
    const additionalBit = Number(this.base) * (factor - factorFloor);
    const newOffset = factor * this.offset + additionalBit;
    return new HighPrecisionTime(newBase, newOffset);
  }

  // Return true if other time is within some epsilon, default 1 femtosecond
  eq(other: Timeish, epsilon: number = 1e-6): boolean {
    const x = HighPrecisionTime.fromHPTimeOrTime(other);
    return Math.abs(this.sub(x).nanos) < epsilon;
  }

  private static fromHPTimeOrTime(x: HighPrecisionTime|
                                  time): HighPrecisionTime {
    if (x instanceof HighPrecisionTime) {
      return x;
    } else if (typeof x === 'bigint') {
      return HighPrecisionTime.fromTime(x);
    } else {
      const y: never = x;
      throw new Error(`Invalid type ${y}`);
    }
  }

  lt(other: Timeish): boolean {
    const x = HighPrecisionTime.fromHPTimeOrTime(other);
    if (this.base < x.base) {
      return true;
    } else if (this.base === x.base) {
      return this.offset < x.offset;
    } else {
      return false;
    }
  }

  lte(other: Timeish): boolean {
    if (this.eq(other)) {
      return true;
    } else {
      return this.lt(other);
    }
  }

  gt(other: Timeish): boolean {
    return !this.lte(other);
  }

  gte(other: Timeish): boolean {
    return !this.lt(other);
  }

  clamp(lower: HighPrecisionTime, upper: HighPrecisionTime): HighPrecisionTime {
    if (this.lt(lower)) {
      return lower;
    } else if (this.gt(upper)) {
      return upper;
    } else {
      return this;
    }
  }

  toString(): string {
    const offsetAsString = this.offset.toString();
    if (offsetAsString === '0') {
      return this.base.toString();
    } else {
      return `${this.base}${offsetAsString.substring(1)}`;
    }
  }

  abs(): HighPrecisionTime {
    if (this.base >= 0n) {
      return this;
    }
    const newBase = -this.base;
    const newOffset = -this.offset;
    return new HighPrecisionTime(newBase, newOffset);
  }
}

export class HighPrecisionTimeSpan implements Span<HighPrecisionTime> {
  readonly start: HighPrecisionTime;
  readonly end: HighPrecisionTime;

  static readonly ZERO = new HighPrecisionTimeSpan(
      HighPrecisionTime.ZERO,
      HighPrecisionTime.ZERO,
  );

  constructor(start: time|HighPrecisionTime, end: time|HighPrecisionTime) {
    this.start = (start instanceof HighPrecisionTime) ?
        start :
        HighPrecisionTime.fromTime(start);
    this.end = (end instanceof HighPrecisionTime) ?
        end :
        HighPrecisionTime.fromTime(end);
    assertTrue(
        this.start.lte(this.end),
        `TimeSpan start [${this.start}] cannot be greater than end [${
            this.end}]`);
  }

  static fromTime(start: time, end: time): HighPrecisionTimeSpan {
    return new HighPrecisionTimeSpan(
        HighPrecisionTime.fromTime(start),
        HighPrecisionTime.fromTime(end),
    );
  }

  get duration(): HighPrecisionTime {
    return this.end.sub(this.start);
  }

  get midpoint(): HighPrecisionTime {
    return this.start.add(this.end).divide(2);
  }

  equals(other: Span<HighPrecisionTime>): boolean {
    return this.start.eq(other.start) && this.end.eq(other.end);
  }

  contains(x: HighPrecisionTime|Span<HighPrecisionTime>): boolean {
    if (x instanceof HighPrecisionTime) {
      return this.start.lte(x) && x.lt(this.end);
    } else {
      return this.start.lte(x.start) && x.end.lte(this.end);
    }
  }

  intersectsInterval(x: Span<HighPrecisionTime>): boolean {
    return !(x.end.lte(this.start) || x.start.gte(this.end));
  }

  intersects(start: HighPrecisionTime, end: HighPrecisionTime): boolean {
    return !(end.lte(this.start) || start.gte(this.end));
  }

  add(time: HighPrecisionTime): Span<HighPrecisionTime> {
    return new HighPrecisionTimeSpan(this.start.add(time), this.end.add(time));
  }

  // Move the start and end away from each other a certain amount
  pad(time: HighPrecisionTime): Span<HighPrecisionTime> {
    return new HighPrecisionTimeSpan(
        this.start.sub(time),
        this.end.add(time),
    );
  }
}
