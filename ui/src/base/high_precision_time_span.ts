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

import {TimeSpan, time} from './time';
import {HighPrecisionTime} from './high_precision_time';

/**
 * Represents a time span using a high precision time value to represent the
 * start of the span, and a number to represent the duration of the span.
 */
export class HighPrecisionTimeSpan {
  static readonly ZERO = new HighPrecisionTimeSpan(HighPrecisionTime.ZERO, 0);

  readonly start: HighPrecisionTime;
  readonly duration: number;

  constructor(start: HighPrecisionTime, duration: number) {
    this.start = start;
    this.duration = duration;
  }

  /**
   * Create a new span from integral start and end points.
   *
   * @param start The start of the span.
   * @param end The end of the span.
   */
  static fromTime(start: time, end: time): HighPrecisionTimeSpan {
    return new HighPrecisionTimeSpan(
      new HighPrecisionTime(start),
      Number(end - start),
    );
  }

  /**
   * The center point of the span.
   */
  get midpoint(): HighPrecisionTime {
    return this.start.addNumber(this.duration / 2);
  }

  /**
   * The end of the span.
   */
  get end(): HighPrecisionTime {
    return this.start.addNumber(this.duration);
  }

  /**
   * Checks if this span exactly equals another.
   */
  equals(other: HighPrecisionTimeSpan): boolean {
    return this.start.equals(other.start) && this.duration === other.duration;
  }

  /**
   * Create a new span with the same duration but the start point moved through
   * time by some amount of time.
   */
  translate(time: number): HighPrecisionTimeSpan {
    return new HighPrecisionTimeSpan(this.start.addNumber(time), this.duration);
  }

  /**
   * Create a new span with the the start of the span moved backward and the end
   * of the span moved forward by a certain amount of time.
   */
  pad(time: number): HighPrecisionTimeSpan {
    return new HighPrecisionTimeSpan(
      this.start.subNumber(time),
      this.duration + 2 * time,
    );
  }

  /**
   * Create a new span which is zoomed in or out centered on a specific point.
   *
   * @param ratio The scaling ratio, the new duration will be the current
   * duration * ratio.
   * @param center The center point as a normalized value between 0 and 1 where
   * 0 is the start of the time window and 1 is the end.
   * @param minDur Don't allow the time span to become shorter than this.
   */
  scale(ratio: number, center: number, minDur: number): HighPrecisionTimeSpan {
    const currentDuration = this.duration;
    const newDuration = Math.max(currentDuration * ratio, minDur);
    // Delta between new and old duration
    // +ve if new duration is shorter than old duration
    const durationDeltaNanos = currentDuration - newDuration;
    // If offset is 0, don't move the start at all
    // If offset if 1, move the start by the amount the duration has changed
    // If new duration is shorter - move start to right
    // If new duration is longer - move start to left
    const start = this.start.addNumber(durationDeltaNanos * center);
    return new HighPrecisionTimeSpan(start, newDuration);
  }

  /**
   * Create a new span that represents the intersection of this span with
   * another.
   *
   * If the two spans do not overlap at all, the empty span is returned.
   *
   * @param start THe start of the other span.
   * @param end The end of the other span.
   */
  intersect(start: time, end: time): HighPrecisionTimeSpan {
    if (!this.overlaps(start, end)) {
      return HighPrecisionTimeSpan.ZERO;
    }
    const newStart = this.start.clamp(start, end);
    const newEnd = this.end.clamp(start, end);
    const newDuration = newEnd.sub(newStart).toNumber();
    return new HighPrecisionTimeSpan(newStart, newDuration);
  }

  /**
   * Create a new timespan which fits within the specified bounds, preserving
   * its duration if possible.
   *
   * This function moves the timespan forwards or backwards in time while
   * keeping its duration unchanged, so that it fits entirely within the range
   * defined by `start` and `end`.
   *
   * If the specified bounds are smaller than the current timespan's duration, a
   * new timespan matching the bounds is returned.
   *
   * @param start The start of the bounds within which the timespan should fit.
   * @param end The end of the bounds within which the timespan should fit.
   *
   * @example
   * // assume `timespan` is defined as: [5, 8)
   * timespan.fitWithin(10n, 20n); // -> [10, 13)
   * timespan.fitWithin(-10n, -5n); // -> [-8, -5)
   * timespan.fitWithin(1n, 2n); // -> [1, 2)
   */
  fitWithin(start: time, end: time): HighPrecisionTimeSpan {
    if (this.duration > Number(end - start)) {
      // Current span is greater than the limits
      return HighPrecisionTimeSpan.fromTime(start, end);
    }
    if (this.start.integral < start) {
      // Current span starts before limits
      return new HighPrecisionTimeSpan(
        new HighPrecisionTime(start),
        this.duration,
      );
    }
    if (this.end.gt(end)) {
      // Current span ends after limits
      return new HighPrecisionTimeSpan(
        new HighPrecisionTime(end).subNumber(this.duration),
        this.duration,
      );
    }
    return this;
  }

  /**
   * Clamp duration to some minimum value. The start remains the same, just the
   * duration is changed.
   */
  clampDuration(minDuration: number): HighPrecisionTimeSpan {
    if (this.duration < minDuration) {
      return new HighPrecisionTimeSpan(this.start, minDuration);
    } else {
      return this;
    }
  }

  /**
   * Checks whether this span completely contains a time instant.
   */
  contains(t: time): boolean {
    return this.start.lte(t) && this.end.gt(t);
  }

  /**
   * Checks whether this span entirely contains another span.
   *
   * @param start The start of the span to check.
   * @param end The end of the span to check.
   */
  containsSpan(start: time, end: time): boolean {
    return this.start.lte(start) && this.end.gte(end);
  }

  /**
   * Checks if this span overlaps at all with another.
   *
   * @param start The start of the span to check.
   * @param end The end of the span to check.
   */
  overlaps(start: time, end: time): boolean {
    return !(this.start.gte(end) || this.end.lte(start));
  }

  /**
   * Get the span of integer intervals values that overlap this span.
   */
  toTimeSpan(): TimeSpan {
    return new TimeSpan(this.start.toTime('floor'), this.end.toTime('ceil'));
  }
}
