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

import {BigintMath} from './bigint_math';
import {Brand} from './brand';
import {assertTrue} from './logging';

// The |time| type represents trace time in the same units and domain as trace
// processor (i.e. typically boot time in nanoseconds, but most of the UI should
// be completely agnostic to this).
export type time = Brand<bigint, 'time'>;

// The |duration| type is used to represent the duration of time between two
// |time|s. The domain is irrelevant because a duration is relative.
export type duration = bigint;

// The conversion factor for converting between different time units.
const TIME_UNITS_PER_SEC = 1e9;
const TIME_UNITS_PER_MILLISEC = 1e6;
const TIME_UNITS_PER_MICROSEC = 1e3;

export class Time {
  // Negative time is never found in a trace - so -1 is commonly used as a flag
  // to represent a value is undefined or unset, without having to use a
  // nullable or union type.
  static readonly INVALID = Time.fromRaw(-1n);

  // The min and max possible values, considering times cannot be negative.
  static readonly MIN = Time.fromRaw(0n);
  static readonly MAX = Time.fromRaw(BigintMath.INT64_MAX);

  static readonly ZERO = Time.fromRaw(0n);

  // Cast a bigint to a |time|. Supports potentially |undefined| values.
  // I.e. it performs the following conversions:
  // - `bigint` -> `time`
  // - `bigint|undefined` -> `time|undefined`
  //
  // Use this function with caution. The function is effectively a no-op in JS,
  // but using it tells TypeScript that "this value is a time value". It's up to
  // the caller to ensure the value is in the correct units and time domain.
  //
  // If you're reaching for this function after doing some maths on a |time|
  // value and it's decayed to a |bigint| consider using the static math methods
  // in |Time| instead, as they will do the appropriate casting for you.
  static fromRaw(v: bigint): time;
  static fromRaw(v?: bigint): time | undefined;
  static fromRaw(v?: bigint): time | undefined {
    return v as time | undefined;
  }

  // Convert seconds (number) to a time value.
  // Note: number -> BigInt conversion is relatively slow.
  static fromSeconds(seconds: number): time {
    return Time.fromRaw(BigInt(Math.floor(seconds * TIME_UNITS_PER_SEC)));
  }

  // Convert time value to seconds and return as a number (i.e. float).
  // Warning: This function is lossy, i.e. precision is lost when converting
  // BigInt -> number.
  // Note: BigInt -> number conversion is relatively slow.
  static toSeconds(t: time): number {
    return Number(t) / TIME_UNITS_PER_SEC;
  }

  // Convert milliseconds (number) to a time value.
  // Note: number -> BigInt conversion is relatively slow.
  static fromMillis(millis: number): time {
    return Time.fromRaw(BigInt(Math.floor(millis * TIME_UNITS_PER_MILLISEC)));
  }

  // Convert time value to milliseconds and return as a number (i.e. float).
  // Warning: This function is lossy, i.e. precision is lost when converting
  // BigInt -> number.
  // Note: BigInt -> number conversion is relatively slow.
  static toMillis(t: time): number {
    return Number(t) / TIME_UNITS_PER_MILLISEC;
  }

  // Convert microseconds (number) to a time value.
  // Note: number -> BigInt conversion is relatively slow.
  static fromMicros(millis: number): time {
    return Time.fromRaw(BigInt(Math.floor(millis * TIME_UNITS_PER_MICROSEC)));
  }

  // Convert time value to microseconds and return as a number (i.e. float).
  // Warning: This function is lossy, i.e. precision is lost when converting
  // BigInt -> number.
  // Note: BigInt -> number conversion is relatively slow.
  static toMicros(t: time): number {
    return Number(t) / TIME_UNITS_PER_MICROSEC;
  }

  // Convert a Date object to a time value, given an offset from the unix epoch.
  // Note: number -> BigInt conversion is relatively slow.
  static fromDate(d: Date, offset: duration): time {
    const millis = d.getTime();
    const t = Time.fromMillis(millis);
    return Time.add(t, offset);
  }

  // Convert time value to a Date object, given an offset from the unix epoch.
  // Warning: This function is lossy, i.e. precision is lost when converting
  // BigInt -> number.
  // Note: BigInt -> number conversion is relatively slow.
  static toDate(t: time, offset: duration): Date {
    const timeSinceEpoch = Time.sub(t, offset);
    const millis = Time.toMillis(timeSinceEpoch);
    return new Date(millis);
  }

  // Find the closest previous midnight for a given time value.
  static getLatestMidnight(time: time, offset: duration): time {
    const date = Time.toDate(time, offset);
    const floorDay = new Date(
      Date.UTC(date.getUTCFullYear(), date.getUTCMonth(), date.getUTCDate()),
    );

    return Time.fromDate(floorDay, offset);
  }

  static add(t: time, d: duration): time {
    return Time.fromRaw(t + d);
  }

  static sub(t: time, d: duration): time {
    return Time.fromRaw(t - d);
  }

  static diff(a: time, b: time): duration {
    return a - b;
  }

  static min(a: time, b: time): time {
    return Time.fromRaw(BigintMath.min(a, b));
  }

  static max(a: time, b: time): time {
    return Time.fromRaw(BigintMath.max(a, b));
  }

  static quantFloor(a: time, b: duration): time {
    return Time.fromRaw(BigintMath.quantFloor(a, b));
  }

  static quantCeil(a: time, b: duration): time {
    return Time.fromRaw(BigintMath.quantCeil(a, b));
  }

  static quant(a: time, b: duration): time {
    return Time.fromRaw(BigintMath.quant(a, b));
  }

  static formatSeconds(time: time): string {
    return Time.toSeconds(time).toString() + ' s';
  }

