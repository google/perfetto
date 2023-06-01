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

import {assertTrue} from '../base/logging';
import {ColumnType} from './query_result';

// TODO(hjd): Combine with timeToCode.
export function timeToString(sec: number) {
  const units = ['s', 'ms', 'us', 'ns'];
  const sign = Math.sign(sec);
  let n = Math.abs(sec);
  let u = 0;
  while (n < 1 && n !== 0 && u < units.length - 1) {
    n *= 1000;
    u++;
  }
  return `${sign < 0 ? '-' : ''}${Math.round(n * 10) / 10} ${units[u]}`;
}

export function tpTimeToString(time: TPTime) {
  // TODO(stevegolton): Write a formatter to format bigint timestamps natively.
  return timeToString(tpTimeToSeconds(time));
}

export function fromNs(ns: number) {
  return ns / 1e9;
}

export function toNsFloor(seconds: number) {
  return Math.floor(seconds * 1e9);
}

export function toNsCeil(seconds: number) {
  return Math.ceil(seconds * 1e9);
}

export function toNs(seconds: number) {
  return Math.round(seconds * 1e9);
}

// 1000000023ns -> "1.000 000 023"
export function formatTimestamp(sec: number) {
  const parts = sec.toFixed(9).split('.');
  parts[1] = parts[1].replace(/\B(?=(\d{3})+(?!\d))/g, ' ');
  return parts.join('.');
}

export function formatTPTime(time: TPTime) {
  // TODO(stevegolton): Write a formatter to format bigint timestamps natively.
  return formatTimestamp(tpTimeToSeconds(time));
}

// TODO(hjd): Rename to formatTimestampWithUnits
// 1000000023ns -> "1s 23ns"
export function timeToCode(sec: number): string {
  let result = '';
  let ns = Math.round(sec * 1e9);
  if (ns < 1) return '0s';
  const unitAndValue = [
    ['m', 60000000000],
    ['s', 1000000000],
    ['ms', 1000000],
    ['us', 1000],
    ['ns', 1],
  ];
  unitAndValue.forEach((pair) => {
    const unit = pair[0] as string;
    const val = pair[1] as number;
    if (ns >= val) {
      const i = Math.floor(ns / val);
      ns -= i * val;
      result += i.toLocaleString() + unit + ' ';
    }
  });
  return result.slice(0, -1);
}

export function tpTimeToCode(time: TPTime) {
  // TODO(stevegolton): Write a formatter to format bigint timestamps natively.
  return timeToCode(tpTimeToSeconds(time));
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
