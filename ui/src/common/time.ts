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

import {BigintMath} from '../base/bigint_math';
import {assertTrue} from '../base/logging';
import {asTPTimestamp, toTraceTime} from '../frontend/sql_types';

import {ColumnType} from './query_result';

// Print time to a few significant figures.
// Use this when readability is more desireable than precision.
// Examples: 1234 -> 1.23ns
//           123456789 -> 123ms
//           123,123,123,123,123 -> 34h 12m
//           1,000,000,023 -> 1 s
//           1,230,000,023 -> 1.2 s
export function formatDurationShort(time: TPTime) {
  const sec = tpTimeToSeconds(time);
  const units = ['s', 'ms', 'us', 'ns'];
  const sign = Math.sign(sec);
  let n = Math.abs(sec);
  let u = 0;
  while (n < 1 && n !== 0 && u < units.length - 1) {
    n *= 1000;
    u++;
  }
  return `${sign < 0 ? '-' : ''}${Math.round(n * 10) / 10}${units[u]}`;
}

// Print time with absolute precision.
// TODO(stevegolton): Merge this with formatDurationShort
export function formatDuration(time: TPTime): string {
  let result = '';
  if (time < 1) return '0s';
  const unitAndValue: [string, bigint][] = [
    ['m', 60000000000n],
    ['s', 1000000000n],
    ['ms', 1000000n],
    ['us', 1000n],
    ['ns', 1n],
  ];
  unitAndValue.forEach(([unit, unitSize]) => {
    if (time >= unitSize) {
      const unitCount = time / unitSize;
      result += unitCount.toLocaleString() + unit + ' ';
      time %= unitSize;
    }
  });
  return result.slice(0, -1);
}

// This class takes a time and converts it to a set of strings representing a
// time code where each string represents a group of time units formatted with
// an appropriate number of leading zeros.
export class Timecode {
  public readonly sign: string;
  public readonly days: string;
  public readonly hours: string;
  public readonly minutes: string;
  public readonly seconds: string;
  public readonly millis: string;
  public readonly micros: string;
  public readonly nanos: string;

  constructor(time: TPTime) {
    this.sign = time < 0 ? '-' : '';

    const absTime = BigintMath.abs(time);

    const days = (absTime / 86_400_000_000_000n);
    const hours = (absTime / 3_600_000_000_000n) % 24n;
    const minutes = (absTime / 60_000_000_000n) % 60n;
    const seconds = (absTime / 1_000_000_000n) % 60n;
    const millis = (absTime / 1_000_000n) % 1_000n;
    const micros = (absTime / 1_000n) % 1_000n;
    const nanos = absTime % 1_000n;

    this.days = days.toString();
    this.hours = hours.toString().padStart(2, '0');
    this.minutes = minutes.toString().padStart(2, '0');
    this.seconds = seconds.toString().padStart(2, '0');
    this.millis = millis.toString().padStart(3, '0');
    this.micros = micros.toString().padStart(3, '0');
    this.nanos = nanos.toString().padStart(3, '0');
  }

  // Get the upper part of the timecode formatted as: [-]DdHH:MM:SS.
  get dhhmmss(): string {
    const days = this.days === '0' ? '' : `${this.days}d`;
    return `${this.sign}${days}${this.hours}:${this.minutes}:${this.seconds}`;
  }

  // Get the subsecond part of the timecode formatted as: mmm uuu nnn.
  // The "space" char is configurable but defaults to a normal space.
  subsec(spaceChar: string = ' '): string {
    return `${this.millis}${spaceChar}${this.micros}${spaceChar}${this.nanos}`;
  }

  // Formats the entire timecode to a string.
  toString(spaceChar: string = ' '): string {
    return `${this.dhhmmss}.${this.subsec(spaceChar)}`;
  }
}

// Single entry point where timestamps can be converted to the globally
// configured domain.
// In the future this will be configurable.
export function toDomainTime(time: TPTime): TPTime {
  return toTraceTime(asTPTimestamp(time));
}