  static formatMilliseconds(time: time): string {
    return Time.toMillis(time).toString() + ' ms';
  }

  static formatMicroseconds(time: time): string {
    return Time.toMicros(time).toString() + ' us';
  }

  static toTimecode(time: time): Timecode {
    return new Timecode(time);
  }
}

export class Duration {
  // The min and max possible duration values - durations can be negative.
  static MIN = BigintMath.INT64_MIN;
  static MAX = BigintMath.INT64_MAX;
  static ZERO = 0n;

  // Cast a bigint to a |duration|. Supports potentially |undefined| values.
  // I.e. it performs the following conversions:
  // - `bigint` -> `duration`
  // - `bigint|undefined` -> `duration|undefined`
  //
  // Use this function with caution. The function is effectively a no-op in JS,
  // but using it tells TypeScript that "this value is a duration value". It's
  // up to the caller to ensure the value is in the correct units.
  //
  // If you're reaching for this function after doing some maths on a |duration|
  // value and it's decayed to a |bigint| consider using the static math methods
  // in |duration| instead, as they will do the appropriate casting for you.
  static fromRaw(v: bigint): duration;
  static fromRaw(v?: bigint): duration | undefined;
  static fromRaw(v?: bigint): duration | undefined {
    return v as duration | undefined;
  }

  static min(a: duration, b: duration): duration {
    return BigintMath.min(a, b);
  }

  static max(a: duration, b: duration): duration {
    return BigintMath.max(a, b);
  }

  static fromMillis(millis: number) {
    return BigInt(Math.floor((millis / 1e3) * TIME_UNITS_PER_SEC));
  }

  // Convert time to seconds as a number.
  // Use this function with caution. It loses precision and is slow.
  static toSeconds(d: duration) {
    return Number(d) / TIME_UNITS_PER_SEC;
  }

  // Convert time to seconds as a number.
  // Use this function with caution. It loses precision and is slow.
  static toMilliseconds(d: duration) {
    return Number(d) / TIME_UNITS_PER_MILLISEC;
  }

  // Convert time to seconds as a number.
  // Use this function with caution. It loses precision and is slow.
  static toMicroSeconds(d: duration) {
    return Number(d) / TIME_UNITS_PER_MICROSEC;
  }

  // Print duration as as human readable string - i.e. to only a handful of
  // significant figues.
  // Use this when readability is more desireable than precision.
  // Examples: 1234 -> 1.23ns
  //           123456789 -> 123ms
  //           123,123,123,123,123 -> 34h 12m
  //           1,000,000,023 -> 1 s
  //           1,230,000,023 -> 1.2 s
  static humanise(dur: duration): string {
    const sec = Duration.toSeconds(dur);
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

  // Print duration with absolute precision.
  static format(duration: duration): string {
    let result = '';
    if (duration < 1) return '0s';
    const unitAndValue: [string, bigint][] = [
      ['h', 3_600_000_000_000n],
      ['m', 60_000_000_000n],
      ['s', 1_000_000_000n],
      ['ms', 1_000_000n],
      ['us', 1_000n],
      ['ns', 1n],
    ];
    unitAndValue.forEach(([unit, unitSize]) => {
      if (duration >= unitSize) {
        const unitCount = duration / unitSize;
        result += unitCount.toLocaleString() + unit + ' ';
        duration = duration % unitSize;
      }
    });
    return result.slice(0, -1);
  }

  static formatSeconds(dur: duration): string {
    return Duration.toSeconds(dur).toString() + ' s';
  }

  static formatMilliseconds(dur: duration): string {
    return Duration.toMilliseconds(dur).toString() + ' s';
  }

  static formatMicroseconds(dur: duration): string {
    return Duration.toMicroSeconds(dur).toString() + ' s';
  }
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

  constructor(time: time) {
    this.sign = time < 0 ? '-' : '';

    const absTime = BigintMath.abs(time);

    const days = absTime / 86_400_000_000_000n;
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

export function currentDateHourAndMinute(): string {
  const date = new Date();
  return `${date
    .toISOString()
    .substr(0, 10)}-${date.getHours()}-${date.getMinutes()}`;
}

export class TimeSpan {
  static readonly ZERO = new TimeSpan(Time.ZERO, Time.ZERO);

  readonly start: time;
  readonly end: time;

  constructor(start: time, end: time) {
    assertTrue(
      start <= end,
      `Span start [${start}] cannot be greater than end [${end}]`,
    );
    this.start = start;
    this.end = end;
  }

  static fromTimeAndDuration(start: time, duration: duration): TimeSpan {
    return new TimeSpan(start, Time.add(start, duration));
  }

  get duration(): duration {
    return this.end - this.start;
  }

  get midpoint(): time {
    return Time.fromRaw((this.start + this.end) / 2n);
  }

  contains(t: time): boolean {
    return this.start <= t && t < this.end;
  }

  containsSpan(start: time, end: time): boolean {
    return this.start <= start && end <= this.end;
  }

  overlaps(start: time, end: time): boolean {
    return !(end <= this.start || start >= this.end);
  }

  equals(span: TimeSpan): boolean {
    return this.start === span.start && this.end === span.end;
  }

  translate(x: duration): TimeSpan {
    return new TimeSpan(Time.add(this.start, x), Time.add(this.end, x));
  }

  pad(padding: duration): TimeSpan {
    return new TimeSpan(
      Time.sub(this.start, padding),
      Time.add(this.end, padding),
    );
  }
}

// Print the date only for a given date in ISO format.
export function toISODateOnly(date: Date) {
  const year = date.getUTCFullYear();
  const month = String(date.getUTCMonth() + 1).padStart(2, '0');
  const day = String(date.getUTCDate()).padStart(2, '0');

  return `${year}-${month}-${day}`;
}