export function toNs(seconds: number) {
  return Math.round(seconds * 1e9);
}

export function currentDateHourAndMinute(): string {
  const date = new Date();
  return `${date.toISOString().substr(0, 10)}-${date.getHours()}-${
      date.getMinutes()}`;
}

// Aliased "Trace Processor" time and duration types.
// Note(stevegolton): While it might be nice to type brand these in the future,
// for now we're going to keep things simple. We do a lot of maths with these
// timestamps and type branding requires a lot of jumping through hoops to
// coerse the type back to the correct format.
export type TPTime = bigint;
export type TPDuration = bigint;

export function tpTimeFromNanos(nanos: number): TPTime {
  return BigInt(Math.floor(nanos));
}

export function tpTimeFromSeconds(seconds: number): TPTime {
  return BigInt(Math.floor(seconds * 1e9));
}

export function tpTimeToNanos(time: TPTime): number {
  return Number(time);
}

export function tpTimeToMillis(time: TPTime): number {
  return Number(time) / 1e6;
}

export function tpTimeToSeconds(time: TPTime): number {
  return Number(time) / 1e9;
}

// Create a TPTime from an arbitrary SQL value.
// Throws if the value cannot be reasonably converted to a bigint.
// Assumes value is in nanoseconds.
export function tpTimeFromSql(value: ColumnType): TPTime {
  if (typeof value === 'bigint') {
    return value;
  } else if (typeof value === 'number') {
    return tpTimeFromNanos(value);
  } else if (value === null) {
    return 0n;
  } else {
    throw Error(`Refusing to create Timestamp from unrelated type ${value}`);
  }
}

export function tpDurationToSeconds(dur: TPDuration): number {
  return tpTimeToSeconds(dur);
}

export function tpDurationToNanos(dur: TPDuration): number {
  return tpTimeToSeconds(dur);
}

export function tpDurationFromNanos(nanos: number): TPDuration {
  return tpTimeFromNanos(nanos);
}

export function tpDurationFromSql(nanos: ColumnType): TPDuration {
  return tpTimeFromSql(nanos);
}

export interface Span<Unit, Duration = Unit> {
  get start(): Unit;
  get end(): Unit;
  get duration(): Duration;
  get midpoint(): Unit;
  contains(span: Unit|Span<Unit, Duration>): boolean;
  intersects(x: Span<Unit>): boolean;
  equals(span: Span<Unit, Duration>): boolean;
  add(offset: Duration): Span<Unit, Duration>;
  pad(padding: Duration): Span<Unit, Duration>;
}

export class TPTimeSpan implements Span<TPTime, TPDuration> {
  readonly start: TPTime;
  readonly end: TPTime;

  constructor(start: TPTime, end: TPTime) {
    assertTrue(
        start <= end,
        `Span start [${start}] cannot be greater than end [${end}]`);
    this.start = start;
    this.end = end;
  }

  get duration(): TPDuration {
    return this.end - this.start;
  }

  get midpoint(): TPTime {
    return (this.start + this.end) / 2n;
  }

  contains(x: TPTime|Span<TPTime, TPDuration>): boolean {
    if (typeof x === 'bigint') {
      return this.start <= x && x < this.end;
    } else {
      return this.start <= x.start && x.end <= this.end;
    }
  }

  intersects(x: Span<TPTime, TPDuration>): boolean {
    return !(x.end <= this.start || x.start >= this.end);
  }

  equals(span: Span<TPTime, TPDuration>): boolean {
    return this.start === span.start && this.end === span.end;
  }

  add(x: TPTime): Span<TPTime, TPDuration> {
    return new TPTimeSpan(this.start + x, this.end + x);
  }

  pad(padding: TPDuration): Span<TPTime, TPDuration> {
    return new TPTimeSpan(this.start - padding, this.end + padding);
  }
}
